#!/usr/bin/env python3
"""LinkDesk registration + opaque TLS stream relay. Python 3.10+, stdlib only."""
import argparse
import asyncio
import hashlib
import hmac
import json
import logging
import os
import secrets
import signal
import sqlite3
import ssl
import socket
import ipaddress
import struct
import time
from dataclasses import dataclass, field
from pathlib import Path

LOG = logging.getLogger("linkdesk")
MAX_JSON = 16384


class Rejected(Exception):
    pass


async def read_json(reader, timeout=120):
    header = await asyncio.wait_for(reader.readexactly(5), timeout)
    kind, size = struct.unpack("!BI", header)
    if kind != 1 or not 0 < size <= MAX_JSON:
        raise Rejected("Invalid packet")
    payload = await asyncio.wait_for(reader.readexactly(size), timeout)
    try:
        value = json.loads(payload)
    except (ValueError, UnicodeError):
        raise Rejected("Invalid JSON") from None
    if not isinstance(value, dict):
        raise Rejected("Expected object")
    return value


async def send_json(writer, value):
    data = json.dumps(value, separators=(",", ":")).encode()
    writer.write(struct.pack("!BI", 1, len(data)) + data)
    await asyncio.wait_for(writer.drain(), 10)


def token_hash(token):
    if not isinstance(token, str) or len(token) != 64:
        raise Rejected("Invalid device credential")
    try:
        bytes.fromhex(token)
    except ValueError:
        raise Rejected("Invalid device credential") from None
    return hashlib.sha256(token.encode()).hexdigest()


@dataclass
class Device:
    id: str
    writer: asyncio.StreamWriter
    available: bool = False
    fingerprint: str = ""
    lan: list = field(default_factory=list)
    lan_port: int = 0
    instance: str = ""
    last_seen: float = field(default_factory=time.monotonic)


@dataclass
class Session:
    id: str
    target: Device
    viewer_id: str
    viewer_writer: asyncio.StreamWriter
    ticket: str
    joined: asyncio.Event = field(default_factory=asyncio.Event)
    host_reader: asyncio.StreamReader = None
    host_writer: asyncio.StreamWriter = None
    done: asyncio.Event = field(default_factory=asyncio.Event)


class RelayServer:
    def __init__(self, database, enrollment_key, max_connections=256, max_sessions=64):
        if len(enrollment_key) < 32:
            raise ValueError("LINKDESK_ENROLLMENT_KEY must contain at least 32 characters")
        self.key = enrollment_key
        self.db = sqlite3.connect(database)
        self.db.execute("CREATE TABLE IF NOT EXISTS devices (id TEXT PRIMARY KEY, token_hash TEXT UNIQUE NOT NULL)")
        self.db.commit()
        self.devices = {}
        self.sessions = {}
        self.attempts = {}
        self.connections = set()
        self.max_connections = max_connections
        self.max_sessions = max_sessions
        self.server = None

    def credential(self, message):
        if not isinstance(message.get("key"), str) or not hmac.compare_digest(message["key"], self.key):
            raise Rejected("服务器接入密钥错误")
        if message.get("version") != 1:
            raise Rejected("客户端协议版本不兼容")
        return token_hash(message.get("token"))

    def register_id(self, message, digest):
        requested = message.get("id", "")
        if not isinstance(requested, str):
            raise Rejected("Invalid device ID")
        row = self.db.execute("SELECT id, token_hash FROM devices WHERE token_hash = ?", (digest,)).fetchone()
        if requested and (not row or row[0] != requested):
            raise Rejected("本机设备凭据与 ID 不匹配")
        if row:
            return row[0]
        if self.db.execute("SELECT count(*) FROM devices").fetchone()[0] >= 10000:
            raise Rejected("设备注册数量达到限制")
        while True:
            device_id = str(secrets.randbelow(900000000) + 100000000)
            try:
                self.db.execute("INSERT INTO devices VALUES (?, ?)", (device_id, digest))
                self.db.commit()
                return device_id
            except sqlite3.IntegrityError:
                continue

    def verify_id(self, message, digest):
        device_id = message.get("id")
        if not isinstance(device_id, str):
            raise Rejected("Invalid device ID")
        row = self.db.execute("SELECT token_hash FROM devices WHERE id = ?", (device_id,)).fetchone()
        if not row or not hmac.compare_digest(row[0], digest) or device_id not in self.devices:
            raise Rejected("设备未注册或已离线")
        return device_id

    def busy(self, device_id):
        return any(s.target.id == device_id and not s.done.is_set() for s in self.sessions.values())

    async def start(self, host, port, context):
        self.server = await asyncio.start_server(self.handle, host, port, ssl=context,
            ssl_handshake_timeout=30, limit=1024*1024, backlog=128)
        # Keep TLS TCP segments below common tunnel/overlay path MTUs.
        # Apply to the listener so both peers negotiate the limit at SYN time.
        if hasattr(socket,"TCP_MAXSEG"):
            for listener in self.server.sockets:
                try:
                    listener.setsockopt(socket.IPPROTO_TCP,socket.TCP_MAXSEG,1200)
                except OSError:
                    pass
        return self.server

    async def close(self):
        if self.server:
            self.server.close()
        for writer in tuple(self.connections):
            writer.close()
        # Wake pending session handlers and give them time to release their resources.
        for session in tuple(self.sessions.values()):
            session.joined.set()
            session.done.set()
        if self.server:
            try:
                await asyncio.wait_for(self.server.wait_closed(), 1)
            except asyncio.TimeoutError:
                for writer in tuple(self.connections):
                    writer.transport.abort()
        await asyncio.sleep(0)

    async def handle(self, reader, writer):
        if len(self.connections) >= self.max_connections:
            writer.close()
            return
        self.connections.add(writer)
        writer.transport.set_write_buffer_limits(high=1024*1024, low=256*1024)
        transport_socket=writer.get_extra_info("socket")
        if transport_socket is not None and hasattr(socket,"TCP_CONGESTION"):
            try:
                transport_socket.setsockopt(socket.IPPROTO_TCP,socket.TCP_CONGESTION,b"bbr")
            except OSError:
                pass  # BBR is optional; use the OS default if unavailable.
        address = writer.get_extra_info("peername")
        ip = address[0] if address else "unknown"
        now = time.monotonic()
        self.attempts = {k: v for k, v in self.attempts.items() if now - v[0] < 60}
        count = self.attempts.get(ip, (now, 0))[1]
        try:
            if count >= 60 or len(self.attempts) >= 4096:
                raise Rejected("连接过于频繁，请稍后重试")
            since, count = self.attempts.get(ip, (now, 0))
            self.attempts[ip] = (since, count + 1)
            message = await read_json(reader, 45)
            digest = self.credential(message)
            action = message.get("type")
            if action == "register":
                await self.register(reader, writer, message, digest)
            elif action == "connect":
                await self.connect_session(reader, writer, message, digest)
            elif action == "join":
                await self.join(reader, writer, message, digest)
            else:
                raise Rejected("Unknown request")
        except Rejected as exc:
            try:
                await send_json(writer, {"type": "error", "message": str(exc)})
            except (OSError, asyncio.TimeoutError):
                pass
        except asyncio.TimeoutError:
            LOG.warning("Connection timed out from %s", ip)
        except (OSError, asyncio.IncompleteReadError, ssl.SSLError):
            pass
        except Exception:
            LOG.exception("Unexpected relay handler error")
        finally:
            self.connections.discard(writer)
            writer.close()
            try:
                await asyncio.wait_for(writer.wait_closed(), 2)
            except (OSError, asyncio.TimeoutError):
                pass

    async def register(self, reader, writer, message, digest):
        device_id = self.register_id(message, digest)
        instance = message.get("instance", "")
        if not isinstance(instance, str) or (instance and (len(instance) != 32 or
                any(c not in "0123456789abcdef" for c in instance))):
            raise Rejected("Invalid client instance")
        device = Device(device_id, writer, instance=instance)
        self.availability(device, message)
        previous = self.devices.get(device_id)
        if previous:
            same_instance = bool(instance) and instance == previous.instance
            stale = previous.writer.is_closing() or time.monotonic() - previous.last_seen > 15
            if not same_instance and not stale:
                raise Rejected("此设备 ID 已在线，请关闭另一份客户端；若刚意外退出，请等待约 15 秒后重试")
            # register_id validated device credentials; discard sessions before publishing the replacement.
            self.end_for(device_id)
            previous.writer.close()
            previous.writer.transport.abort()
            LOG.info("Device %s replacing old control connection (%s)", device_id,
                     "same instance" if same_instance else "stale")
        self.devices[device_id] = device
        LOG.info("Device %s online", device_id)
        try:
            await send_json(writer, {"type": "registered", "id": device_id, "version": 1, "lanDirect": True})
            while True:
                msg = await read_json(reader, 300)
                if self.devices.get(device_id) is not device:
                    return
                device.last_seen = time.monotonic()
                action = msg.get("type")
                if action == "ping":
                    await send_json(writer, {"type": "pong"})
                elif action == "resolve":
                    requested=msg.get("target")
                    target=self.devices.get(requested) if isinstance(requested,str) else None
                    if not target or not target.available:
                        await send_json(writer,{"type":"resolved","target":msg.get("target"),"lan":[],"port":0})
                    else:
                        await send_json(writer,{"type":"resolved","target":target.id,"lan":target.lan,"port":target.lan_port,"fingerprint":target.fingerprint})
                elif action == "available":
                    self.availability(device, msg)
                    if not device.available:
                        self.end_for(device_id, hosts_only=True)
                    await send_json(writer, {"type": "availability", "enabled": device.available})
                elif action == "end":
                    self.end_for(device_id, hosts_only=True)
                else:
                    raise Rejected("Invalid control request")
        finally:
            if self.devices.get(device_id) is device:
                del self.devices[device_id]
                self.end_for(device_id)
                LOG.info("Device %s offline", device_id)

    @staticmethod
    def availability(device, message):
        enabled = message.get("enabled", message.get("available")) is True
        fingerprint = message.get("fingerprint", "")
        if enabled and (not isinstance(fingerprint, str) or len(fingerprint) != 64 or
                        any(c not in "0123456789abcdef" for c in fingerprint)):
            raise Rejected("Invalid peer certificate fingerprint")
        device.available = enabled
        device.fingerprint = fingerprint if enabled else ""
        device.lan=[]; device.lan_port=0
        addresses=message.get("lan",[]); port=message.get("lanPort",0)
        if enabled and isinstance(addresses,list) and isinstance(port,int) and not isinstance(port,bool) and 0<port<=65535:
            for value in addresses[:8]:
                try:
                    address=ipaddress.IPv4Address(value)
                    if address.is_private and not address.is_loopback and not address.is_multicast and not address.is_unspecified:
                        device.lan.append(str(address))
                except (ValueError,TypeError,ipaddress.AddressValueError):
                    continue
            device.lan_port=port

    def end_for(self, device_id, hosts_only=False):
        for s in tuple(self.sessions.values()):
            if s.target.id == device_id or (not hosts_only and s.viewer_id == device_id):
                self.sessions.pop(s.id, None)
                s.viewer_writer.close()
                if s.host_writer:
                    s.host_writer.close()
                s.joined.set()
                s.done.set()

    async def connect_session(self, reader, writer, message, digest):
        viewer_id = self.verify_id(message, digest)
        target_id = message.get("target")
        if not isinstance(target_id, str):
            raise Rejected("Invalid target")
        target = self.devices.get(target_id)
        if not target or not target.available:
            raise Rejected("远程设备离线或尚未开启共享")
        if viewer_id == target_id:
            raise Rejected("不能通过中继连接本机设备 ID")
        if self.busy(target_id):
            raise Rejected("远程设备已有连接请求或会话")
        if len(self.sessions) >= self.max_sessions:
            raise Rejected("中继服务器会话数量达到限制")
        s = Session(secrets.token_hex(16), target, viewer_id, writer, secrets.token_hex(32))
        self.sessions[s.id] = s
        try:
            await send_json(target.writer, {"type": "incoming", "session": s.id, "ticket": s.ticket, "viewer": viewer_id})
            await asyncio.wait_for(s.joined.wait(), 20)
            if not s.host_writer or s.done.is_set():
                raise Rejected("远程设备没有接受中继连接")
            await send_json(writer, {"type": "ready", "session": s.id, "fingerprint": target.fingerprint})
            await send_json(s.host_writer, {"type": "ready", "session": s.id})
            tasks = [asyncio.create_task(self.pump(reader, s.host_writer)),
                     asyncio.create_task(self.pump(s.host_reader, writer))]
            try:
                await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            finally:
                for task in tasks:
                    task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
        finally:
            self.sessions.pop(s.id, None)
            s.done.set()
            if s.host_writer:
                s.host_writer.close()
            if not target.writer.is_closing():
                try:
                    await send_json(target.writer, {"type": "session_end", "session": s.id})
                except (OSError, asyncio.TimeoutError):
                    pass

    async def join(self, reader, writer, message, digest):
        device_id = self.verify_id(message, digest)
        session_id, ticket = message.get("session"), message.get("ticket")
        if not isinstance(session_id, str) or not isinstance(ticket, str):
            raise Rejected("Invalid ticket")
        s = self.sessions.get(session_id)
        if not s or s.target.id != device_id or s.host_writer or s.done.is_set() or not hmac.compare_digest(ticket, s.ticket):
            raise Rejected("中继凭据无效或已使用")
        s.host_reader, s.host_writer = reader, writer
        s.ticket = ""  # Single use.
        s.joined.set()
        await s.done.wait()

    @staticmethod
    async def pump(reader, writer):
        # These bytes are the peer-to-peer TLS stream: do not decode passwords or frames.
        while True:
            chunk = await asyncio.wait_for(reader.read(128*1024), 120)
            if not chunk:
                return
            writer.write(chunk)
            await asyncio.wait_for(writer.drain(), 90)


async def main(args):
    key = os.environ.get("LINKDESK_ENROLLMENT_KEY", "")
    Path(args.database).parent.mkdir(parents=True, exist_ok=True)
    relay = RelayServer(args.database, key, args.max_connections, args.max_sessions)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(args.cert, args.private_key)
    listener = await relay.start(args.bind, args.port, context)
    actual_port = listener.sockets[0].getsockname()[1]
    LOG.info("LinkDesk relay listening on %s:%s", args.bind, actual_port)
    stop = asyncio.Event()
    for signum in (signal.SIGINT, signal.SIGTERM):
        asyncio.get_running_loop().add_signal_handler(signum, stop.set)
    await stop.wait()
    await relay.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=443)
    parser.add_argument("--cert", required=True)
    parser.add_argument("--private-key", required=True)
    parser.add_argument("--database", default="/var/lib/linkdesk/devices.sqlite3")
    parser.add_argument("--max-connections", type=int, default=256)
    parser.add_argument("--max-sessions", type=int, default=64)
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    asyncio.run(main(args))

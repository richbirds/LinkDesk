import asyncio
import hashlib
import secrets
import ssl
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from relay_server import RelayServer, read_json, send_json


class RelayTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-sha256', '-days', '1',
                        '-keyout', str(root/'key.pem'), '-out', str(root/'cert.pem'), '-subj', '/CN=localhost'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(root/'cert.pem', root/'key.pem')
        self.client_context = ssl.create_default_context(cafile=str(root/'cert.pem'))
        self.client_context.check_hostname = False
        self.key = secrets.token_hex(32)
        self.relay = RelayServer(root/'devices.sqlite3', self.key)
        listener = await self.relay.start('127.0.0.1', 0, context)
        self.port = listener.sockets[0].getsockname()[1]
        self.writers = []

    async def asyncTearDown(self):
        for writer in self.writers:
            writer.close()
        await self.relay.close()
        await asyncio.sleep(0.05)
        self.relay.db.close()
        self.temp.cleanup()

    async def channel(self):
        reader, writer = await asyncio.open_connection('127.0.0.1', self.port, ssl=self.client_context)
        self.writers.append(writer)
        return reader, writer

    def auth(self, token, kind='register', **extra):
        return dict(type=kind, version=1, key=self.key, token=token, **extra)

    async def register(self, available=False, instance=""):
        token = secrets.token_hex(32)
        reader, writer = await self.channel()
        await send_json(writer, self.auth(token, available=available, fingerprint='a'*64, instance=instance))
        response = await read_json(reader)
        self.assertEqual(response['type'], 'registered')
        return response['id'], token, reader, writer

    async def test_machine_identity_migration_rotates_and_retries(self):
        device_id, old, _, old_writer = await self.register(True)
        new = secrets.token_hex(32)
        async def migrate(token, replacement, key=None):
            reader, writer = await self.channel()
            message = self.auth(token, 'migrate', id=device_id, newToken=replacement)
            if key is not None:
                message['key'] = key
            await send_json(writer, message)
            return await read_json(reader)
        self.assertEqual((await migrate(secrets.token_hex(32), new))['type'], 'error')
        self.assertEqual((await migrate(old, new, 'wrong-key'))['type'], 'error')
        self.assertEqual((await migrate(old, old))['type'], 'error')
        self.assertEqual((await migrate(old, new)), {'type': 'migrated', 'id': device_id})
        self.assertNotIn(device_id, self.relay.devices)
        reader, writer = await self.channel()
        await send_json(writer, self.auth(old, id=device_id))
        self.assertEqual((await read_json(reader))['type'], 'error')
        reader, writer = await self.channel()
        await send_json(writer, self.auth(new, id=device_id, available=True, fingerprint='b'*64))
        self.assertEqual((await read_json(reader))['id'], device_id)
        active = self.relay.devices[device_id]
        self.assertEqual((await migrate(old, new))['type'], 'migrated')
        self.assertIs(self.relay.devices[device_id], active)
        self.assertEqual((await migrate(old, secrets.token_hex(32)))['type'], 'error')

    async def test_id_hijack_and_wrong_enrollment_key(self):
        device_id, token, _, _ = await self.register()
        reader, writer = await self.channel()
        await send_json(writer, self.auth(secrets.token_hex(32), id=device_id))
        self.assertEqual((await read_json(reader))['type'], 'error')
        reader, writer = await self.channel()
        message = self.auth(token)
        message['key'] = 'b'*64
        await send_json(writer, message)
        self.assertIn('密钥错误', (await read_json(reader))['message'])
        self.assertEqual(len(self.relay.devices), 1)

    async def test_opaque_forwarding_and_ticket_one_use(self):
        host_id, host_token, host_control, _ = await self.register(True)
        viewer_id, viewer_token, _, _ = await self.register()
        viewer_read, viewer_write = await self.channel()
        await send_json(viewer_write, self.auth(viewer_token, 'connect', id=viewer_id, target=host_id))
        incoming = await read_json(host_control)
        host_read, host_write = await self.channel()
        join = self.auth(host_token, 'join', id=host_id, session=incoming['session'], ticket=incoming['ticket'])
        await send_json(host_write, join)
        self.assertEqual((await read_json(viewer_read))['fingerprint'], 'a'*64)
        self.assertEqual((await read_json(host_read))['type'], 'ready')
        replay_read, replay_write = await self.channel()
        await send_json(replay_write, join)
        self.assertIn('已使用', (await read_json(replay_read))['message'])
        payload = b'\x16\x03\x03' + secrets.token_bytes(200000)
        viewer_write.write(payload)
        await viewer_write.drain()
        self.assertEqual(await asyncio.wait_for(host_read.readexactly(len(payload)), 5), payload)
        host_write.write(payload[::-1])
        await host_write.drain()
        self.assertEqual(await asyncio.wait_for(viewer_read.readexactly(len(payload)), 5), payload[::-1])
        viewer_write.close()
        end = await read_json(host_control)
        self.assertEqual(end['type'], 'session_end')
        self.assertFalse(self.relay.sessions)

    async def test_offline_and_unavailable(self):
        target_id, _, _, _ = await self.register(False)
        device_id, token, _, _ = await self.register()
        for target in (target_id, '000000000'):
            reader, writer = await self.channel()
            await send_json(writer, self.auth(token, 'connect', id=device_id, target=target))
            self.assertIn('离线', (await read_json(reader))['message'])
        self.assertFalse(self.relay.sessions)

    async def test_oversized_packet_rejected_without_payload(self):
        reader, writer = await self.channel()
        writer.write(struct.pack('!BI', 1, 16385))
        await writer.drain()
        self.assertEqual((await read_json(reader))['type'], 'error')
        self.assertFalse(self.relay.devices)

    async def test_identity_survives_disconnect(self):
        device_id, token, _, writer = await self.register()
        writer.close()
        await writer.wait_closed()
        for _ in range(100):
            if device_id not in self.relay.devices:
                break
            await asyncio.sleep(.01)
        reader, writer = await self.channel()
        await send_json(writer, self.auth(token, id=device_id))
        self.assertEqual((await read_json(reader))['id'], device_id)
        digest = self.relay.db.execute('SELECT token_hash FROM devices WHERE id=?', (device_id,)).fetchone()[0]
        self.assertEqual(digest, hashlib.sha256(token.encode()).hexdigest())

    async def test_same_instance_replaces_live_connection_without_cleanup_race(self):
        instance = secrets.token_hex(16)
        device_id, token, _, _ = await self.register(instance=instance)
        old = self.relay.devices[device_id]
        for _ in range(3):
            reader, writer = await self.channel()
            await send_json(writer, self.auth(token, id=device_id, instance=instance))
            self.assertEqual((await read_json(reader))['type'], 'registered')
            await asyncio.sleep(.03)  # Let the previous handler's finally run.
            self.assertIsNot(self.relay.devices[device_id], old)
            await send_json(writer, {'type': 'ping'})
            self.assertEqual((await read_json(reader))['type'], 'pong')
            self.assertEqual(len(self.relay.devices), 1)
            old = self.relay.devices[device_id]

    async def test_fresh_other_instance_rejected_but_stale_recoverable(self):
        device_id, token, original_reader, original_writer = await self.register(instance=secrets.token_hex(16))
        original = self.relay.devices[device_id]
        reader, writer = await self.channel()
        await send_json(writer, self.auth(token, id=device_id, instance=secrets.token_hex(16)))
        self.assertIn('已在线', (await read_json(reader))['message'])
        self.assertIs(self.relay.devices[device_id], original)
        await send_json(original_writer, {'type': 'ping'})
        self.assertEqual((await read_json(original_reader))['type'], 'pong')
        original.last_seen -= 16
        reader, writer = await self.channel()
        await send_json(writer, self.auth(token, id=device_id, instance=secrets.token_hex(16)))
        self.assertEqual((await read_json(reader))['type'], 'registered')
        await asyncio.sleep(.03)
        self.assertIsNot(self.relay.devices[device_id], original)

    async def test_replacement_frees_pending_session_immediately(self):
        instance=secrets.token_hex(16)
        host_id, host_token, host_control, _ = await self.register(True,instance)
        viewer_id, viewer_token, _, _ = await self.register()
        reader, writer = await self.channel()
        await send_json(writer, self.auth(viewer_token,'connect',id=viewer_id,target=host_id))
        await read_json(host_control)
        self.assertTrue(self.relay.busy(host_id))
        replacement_reader, replacement_writer = await self.channel()
        await send_json(replacement_writer, self.auth(host_token,id=host_id,instance=instance,available=True,fingerprint='a'*64))
        self.assertEqual((await read_json(replacement_reader))['type'],'registered')
        self.assertFalse(self.relay.busy(host_id))
        self.assertFalse(self.relay.sessions)


if __name__ == '__main__':
    unittest.main()

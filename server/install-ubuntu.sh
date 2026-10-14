#!/usr/bin/env bash
set -euo pipefail
if [[ "$EUID" -ne 0 ]]; then
  echo '请使用 sudo 运行此脚本。' >&2
  exit 1
fi
SERVER_HOST="${1:?用法: sudo ./install-ubuntu.sh 公网IP或域名 [端口，默认443]}"
SERVER_PORT="${2:-443}"
python3 - "$SERVER_HOST" "$SERVER_PORT" <<'PY'
import re, sys
host, port = sys.argv[1:]
if not re.fullmatch(r'[A-Za-z0-9.:-]{1,253}', host) or not port.isdigit() or not 1 <= int(port) <= 65535:
    raise SystemExit('服务器地址或端口无效')
if sys.version_info < (3, 10):
    raise SystemExit('需要 Ubuntu 22.04/24.04 或 Python 3.10+')
PY
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
apt-get update
apt-get install -y python3 openssl ca-certificates
if ! id linkdesk >/dev/null 2>&1; then
  useradd --system --home /var/lib/linkdesk --shell /usr/sbin/nologin linkdesk
fi
install -d -o root -g root -m 0755 /opt/linkdesk
install -d -o root -g linkdesk -m 0750 /etc/linkdesk
install -d -o linkdesk -g linkdesk -m 0700 /var/lib/linkdesk
install -o root -g root -m 0644 "$SCRIPT_DIR/relay_server.py" /opt/linkdesk/relay_server.py
umask 077
if [[ ! -f /etc/linkdesk/server.pem || ! -f /etc/linkdesk/server.key ]]; then
  openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
    -keyout /etc/linkdesk/server.key -out /etc/linkdesk/server.pem -subj '/CN=LinkDesk Relay'
fi
chown root:linkdesk /etc/linkdesk/server.key /etc/linkdesk/server.pem
chmod 0640 /etc/linkdesk/server.key /etc/linkdesk/server.pem
python3 - "$SERVER_HOST" "$SERVER_PORT" <<'PY'
import hashlib, json, os, secrets, ssl, sys
from pathlib import Path
host, port = sys.argv[1:]
env = Path('/etc/linkdesk/server.env')
old = dict(line.split('=', 1) for line in env.read_text().splitlines() if '=' in line) if env.exists() else {}
key = old.get('LINKDESK_ENROLLMENT_KEY') or secrets.token_hex(32)
env.write_text(f'LINKDESK_ENROLLMENT_KEY={key}\nLINKDESK_PORT={int(port)}\n')
os.chmod(env, 0o600)
cert = ssl.PEM_cert_to_DER_cert(Path('/etc/linkdesk/server.pem').read_text())
config = {'server': host, 'port': int(port), 'fingerprint': hashlib.sha256(cert).hexdigest(), 'enrollmentKey': key}
path = Path('/etc/linkdesk/linkdesk-server.json')
path.write_text(json.dumps(config, indent=2) + '\n')
os.chmod(path, 0o600)
print('客户端配置已生成：/etc/linkdesk/linkdesk-server.json')
print('服务器证书 SHA-256：' + config['fingerprint'])
PY
install -o root -g root -m 0644 "$SCRIPT_DIR/linkdesk-relay.service" /etc/systemd/system/linkdesk-relay.service
systemctl daemon-reload
systemctl enable linkdesk-relay
systemctl restart linkdesk-relay
systemctl --no-pager --full status linkdesk-relay
echo "请在云平台安全组和本机防火墙放行 TCP $SERVER_PORT，并将客户端配置文件安全复制到两台 Mac。"

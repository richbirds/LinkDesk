# Ubuntu 公网中继部署

适用于 Ubuntu 22.04 / 24.04，Python 3.10+，无 pip 依赖。两台 Mac 均主动建立到服务器的 TLS 连接，无需配置家庭路由器端口映射。

## 安装

将整个 `server/` 文件夹上传到 Ubuntu，在服务器运行：

```bash
sudo bash server/install-ubuntu.sh YOUR_PUBLIC_IP_OR_DOMAIN 443
```

脚本安装 Python/OpenSSL，创建独立 `linkdesk` 服务用户，生成本机独立 TLS 证书和接入密钥，启用 systemd 自启动，并输出证书指纹。再次执行会保留现有证书、密钥和设备数据库。

默认占用 TCP 443。如果服务器已有网站占用 443，可改用 45889，并在客户端导入对应配置。这里的 443 提供原生 TLS 协议，不是 HTTPS；要求 HTTP 代理或 DPI 白名单的企业网络可能不允许。

在云平台安全组放行所选 TCP 端口。本机 UFW 已启用时：

```bash
sudo ufw allow 443/tcp
```

安装脚本不会主动修改防火墙或 SSH 规则。

## 客户端配置

部署生成 `/etc/linkdesk/linkdesk-server.json`（root 权限，包含接入密钥）。安全复制该文件到两台 Mac，在软件左侧「服务器设置」中导入，然后点击「保存并连接」。普通使用只需九位设备 ID 和八位临时连接密码。

如果使用普通 SSH 用户，可在服务器临时导出配置供自己下载：

```bash
sudo install -o "$(id -un)" -g "$(id -gn)" -m 0600 /etc/linkdesk/linkdesk-server.json "$HOME/linkdesk-server.json"
```

然后在 Mac 上执行（替换用户和服务器地址）：

```bash
scp ubuntu@YOUR_SERVER:linkdesk-server.json ./linkdesk-server.json
```

配置文件只分发给自己的客户端。它允许接入这台服务器，但不能替代被控端的连接密码。不要复制客户端的设备凭据文件到另一台 Mac：每台机器应自行注册独立 ID。

## 运维

```bash
sudo systemctl status linkdesk-relay
sudo journalctl -u linkdesk-relay -f
sudo systemctl restart linkdesk-relay
```

数据：`/var/lib/linkdesk/devices.sqlite3`。证书和密钥：`/etc/linkdesk/`。更新 `relay_server.py` 后重启服务即可，客户端会自动重连并保留设备 ID。备份设备数据库和证书；数据库丢失会造成旧 ID 无法恢复。轮换服务器证书后需重新分发指纹配置。

服务端存储设备 ID 和本机身份 token 的 SHA-256；不存储远程桌面连接密码，不解析会话画面。中继内部承载两端之间的 TLS 字节流。服务器仍是设备 ID 与证书指纹映射的信任方，需要妥善管理。

## 容量

首版画面负载限制约 6Mbps，每个会话允许 150KB 突发，协议开销另计。4 核 4GB 足够用于小规模试用；15Mbps 出站带宽先按 1 路使用，两路需实测延迟与流量。6Mbps 连续一小时约 2.7GB 出站流量，请核实套餐月流量规则。没有录屏和文件存储，磁盘主要用于设备数据库与系统日志。

默认限制：256 条连接、64 个会话、10000 台注册设备，同一来源每分钟最多 60 次新连接。并发容量不是性能承诺，可按带宽调低 `--max-sessions`。安装服务的进程内存上限为 512MB。

本地诊断启动：

```bash
export LINKDESK_ENROLLMENT_KEY=YOUR_64_CHARACTER_RANDOM_KEY
python3 server/relay_server.py --bind 127.0.0.1 --port 45889 --cert server.pem --private-key server.key --database ./devices.sqlite3
```

<p align="center"><img src="assets/icons/linkdesk.png" width="128" alt="LinkDesk icon" /></p>

# LinkDesk

C++17 / Qt 6 的 Mac、Windows 远程桌面客户端，以及 Python Ubuntu 注册与中转服务。使用九位设备 ID 和连接密码连接，支持同网段直连及公网中转。

当前版本 **v0.9.3**。

## 功能

- 端到端 TLS、设备证书指纹检查、连接密码验证及断线重连。
- 办公增量传图：dirty/move 区域复制、滚动复用、静止跳过编码、独立光标、空闲文字无损补帧。
- 动态场景可切换硬件 H.264；Windows Media Foundation、Mac VideoToolbox，失败回退 PNG/JPEG。
- 1–30 FPS 目标帧率，画质/分辨率可选；实际帧率受采集和网络影响。
- Windows DXGI GPU 采集，失败时回退 GDI；Mac ScreenCaptureKit 采集。
- 首次使用默认保存本机密码、允许鼠标键盘控制并开启共享；此后保留用户的开关选择。Mac 仍需系统屏幕录制与辅助功能授权。
- 连接列表、分组、命名、多台缩略图；双击只展开选中的桌面。
- 完整画面不设固定带宽上限，缩略图采用低带宽设置。
- 关闭窗口后驻留 Mac 顶部托盘 / Windows 右下角托盘；菜单可还原界面，完全退出需要确认。
- 传输卡片显示每秒传送速度；退出时取消后台校验并保留接收临时文件供续传。
- 缩略图右键或“⋯”菜单：传送文件、重启、关机；电源操作需要受控端控制授权和操作确认。
- 拖放文件到已连接的缩略图，卡片显示进度；断线续传、SHA-256 校验、同名文件自动保留。
- 设置中选择接收文件夹（默认系统下载目录）和登录系统后自动启动。
- 管理员密码保护服务器设置，首次默认密码为 `888888`。
- Mac 本机加密保存密码，不使用钥匙串；Windows 使用当前用户 DPAPI。

目前尚不支持音频、文件夹传输、剪贴板同步、公网 NAT 穿透、HEVC 或 4:4:4 视频。30 FPS 是目标，不是性能保证。已验证 Mac 与 Windows 构建、回环 TLS 和双向文件续传；不同网络与硬件的性能仍需实测。

## 快速开始

1. 在 Ubuntu 22.04/24.04 部署服务，见 [服务端部署指南](server/README.md)。
2. 将服务器生成的私有 `linkdesk-server.json` 导入两端「软件设置 → 服务器设置」，输入管理员密码解锁后保存。
3. 被控端开启共享；Mac 需授权屏幕录制。需要控制时勾选「允许鼠标键盘控制」，Mac 还需授权辅助功能。
4. 主控端输入设备 ID 和连接密码。
5. 两端升级至 v0.8.0 后，可拖放文件到缩略图；应用重启后重新选择同一未变更文件可续传。接收完成可选择打开文件夹。
6. 自动启动为当前用户登录后启动。移动安装位置后需重新设置；它不会绕过系统登录或远程控制授权。

[配置模板](examples/linkdesk-server.example.json) 中仅有占位内容。真实配置、密钥、日志和带配置安装包不包含在仓库中，也不应提交。

## 构建

Qt 6.5+ Desktop Kit、CMake 3.21+、C++17 编译器。Python 3.10+ 仅用于服务端和中转集成测试。

```sh
cmake -S ForMac -B ForMac/build -G Ninja -DCMAKE_PREFIX_PATH=/path/to/Qt/macos
cmake --build ForMac/build
ctest --test-dir ForMac/build --output-on-failure
```

- [Mac 构建与打包](ForMac/README.md)
- [Windows 构建与打包](ForWin/README.md)
- [版本与同步说明](docs/RELEASES.md)
- [采集、编码、网络反馈与合成基准](docs/STREAMING.md)
- [图标设计与生成说明](assets/icons/README.md)

也可在 Qt Creator 打开根目录或平台目录的 `CMakeLists.txt`。构建没有默认私有服务器，用户需导入自己的配置。

## 目录

`src/` 共享客户端；`ForMac/`、`ForWin/` 平台实现；`server/` 中转服务；`tests/` 回归与集成测试；`assets/icons/` 两平台图标。

Qt 的第三方许可和声明见 [third_party/qt](third_party/qt)。

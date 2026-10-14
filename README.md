<p align="center"><img src="assets/icons/linkdesk.png" width="128" alt="LinkDesk icon" /></p>

# LinkDesk

C++17 / Qt 6 的 Mac、Windows 远程桌面客户端，以及 Python Ubuntu 注册与中转服务。使用九位设备 ID 和连接密码连接，支持同网段直连及公网中转。

当前功能版本 **v0.6.0**。

## 功能

- 端到端 TLS、设备证书指纹检查、连接密码验证及断线重连。
- 智能增量传图：只发送变化区域，静止画面复用，局部更新无损压缩。
- 1–30 FPS 目标帧率，画质/分辨率可选；实际帧率受采集和网络影响。
- Windows DXGI GPU 采集，失败时回退 GDI；Mac 原生屏幕采集。
- 鼠标、键盘控制，需要被控端主动授权。
- 连接列表、分组、命名、多台缩略图；双击只展开选中的桌面。
- 完整画面不设固定带宽上限，缩略图采用低带宽设置。
- 管理员密码保护服务器设置，首次默认密码为 `888888`。
- Mac 本机加密保存密码，不使用钥匙串；Windows 使用当前用户 DPAPI。

目前尚不支持音频、文件传输、剪贴板同步、公网 NAT 穿透或 H.264/HEVC 视频编码。30 FPS 是目标，不是性能保证。

## 快速开始

1. 在 Ubuntu 22.04/24.04 部署服务，见 [服务端部署指南](server/README.md)。
2. 将服务器生成的私有 `linkdesk-server.json` 导入两端「软件设置 → 服务器设置」，输入管理员密码解锁后保存。
3. 被控端开启共享；Mac 需授权屏幕录制。需要控制时勾选「允许鼠标键盘控制」，Mac 还需授权辅助功能。
4. 主控端输入设备 ID 和连接密码。

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
- [图标设计与生成说明](assets/icons/README.md)

也可在 Qt Creator 打开根目录或平台目录的 `CMakeLists.txt`。构建没有默认私有服务器，用户需导入自己的配置。

## 目录

`src/` 共享客户端；`ForMac/`、`ForWin/` 平台实现；`server/` 中转服务；`tests/` 回归与集成测试；`assets/icons/` 两平台图标。

Qt 的第三方许可和声明见 [third_party/qt](third_party/qt)。

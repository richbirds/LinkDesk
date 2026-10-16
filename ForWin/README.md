# LinkDesk Windows 客户端

## 使用

解压发布包中的整个 LinkDesk 文件夹，双击 LinkDesk.exe。不要单独复制 exe；Qt DLL 和插件必须一起保留。无预配置包可在软件设置中导入自己的服务器配置。

两台电脑等待“服务器在线”；被控端开启共享，按需要勾选鼠标键盘控制。主控端输入对方九位设备 ID 和八位临时密码。与 Mac 客户端复用同一协议、同一 Ubuntu 中继服务器。

Windows 需保持已登录的交互桌面和电脑唤醒。此版本不会绕过 UAC，普通权限不能控制管理员窗口、登录屏幕或 UAC 安全桌面。支持主屏幕和常用英文按键；暂不支持音频、剪贴板或中文输入法。

设备证书在首次共享时使用 Windows CNG 生成；TLS 使用 Qt Schannel 插件，用户无需安装 OpenSSL。server.json 含私有服务器接入密钥，仅向自己的用户分发。设备身份保存在本机用户数据目录，勿在不同电脑之间复制。

## 构建

- Windows Qt 6.5+ Desktop Kit，v0.7.0 使用 Qt 6.8.3 / MinGW 13.1 x64 验证。
- CMake、Ninja；MinGW Kit 需匹配版本的编译器。MSVC Kit 需从对应 Developer PowerShell 启动。
- Python 3.10+ 可选；仅运行本地 Python 中继测试时需要，客户端运行不需要。

在 PowerShell 运行（示例路径需替换为实际 Qt 安装目录）：

```powershell
./ForWin/scripts/build-windows.ps1 -QtRoot C:/Qt/6.11.2/mingw_64 -CompilerRoot C:/Qt/Tools/mingw1310_64 -CMake C:/Qt/Tools/CMake_64/bin/cmake.exe -Ninja C:/Qt/Tools/Ninja/ninja.exe
./ForWin/scripts/package-windows.ps1 -QtRoot C:/Qt/6.11.2/mingw_64 -CompilerRoot C:/Qt/Tools/mingw1310_64
```

生成 `ForWin/build/LinkDesk.exe`，打包后生成 `ForWin/dist/LinkDesk-Windows-x64.zip`。若根目录有 `config/linkdesk-server.json`，会自动将其作为该私有服务器的客户端配置。

构建脚本支持 `-BuildDir` 和 `-SourceRoot`，打包脚本支持相同的 `-BuildDir`。若 Qt moc 无法处理中文路径，可使用 ASCII 路径的目录联接作为 `SourceRoot`，并在外部 ASCII 目录构建；本机 SDK 路径只保存到忽略的 `.local-build.env`。

硬件 H.264 依赖系统 Media Foundation 与显卡编码能力。组件缺失、编码失败或对端解码失败时自动使用增量图像。可用 `LINKDESK_DISABLE_H264=1` 验证兼容回退。硬件不可用时不影响应用启动。

Qt Creator 可直接打开 `ForWin/CMakeLists.txt`，选择 Windows Kit。构建代码来自根目录 src/，无需复制 Mac 工程。

## 桌面暂时无法采集

锁屏、UAC 或部分安全软件提示可能切换或保护当前桌面，导致普通用户进程无法采集。连接列表会显示采集失败原因，并保持连接、自动重试；在远端处理提示并返回普通桌面后可恢复。当前客户端不是系统服务，不支持操作受保护桌面。无显示器还可能需要额外的活动显示输出；不能仅凭等待提示判断一定需要安装驱动。

## 平台代码

- src/input_win.cpp：SendInput 与物理屏幕坐标映射。
- src/platform_win.cpp：CNG 证书、Schannel 选择、DXGI GPU 主屏采集及 GDI 回退。
- src/video_win.cpp：低延迟硬件 H.264、NV12 转换和解码。
- scripts/：原生 Windows 构建与 windeployqt 打包，打包启动检查。


- resources/LinkDesk.rc.in：Windows 可执行文件图标资源。

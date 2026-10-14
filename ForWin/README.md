# LinkDesk Windows 客户端

## 使用

解压发布包中的整个 LinkDesk 文件夹，双击 LinkDesk.exe。不要单独复制 exe；Qt DLL、插件和 server.json 必须一起保留。

两台电脑等待“服务器在线”；被控端开启共享，按需要勾选鼠标键盘控制。主控端输入对方九位设备 ID 和八位临时密码。与 Mac 客户端复用同一协议、同一 Ubuntu 中继服务器。

Windows 需保持已登录的交互桌面和电脑唤醒。此版本不会绕过 UAC，普通权限不能控制管理员窗口、登录屏幕或 UAC 安全桌面。支持主屏幕和常用英文按键；暂不支持音频、文件传输、剪贴板或中文输入法。

设备证书在首次共享时使用 Windows CNG 生成；TLS 使用 Qt Schannel 插件，用户无需安装 OpenSSL。server.json 含私有服务器接入密钥，仅向自己的用户分发。设备身份保存在本机用户数据目录，勿在不同电脑之间复制。

## 构建

- Windows Qt 6.5+ Desktop Kit，Qt 6.11.2 MinGW 13.1 x64 为本次构建工具链。
- CMake、Ninja；MinGW Kit 需匹配版本的编译器。MSVC Kit 需从对应 Developer PowerShell 启动。
- Python 3.10+ 可选；仅运行本地 Python 中继测试时需要，客户端运行不需要。

在 PowerShell 运行（示例路径需替换为实际 Qt 安装目录）：

```powershell
./ForWin/scripts/build-windows.ps1 -QtRoot C:/Qt/6.11.2/mingw_64 -CompilerRoot C:/Qt/Tools/mingw1310_64 -CMake C:/Qt/Tools/CMake_64/bin/cmake.exe -Ninja C:/Qt/Tools/Ninja/ninja.exe
./ForWin/scripts/package-windows.ps1 -QtRoot C:/Qt/6.11.2/mingw_64 -CompilerRoot C:/Qt/Tools/mingw1310_64
```

生成 `ForWin/build/LinkDesk.exe`，打包后生成 `ForWin/dist/LinkDesk-Windows-x64.zip`。若根目录有 `config/linkdesk-server.json`，会自动将其作为该私有服务器的客户端配置。

Qt Creator 可直接打开 `ForWin/CMakeLists.txt`，选择 Windows Kit。构建代码来自根目录 src/，无需复制 Mac 工程。

## 平台代码

- src/input_win.cpp：SendInput 与物理屏幕坐标映射。
- src/platform_win.cpp：CNG 证书、Schannel 选择、DXGI GPU 主屏采集及 GDI 回退。
- scripts/：原生 Windows 构建与 windeployqt 打包，打包启动检查。


- resources/LinkDesk.rc.in：Windows 可执行文件图标资源。

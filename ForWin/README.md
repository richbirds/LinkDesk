# LinkDesk Windows 客户端

## 使用

解压发布包中的整个 LinkDesk 文件夹，双击 LinkDesk.exe。不要单独复制 exe；Qt DLL 和插件必须一起保留。无预配置包可在软件设置中导入自己的服务器配置。

两台电脑等待“服务器在线”；被控端开启共享，按需要勾选鼠标键盘控制。主控端输入对方九位设备 ID 和八位临时密码。与 Mac 客户端复用同一协议、同一 Ubuntu 中继服务器。

普通客户端模式需保持已登录的交互桌面和电脑唤醒；普通用户进程不能控制登录屏幕或 UAC 安全桌面。需要开机未登录、锁屏或注销后远控时，可由管理员从软件设置安装无人值守机器服务，并显式授权。它不会保存 Windows 登录密码或修改自动登录设置，Windows 凭据仍由用户手动输入。支持主屏幕和常用英文按键；暂不支持音频、剪贴板或中文输入法。

设备证书在首次共享时使用 Windows CNG 生成；TLS 使用 Qt Schannel 插件，用户无需安装 OpenSSL。server.json 含私有服务器接入密钥，仅向自己的用户分发。普通模式设备身份保存在本机用户数据目录；机器服务凭据由 ACL 保护并单独加密保存。勿在不同电脑之间复制身份。

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

锁屏、UAC 或部分安全软件提示可能切换或保护当前桌面，导致普通用户进程无法采集。连接列表会显示采集失败原因，并保持连接、自动重试；在远端处理提示并返回普通桌面后可恢复。未安装机器服务时，不支持操作受保护桌面。安装后由 SCM 服务监督正确会话中的专用工作进程，桌面切换时清除旧画面并重新采集。无显示器还可能需要额外的活动显示输出；不能仅凭等待提示判断一定需要安装驱动。

## 无人值守机器服务

完整发布包包含 `LinkDeskService.exe`、`LinkDeskWorker.exe` 和
`LinkDeskServiceSetup.exe`。在「软件设置」中由管理员安装并设置机器连接密码，
显式开启无人值守授权。文件、电源操作和 SAS 分别授权；普通界面和文件进程
保持普通用户权限。原设备 ID 迁移需要同步升级中转服务，并轮换旧注册凭据。

GUI 的登录自动启动和 SCM 的开机服务相互独立。关闭或完全退出 GUI 不会停止
已授权的机器服务；暂停共享会跨重启保存。停止、卸载或修改机器服务策略需要
管理员权限。卸载保留受保护的机器身份，便于重新安装后继续使用。

详见 [服务架构、安装及验收流程](../docs/windows-service.md)。BitLocker 预启动和
固件界面不属于 Windows 服务远控范围。没有显示器时，仍需可采集的活动显示输出。

## 平台代码

- src/input_win.cpp：SendInput 与物理屏幕坐标映射。
- src/platform_win.cpp：CNG 证书、Schannel 选择、DXGI GPU 主屏采集及 GDI 回退。
- src/video_win.cpp：低延迟硬件 H.264、NV12 转换和解码。
- scripts/：原生 Windows 构建与 windeployqt 打包，打包启动检查。
- service/：SCM 宿主、会话监督、受控采集/输入工作进程、普通用户文件进程及受限本机 IPC。


- resources/LinkDesk.rc.in：Windows 可执行文件图标资源。

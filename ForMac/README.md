# Mac 客户端

Apple Silicon / arm64，macOS 13+，Qt 6.5+ Desktop Kit。

Qt Creator 打开 `ForMac/CMakeLists.txt` 或根目录。脚本默认 SDK 在 `~/Qt/6.11.2/macos`，也可设置 `QT_ROOT`、`QT_TOOLS`，或使用忽略的 `.local-build.env`。

```sh
QT_ROOT=/path/to/Qt/macos QT_TOOLS=/path/to/Qt/Tools bash ForMac/scripts/build-macos.command
QT_ROOT=/path/to/Qt/macos bash ForMac/scripts/package-macos.command
```

输出为 `ForMac/dist/LinkDesk.app` 与 ZIP。图标来自 `assets/icons/LinkDesk.icns`。应用使用本地临时签名，未进行 Apple 公证。

私有服务器配置可置于忽略的 `config/linkdesk-server.json`，或启动后通过软件设置导入。带私有配置的安装包只向自己的使用者分发。

Mac 共享需屏幕录制权限，控制需辅助功能权限。连接密码采用本机文件加密保存，不调用钥匙串。

v0.7.0 使用 ScreenCaptureKit 获取最新画面与保守 dirty 元数据，AppKit 单独读取光标，VideoToolbox 提供硬件 H.264。需要 Apple SDK 中的 ScreenCaptureKit、VideoToolbox、CoreMedia 和 CoreVideo 框架；CMake 自动链接并为采集实现启用 Objective-C++ ARC。

新 Mac 后端已完成源码检查，但未在本次 Windows 环境中编译或运行。发布 Mac 安装包前应在 macOS 验证权限、Retina 坐标、停止共享、视频与 PNG 切换，以及 Windows/Mac 双向连接。运行合成基准的说明见 [STREAMING.md](../docs/STREAMING.md)。

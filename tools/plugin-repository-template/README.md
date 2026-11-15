# Hotaru 插件仓库

当前共有 **3 个插件**：1 个基础服务、2 个功能插件，均为正式版。

## 基础服务

| 插件 | 版本 | 功能 | 下载 |
| --- | --- | --- | --- |
| 局域网连接 (`lan-core`) | 1.0.1 | 发现、配对和管理同一局域网内的设备。 | [Windows x64 ZIP](https://github.com/EienSakura/Hotaru-Plugins/releases/download/lan-v1.0.1/hotaru-plugin-lan-core-1.0.1-x64.zip) |

## 功能插件

| 插件 | 版本 | 功能 | 下载 |
| --- | --- | --- | --- |
| 局域网剪贴板共享 (`lan-clipboard`) | 1.0.1 | 在已配对的设备间同步新复制的文本和图片。 | [Windows x64 ZIP](https://github.com/EienSakura/Hotaru-Plugins/releases/download/lan-v1.0.1/hotaru-plugin-lan-clipboard-1.0.1-x64.zip) |
| 局域网文件分享 (`lan-files`) | 1.0.1 | 向已配对的设备发送文件或文件夹，接收方确认后开始传输。 | [Windows x64 ZIP](https://github.com/EienSakura/Hotaru-Plugins/releases/download/lan-v1.0.1/hotaru-plugin-lan-files-1.0.1-x64.zip) |

## 使用

使用 Hotaru 1.0.2 或兼容的更新版本，在「设置 → 插件 → 官方仓库」安装。安装功能插件时会自动检查并补齐前置。也可下载 ZIP 后选择「导入本地插件」，先导入局域网连接，再导入需要的功能插件。

在两台使用同一专用局域网的电脑上启用局域网连接，按提示完成配对。两端启用剪贴板共享后，复制文本或图片即可同步，搜索 `clip` 可查看记录。两端启用文件分享后，在搜索框输入 `@` 可选择在线设备发送文件。

## 构建和打包

准备 Windows x64、Visual Studio 2022 的 C++ 工具链、CMake、vcpkg 和已构建的 BoringSSL 0.20250818.0。Qt 版本由 `vcpkg.json` 锁定。将下面三个参数替换成实际依赖路径，在仓库根目录运行：

```powershell
.\tools\build-lan.ps1 -VcpkgRoot (Resolve-Path .\deps\vcpkg).Path `
    -BoringSslSource .\deps\boringssl-0.20250818.0 `
    -BoringSslCrypto .\deps\boringssl-build\Release\crypto.lib
```

脚本会构建并自测三个插件，在 `dist/` 生成 `hotaru-plugin-<ID>-<版本>-x64.zip`。

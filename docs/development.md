# Hotaru 开发说明

[返回 README](../README.md) · [使用指南](usage.md) · [功能变更记录](changes.md)

## 技术栈与模块

当前开发目标为 Windows x64，使用 C++20、Qt 6.10.2、CMake 3.24及以上版本和Visual Studio 2022。Qt、libarchive和bit7z通过 `vcpkg.json` 清单管理；版本信息从 `ltool.iss` 读取，Hotaru 首版为 `1.0.0`。

| 模块 | 职责 |
| --- | --- |
| `src/main.cpp` | 热键、托盘、单实例、启动和命令行入口。 |
| `src/SearchWindow.*` | 搜索、固定面板、分组弹层、菜单和设置窗口入口。 |
| `src/PinnedApps.*` | 固定项、页面、分组和布局配置操作。 |
| `src/LauncherGrid.*` | 自定义格位绘制、图标、拖放和翻页。 |
| `src/LauncherSkin.*`、`src/Theme.*` | 启动器外观、皮肤资源、动画和基础配色。 |
| `src/ConfigPage.*`、`src/SettingsDialog.*` | 通用配置、外观和固定软件管理。 |
| `src/platform/` | Windows图标、快捷方式及其他系统接口。 |
| `plugins/` | 可选工具DLL源码，复用 `ltoolcore`。 |

主程序输出为 `Hotaru.exe`，界面名称为“萤 Hotaru”。内部CMake目标仍为 `ltool`，公共核心保留 `ltoolcore.dll`，插件目录保留 `ltool-plugins/`；插件IID、导出符号、`LTOOL_DATA_DIR` 和已有数据标识不变，以兼容现有插件及配置。

## 项目内构建环境

先准备已有的Visual Studio C++桌面开发工具和Windows SDK。在项目根目录打开PowerShell，以下设置只影响当前进程及其子进程，将下载、缓存、临时目录和测试数据放在项目中：

```powershell
$projectRoot = (Get-Location).Path
$env:VCPKG_ROOT = Join-Path $projectRoot 'build/vcpkg-tool'
$env:VCPKG_DOWNLOADS = Join-Path $projectRoot 'build/vcpkg-downloads'
$env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $projectRoot 'build/vcpkg-cache'
$env:VCPKG_BINARY_SOURCES = "clear;files,$env:VCPKG_DEFAULT_BINARY_CACHE,readwrite"
$env:VCPKG_DISABLE_METRICS = '1'
$env:VCPKG_MAX_CONCURRENCY = '4'
$env:TEMP = Join-Path $projectRoot 'build/hotaru-local/temp'
$env:TMP = $env:TEMP
$env:APPDATA = Join-Path $projectRoot 'build/hotaru-local/appdata'
$env:LOCALAPPDATA = Join-Path $projectRoot 'build/hotaru-local/localappdata'
$env:LTOOL_DATA_DIR = Join-Path $projectRoot 'build/hotaru-local/data'
$env:QT_SHADER_CACHE_PATH = Join-Path $projectRoot 'build/hotaru-local/shadercache'
@($env:VCPKG_DOWNLOADS, $env:VCPKG_DEFAULT_BINARY_CACHE, $env:TEMP,
  $env:APPDATA, $env:LOCALAPPDATA, $env:LTOOL_DATA_DIR,
  $env:QT_SHADER_CACHE_PATH) | ForEach-Object {
    New-Item -ItemType Directory -Force -Path $_ | Out-Null
}
```

新开终端需要重新设置。已有项目内环境脚本的开发环境可加载该脚本，但全新克隆不依赖任何未提交的本机脚本。

首次准备vcpkg时，下载到上面的项目内路径；已有工具则复用：

```powershell
if (!(Test-Path -LiteralPath $env:VCPKG_ROOT)) {
    git clone https://github.com/microsoft/vcpkg $env:VCPKG_ROOT
}
if (!(Test-Path -LiteralPath (Join-Path $env:VCPKG_ROOT 'vcpkg.exe'))) {
    & (Join-Path $env:VCPKG_ROOT 'bootstrap-vcpkg.bat') -disableMetrics
}
```

这一步会下载依赖工具，后续首次配置会按清单构建依赖。已有开发环境无需重复安装Qt或修改全局环境变量。

## 配置和编译

在上述同一个PowerShell进程中执行：

```powershell
cmake -S . -B build -G 'Visual Studio 17 2022' -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  "-DVCPKG_INSTALLED_DIR=$projectRoot/vcpkg_installed" `
  -DHOTARU_BUNDLE_PLUGINS=OFF
cmake --build build --config Release --target ltool --parallel 4
```

`Hotaru.exe`、`ltoolcore.dll`和Qt运行库部署到 `build/Release/`。不捆绑插件只控制本次构建目标，不会清空已有 `ltool-plugins/`。

在可写的项目目录中启动，无需安装到系统目录：

```powershell
$env:QT_QPA_PLATFORM = 'windows'
$env:QT_PLUGIN_PATH = Join-Path $projectRoot 'build/Release'
Start-Process -FilePath './build/Release/Hotaru.exe' -ArgumentList '--show' `
  -WorkingDirectory (Join-Path $projectRoot 'build/Release') -WindowStyle Hidden
```

此处使用环境中的 `LTOOL_DATA_DIR`。如需使用已有开发数据，应显式指定对应目录；测试不要指向真实用户配置。

## 工具插件

默认 `HOTARU_BUNDLE_PLUGINS=OFF`。需要同时编译仓库保留的工具时，在CMake配置中改为 `ON`，再构建 `ltool`。该操作会编译并部署工具DLL，通常开发启动器无需开启。

插件接口仍为 `LToolPlugin` API v4，公共构建片段为 `cmake/ltool-plugin.cmake`。图片排版、随机数、JSON和Base64分别对应 `plugins/grid`、`plugins/random`、`plugins/json`、`plugins/base64`；本轮未改动这些插件实现。

保持API号不代表任意历史DLL都与当前核心二进制兼容。插件应针对匹配的核心/SDK构建；涉及公共布局变更时需执行项目既有的ABI检查，再验收实际插件功能。皮肤ZIP和工具DLL分别安装，皮肤不改变插件API。

商店默认保留上游 ltool `v1.5.0` 插件清单地址，与 Hotaru 的仓库和版本号独立；尚未测试适配，需自行适配。配置中的自定义清单地址继续优先，不会自动改为本项目仓库。

### 打包选择

- 默认 CMake 构建与 Inno Setup 安装器均不包含工具插件，已有本地 DLL 不会因默认打包而自动收入。
- 如需额外插件，在 CMake 显式设置 `HOTARU_BUNDLE_PLUGINS=ON` 并构建、验证。CI 手动运行的 `package_plugins` 选项默认关闭，开启后提供独立的 `Hotaru-windows-plugins-extra` 产物（DLL、清单和 SDK），主安装器仍不带插件。
- 本地确需制作含插件安装器时，完成适配验证后再显式使用 `ISCC -DFINAL -DBUNDLE_PLUGINS ltool.iss`；常规 `ISCC -DFINAL ltool.iss` 不带插件。
- 发布说明脚本默认只收集当前版本 `Hotaru-<版本>-setup.exe` 和校验表。仅额外传入 `--include-plugins` 才收集插件清单、SDK 和 DLL，并检查清单与 DLL 哈希一致；不自动使用本机已有插件作为发布资产。
- 主题采用本仓库 Release 的独立 ZIP 附件分发，不另建主题仓库，不进入源码 Git、程序资源、默认安装器或额外插件包。2233娘主题包使用独立附件发布，素材权利归原作者；维护者须确认素材来源与再分发权限。主题由维护者单独上传到对应标签的 Release 草稿；CI 发布时保留独立附件并纳入 SHA-256 校验表，不从源码或本机自动收集主题。设计规范见 [主题包设计规范](theme-package.md)。

## 验证

默认先执行相关构建、静态检查和离屏验证，不运行会显示窗口或抢占鼠标、焦点的测试。离屏验证前必须确认平台插件存在，不得失败后回退到 Windows 可见后端。示例只检查关于页，不操作鼠标、不联网：

```powershell
$env:QT_PLUGIN_PATH = Join-Path $projectRoot 'vcpkg_installed/x64-windows/Qt6/plugins'
if (!(Test-Path (Join-Path $env:QT_PLUGIN_PATH 'platforms/qoffscreen.dll'))) {
    throw '缺少匹配的离屏平台插件，停止本项验证'
}
$env:QT_QPA_PLATFORM = 'offscreen'
$logs = Join-Path $projectRoot 'build/hotaru-local/checks'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$process = Start-Process './build/Release/Hotaru.exe' -ArgumentList '--about' `
  -PassThru -RedirectStandardOutput (Join-Path $logs 'about.out.log') `
  -RedirectStandardError (Join-Path $logs 'about.err.log')
if (!$process.WaitForExit(60000)) { $process.Kill(); throw '离屏验证超时' }
if ($process.ExitCode -ne 0) { throw '关于页验证失败，查看日志' }
git diff --check
```

依赖目录按实际 CMake 的 `VCPKG_INSTALLED_DIR` 调整；全新构建可能位于 `build/vcpkg_installed/`。测试必须使用隔离配置，不指向用户数据。程序提供以下入口，按本次修改范围选择，不因文档或文案改动重复整套交互回归：

| 入口 | 验证范围 |
| --- | --- |
| `--hotarutest` | 固定项、格位、页面、分组、取消恢复、主题校验、搜索操作和设置集成。 |
| `--cfgcheck` | 配置页面布局和配色。 |
| `--storetest` | 插件更新决策及安装回调等原有逻辑。 |
| `--pinyin` | 拼音搜索回归。 |
| `--hotarupreview` | 运行Hotaru回归并输出窗口预览；配合Windows后端检查实际绘制。 |

只有维护者明确要求真实桌面验收时才运行原生预览。原生预览使用 `QT_QPA_PLATFORM=windows`，分别设置 `QT_SCALE_FACTOR=1`、`1.5`和`2`，串行运行 `--hotarupreview`。有自行提供的主题ZIP时可额外传入 `--theme-package <zip路径>`，无包时仍可验证基础主题。

需要离屏自测时，将 `QT_PLUGIN_PATH` 指向包含 `platforms/qoffscreen.dll` 的匹配Qt插件目录，再设置 `QT_QPA_PLATFORM=offscreen`。默认部署只提供Windows平台插件，不能直接指定缺失的离屏后端。

预览输出到当前 `TEMP` 下的 `hotaru-previews/`，每档结束后应另存截图再运行下一档。以上环境中它位于项目内。合成鼠标事件、缩放因子和原生窗口渲染检查，不代替物理鼠标手感、真实跨屏DPI切换及真实UAC授权测试。

## 分支与发布现状

- `main`用于跟踪上游；Hotaru日常开发使用 `hotaru/dev`，长期分支为 `hotaru/main`。
- 保留 `upstream` 指向 [mateo-ki/ltool](https://github.com/mateo-ki/ltool)，用于后续取回和合并更新；本项目仓库为 [EienSakura/Hotaru](https://github.com/EienSakura/Hotaru)，远端配置与推送另行授权。
- 默认插件清单保留上游源并注明尚未验证适配；关于页只展示 Hotaru 信息并链接本项目仓库与发布页，在线升级隐藏并禁用。
- Inno Setup应用名、执行文件名、安装包名和安装标识已与上游分离，CI执行文件及安装包路径同步改为Hotaru；安装器未实际安装，云端CI和独立发布尚未验收。
- 旧 `build/verify/` 交付脚本只保留在维护者本地，不随源码分发；开发测试直接运行项目内构建产物。

产品版本只在维护者明确要求，或同意一个版本发布节点后更新，不随功能修改、构建或提交自动递增。

提交消息统一使用 `feat(scope): 中文说明`，提交和推送由维护者明确授权。构建产物、个人配置、第三方皮肤素材及本地开发辅助资料不进入源码提交。

## 资料范围

公开文档描述产品使用、构建、功能变化及[主题包设计规范](theme-package.md)。主题包格式、布局与打包规则面向主题制作者公开；内部接口、实现约束及现场调试记录继续维护在本地 Trellis，不随源码提交或推送。公开构建与主题制作不依赖本地 Trellis 文件。

当前行为以代码、使用指南及功能变更记录为准；历史调试记录不再作为公开文档维护。

## 自动发布

个人仓库使用 `origin`，上游仓库保留为 `upstream`。日常代码推送到 `hotaru/dev`，稳定代码通过 squash 合并到 `hotaru/main`，每个合并节点只保留一笔汇总提交；默认分支使用 `hotaru/main`，不推送上游 `main` 或继承的版本标签。

推送与 `ltool.iss` 版本一致的 `v*` 标签会触发 GitHub Actions：执行静态门禁、构建、离屏回归、生成安装器与校验表，通过后发布 GitHub Release。分支推送只构建，不发布。首版的历史比较使用 `tools/upstream-base.txt` 中的分叉提交，避免依赖个人远端未发布的上游标签。vcpkg 使用文件二进制缓存，由 `actions/cache` 保存目录，不使用已经移除的 `x-gha` 接口。

主题 ZIP 不进入 Git。维护者可以先创建同标签的 Release 草稿，单独上传形如 `Hotaru-theme-2233-1.4.2.zip` 的附件；自动发布会下载这些显式提供的主题，检查 ZIP 和清单，加入 `sha256.txt`，然后更新草稿并公开。主题文件名使用 `Hotaru-theme-` 前缀及英文字母、数字、点、下划线和短横线；显示名称保留在 ZIP 清单中。失败时草稿保持未公开，可以修复后重跑。

只有维护者明确授权时才提交、推送或创建版本标签；工作流不会自行递增版本。默认发布仍不包含插件。

### 独立历史与上游同步

`hotaru/main` 和 `hotaru/dev` 从 Hotaru 1.0.0 的独立首提交开始，不继承上游提交历史。许可证、版权及 README 来源说明继续保留。`upstream` 仍指向原项目；维护者在本地保存完整历史与同步记录，先在本地同步分支验证上游更新，再将审核后的代码差异应用到 `hotaru/dev`，不能把带上游父历史的合并提交推回产品分支。

CI 从 `tools/upstream-base.txt` 下载固定上游提交，仅用于插件版本及接口布局比较；该下载不会成为产品分支的父历史。首版发布说明只记录 Hotaru 的独立首提交，后续以产品版本标签比较。

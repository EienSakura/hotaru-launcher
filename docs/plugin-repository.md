# Hotaru 插件仓库

在设置的「插件」页点击右上角「官方仓库」打开仓库弹窗；插件主页面显示已安装的插件。正式版和开发版共用官方仓库。正式版只显示符合宿主条件的正式包，开发版也显示预览包。两版都允许「导入本地插件」安装兼容的 DLL／ZIP，首次安装默认关闭。本地包不会因文件名或元数据自称官方而获得「Hotaru直属」标记。原 ltool 兼容商店继续独立，在插件主页面也能找到兼容插件的管理入口。

## 仓库与构建

运行 tools/export-plugin-repository.ps1 -Destination <新目录> 导出独立仓库初始内容。目录中含 SDK 头、固定 vcpkg 清单、三个 LAN 插件、Windows CI、打包工具与发布说明。CI 只构建、测试并保存附件；目录签名在具备 Python 3.11 和 cryptography 的离线机器完成。预览插件改为正式渠道前，必须记录对应宿主版本、插件版本、ZIP 的 SHA-256 和 Windows 10／11 双机验收报告。

分发 ZIP 命名为 hotaru-plugin-<id>-<version>-x64.zip。每个 ZIP 含一个 DLL 和可选的 TXT 许可证，不使用子目录。先发布版本 ZIP，再用 tools/make-catalog.py 生成目录。官方源为 EienSakura/Hotaru-Plugins 的 GitHub Releases。只有 catalog-v1-<序号> 设为 latest；它包含 index-v1.json 和 index-v1.sig。启用不可变发布后，改包需发新版本，改目录需发新序号。

## 目录与安装

- v1 索引字段：format（1）、sequence（正整数）、issued、expires（ISO 8601）、plugins。每个条目含 id、version、name、description、category（service/feature）、channel（stable/preview）、iid、api、abi、platform（windows）、arch（x64）、host（版本范围）、dependencies（插件 ID 到版本范围）、tag、asset、files。版本范围的 min 包含、max 不包含，空字符串表示无限制。asset 和 files 条目均使用 name、size、sha256。
- index-v1.sig 是对索引原始字节的 Ed25519 签名，每段 64 字节，最多 4 段。默认构建（含 GitHub CI）内置官方公钥。需要覆盖时，通过 CMake 的 HOTARU_PLUGIN_PUBLIC_KEYS 指定逗号分隔的原始公钥 hex；显式留空的构建会拒绝远程目录。已有构建若缓存了空值，可在原配置命令中加上 -U HOTARU_PLUGIN_PUBLIC_KEYS，恢复源码默认值。轮换期间先随宿主发新公钥，再使用双签。私钥不进构建或源码。ABI 由宿主 CMake 输出的 hotaru-plugin-abi.txt 提供，插件 SDK 按该标识构建。
- 官方目录有效期最长 365 天。续期沿用原密钥，更新日期并增加序号，签发新目录设为 latest。宿主无需随每次续期更新；仅理解 90 天上限的旧版需要先升级。
- 官方目录缓存按宿主渠道隔离，有效目录默认 24 小时刷新。验签或刷新失败保留原缓存；目录过期可浏览但不能安装，弹窗会引导手动下载并导入。已有插件无需联网运行。
- 官方目录和签名从 GitHub 获取。插件 ZIP 支持配置中的 HTTPS 镜像前缀，失败或哈希不符时换下一条，最后回到 GitHub。镜像不会改变签名、大小、哈希、DLL 元数据及依赖的检查。
- 当前没有原生插件加载或安装时，首次官方安装可在安全验证后当场试加载，仍默认关闭；已安装插件的替换及本地原生插件导入沿用重启流程。试加载失败或中断时回退活动清单。
- 直属插件目录位于应用数据目录下的 hotaru-plugin-repository（正式）或 hotaru-plugin-repository-dev（开发）。packages、staging、catalog.json、pending.json 和 active.json 均在这个根目录下；不要通过手工复制 DLL 到 packages 来安装。兼容插件仍由其原目录与管理页维护。

发布资产与 latest 的行为见 [GitHub 文档](https://docs.github.com/en/rest/releases/releases#get-the-latest-release)。

# Hotaru 主题包设计规范

[返回 README](../README.md) · [使用指南](usage.md) · [开发说明](development.md)

本文面向主题制作者，描述 Hotaru 1.0.0 已支持的 ZIP 格式。主题负责启动器外观，不执行代码，不改变设置页配色、固定项数据、网格行列或插件接口。

## 分发与安装

主题使用**本项目 GitHub Release 的独立 ZIP 附件**，与主程序分开下载和安装。目前不建立独立主题仓库，也不将主题资源打入主程序安装器。主题包可以使用独立版本号，发行说明应标明主题版本、最低 Hotaru 版本、作者、素材来源及授权信息。

主题 ZIP 和第三方素材不进入源码 Git 历史。Release 附件与 GitHub 自动生成的源码 ZIP 是不同的下载项，不要把源码 ZIP 当成主题安装；参阅 [GitHub Release 说明](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases)。主题须由维护者主动上传到同标签的 Release 草稿，源码 CI 不自动收集素材；自动发布会保留这些显式提供的附件并将它们纳入 `sha256.txt`。附件文件名使用 `Hotaru-theme-` 前缀及英文字母、数字、点、下划线或短横线，例如 `Hotaru-theme-2233-1.4.2.zip`，清单中的显示名称可以使用中文。

2233娘主题包以独立 ZIP 附件提供，原始素材权利归原作者；项目源码的 Apache-2.0 许可证不涵盖第三方素材。维护者应确认素材再分发权限。程序无需依赖该包即可构建和运行。

用户在“配置 → 外观 → 导入主题包”选择 ZIP，导入成功后立即应用。切换会记住选择；同 ID 导入会替换旧包，校验或安装失败时保留原包；删除当前用户主题后回到内置黑色。内置主题不可覆盖或删除。

## 目录结构

ZIP 根目录必须直接包含 `theme.json`，不能在外面再套一层文件夹：

```text
example-theme-1.0.0.zip
├─ theme.json
├─ theme.qss
├─ assets/
│  ├─ frame.png
│  └─ mascot.gif
└─ fonts/              可选，首期不会自动加载字体
```

仅换色的主题也需要有效的资源目录；可保留 ZIP 中的 `assets/` 目录条目。包内文件仅允许 JSON、QSS、PNG、JPG/JPEG、WEBP、GIF、BMP、TTF、OTF；SVG、DLL、EXE、脚本及其他类型不支持。授权说明可使用 JSON 元数据或放在 Release 说明中，不要向 ZIP 混入当前不支持的文件类型。

## 清单与颜色

新包推荐使用 `schemaVersion: 2`。以下是无需外框的最小示例：

```json
{
  "schemaVersion": 2,
  "id": "example-theme",
  "name": "示例主题",
  "version": "1.0.0",
  "author": "主题作者",
  "minAppVersion": "1.0.0",
  "stylesheet": "theme.qss",
  "assetRoot": "assets",
  "palette": {
    "panel": "#182334",
    "text": "#ffffff",
    "field": "#24354a",
    "accent": "#63b3ff"
  }
}
```

| 字段 | 规则 |
| --- | --- |
| `schemaVersion` | 支持整数 `1`、`2`；省略按 `1` 兼容。新主题使用 `2`。 |
| `id` | 1–64 位英文字母、数字、下划线或短横线。更新主题时保持不变，不能使用内置 ID。 |
| `name` | 非空显示名称，可以使用中文。改名不应同时改 ID。 |
| `version` | 非空主题版本，建议使用 `x.y.z`；与软件版本独立。 |
| `author` | 作者或维护者，发布时应填写并保留素材原作者归属。 |
| `minAppVersion` | 有效版本号，不得高于当前程序版本；它约束软件版本，不是主题格式版本。 |
| `stylesheet` | 相对包根目录的 QSS 路径，默认 `theme.qss`。 |
| `assetRoot` | 相对包根目录的资源目录，默认 `assets`，目录必须存在。 |
| `palette` | 可选对象，只能覆盖程序已知的颜色角色，值须为有效 Qt 颜色字符串。 |
| `surface` | 可选外框与布局对象，见下一节；不声明时使用程序基础布局。 |

常用颜色角色包括 `window`、`panel`、`panelEdge`、`text`、`muted`、`field`、`edit`、`editText`、`border`、`accent` 和 `onAccent`。完整角色以 [Theme.cpp](../src/Theme.cpp) 的基础角色表为准；未知角色会被拒绝。设置页、标准提示框和插件页面继续使用程序基础配色，主题包不能覆盖这些窗口的全局样式。

## 外框与统一坐标

将以下 `surface` 加入上面的清单，可以使用九宫格外框和装饰。示例要求自备 **800×600** 的 `assets/frame.png` 和 **80×80** 的 `assets/mascot.gif`；这些名称不指向任何内置素材。

```json
{
  "surface": {
    "canvasSize": [800, 600],
    "frame": "frame.png",
    "framePadding": [0, 0, 0, 0],
    "frameMargins": [24, 160, 24, 80],
    "searchRect": [32, 104, 736, 48],
    "contentRect": [32, 176, 736, 344],
    "footerRect": [32, 544, 736, 28],
    "settingsRect": [740, 544, 28, 28],
    "decorations": [
      {
        "image": "mascot.gif",
        "anchor": "top-right",
        "reference": "window",
        "offset": [-24, 8],
        "size": [80, 80]
      }
    ]
  }
}
```

所有布局坐标使用 **Qt 逻辑像素**，矩形格式为 `[x, y, width, height]`，不能再乘 DPI 倍率。`frame` 和装饰的 `image` 相对 `assetRoot`；QSS 的图片 URL 则相对包根目录。

| 字段 | 设计规则 |
| --- | --- |
| `canvasSize` | 设计画布，宽 360–4096、高 240–4096。v2 必须等于原始外框尺寸加 `framePadding` 两侧留白。 |
| `framePadding` | 窗口透明留白，顺序为左、上、右、下，默认全为 0。 |
| `frameMargins` | 原始外框九宫格边距，顺序同上；两侧边距之和必须小于图片对应尺寸。四角保留，中段拉伸。 |
| `searchRect` | 搜索区域，高度固定为 48，完整位于画布内。 |
| `contentRect` | 网格或结果区，不得与搜索、页脚重叠；居中设计应保持两侧留白对称。 |
| `footerRect` | 页脚区域，高度至少 28，容纳索引、页码与设置。 |
| `settingsRect` | 完整位于页脚内的正方形，边长 28–40，推荐 28。实际设置按钮以该区水平中心及页脚底边对齐。 |

不要为单侧角色整体移动网格，角色可通过显式覆盖内容区实现贴边效果。图标、虚线格和鼠标命中均由程序按统一格位计算；主题不自行绘制另一套格线。完整面板以设计画布为最小尺寸，设计时应考虑目标屏幕的可用空间。

## 静态装饰与 GIF

最多支持 8 个装饰层。PNG 等静态图与 GIF 使用相同定位方式，保持资源比例，全部鼠标穿透，不阻挡图标点击和拖动。

| 字段 | 规则 |
| --- | --- |
| `image` | 相对 `assetRoot` 的可解码图片，宽高均不超过 4096。 |
| `anchor` | `top-left`、`top-right`、`bottom-left`、`bottom-right`。 |
| `reference` | `window`（默认，整个画布）、`frame`（扣除透明留白）、`content`（内容区）。 |
| `offset` | `[x, y]` 偏移，正方向向右、向下；右/下锚点先扣除装饰宽高，再应用偏移。 |
| `size` | `[width, height]` 显示尺寸，必须严格保持原图或裁切区域的宽高比。 |
| `sourceRect` | 可选原图像素裁切区，必须是原图内部的正矩形；省略使用整图。 |
| `allowContentOverlap` | 可选布尔值，默认 `false`。`true` 允许覆盖内容区，仍不能超出画布或覆盖搜索、页脚。 |

GIF 应先计算所有帧可见区域的并集，再为整个动画设置一个固定 `sourceRect`。不要逐帧改变裁切或位置，否则动画容易抖动。`size` 按裁切后的比例设置，透明留白较多时也不要通过拉伸角色来贴边。

切换、替换或删除主题时，程序停止并释放旧动画。无需主题脚本负责资源清理。

## QSS 只控制视觉

v2 将布局统一交给清单，QSS 用于颜色、边框和圆角。例如：

```css
QLineEdit#input {
    color: @text;
    background: @field;
    border: 1px solid @border;
    border-radius: 6px;
}
QWidget#hotaruGrid {
    background: transparent;
}
```

不要在 v2 QSS 中写 `width`、`height`、`min/max-width`、`min/max-height`、`margin`、`padding`、`position`、`left/top/right/bottom` 或 `qproperty-*`；这些几何属性会被拒绝。使用明确控件选择器，避免用通配背景盖住透明外框和装饰。

图片引用写作 `url(assets/image.png)`，文件必须真实存在。绝对路径、网络 URL、带冒号或 `..` 路径分量的引用不允许。字体文件可以随包保存，但当前版本不会自动注册或应用这些字体。

## 完整面板与紧凑搜索

“呼出时显示常驻网格”开启时显示完整面板。关闭后，空查询状态只显示搜索区皮肤与顶部装饰，隐藏网格、页脚、中段和底框；设置入口移到搜索条右侧。不要在外框底部放置空搜索状态必须可见的信息。

输入后，窗口按搜索结果展开并恢复结果区外框，底部装饰恢复显示和播放；清空查询后再次收起，底部装饰隐藏并暂停。顶部以 `window/frame` 为参考的装饰继续显示；以 `content` 为参考或底部锚定的装饰跟随展开状态。主题包不需要另设紧凑模式字段。

## 打包与交付检查

准备好 `example-theme/` 内的清单、QSS 和自制资源后，在其上一级目录执行：

```powershell
Compress-Archive -LiteralPath @(
  './example-theme/theme.json',
  './example-theme/theme.qss',
  './example-theme/assets'
) -DestinationPath './example-theme-1.0.0.zip'
```

如使用 `fonts/`，将该目录一并加入。检查 ZIP 顶层确实是清单、样式和资源目录，而不是 `example-theme/`。不要打包程序、DLL、用户配置、缓存或临时文件。

发行前应检查以下场景：

- 无第三方资源时程序仍可启动；主题导入、切换、同 ID 替换、删除和重启恢复正常。
- 在设计尺寸、放大尺寸和不同 DPI 下检查网格居中、完整格位与点击区域，页脚不侵入内容区。
- 检查空面板、完整固定网格、多条搜索结果、紧凑搜索及清空查询，确认没有横向滚动条或残留底框。
- GIF 不抖动、不拉伸，角色贴边且点击穿透；设置页和提示框配色不被主题改变。
- 缺图、非法 JSON、越界布局、错误比例、目录穿越和不支持的文件类型应被拒绝，当前主题保持可用。
- Release 说明列明主题版本、最低软件版本、素材归属及授权；建议同时提供 ZIP 的 SHA-256。只有已确认可再分发的素材才发布。

开发验证优先使用静态检查与离屏方式。需要真实桌面/DPI验收时单独安排，不能让文档检查自动占用鼠标和用户窗口。

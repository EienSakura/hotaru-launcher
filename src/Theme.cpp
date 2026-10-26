// 一套颜色两个模式。规格在 Theme.h，这儿只放"角色 → 两套取值"那张表和它的用法。
#include "LauncherTheme.h"

#include "Archives.h"
#include "PinnedApps.h"
#include "Utils.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QInputDialog>
#include <QMessageBox>
#include <QFile>
#include <QPalette>
#include <QRegularExpression>
#include <QSettings>
#include <QWidget>
#include <QPair>
#include <QVector>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QImageReader>
#include <QVersionNumber>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>

#include <cmath>

namespace Theme {
namespace {

struct Role { const char* name; const char* dark; const char* light; };

// 弹层那两份 QSS 只在这里存一份：`menuStyle()`/`tipStyle()` 是"当场算一次"，
// `bindMenu()`/`applyToApp()` 走的是同一份 token 串，所以切模式时挂过的那批会跟着重挂。
const char* const kMenuQss =
    "QMenu { background: @field; border: 1px solid @border; border-radius: 8px;"
    " padding: 4px; color: @text; font-size: 10pt; }"
    "QMenu::item { padding: 6px 20px 6px 12px; border-radius: 6px; }"
    "QMenu::item:selected { background: @accent; color: @onAccent; }"
    "QMenu::separator { height: 1px; background: @border; margin: 4px 6px; }";

const char* const kTipQss =
    "QToolTip { background: @field; color: @text;"
    " border: 1px solid @border; padding: 6px 8px; }";

// Windows 标准风格可能仍绘制浅色底，必须同时指定弹窗底色和文字。
const char* const kDialogQss = R"QSS(
QDialog { background: @window; color: @text; }
QLabel { background: transparent; color: @text; }
QPushButton { background: @button; color: @text; border: 1px solid @border;
    border-radius: 6px; padding: 6px 14px; min-width: 64px; }
QPushButton:hover { background: @hover; }
QPushButton:pressed { background: @pressed; }
QPushButton:default, QPushButton:focus { border-color: @accent; }
QPushButton:disabled { background: @field; color: @disabled; }
QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox, QComboBox {
    background: @field; color: @text; border: 1px solid @border;
    border-radius: 5px; padding: 5px; selection-background-color: @accent;
    selection-color: @onAccent; }
QCheckBox { color: @text; background: transparent; }
)QSS";

class DialogStyler : public QObject
{
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if ((event->type() == QEvent::Polish || event->type() == QEvent::ThemeChange)
            && (qobject_cast<QMessageBox*>(watched) || qobject_cast<QInputDialog*>(watched)
                || watched->property("hotaruBaseDialog").toBool()))
        {
            auto* dialog = static_cast<QWidget*>(watched);
            // 已安装的上游插件仍可能传入旧标题，显示时沿用宿主品牌。
            if (dialog->windowTitle().compare(QLatin1String("ltool"), Qt::CaseInsensitive) == 0)
                dialog->setWindowTitle(QApplication::applicationDisplayName());
            bindDialog(dialog);
        }
        return false;
    }
};

// 日间这一份是 2026-10-20 他挑的 **P1 = iOS 设置页那套「浅灰页面 + 纯白卡片 + 发丝线」**。
// 前面两版都被他否了：第一版"整块接近纯白"（他："太白了换一个白色"）、第二版 A 冷灰纸
//（他："太丑了太难受了"，多半是 `#e9edf3` 那颗带蓝味 —— 蓝减红 +10）。
// 苹果那套的关键不是把底压深，是**中性**（页面 `#f2f2f7` 蓝减红只有 +5）加分组层次：
// 页面 `#f2f2f7` → 卡片 `#ffffff`（1.12:1）→ 发丝线 `#d1d1d6`。
// 现算的对比度（WCAG 相对亮度）：正文 #1d1d1f 压白卡 = 16.83:1、次级 #6e6e73 = 5.07:1、
// 白字压主色 #0066CC = 5.57:1、启动器那圈描边 #868a90 压页面 = 3.11:1。
// **一句要说清的**：苹果自己的规范不过 AA —— iOS 的 secondaryLabel 是 rgba(0,0,0,0.5)
// = 压白底 3.95:1，白字压 iOS 蓝 #007AFF = 4.02:1。所以这两颗没照抄，按 AA 调深了
//（次级用 #6e6e73、主色用官网那颗链接蓝 #0066CC）。`dim` 是唯一保留苹果原味的
// 低对比档（#a1a1a6 = 2.57:1），因为它画的是**按不动的非可执行行** —— 禁用态按 WCAG 免检。
// 夜间那一列是这一版一直以来的样子，一个字符都没动。
const Role kRoles[] = {
    { "window",     "#1b1c20", "#f2f2f7" },
    { "base",       "#17181c", "#ffffff" },
    { "alt",        "#232429", "#f2f2f7" },
    { "text",       "#e8ebf2", "#1d1d1f" },
    // 日间这三档在苹果那套里本来就是一档（secondaryLabel），所以并成一个值；
    // 夜里那三档差得开（#8b93a1 / #9aa3b2 / #78808c），保留各自的名字不动。
    { "muted",      "#8b93a1", "#6e6e73" },
    { "groupText",  "#9aa3b2", "#6e6e73" },
    { "accent",     "#3f6ae0", "#0066cc" },
    { "accentUp",   "#4f79e8", "#0072ce" },
    { "accentDown", "#3560cc", "#0058b0" },
    { "border",     "#33353d", "#d1d1d6" },
    { "line",       "#2c2d34", "#e5e5ea" },
    { "field",      "#232429", "#ffffff" },
    { "group",      "#1f2025", "#ffffff" },
    { "edit",       "#17181c", "#ffffff" },
    { "button",     "#2b2d34", "#f2f2f7" },
    { "hover",      "#33353d", "#e8e8ed" },
    { "pressed",    "#26272c", "#dedee3" },
    { "disabled",   "#5b616e", "#a1a1a6" },
    { "disabledLine", "#4d525d", "#d1d1d6" },
    { "okText",     "#9fe0a8", "#12692b" },
    { "ok",         "#10b981", "#0b7a4b" },
    { "infoText",   "#78c8ff", "#0066cc" },
    { "borderHi",   "#3a3c45", "#b8b8be" },
    { "borderHiUp", "#4f5360", "#a1a1a6" },
    { "buttonUp",   "#363842", "#e8e8ed" },
    { "buttonDown", "#222328", "#dedee3" },
    { "editText",   "#eef1f7", "#1d1d1f" },
    { "dim",        "#a0a6b2", "#a1a1a6" },
    { "sectionText","#78808c", "#6e6e73" },
    { "badgeBg",    "#5a606c", "#6e6e73" },
    { "selBg",      "#2c3550", "#d1e3f6" },
    { "good",       "#60be78", "#1f7a3d" },
    { "warn",       "#d8a854", "#8a5b0b" },
    { "bad",        "#e06c6c", "#b3261e" },
    { "mutedBadge", "#8c94a0", "#6e6e73" },
    { "neutralBadge", "#7896d2", "#0066cc" },
    { "cyan",       "#06b6d4", "#0a6c86" },
    // 启动器那扇是 WA_TranslucentBackground + 无边框 —— "搜索框在哪儿"这件事只由这一圈描边
    // 说给他看，所以 panelEdge 必须和 panel 自己差得开（2026-10-20 他第一句反馈就是
    // "这个白色不好，完全看不清搜索框的边框"：那份底是纯白、描边和夜里共用一颗黑压 26/255，
    // grab() 出来量到 1.51:1，等于没有）。#AARRGGBB 是 QColor 和 QSS 都认的写法，
    // 半透明因此能存进表里，不用在调用点补 alpha。
    // 中间试过一档"真透下去"（日 80% / 夜 85%，拿纯白纯黑两种极端桌面算出来的），
    // 他看完说"先把透明干掉" —— 所以两份都回到不透（夜里那颗 247/255 是这一版一直以来的值，
    // 不是 255；那是它本来的样子，不是残留的透明档）。
    { "panel",      "#f71c1e24", "#fff2f2f7" },
    { "panelEdge",  "#1affffff", "#868a90" },
    { "rowSel",     "#1cffffff", "#2e0066cc" },
    { "hairline",   "#ffffff", "#14182a" },
    // 下面这几颗里**还写着 rgba() 的那些**QColor 解析不了 —— 只准在 QSS 里写 @name，
    // 自绘要半透明请用 color("角色", alpha)，或者照卡片那两颗改成 #AARRGGBB（两边都吃）。
    { "tintCyan",   "rgba(6, 182, 212, 0.16)", "rgba(6, 182, 212, 0.12)" },
    { "tintCyanLine", "rgba(6, 182, 212, 0.32)", "rgba(6, 182, 212, 0.40)" },
    { "hoverSoft",  "rgba(255,255,255,26)", "rgba(20,24,40,22)" },
    // 卡片那两颗原本是 `rgba(255,255,255,0.08)` / `0.12` —— **只有自绘剪贴板那一张卡需要 QColor 形式**，
    // 而 QColor 不认 rgba()，于是它只能把三颗深色值抄进自己文件里，日间模式一开就是
    // "白卡片上写白字"（他 2026-10-20 报的那句）。改成 `#AARRGGBB` 一份值两边都吃：
    // QSS 认它，QColor 也认它（`rowSel`/`hairline` 那一族早就是这么办的）。alpha 换算：0.08→0x14、0.12→0x1f。
    { "card",       "#14ffffff", "#ffffffff" },
    // 日间那颗描边原来跟着 iOS 的 separator 取 `#d1d1d6` —— 拿判据一量只有 **1.52:1**，
    // 白卡压白页、边界糊成一片（他这次说的"背景完全看不清"有这一半）。
    // 换成启动器那圈描边同一支 `panelEdge` 的日间值：非文本对比度 3.47:1（WCAG 1.4.11 要 ≥3:1）。
    // 夜里那一份保持"12% 白"没动 —— 他这次点的是日间，而且这条和启动器一样按"夜里只钉住别更糊"判。
    { "cardLine",   "#1fffffff", "#ff868a90" },
    { "scrollHandle", "rgba(255,255,255,60)", "rgba(20,24,40,60)" },
    { "onAccent",   "white", "white" },
};

const char* valueOf(const Role& r)
{
    return current() == Mode::Dark ? r.dark : r.light;
}

// 接住发给这个控件的 ThemeChange，把自己那份带 @token 的 QSS 按新模式重算一遍。
class Binder : public QObject
{
public:
    explicit Binder(const char* qssWithTokens, bool launcher = false) : m_qss(qssWithTokens), m_launcher(launcher) {}

protected:
    bool eventFilter(QObject* watched, QEvent* e) override
    {
        if (e->type() == QEvent::ThemeChange) {
            if (auto* w = qobject_cast<QWidget*>(watched))
                w->setStyleSheet(m_launcher ? expandLauncher(QString::fromUtf8(m_qss)) : expand(m_qss.constData()));
        }
        return false;
    }

private:
    QByteArray m_qss;
    bool m_launcher;
};

const Role* findRole(const QString& name)
{
    for (const Role& r : kRoles)
        if (name == QLatin1String(r.name))
            return &r;
    return nullptr;
}

QString themesDir()
{
    return Utils::dataDir() + QStringLiteral("/hotaru/themes");
}

QString packageCacheKey;
Package packageCache;

bool builtinId(const QString& id)
{
    return id == QLatin1String("hotaru-black") || id == QLatin1String("hotaru-grey")
        || id == QLatin1String("hotaru-white");
}

Theme::Package builtinPackage(const QString& id = QStringLiteral("hotaru-black"))
{
    Theme::Package p;
    p.id = id;
    p.name = id == QLatin1String("hotaru-grey") ? QStringLiteral("萤·灰色")
        : id == QLatin1String("hotaru-white") ? QStringLiteral("萤·白色") : QStringLiteral("萤·黑色");
    p.version = QStringLiteral("1.0.0");
    p.author = QStringLiteral("Hotaru");
    p.builtin = p.valid = true;
    // 基础主题由 Qt 绘制，第三方素材不进入程序资源。
    if (id == QLatin1String("hotaru-grey"))
        p.palette = {{"window", "#343434"}, {"panel", "#343434"}, {"base", "#343434"},
                     {"edit", "#292929"}, {"field", "#404040"}, {"text", "#ffffff"}};
    else if (id == QLatin1String("hotaru-black"))
        p.palette = {{"panel", "#1e1e1e"}, {"panelEdge", "#20ffffff"}};
    else p.palette = {{"panel", "#f4f4f4"}};
    return p;
}

bool safeRelative(const QString& path)
{
    const QString name = QDir::fromNativeSeparators(path);
    return !name.isEmpty() && !QDir::isAbsolutePath(name) && !name.contains(QLatin1Char(':'))
        && !name.split(QLatin1Char('/')).contains(QStringLiteral(".."));
}

bool safePackageId(const QString& id)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    return re.match(id).hasMatch();
}

const QRegularExpression kResourceUrls(QStringLiteral(R"URL(url\(\s*["']?([^\)"']+)["']?\s*\))URL"));

bool validStylesheetResources(const Package& package)
{
    QFile file(QDir(package.rootPath).filePath(package.stylesheet));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QString qss=QString::fromUtf8(file.readAll());
    // v2 将几何交给清单，禁止样式表再次移动、撑大或覆盖布局属性。
    static const QRegularExpression geometry(QStringLiteral(
        R"((?:^|[;{])\s*(?:min-width|max-width|width|min-height|max-height|height|margin(?:-[\w-]+)?|padding(?:-[\w-]+)?|position|left|top|right|bottom|qproperty-[\w-]+)\s*:)"),
        QRegularExpression::CaseInsensitiveOption);
    if (package.schemaVersion>=2 && geometry.match(qss).hasMatch()) return false;
    auto matches=kResourceUrls.globalMatch(qss);
    while (matches.hasNext()) {
        const QString relative=matches.next().captured(1).trimmed();
        if (!safeRelative(relative) || !QFileInfo(QDir(package.rootPath).filePath(relative)).isFile()) return false;
    }
    return true;
}

bool hasExecutable(const QString& root)
{
    const QStringList allowed = {"json", "qss", "png", "jpg", "jpeg", "webp", "gif", "bmp", "ttf", "otf"};
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo file(it.next());
        if (file.isSymLink() || !allowed.contains(file.suffix().toLower())) return true;
    }
    return false;
}

bool readSurface(const QJsonObject& object, Package& package)
{
    if (object.isEmpty()) return false;
    const auto values = [](const QJsonValue& value, int count) {
        const auto a = value.toArray();
        QVector<int> out;
        if (a.size() != count) return out;
        for (const auto& v : a) {
            if (!v.isDouble() || v.toDouble() != v.toInt() || qAbs(v.toDouble()) > 8192) return QVector<int>{};
            out << v.toInt();
        }
        return out;
    };
    const auto size = values(object.value("canvasSize"), 2);
    const auto margins = values(object.value("frameMargins"), 4);
    const auto padding = values(object.value("framePadding").isUndefined()
        ? QJsonValue(QJsonArray{0,0,0,0}) : object.value("framePadding"), 4);
    const auto search = values(object.value("searchRect"), 4);
    const auto content = values(object.value("contentRect"), 4);
    if (size.size()!=2 || margins.size()!=4 || padding.size()!=4 || search.size()!=4 || content.size()!=4) return false;
    if (size[0]<360 || size[1]<240 || size[0]>4096 || size[1]>4096) return false;
    for (const auto& group : {margins, padding}) for (int v : group) if (v<0) return false;
    auto& surface = package.surface;
    surface.canvasSize = QSize(size[0],size[1]);
    surface.frame = object.value("frame").toString();
    surface.frameMargins = QMargins(margins[0],margins[1],margins[2],margins[3]);
    surface.framePadding = QMargins(padding[0],padding[1],padding[2],padding[3]);
    surface.searchRect = QRect(search[0],search[1],search[2],search[3]);
    surface.contentRect = QRect(content[0],content[1],content[2],content[3]);
    const QRect canvas(QPoint(0,0),surface.canvasSize);
    if (surface.searchRect.isEmpty() || surface.contentRect.isEmpty()
        || !canvas.contains(surface.searchRect) || !canvas.contains(surface.contentRect)
        || surface.searchRect.intersects(surface.contentRect)) return false;
    const auto imageSize = [&](const QString& path) {
        if (!safeRelative(path)) return QSize();
        QImageReader reader(QDir(package.rootPath).filePath(package.assetRoot + '/' + path));
        const QSize actual = reader.size();
        return actual.isValid() && actual.width()<=4096 && actual.height()<=4096
            && !reader.read().isNull() ? actual : QSize();
    };
    const QSize frameSize = imageSize(surface.frame);
    if (!frameSize.isValid() || margins[0]+margins[2]>=frameSize.width() || margins[1]+margins[3]>=frameSize.height()
        || margins[0]+margins[2]+padding[0]+padding[2]>=size[0]
        || margins[1]+margins[3]+padding[1]+padding[3]>=size[1]) return false;
    if (package.schemaVersion >= 2) {
        // v2 的设计坐标必须对应原图加透明留白，避免首次加载就发生缩放偏移。
        if (frameSize + QSize(padding[0]+padding[2], padding[1]+padding[3]) != surface.canvasSize) return false;
        const auto footer = values(object.value("footerRect"), 4);
        const auto settings = values(object.value("settingsRect"), 4);
        if (footer.size()!=4 || settings.size()!=4) return false;
        surface.footerRect = QRect(footer[0],footer[1],footer[2],footer[3]);
        surface.settingsRect = QRect(settings[0],settings[1],settings[2],settings[3]);
        if (surface.searchRect.height()!=48 || surface.footerRect.height()<28
            || surface.settingsRect.width()!=surface.settingsRect.height()
            || surface.settingsRect.width()<28 || surface.settingsRect.width()>40
            || !canvas.contains(surface.footerRect) || !surface.footerRect.contains(surface.settingsRect)
            || surface.footerRect.intersects(surface.contentRect) || surface.footerRect.intersects(surface.searchRect)) return false;
    }
    if (object.contains("decorations") && !object.value("decorations").isArray()) return false;
    const auto decorations = object.value("decorations").toArray();
    if (decorations.size()>8) return false;
    for (const auto& value : decorations) {
        if (!value.isObject()) return false;
        const auto d=value.toObject();
        const auto offset=values(d.value("offset"),2), dims=values(d.value("size"),2);
        Decoration decoration;
        decoration.image=d.value("image").toString(); decoration.anchor=d.value("anchor").toString();
        decoration.reference=d.value("reference").toString(QStringLiteral("window"));
        if (!QStringList{"top-left","top-right","bottom-left","bottom-right"}.contains(decoration.anchor)
            || !QStringList{"window","frame","content"}.contains(decoration.reference)
            || offset.size()!=2 || dims.size()!=2 || dims[0]<=0 || dims[1]<=0
            || dims[0]>size[0] || dims[1]>size[1] || !imageSize(decoration.image).isValid()) return false;
        decoration.offset=QPoint(offset[0],offset[1]); decoration.size=QSize(dims[0],dims[1]);
        if (d.contains("allowContentOverlap") && !d.value("allowContentOverlap").isBool()) return false;
        decoration.allowContentOverlap=d.value("allowContentOverlap").toBool(false);
        if (d.contains("sourceRect")) {
            const auto source=values(d.value("sourceRect"),4);
            if (source.size()!=4 || source[2]<=0 || source[3]<=0) return false;
            decoration.sourceRect=QRect(source[0],source[1],source[2],source[3]);
            if (!QRect(QPoint(),imageSize(decoration.image)).contains(decoration.sourceRect)) return false;
        }
        if (package.schemaVersion >= 2) {
            const QSize original=decoration.sourceRect.isEmpty()?imageSize(decoration.image):decoration.sourceRect.size();
            if (original.width()*dims[1]!=original.height()*dims[0]) return false;
            QRect reference = canvas;
            if (decoration.reference==QLatin1String("frame")) reference=canvas.marginsRemoved(surface.framePadding);
            if (decoration.reference==QLatin1String("content")) reference=surface.contentRect;
            QPoint pos=reference.topLeft()+decoration.offset;
            if (decoration.anchor.endsWith(QLatin1String("right"))) pos.rx()+=reference.width()-decoration.size.width();
            if (decoration.anchor.startsWith(QLatin1String("bottom"))) pos.ry()+=reference.height()-decoration.size.height();
            const QRect area(pos,decoration.size);
            if (!canvas.contains(area) || area.intersects(surface.searchRect)
                || (!decoration.allowContentOverlap && area.intersects(surface.contentRect))
                || area.intersects(surface.footerRect)) return false;
        }
        surface.decorations << decoration;
    }
    return true;
}

Theme::Package readPackage(const QString& root)
{
    Theme::Package p;
    p.rootPath = root;
    QFile f(root + QStringLiteral("/theme.json"));
    if (!f.open(QIODevice::ReadOnly)) {
        p.error = QStringLiteral("缺少 theme.json");
        return p;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        p.error = QStringLiteral("theme.json 不是有效 JSON：%1").arg(pe.errorString());
        return p;
    }
    const QJsonObject o = doc.object();
    if (o.contains("schemaVersion") && (!o.value("schemaVersion").isDouble()
        || (o.value("schemaVersion").toDouble()!=1 && o.value("schemaVersion").toDouble()!=2))) {
        p.error=QStringLiteral("不支持的主题 schemaVersion，仅支持 1 或 2");
        return p;
    }
    p.schemaVersion=o.value("schemaVersion").toInt(1);
    p.id = o.value(QStringLiteral("id")).toString();
    p.name = o.value(QStringLiteral("name")).toString();
    p.version = o.value(QStringLiteral("version")).toString();
    p.author = o.value(QStringLiteral("author")).toString();
    p.stylesheet = o.value(QStringLiteral("stylesheet")).toString(QStringLiteral("theme.qss"));
    p.assetRoot = o.value(QStringLiteral("assetRoot")).toString(QStringLiteral("assets"));
    if (!safePackageId(p.id))
        p.error = QStringLiteral("主题 id 只能包含英文、数字、下划线和短横线");
    else if (p.name.isEmpty())
        p.error = QStringLiteral("主题缺少 name");
    else if (p.version.isEmpty())
        p.error = QStringLiteral("主题缺少 version");
    else if (o.value(QStringLiteral("minAppVersion")).toString().isEmpty())
        p.error = QStringLiteral("主题缺少 minAppVersion");
    else if (!safeRelative(p.stylesheet) || !safeRelative(p.assetRoot))
        p.error = QStringLiteral("主题资源路径不安全");
    else if (!QFileInfo::exists(QDir(root).absoluteFilePath(p.stylesheet)))
        p.error = QStringLiteral("缺少主题样式表：%1").arg(p.stylesheet);
    else if (hasExecutable(root))
        p.error = QStringLiteral("主题包只允许 JSON、QSS、图片和字体资源");
    else if (!QFileInfo(QDir(root).filePath(p.assetRoot)).isDir())
        p.error = QStringLiteral("缺少主题资源目录");
    else if (!validStylesheetResources(p))
        p.error = QStringLiteral("样式表资源路径不安全、文件缺失或 v2 样式包含布局属性");
    else if (QVersionNumber::fromString(o.value("minAppVersion").toString()).isNull()
             || QVersionNumber::fromString(o.value("minAppVersion").toString()) > QVersionNumber::fromString(QStringLiteral(LTOOL_VERSION)))
        p.error = QStringLiteral("主题要求更新版本的萤或版本号无效");
    else if (o.contains("palette") && !o.value("palette").isObject())
        p.error = QStringLiteral("主题 palette 必须是对象");
    else {
        const auto palette = o.value("palette").toObject();
        for (auto it=palette.begin(); it!=palette.end(); ++it) {
            if (!findRole(it.key()) || !it.value().isString() || !QColor(it.value().toString()).isValid()) {
                p.error = QStringLiteral("主题颜色角色无效：%1").arg(it.key());
                return p;
            }
            p.palette.insert(it.key(),it.value().toString());
        }
        if (o.contains("surface") && (!o.value("surface").isObject() || !readSurface(o.value("surface").toObject(),p)))
            p.error = QStringLiteral("主题外框、布局或装饰资源无效");
        else p.valid = true;
    }
    return p;
}

const char* const kHotaruQss = "QWidget#hotaruGrid { background: transparent; }";

void broadcastThemeChange()
{
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        for (QWidget* w : app->topLevelWidgets()) {
            QEvent ev(QEvent::ThemeChange);
            QCoreApplication::sendEvent(w, &ev);
            const auto kids = w->findChildren<QWidget*>();
            for (QWidget* k : kids)
                QCoreApplication::sendEvent(k, &ev);
        }
    }
}

} // namespace

Mode current()
{
    QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
    return cfg.value("ui/theme").toString().compare(QLatin1String("light"), Qt::CaseInsensitive) == 0
               ? Mode::Light : Mode::Dark;
}

const char* name(Mode m)
{
    return m == Mode::Light ? "日间" : "夜间";
}

QString token(const char* role)
{
    const Role* r = findRole(QLatin1String(role));
    return r ? QString::fromUtf8(valueOf(*r)) : QString();
}

QString launcherToken(const char* role)
{
    const auto package=currentPackage();
    const QString override=package.palette.value(QLatin1String(role));
    if (!override.isEmpty()) return override;
    const Role* r=findRole(QLatin1String(role));
    const bool light=package.builtin ? package.id==QLatin1String("hotaru-white")
        : package.palette.contains(QStringLiteral("text"))
            ? QColor(package.palette.value(QStringLiteral("text"))).lightness()<128 : current()==Mode::Light;
    return r ? QString::fromUtf8(light?r->light:r->dark) : QString();
}

QColor launcherColor(const char* role) { return QColor(launcherToken(role)); }

QColor color(const char* role)
{
    return QColor(token(role));
}

QColor color(const char* role, int alpha)
{
    QColor c = color(role);
    c.setAlpha(alpha);
    return c;
}

// WCAG 2.1 的相对亮度与对比度。判据要问"这两颗颜色放一起看得见吗"，只能有一个算法 ——
// 原来 `--cfgcheck` 里那份局部 lambda 就是第二份，搬这儿了。
double relativeLuminance(QRgb c)
{
    const auto ch = [](int v) {
        const double s = v / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * ch(qRed(c)) + 0.7152 * ch(qGreen(c)) + 0.0722 * ch(qBlue(c));
}

double contrast(QRgb a, QRgb b)
{
    const double la = relativeLuminance(a), lb = relativeLuminance(b);
    const double hi = la > lb ? la : lb, lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

QColor over(const QColor& top, const QColor& bottom)
{
    const double a = top.alphaF();
    return QColor::fromRgbF(a * top.redF() + (1.0 - a) * bottom.redF(),
                            a * top.greenF() + (1.0 - a) * bottom.greenF(),
                            a * top.blueF() + (1.0 - a) * bottom.blueF());
}

static QString expandTokens(const QString& source, bool launcher)
{
    QString out = source;
    // 名字必须整段吃掉再查表：按表顺序 replace 会先替 `@card` 把 `@cardLine` 变成
    // `<card 的值>Line`（`@accent`/`@accentUp`、`@disabled`/`@disabledLine`、
    // `@group`/`@groupText` 是同一对陷阱）。不认识的名字原样留着，好排查。
    static const QRegularExpression re(QStringLiteral("@([A-Za-z][A-Za-z0-9]*)"));
    QVector<QPair<int, QString>> hits;
    auto it = re.globalMatch(out);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        if (findRole(m.captured(1)))
            hits.append({int(m.capturedStart()), m.captured(1)});
    }
    for (auto it2 = hits.rbegin(); it2 != hits.rend(); ++it2)
        out.replace(it2->first, it2->second.size() + 1, launcher
            ? launcherToken(it2->second.toUtf8().constData()) : token(it2->second.toUtf8().constData()));
    return out;
}

QString expand(const char* qssWithTokens) { return expandTokens(QString::fromUtf8(qssWithTokens),false); }
QString expandLauncher(const QString& qssWithTokens) { return expandTokens(qssWithTokens,true); }

QString expand(const QString& qssWithTokens)
{
    return expand(qssWithTokens.toUtf8().constData());
}

QString menuStyle()
{
    return expand(kMenuQss);
}

QString tipStyle()
{
    return expand(kTipQss);
}

void bind(QWidget* w, const char* qssWithTokens)
{
    w->setStyleSheet(expand(qssWithTokens));
    // 过滤器挂在 w 自己身上：w 析构时 Qt 会自动摘掉，不会留悬空指针。
    auto* b = new Binder(qssWithTokens);
    b->setParent(w);
    w->installEventFilter(b);
}

void bindMenu(QWidget* menu)
{
    bind(menu, kMenuQss);
}

void bindDialog(QWidget* dialog)
{
    dialog->setProperty("hotaruBaseDialog", true);
    QPalette pal = QApplication::palette();
    // 未激活的确认框也必须保持正文对比度，只有禁用控件使用弱化颜色。
    for (const auto group : {QPalette::Active, QPalette::Inactive}) {
        pal.setColor(group, QPalette::WindowText, color("text"));
        pal.setColor(group, QPalette::Text, color("text"));
        pal.setColor(group, QPalette::ButtonText, color("text"));
    }
    dialog->setPalette(pal);
    dialog->setStyleSheet(expand(kDialogQss));
}

void bindLauncher(QWidget* w, const char* qssWithTokens)
{
    w->setStyleSheet(expandLauncher(QString::fromUtf8(qssWithTokens)));
    auto* binder=new Binder(qssWithTokens,true);
    binder->setParent(w);
    w->installEventFilter(binder);
}

void applyToLauncher(QWidget* w)
{
    QPalette pal=w->palette();
    pal.setColor(QPalette::Window,launcherColor("window"));
    pal.setColor(QPalette::Base,launcherColor("edit"));
    pal.setColor(QPalette::WindowText,launcherColor("text"));
    pal.setColor(QPalette::Text,launcherColor("text"));
    pal.setColor(QPalette::PlaceholderText,launcherColor("muted"));
    pal.setColor(QPalette::Highlight,launcherColor("accent"));
    pal.setColor(QPalette::HighlightedText,launcherColor("onAccent"));
    w->setPalette(pal);
    w->setStyleSheet(packageStylesheet());
}

void applyToApp(QApplication& app)
{
    if (!app.findChild<QObject*>(QStringLiteral("hotaruDialogStyler"), Qt::FindDirectChildrenOnly)) {
        auto* styler = new DialogStyler(&app);
        styler->setObjectName(QStringLiteral("hotaruDialogStyler"));
        app.installEventFilter(styler);
    }
    const bool light = current() == Mode::Light;
    QPalette pal = app.palette();
    const QColor window = color("window"), base = color("base"), alt = color("alt");
    const QColor text = color("text"), muted = color("muted"), accent = color("accent");
    pal.setColor(QPalette::Window, window);
    pal.setColor(QPalette::Base, light ? base : color("edit"));
    pal.setColor(QPalette::AlternateBase, alt);
    pal.setColor(QPalette::Text, text);
    pal.setColor(QPalette::WindowText, text);
    pal.setColor(QPalette::Button, color("button"));
    pal.setColor(QPalette::ButtonText, text);
    pal.setColor(QPalette::ToolTipBase, alt);
    pal.setColor(QPalette::ToolTipText, text);
    pal.setColor(QPalette::Highlight, accent);
    pal.setColor(QPalette::HighlightedText, Qt::white);
    pal.setColor(QPalette::PlaceholderText, muted);
    pal.setColor(QPalette::Link, accent);
    for (QPalette::ColorGroup g : { QPalette::Disabled, QPalette::Inactive }) {
        pal.setColor(g, QPalette::Text, muted);
        pal.setColor(g, QPalette::WindowText, muted);
        pal.setColor(g, QPalette::ButtonText, muted);
        pal.setColor(g, QPalette::Highlight, color("border"));
    }
    app.setPalette(pal);
    app.setStyleSheet(tipStyle());
}

QVector<Package> packages()
{
    QVector<Package> out;
    for (const QString& id : {QStringLiteral("hotaru-black"), QStringLiteral("hotaru-grey"), QStringLiteral("hotaru-white")})
        out.append(builtinPackage(id));
    const QDir dir(themesDir());
    if (!dir.exists())
        return out;
    for (const QFileInfo& fi : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (fi.fileName().startsWith(QLatin1Char('.'))) continue;
        Package p = readPackage(fi.absoluteFilePath());
        if (builtinId(p.id)) continue;
        if (!p.id.isEmpty() || !p.error.isEmpty())
            out.append(p);
    }
    return out;
}

Package currentPackage()
{
    auto cfg = PinnedApps::config();
    const QString key = Utils::dataDir() + QLatin1Char('|') + cfg.themeId;
    if (key == packageCacheKey) return packageCache;
    Package found;
    if (builtinId(cfg.themeId)) found = builtinPackage(cfg.themeId);
    else found = readPackage(QDir(themesDir()).filePath(safePackageId(cfg.themeId) ? cfg.themeId : QStringLiteral(".missing")));
    if (!found.valid) {
        found = builtinPackage();
        cfg.themeId = found.id;
        PinnedApps::saveConfig(cfg);
    }
    packageCache = found;
    packageCacheKey = Utils::dataDir() + QLatin1Char('|') + cfg.themeId;
    return packageCache;
}

QString currentPackageId() { return currentPackage().id; }

bool activatePackage(const QString& id, QString* error)
{
    const auto all = packages();
    for (const Package& p : all) {
        if (p.id != id)
            continue;
        if (!p.valid) {
            if (error)
                *error = p.error;
            return false;
        }
        PinnedApps::LauncherConfig cfg = PinnedApps::config();
        cfg.themeId = id;
        packageCacheKey.clear();
        if (!PinnedApps::saveConfig(cfg, error))
            return false;
        if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance()))
            applyToApp(*app);
        broadcastThemeChange();
        return true;
    }
    if (error)
        *error = QStringLiteral("找不到主题：%1").arg(id);
    return false;
}

bool importPackage(const QString& zipPath, QString* error)
{
    const QString staging = themesDir() + QStringLiteral("/.import-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(staging)) {
        if (error)
            *error = QStringLiteral("无法创建主题临时目录：%1").arg(staging);
        return false;
    }
    const Archive::Result r = Archive::extract(zipPath, staging,
                                               Archive::Report{}, Archive::Cancel{});
    if (!r.ok) {
        QDir(staging).removeRecursively();
        if (error)
            *error = r.error;
        return false;
    }
    Package p = readPackage(staging);
    if (!p.valid) {
        QDir(staging).removeRecursively();
        if (error)
            *error = p.error;
        return false;
    }
    if (builtinId(p.id)) {
        QDir(staging).removeRecursively();
        if (error)
            *error = QStringLiteral("不能覆盖内置主题");
        return false;
    }
    const QString target = themesDir() + QLatin1Char('/') + p.id;
    QDir().mkpath(themesDir());
    const QString previousId = currentPackageId();
    const bool replacing = QDir(target).exists();
    // 先释放当前主题的 QMovie 文件句柄，再保留完整旧包以便失败回滚。
    if (previousId == p.id && !activatePackage(QStringLiteral("hotaru-black"), error)) {
        QDir(staging).removeRecursively();
        return false;
    }
    const QString backup = staging + QStringLiteral("-backup");
    if (replacing && !QDir().rename(target, backup)) {
        QDir(staging).removeRecursively();
        activatePackage(previousId);
        if (error) *error = QStringLiteral("无法备份已有主题：%1").arg(p.id);
        return false;
    }
    if (!QDir().rename(staging, target)) {
        QDir(staging).removeRecursively();
        if (replacing) QDir().rename(backup, target);
        activatePackage(previousId);
        if (error)
            *error = QStringLiteral("无法安装主题：%1").arg(p.id);
        return false;
    }
    if (!activatePackage(p.id, error)) {
        QDir(target).removeRecursively();
        if (replacing) QDir().rename(backup, target);
        activatePackage(previousId);
        return false;
    }
    if (replacing) QDir(backup).removeRecursively();
    return true;
}

bool removePackage(const QString& id, QString* error)
{
    if (!safePackageId(id)) {
        if (error)
            *error = QStringLiteral("主题 id 不合法");
        return false;
    }
    if (builtinId(id)) {
        if (error)
            *error = QStringLiteral("内置主题不能删除");
        return false;
    }
    const QString path = themesDir() + QLatin1Char('/') + id;
    if (!QDir(path).exists()) {
        if (error)
            *error = QStringLiteral("主题不存在：%1").arg(id);
        return false;
    }
    if (currentPackageId() == id && !activatePackage(QStringLiteral("hotaru-black"), error)) return false;
    if (!QDir(path).removeRecursively()) {
        if (error)
            *error = QStringLiteral("删除主题失败：%1").arg(id);
        return false;
    }
    return true;
}

QString assetPath(const QString& relativePath)
{
    const Package p=currentPackage();
    if (p.builtin || !p.valid || !safeRelative(relativePath)) return {};
    return QDir(p.rootPath).filePath(p.assetRoot + QLatin1Char('/') + relativePath);
}

QString packageStylesheet()
{
    const Package p = currentPackage();
    if (p.builtin) return expandLauncher(kHotaruQss);
    QFile file(QDir(p.rootPath).filePath(p.stylesheet));
    if (!file.open(QIODevice::ReadOnly)) return expandLauncher(kHotaruQss);
    QString qss = QString::fromUtf8(file.readAll());
    // 逐条解析资源引用，拒绝网络地址和越出主题目录的路径。
    auto matches = kResourceUrls.globalMatch(qss);
    QVector<QPair<int, QPair<int, QString>>> replacements;
    while (matches.hasNext()) {
        const auto match = matches.next(); const QString relative = match.captured(1).trimmed();
        if (!safeRelative(relative)) return expandLauncher(kHotaruQss);
        const QString path = QDir(p.rootPath).filePath(relative);
        if (!QFileInfo::exists(path)) return expandLauncher(kHotaruQss);
        replacements.append({int(match.capturedStart()), {int(match.capturedLength()),
            QStringLiteral("url(\"%1\")").arg(QUrl::fromLocalFile(path).toString())}});
    }
    for (auto it=replacements.crbegin();it!=replacements.crend();++it)
        qss.replace(it->first,it->second.first,it->second.second);
    return expandLauncher(kHotaruQss) + expandLauncher(qss);
}

void setMode(Mode m)
{
    QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
    const Mode old = current();
    auto launcher = PinnedApps::config();
    if (builtinId(launcher.themeId)) {
        launcher.themeId = m == Mode::Light ? QStringLiteral("hotaru-white") : QStringLiteral("hotaru-black");
        PinnedApps::saveConfig(launcher);
        packageCacheKey.clear();
    }
    cfg.setValue("ui/theme", m == Mode::Light ? QLatin1String("light") : QLatin1String("dark"));
    cfg.sync();
    if (m == old && !builtinId(launcher.themeId))
        return;   // 配置页构造时给"当前那档"打勾也会走这儿：值没变就别空转一遍全场广播
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        applyToApp(*app);
        // 样式表挂在**具体控件**身上（Theme::bind 挂的那批），改 qApp 传导不下去，
        // 而顶层窗口之外没人会自动收到通知 —— 所以顶层和它的子控件都要发一遍。
        // 没接这个事件的控件走 QWidget 默认处理（不重 polish、不重画），代价只是一次空转。
        for (QWidget* w : app->topLevelWidgets()) {
            QEvent ev(QEvent::ThemeChange);
            QCoreApplication::sendEvent(w, &ev);
            const auto kids = w->findChildren<QWidget*>();
            for (QWidget* k : kids)
                QCoreApplication::sendEvent(k, &ev);
        }
    }
}

} // namespace Theme

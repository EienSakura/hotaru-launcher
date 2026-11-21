#include "SettingsDialog.h"
#include "SettingsControlStyle.h"
#include "SettingsFrame.h"
#include "SettingsIcons.h"
#include "SettingsPalette.h"
#include <QGuiApplication>
#include <QDesktopServices>
#include "BuildChannel.h"
#include "HotaruNativeHost.h"
#include "HotaruPluginRepository.h"
#include "PluginStore.h"
#include "PluginHost.h"

#include "ConfigPage.h"
#include "SilentInstallLog.h"
#include "AboutPage.h"
#include "ToolsPage.h"
#include "WebsitesPage.h"
#include "Theme.h"
#include "Utils.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFocusEvent>
#include <QCheckBox>
#include <QSpinBox>
#include <QFormLayout>
#include "ClipboardStore.h"
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>

#include <QHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtMath>

#include <windows.h>

namespace {

// 加载标志的动画帧：一个缺了一口的圆环，按帧转。自绘，不依赖字体里的符号
QIcon spinnerIcon(int frame)
{
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(QColor(120, 200, 255), 2, Qt::SolidLine, Qt::FlatCap);
    p.setPen(pen);
    p.drawArc(QRectF(1.5, 1.5, 11, 11), (360 - frame * 45) * 16, 110 * 16);
    p.end();
    return QIcon(pm);
}

QIcon sectionIcon(int section, const SettingsPalette& colors)
{
    constexpr SettingsGlyph glyphs[] = {
        SettingsGlyph::Search, SettingsGlyph::Plug, SettingsGlyph::Gear, SettingsGlyph::Info
    };
    auto icon = settingsIcon(glyphs[section], QColor(colors.muted));
    icon.addPixmap(settingsIcon(glyphs[section], QColor(colors.accent)).pixmap(72, 72), QIcon::Selected);
    return icon;
}

constexpr int kFrameRadius = 17;
constexpr int kTitleHeight = 40;

class SettingsNavigation final : public QListWidget {
public:
    using QListWidget::QListWidget;

protected:
    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::FocusIn) {
            const auto reason = static_cast<QFocusEvent*>(event)->reason();
            if (reason != Qt::ActiveWindowFocusReason)
                setKeyboardFocus(reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason);
        } else if (event->type() == QEvent::KeyPress) {
            setKeyboardFocus(true);
        }
        return QListWidget::event(event);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        setKeyboardFocus(false);
        QListWidget::mousePressEvent(event);
    }

private:
    void setKeyboardFocus(bool value)
    {
        if (property("keyboardFocus").toBool() == value)
            return;
        setProperty("keyboardFocus", value);
        style()->unpolish(this);
        style()->polish(this);
        viewport()->update();
    }
};

// 设置页整体深色主题（子页面共享）
const char* kSettingsQss = R"QSS(
QLabel { color: @text; background: transparent; }
QLabel#tip { color: @muted; }
QLabel#pageTitle { font-size: 21pt; font-weight: 700; }
QLabel#muted { color: @muted; }
QLabel#settingsRowTitle { font-weight: 600; }
QLabel#settingsBadge { color: @muted; background: @field; border: 1px solid @line;
    border-radius: 6px; padding: 3px 8px; }
QLabel#pluginSummary { color: @muted; background: @group; border: 1px solid @line;
    border-radius: 9px; padding: 9px 12px; }

QGroupBox {
    color: @groupText;
    border: 1px solid @line;
    border-radius: 13px;
    margin-top: 16px;
    padding: 14px 12px 12px 12px;
    font-weight: 600;
    background: @group;
}
QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; }
QPushButton:focus { border-color: @accent; }

QPlainTextEdit {
    background: @edit;
    border: 1px solid @line;
    border-radius: 8px;
    color: @okText;
    font-family: Consolas, monospace;
    font-size: 9pt;
}
QLineEdit, QComboBox, QWidget#settingsCompatibility QSpinBox, QTimeEdit, QKeySequenceEdit {
    background: @field;
    border: 1px solid @border;
    border-radius: 8px;
    padding: 5px 8px;
    color: @text;
    selection-background-color: @accent;
}
QLineEdit:focus, QComboBox:focus, QWidget#settingsCompatibility QSpinBox:focus, QTimeEdit:focus, QKeySequenceEdit:focus {
    border: 1px solid @accent;
}
QComboBox::drop-down { border: none; width: 22px; }
QComboBox QAbstractItemView {
    background: @field;
    color: @text;
    selection-background-color: @accent;
    border: 1px solid @border;
    outline: none;
}

QPushButton {
    background: @button;
    color: @text;
    border: 1px solid @border;
    border-radius: 9px;
    padding: 6px 12px;
    min-height: 18px;
}
QPushButton:hover { background: @hover; }
QPushButton:pressed { background: @pressed; }
QPushButton:disabled { color: @disabled; background: @field; }
QPushButton#primary {
    background: @accent;
    border: 1px solid @accent;
    color: @onAccent;
    font-weight: 600;
}
QPushButton#primary:hover { background: @accentUp; }
QPushButton#primary:pressed { background: @accentDown; }
/* #primary 的 ID 选择器比 :disabled 优先，不补这条的话"禁用的主按钮"看着还是能点 */
QPushButton#primary:disabled { background: @field; border: 1px solid @line; color: @disabled; }
QPushButton[settingsPrimary="true"]:enabled {
    background: @primary; color: @onPrimary; border-color: @primary; font-weight: 600;
}
QPushButton[settingsPrimary="true"]:enabled:hover { background: @accent; }
QPushButton[settingsPrimary="true"]:focus { border-color: @text; }
QPushButton#quickMore { padding: 6px 7px; }
QPushButton#quickMore::menu-indicator { width: 0; }
/* 列表行上的小动作按钮：一行四个，要紧凑，但禁用时必须一眼看出来 */
QPushButton#rowBtn {
    padding: 2px 9px;
    font-size: 8.5pt;
    border-radius: 6px;
}
QPushButton#rowBtn:disabled { background: transparent; border: 1px solid @line; color: @disabledLine; }
/* 软件列表的表头：比正文淡、比说明文字重一点，好认但不抢 */
QToolButton#pluginGroupToggle {
    background: @field;
    color: @text;
    border: 1px solid @line;
    border-radius: 8px;
    padding: 6px 10px;
    font-weight: 600;
    text-align: left;
}
QToolButton#pluginGroupToggle:hover { background: @hover; }
QToolButton#pluginGroupToggle:focus { border-color: @accent; }
QFrame#pluginCard, QFrame#lanDetailCard {
    background: @group; border: 1px solid @line; border-radius: 13px;
}
QFrame#pluginCard:focus { border-color: @accent; }
QWidget#settingsNativeDetail, QWidget#lanDetailBody { background: @window; }
QWidget#settingsNativeDetail QScrollArea,
QWidget#settingsNativeDetail QScrollArea > QWidget#qt_scrollarea_viewport {
    background: @window; border: none;
}
QLabel#lanDetailTitle { font-size: 21pt; font-weight: 700; }
QPushButton#nativeDetailBack { background: transparent; color: @muted; outline: none; }
QPushButton#nativeDetailBack:hover { background: @hover; color: @text; }
QPushButton#nativeDetailBack:focus { border-color: @accent; }
QListWidget#lanPairedDevices {
    background: @field; color: @text; border: 1px solid @line; border-radius: 8px;
    padding: 4px; outline: none;
}
QListWidget#lanPairedDevices::item { padding: 5px 8px; border-radius: 5px; }
QListWidget#lanPairedDevices::item:selected { background: @selBg; color: @text; }
QLabel#lanDropHint {
    background: @group; color: @muted; border: 1px dashed @border;
    border-radius: 8px; padding: 6px 10px;
}
QLabel#hotaruNativeBadge {
    background: @primary;
    color: @onPrimary;
    border-radius: 5px;
    padding: 2px 6px;
    font-size: 8.5pt;
    font-weight: 600;
}
QLabel#pluginVersionBadge {
    background: @hover;
    color: @text;
    border: 1px solid @line;
    border-radius: 5px;
    padding: 1px 6px;
    font-size: 8.5pt;
    font-weight: 600;
}
QLabel#colHead { color: @muted; font-size: 8.5pt; font-weight: 600; background: transparent; }

QCheckBox { color: @text; spacing: 8px; background: transparent; }
QWidget#pluginHome QCheckBox::indicator, QWidget#settingsCompatibility QCheckBox::indicator {
    width: 16px; height: 16px;
    border: 1px solid @border;
    border-radius: 4px;
    background: @field;
}
QWidget#pluginHome QCheckBox::indicator:checked, QWidget#settingsCompatibility QCheckBox::indicator:checked {
    background: @accent; border-color: @accent;
}
QWidget#pluginHome QCheckBox::indicator:hover, QWidget#settingsCompatibility QCheckBox::indicator:hover {
    border-color: @accentUp;
}

QListWidget {
    background: @group;
    border: 1px solid @line;
    border-radius: 10px;
    color: @text;
    outline: none;
}
/* 条目背景不能只靠 padding：Qt 的行高来自 sizeHint，padding 只会把画出来的
   背景块撑得比行还高，于是上下条目的背景互相压住（语言列表行距 29、背景 47，
   压掉 18px，看着就是"上下背景色挤在一起"）。行高在下面用 setSizeHint 给足，
   这里只留水平方向的呼吸和圆角。 */
QListWidget::item { padding: 0 8px; border-radius: 6px; }
QListWidget::item:selected { background: @accent; color: @onAccent; }
QListWidget::item:hover:!selected { background: @hover; }
QListWidget#webList::item { border-bottom: 1px solid @line; border-radius: 0; }
QListWidget#webList::item:last { border-bottom: none; }

QTabWidget::pane { border: none; background: transparent; }
QTabBar::tab { background: @field; color: @groupText; border: 1px solid @line;
    padding: 7px 16px; margin-right: 4px; border-top-left-radius: 8px; border-top-right-radius: 8px; }
QTabBar::tab:selected { background: @button; color: @text; border-bottom-color: @accent; }
QTabBar::tab:hover:!selected { background: @pressed; color: @text; }
QTabBar#pluginCategoryTabs { background: @field; border: 1px solid @line; border-radius: 10px; }
QTabBar#pluginCategoryTabs::tab {
    background: transparent; color: @muted; border: 1px solid transparent;
    border-radius: 7px; margin: 3px; padding: 5px 11px;
}
QTabBar#pluginCategoryTabs::tab:selected { background: @group; color: @text; border-color: @line; }
QScrollArea#settingsPageScroll, QScrollArea#settingsPageScroll > QWidget#qt_scrollarea_viewport,
QWidget#aboutPage, QDialog#hotaruSettings QWidget#settingsNativeDetail,
QDialog#hotaruSettings QWidget#settingsNativeDetail QScrollArea,
QDialog#hotaruSettings QWidget#settingsNativeDetail QScrollArea > QWidget#qt_scrollarea_viewport,
QDialog#hotaruSettings QWidget#lanDetailBody { background: transparent; }
QWidget#rowsHost { background: transparent; }
/* 列表分组表头（软件页）：不属于 #row 的盒子，只要一行小字 + 上方的呼吸 */
QLabel#catHead { color: @muted; font-weight: 600; font-size: 9pt; padding: 6px 2px 0 2px; }
QWidget#row {
    background: @group;
    border: 1px solid @line;
    border-radius: 8px;
}
QWidget#row:hover { background: @hover; border-color: @border; }
QWidget#row QLabel { background: transparent; }

QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
QScrollBar::handle:vertical { background: @border; border-radius: 4px; min-height: 24px; }
QScrollBar::handle:vertical:hover { background: @disabledLine; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
)QSS";

const char* kSettingsShellQss = R"QSS(
QDialog#hotaruSettings { background: transparent; }
QLabel#settingsWindowTitle { color: @text; font-size: 9pt; font-weight: 600; }
QToolButton[settingsCaptionButton="true"] { background: transparent; border: 1px solid transparent; border-radius: 6px; }
QToolButton[settingsCaptionButton="true"]:hover { background: @hover; }
QToolButton[settingsCaptionButton="true"]:focus { border-color: @accent; }
QListWidget#menuList { background: transparent; color: @muted; border: none; outline: none; font-size: 10pt; }
QListWidget#menuList::item { padding: 0 10px; border-radius: 9px; }
QListWidget#menuList::item:hover { background: @hover; color: @text; }
QListWidget#menuList::item:selected { background: @selBg; color: @accent; font-weight: 600; }
QListWidget#menuList[keyboardFocus="true"]::item:selected:focus { border: 1px solid @accent; }
)QSS";

} // namespace

// 这一页的样式表 = 那份 token 化的 QSS + 弹层那一份。抽成一颗函数是因为
// Theme::setMode() 会发 QEvent::ThemeChange 过来，届时原样再算一遍（见 event()）。
void SettingsDialog::applyThemeQss()
{
    // QMenu 那几条不在这里写了：全程序只有 Theme::menuStyle() 那一份（Utils::darkMenuStyle
    // 现在是它的转接，插件那边符号名不能动）
    const auto colors = SettingsPalette::current();
    setStyleSheet(colors.expand(kSettingsShellQss));
    if (m_stack) {
        const auto pageStyle = colors.expand(kSettingsQss) + Theme::menuStyle();
        for (int index = 0; index < m_stack->count(); ++index)
            if (index != 2 && m_built[index])
                m_stack->widget(index)->setStyleSheet(pageStyle);
    }
    for (auto* style : findChildren<QStyle*>(QStringLiteral("nativeDetailControlStyle")))
        static_cast<SettingsControlStyle*>(style)->applyTheme(colors);
    if (m_menu) {
        for (int row = 0; row < m_menu->count(); ++row)
            m_menu->item(row)->setIcon(sectionIcon(row, colors));
    }
    const auto updateButton = [this, colors](const char* name, SettingsGlyph glyph) {
        if (auto* button = findChild<QToolButton*>(QLatin1String(name)))
            button->setIcon(settingsIcon(glyph, QColor(colors.muted)));
    };
    updateButton("settingsMinimize", SettingsGlyph::Minimize);
    updateButton("settingsMaximize", isMaximized() ? SettingsGlyph::Restore : SettingsGlyph::Maximize);
    updateButton("settingsClose", SettingsGlyph::Close);
    update();
}

bool SettingsDialog::event(QEvent* e)
{
    if (e->type() == QEvent::ThemeChange)
        applyThemeQss();     // 配置页拨完那颗单选，这扇已经开着的窗当场就要变脸
    if (e->type() == QEvent::WindowStateChange) {
        if (auto* button = findChild<QToolButton*>(QStringLiteral("settingsMaximize"))) {
            const auto label = isMaximized() ? QStringLiteral("还原") : QStringLiteral("最大化");
            button->setToolTip(label);
            button->setAccessibleName(label);
        }
        applyThemeQss();
    }
    return QDialog::event(e);
}

SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint
                       | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint)
{
    setWindowTitle(BuildChannel::displayName() + QStringLiteral(" 设置"));
    setObjectName(QStringLiteral("hotaruSettings"));
    setAttribute(Qt::WA_TranslucentBackground);
    resize(1140, 790);
    auto* root = new QVBoxLayout(this);
    constexpr int edge = SettingsFrame::kBorder;
    root->setContentsMargins(edge, edge, edge, edge);
    root->setSpacing(0);
    auto* titleBar = new QFrame(this);
    titleBar->setObjectName(QStringLiteral("settingsTitleBar"));
    titleBar->setFixedHeight(kTitleHeight);
    auto* titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(15, 0, 8, 0);
    titleLayout->setSpacing(7);
    auto* brandIcon = new QLabel(titleBar);
    brandIcon->setPixmap(Utils::appIcon().pixmap(QSize(19, 19), devicePixelRatioF()));
    titleLayout->addWidget(brandIcon);
    auto* caption = new QLabel(QStringLiteral("Hotaru 设置"), titleBar);
    caption->setObjectName(QStringLiteral("settingsWindowTitle"));
    titleLayout->addWidget(caption);
    titleLayout->addStretch(1);
    const auto addCaptionButton = [titleBar, titleLayout](const char* name, const QString& label) {
        auto* button = new QToolButton(titleBar);
        button->setObjectName(QLatin1String(name));
        button->setProperty("settingsCaptionButton", true);
        button->setAccessibleName(label);
        button->setToolTip(label);
        button->setFocusPolicy(Qt::TabFocus);
        button->setFixedSize(28, 26);
        button->setIconSize(QSize(14, 14));
        titleLayout->addWidget(button);
        return button;
    };
    connect(addCaptionButton("settingsMinimize", QStringLiteral("最小化")),
            &QToolButton::clicked, this, &QWidget::showMinimized);
    connect(addCaptionButton("settingsMaximize", QStringLiteral("最大化")),
            &QToolButton::clicked, this, [this] { isMaximized() ? showNormal() : showMaximized(); });
    connect(addCaptionButton("settingsClose", QStringLiteral("关闭")),
            &QToolButton::clicked, this, &QWidget::close);
    root->addWidget(titleBar);

    auto* rail = new QFrame(this);
    rail->setObjectName(QStringLiteral("settingsRail"));
    rail->setFixedWidth(190);
    m_menu = new SettingsNavigation(rail);
    m_menu->setObjectName("menuList");
    m_menu->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_menu->setIconSize(QSize(18, 18));
    m_menu->setSpacing(5);              // 条目之间留缝，背景块不再首尾相接
    m_menu->setUniformItemSizes(true);

    const QStringList names = {QStringLiteral("快捷搜索"), QStringLiteral("插件"),
                               QStringLiteral("配置"), QStringLiteral("关于")};
    for (int row = 0; row < names.size(); ++row) {
        auto* it = new QListWidgetItem(names.at(row));
        it->setSizeHint(QSize(0, 41));   // 宽度由视口决定，避免横向滚动。
        m_menu->addItem(it);
    }

    m_stack = new QStackedWidget(this);
    for (int i = 0; i < 4; ++i)
        m_stack->addWidget(new QWidget(m_stack)); // 占位，选中时懒加载

    auto* lay = new QHBoxLayout;
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    auto* menuCol = new QVBoxLayout(rail);
    menuCol->setContentsMargins(11, 18, 11, 10);
    menuCol->setSpacing(6);
    menuCol->addWidget(m_menu, 1);
    buildBusyRow();
    menuCol->addWidget(m_busy);
    lay->addWidget(rail);
    lay->addWidget(m_stack, 1);
    root->addLayout(lay, 1);
    applyThemeQss();

    connect(m_menu, &QListWidget::currentRowChanged, this, &SettingsDialog::ensurePage);
    connect(m_stack, &QStackedWidget::currentChanged, this, [this] { update(); });
    m_menu->setCurrentRow(0);
    m_menu->setFocus(Qt::OtherFocusReason);

}

// 菜单左下角的加载标志：后台有静默安装在跑才显示，点开「静默安装日志」看台（在那里取消）。
// 装完自己消失，不用用户去猜现在到底有没有在跑。
void SettingsDialog::buildBusyRow()
{
    m_busy = new QToolButton(this);
    m_busy->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_busy->setAutoRaise(true);
    m_busy->setCursor(Qt::PointingHandCursor);
    m_busy->setFixedHeight(28);
    Theme::bind(m_busy,
        "QToolButton{background:transparent;border:none;color:@infoText;font-size:8.5pt;"
        "text-align:left;padding:2px 6px;}"
        "QToolButton:hover{background:@hoverSoft;border-radius:6px;}");
    m_busy->setVisible(false);
    connect(m_busy, &QToolButton::clicked, this, [] { SilentInstallLog::showLog(); });

    m_busyTimer = new QTimer(this);
    m_busyTimer->setInterval(140);
    connect(m_busyTimer, &QTimer::timeout, this, &SettingsDialog::refreshBusy);
    m_busyTimer->start();
}

void SettingsDialog::refreshBusy()
{
    const int n = SilentInstallLog::busyCount();
    if (n <= 0) {
        if (m_busy->isVisible())
            m_busy->setVisible(false);
        return;
    }
    m_busyFrame = (m_busyFrame + 1) % 8;
    m_busy->setIcon(spinnerIcon(m_busyFrame));
    const QString text = QString("正在安装 %1 项…").arg(n);
    if (m_busy->text() != text)
        m_busy->setText(text);
    m_busy->setToolTip("后台静默安装进行中，点击打开安装日志（可取消安装）");
    m_busy->setVisible(true);
}

void SettingsDialog::ensurePage(int index)
{
    if (index < 0 || index >= 4)
        return;
    if (m_built[index]) {
        m_stack->setCurrentIndex(index);
        return;
    }
    m_built[index] = true;
    QWidget* page = nullptr;
    switch (index) {
    case 0: page = new WebsitesPage(this); break;
    case 1: page = buildPluginHome(); break;
    case 2: {
        auto* configPage = new ConfigPage(this);
        connect(configPage, &ConfigPage::rescanRequested, this, &SettingsDialog::rescanRequested);
        connect(configPage, &ConfigPage::hotkeyChanged, this, &SettingsDialog::hotkeyChanged);
        connect(configPage, &ConfigPage::hotkeyCaptureActive, this, &SettingsDialog::hotkeyCaptureActive);
        page = configPage;
        break;
    }
    case 3: {
        auto* scroll = new QScrollArea(this);
        scroll->setObjectName(QStringLiteral("settingsPageScroll"));
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setWidget(new AboutPage);
        page = scroll;
        break;
    }
    }
    if (index != 2) {
        page->setStyleSheet(SettingsPalette::current().expand(kSettingsQss) + Theme::menuStyle());
    }
    QWidget* old = m_stack->widget(index);
    m_stack->removeWidget(old);
    delete old;
    m_stack->insertWidget(index, page);
    m_stack->setCurrentIndex(index);
}

QWidget* SettingsDialog::buildPluginHome()
{
    m_pluginStack = new QStackedWidget(this);
    buildPluginPage(m_pluginStack);
    return m_pluginStack;
}

void SettingsDialog::openPluginRepository()
{
    auto* dialog = new QDialog(this, Qt::Dialog | Qt::WindowCloseButtonHint);
    dialog->setObjectName(QStringLiteral("pluginRepositoryDialog"));
    dialog->setWindowTitle(QStringLiteral("Hotaru 官方插件仓库"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(820, 610);

    auto* client = new HotaruPluginRepository::Client(dialog);
    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(0, 0, 0, 16);
    layout->addWidget(buildPluginPage(dialog, client), 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    layout->addWidget(buttons, 0, Qt::AlignHCenter);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(dialog, &QDialog::finished, client, [client] {
        if (client->busy()) client->cancel();
    });
    Theme::bindModal(dialog, true);
    dialog->open();
    client->refresh();
}

QWidget* SettingsDialog::buildPluginPage(QWidget* parent, HotaruPluginRepository::Client* client)
{
    auto* home = new QWidget(parent);
    home->setObjectName(QStringLiteral("pluginHome"));
    if (client)
        Theme::bindSettings(home, kSettingsQss);
    auto* layout = new QVBoxLayout(home);
    layout->setContentsMargins(30, 20, 30, 20);
    layout->setSpacing(13);

    auto* header = new QHBoxLayout;
    auto* titles = new QVBoxLayout;
    titles->setSpacing(4);
    auto* title = new QLabel(client ? QStringLiteral("官方插件仓库") : QStringLiteral("插件"), home);
    title->setObjectName(QStringLiteral("pageTitle"));
    titles->addWidget(title);
    auto* intro = new QLabel(client ? QStringLiteral("浏览适用于当前版本的基础服务和功能插件。")
                                   : QStringLiteral("管理已安装的 Hotaru 插件。"), home);
    intro->setObjectName(QStringLiteral("muted"));
    intro->setWordWrap(true);
    titles->addWidget(intro);
    header->addLayout(titles, 1);
    QPushButton* refreshCatalog = nullptr;
    QPushButton* cancelDownload = nullptr;
    QPushButton* manualDownload = nullptr;
    if (client) {
        manualDownload = new QPushButton(QStringLiteral("手动下载插件"), home);
        manualDownload->setObjectName(QStringLiteral("pluginManualDownload"));
        manualDownload->setToolTip(QStringLiteral("在 GitHub 下载 ZIP，再回到插件页选择「导入本地插件」。"));
        header->addWidget(manualDownload);
        connect(manualDownload, &QPushButton::clicked, home, [] {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/EienSakura/Hotaru-Plugins#readme")));
        });
        refreshCatalog = new QPushButton(QStringLiteral("刷新目录"), home);
        cancelDownload = new QPushButton(QStringLiteral("取消"), home);
        header->addWidget(refreshCatalog);
        header->addWidget(cancelDownload);
        connect(refreshCatalog, &QPushButton::clicked, client, [client] { client->refresh(true); });
        connect(cancelDownload, &QPushButton::clicked, client, &HotaruPluginRepository::Client::cancel);
    } else {
        auto* repository = new QPushButton(QStringLiteral("官方仓库"), home);
        repository->setObjectName(QStringLiteral("openPluginRepository"));
        repository->setProperty("settingsPrimary", true);
        auto* importButton = new QPushButton(QStringLiteral("导入本地插件"), home);
        auto* compatibility = new QPushButton(QStringLiteral("ltool 兼容插件"), home);
        header->addWidget(compatibility);
        header->addWidget(importButton);
        header->addWidget(repository);
        connect(repository, &QPushButton::clicked, this, &SettingsDialog::openPluginRepository);
        connect(compatibility, &QPushButton::clicked, this, [this] { openCompatibilityPlugins(); });
        connect(importButton, &QPushButton::clicked, home, [this] {
            const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入本地插件"), QString(), QStringLiteral("插件包 (*.dll *.zip)"));
            if (path.isEmpty()) return;
            QString error;
            const auto confirm = [this](const QString& message) {
                QMessageBox box(QMessageBox::Question, QStringLiteral("确认替换插件"), message,
                    QMessageBox::Ok | QMessageBox::Cancel, this);
                box.setTextFormat(Qt::PlainText);
                box.setDefaultButton(QMessageBox::Cancel);
                return box.exec() == QMessageBox::Ok;
            };
            if (!HotaruNativeHost::importPackage(path, error, confirm)) {
                if (!error.isEmpty()) QMessageBox::warning(this, QStringLiteral("导入失败"), error);
            } else {
                QMessageBox::information(this, QStringLiteral("导入完成"),
                    QStringLiteral("插件已导入。重启 Hotaru 后可管理新版本。"));
            }
        });
    }
    layout->addLayout(header);

    auto* hint = new QLabel(home);
    hint->setTextFormat(Qt::PlainText);
    hint->setObjectName(QStringLiteral("pluginSummary"));
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto* sectionLabel = new QLabel(client ? QStringLiteral("目录") : QStringLiteral("已安装"), home);
    sectionLabel->setObjectName(QStringLiteral("settingsRowTitle"));
    layout->addWidget(sectionLabel);
    auto* tabs = new QTabBar(home);
    tabs->setObjectName(QStringLiteral("pluginCategoryTabs"));
    tabs->setExpanding(false);
    tabs->setDrawBase(false);
    tabs->addTab(QStringLiteral("全部"));
    tabs->addTab(QStringLiteral("基础服务"));
    tabs->addTab(QStringLiteral("功能插件"));
    layout->addWidget(tabs, 0, Qt::AlignLeft);

    struct PluginGroup {
        QWidget* section;
        QToolButton* toggle;
        QWidget* content;
        QGridLayout* grid;
    };
    QWidget* groupsHost = home;
    auto* groupsLayout = layout;
    if (client) {
        auto* scroll = new QScrollArea(home);
        scroll->setObjectName(QStringLiteral("pluginRepositoryScroll"));
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        groupsHost = new QWidget(scroll);
        groupsHost->setObjectName(QStringLiteral("pluginRepositoryGroups"));
        groupsLayout = new QVBoxLayout(groupsHost);
        groupsLayout->setContentsMargins(0, 0, 0, 0);
        groupsLayout->setSpacing(layout->spacing());
        scroll->setWidget(groupsHost);
        layout->addWidget(scroll, 1);
    }
    const auto makeGroup = [groupsHost, groupsLayout, tabs](int tab) {
        auto* section = new QWidget(groupsHost);
        auto* sectionLayout = new QVBoxLayout(section);
        sectionLayout->setContentsMargins(0, 0, 0, 0);
        sectionLayout->setSpacing(8);
        groupsLayout->addWidget(section);

        auto* toggle = new QToolButton(section);
        toggle->setObjectName(QStringLiteral("pluginGroupToggle"));
        toggle->setText(tabs->tabText(tab));
        toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        toggle->setCheckable(true);
        toggle->setChecked(true);
        toggle->setArrowType(Qt::DownArrow);
        sectionLayout->addWidget(toggle);

        auto* content = new QWidget(section);
        auto* grid = new QGridLayout(content);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(10);
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);
        sectionLayout->addWidget(content);
        return PluginGroup{section, toggle, content, grid};
    };
    const auto services = makeGroup(1);
    const auto features = makeGroup(2);

    const auto makeCard = [this, client, parent](const HotaruNativeHost::PluginInfo& info, QWidget* parent,
                                         const HotaruPluginRepository::Entry* offered = nullptr) {
        namespace Repo = HotaruPluginRepository;
        const auto id = info.id;
        auto* card = new QFrame(parent);
        card->setObjectName(QStringLiteral("pluginCard"));
        card->setFocusPolicy(Qt::StrongFocus);
        auto* body = new QVBoxLayout(card);
        body->setContentsMargins(14, 12, 14, 12);
        body->setSpacing(7);
        card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

        auto* heading = new QHBoxLayout;
        heading->addWidget(new SettingsIconTile(SettingsGlyph::Plug, card));
        auto* name = new QLabel(info.name, card);
        name->setTextFormat(Qt::PlainText);
        name->setWordWrap(true);
        name->setObjectName(QStringLiteral("settingsRowTitle"));
        heading->addWidget(name, 0, Qt::AlignVCenter);
        if (!info.version.isEmpty()) {
            auto* version = new QLabel(info.version, card);
            version->setTextFormat(Qt::PlainText);
            version->setObjectName(QStringLiteral("pluginVersionBadge"));
            heading->addWidget(version, 0, Qt::AlignVCenter);
        }
        if (offered || info.verified || info.source == "bundled") {
            auto* badge = new QLabel(QStringLiteral("Hotaru直属"), card);
            badge->setObjectName(QStringLiteral("hotaruNativeBadge"));
            heading->addWidget(badge, 0, Qt::AlignVCenter);
        }
        heading->addStretch();
        body->addLayout(heading);
        auto* status = new QLabel(card);
        status->setObjectName(QStringLiteral("pluginStatus"));
        status->setTextFormat(Qt::PlainText);
        status->setWordWrap(true);
        auto* description = new QLabel(info.description, card);
        description->setTextFormat(Qt::PlainText);
        description->setObjectName(QStringLiteral("muted"));
        description->setWordWrap(true);
        body->addWidget(description);
        body->addWidget(status);
        body->addStretch();
        auto* actions = new QHBoxLayout;
        if (offered) {
            const auto entry = *offered;
            status->setText(info.preview ? QStringLiteral("来源已验证 · 开发预览") : QStringLiteral("来源已验证"));
            auto* action = new QPushButton(info.installed ? QStringLiteral("更新／替换") : QStringLiteral("安装"), card);
            action->setObjectName(QStringLiteral("pluginCatalogInstall"));
            action->setProperty("settingsPrimary", true);
            actions->addStretch();
            actions->addWidget(action);
            body->addLayout(actions);
            const auto current = Repo::desired().value(id);
            const bool same = current.source == "official" && Repo::sameVersion(current.entry.version, entry.version);
            action->setEnabled(!same && !client->busy() && client->catalog().expires > QDateTime::currentDateTimeUtc());
            if (same) action->setText(QStringLiteral("已安装"));
            auto* dialog = parent;
            connect(action, &QPushButton::clicked, dialog, [dialog, id, client] {
                Repo::InstallPlan plan;
                const auto catalog = client->catalog();
                QString error;
                const auto inventory = Repo::desired();
                QStringList bundled;
                for (const auto& item : HotaruNativeHost::plugins())
                    if (item.source == "bundled") bundled << item.id;
                if (!Repo::planInstall(id, catalog, inventory, BuildChannel::development, plan, error, bundled)) {
                    QMessageBox::warning(dialog, QStringLiteral("无法安装"), error);
                    return;
                }
                QStringList lines;
                for (const auto& e : plan.downloads) {
                    const auto old = inventory.value(e.id);
                    QString installedFrom;
                    if (!old.source.isEmpty())
                        installedFrom = old.entry.version + (old.source == "official" ? QStringLiteral("（官方仓库）") : QStringLiteral("（本地导入）"));
                    else
                        for (const auto& info : HotaruNativeHost::plugins())
                            if (info.id == e.id && info.source == "bundled") installedFrom = QStringLiteral("开发预装");
                    lines << (installedFrom.isEmpty() ? QStringLiteral("%1 %2").arg(e.name, e.version)
                        : QStringLiteral("%1：%2 → %3（官方仓库）").arg(e.name, installedFrom, e.version));
                }
                QMessageBox box(QMessageBox::Question, QStringLiteral("确认安装插件"),
                    lines.join(QLatin1Char('\n')) + QStringLiteral("\n下载大小：") + PluginStore::humanSize(plan.totalBytes),
                    QMessageBox::Ok | QMessageBox::Cancel, dialog);
                box.setTextFormat(Qt::PlainText);
                box.setDefaultButton(QMessageBox::Cancel);
                if (box.exec() == QMessageBox::Ok) client->install(plan, true);
            });
        } else {
            auto* enabled = new QCheckBox(QStringLiteral("启用"), card);
            auto* settings = new QPushButton(QStringLiteral("打开设置"), card);
            auto* remove = new QPushButton(QStringLiteral("卸载"), card);
            actions->addWidget(enabled);
            actions->addStretch();
            actions->addWidget(settings);
            if (info.removable) actions->addWidget(remove);
            else remove->hide();
            body->addLayout(actions);
            const auto refresh = [id, status, enabled, settings, remove] {
                HotaruNativeHost::PluginInfo current;
                for (const auto& candidate : HotaruNativeHost::plugins())
                    if (candidate.id == id) { current = candidate; break; }
                auto label = current.state.isEmpty() ? QStringLiteral("未安装") : current.state;
                if (current.source == "local") label += QStringLiteral(" · 本地导入");
                else if (current.source == "official") label += QStringLiteral(" · 来源已验证");
                if (current.preview && current.installed) label += QStringLiteral(" · 开发预览");
                status->setText(label);
                const QSignalBlocker blocker(enabled);
                enabled->setChecked(current.enabled);
                enabled->setEnabled(current.loaded && !current.pending && current.dependencyReady);
                const bool needs = !current.dependency.isEmpty() && !current.dependencyReady;
                settings->setText(needs ? QStringLiteral("打开前置设置") : QStringLiteral("打开设置"));
                settings->setEnabled(!current.pending && (needs ? !HotaruNativeHost::state(current.dependency).isEmpty()
                    : current.loaded && (current.enabled || id == kHotaruLanCoreId || id == kHotaruLanClipboardId || id == kHotaruLanFilesId)));
                remove->setEnabled(current.installed && current.state != QStringLiteral("待卸载"));
            };
            refresh();
            HotaruNativeHost::subscribeChanged(card, refresh);
            connect(enabled, &QCheckBox::toggled, card, [this, id, enabled, refresh](bool on) {
                QString error;
                if (!HotaruNativeHost::setEnabled(id, on, error)) {
                    refresh();
                    QMessageBox::warning(this, QStringLiteral("插件设置"), error);
                }
            });
            connect(settings, &QPushButton::clicked, this, [this, id, dependency = info.dependency] {
                openNativePlugin(!dependency.isEmpty() && !HotaruNativeHost::enabled(dependency) ? dependency : id);
            });
            connect(remove, &QPushButton::clicked, this, [this, id] {
                QStringList affected;
                QString error;
                if (!HotaruNativeHost::uninstall(id, affected, false, error)) {
                    QMessageBox::warning(this, QStringLiteral("无法卸载"), error); return;
                }
                const auto choice = QMessageBox::question(this, QStringLiteral("卸载插件"),
                    QStringLiteral("以下插件将卸载，重启后完成：\n") + affected.join(QLatin1Char('\n')),
                    QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
                if (choice == QMessageBox::Ok && !HotaruNativeHost::uninstall(id, affected, true, error))
                    QMessageBox::warning(this, QStringLiteral("无法卸载"), error);
            });
        }
        return card;
    };
    const auto makeCompatibilityCard = [this](const PluginStore::Row& row, QWidget* parent) {
        auto* card = new QFrame(parent);
        card->setObjectName(QStringLiteral("pluginCard"));
        card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        auto* body = new QVBoxLayout(card);
        body->setContentsMargins(12, 10, 12, 10);
        auto* title = new QLabel(row.remote.name, card);
        title->setTextFormat(Qt::PlainText);
        title->setWordWrap(true);
        body->addWidget(title);
        const auto file = row.remote.file;
        const bool staged = QFileInfo::exists(QDir(PluginHost::pluginsDir()).filePath(file + ".hotaru-local-stage"))
            || QFileInfo::exists(QDir(PluginHost::pluginsDir()).filePath(file + ".off.hotaru-local-stage"));
        const auto label = row.queuedRemove ? QStringLiteral("待卸载")
            : staged ? QStringLiteral("待重启") : PluginStore::stateWord(row.state);
        auto* status = new QLabel(QStringLiteral("ltool 兼容插件 · %1").arg(label), card);
        status->setTextFormat(Qt::PlainText);
        body->addWidget(status);
        auto* actions = new QHBoxLayout;
        actions->addStretch();
        auto* manage = new QPushButton(QStringLiteral("打开兼容管理"), card);
        actions->addWidget(manage);
        body->addLayout(actions);
        connect(manage, &QPushButton::clicked, this, [this] { openCompatibilityPlugins(); });
        return card;
    };
    const auto bindGroup = [home, tabs](const PluginGroup& group, int category) {
        const auto update = [group, category](int tab) {
            const bool showAll = tab == 0;
            group.section->setVisible(showAll || tab == category);
            group.toggle->setVisible(showAll);
            group.content->setVisible(!showAll || group.toggle->isChecked());
        };
        connect(tabs, &QTabBar::currentChanged, home, update);
        connect(group.toggle, &QToolButton::toggled, home, [group, tabs, update](bool expanded) {
            group.toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
            update(tabs->currentIndex());
        });
        update(tabs->currentIndex());
    };
    bindGroup(services, 1);
    bindGroup(features, 2);

    const auto rebuild = [=] {
        namespace Repo = HotaruPluginRepository;
        const auto clear = [](QGridLayout* grid) {
            while (auto* item = grid->takeAt(0)) {
                if (auto* w = item->widget()) { w->hide(); w->deleteLater(); }
                delete item;
            }
        };
        clear(services.grid);
        clear(features.grid);
        const auto append = [&](const HotaruNativeHost::PluginInfo& info, const Repo::Entry* e = nullptr) {
            const auto& group = (info.category == "service" ||
                (info.category.isEmpty() && info.dependency.isEmpty())) ? services : features;
            const int i = group.grid->count();
            group.grid->addWidget(makeCard(info, group.content, e), i / 2, i % 2);
        };
        const auto local = HotaruNativeHost::plugins();
        int count = 0;
        if (client) {
            const auto catalog = client->catalog();
            for (const auto& e : Repo::catalogListings(catalog, BuildChannel::development)) {
                HotaruNativeHost::PluginInfo info;
                info.id = e.id; info.name = e.name; info.description = e.description;
                info.version = e.version; info.category = e.category; info.preview = e.channel == "preview";
                for (const auto& installed : local)
                    if (installed.id == e.id) info.installed = installed.installed;
                append(info, &e);
                ++count;
            }
        } else {
            for (const auto& info : local)
                if (info.installed) { append(info); ++count; }
            for (const auto& row : PluginStore::localRows()) {
                const auto i = features.grid->count();
                features.grid->addWidget(makeCompatibilityCard(row, features.content), i / 2, i % 2);
                ++count;
            }
        }
        if (client) {
            refreshCatalog->setEnabled(!client->busy());
            cancelDownload->setVisible(client->busy());
        }
        const auto status = client ? client->status() : Repo::activationError();
        const auto empty = client ? QStringLiteral("暂无适用于当前版本的插件") : QStringLiteral("暂无已安装插件");
        auto message = count ? status : status.isEmpty() ? empty : empty + QStringLiteral(" · ") + status;
        if (client) {
            const auto catalog = client->catalog();
            const bool unavailable = !client->busy()
                && (catalog.sequence <= 0 || catalog.expires <= QDateTime::currentDateTimeUtc());
            manualDownload->setVisible(unavailable);
            if (unavailable)
                message += QStringLiteral("\n暂无有效目录。可手动下载 ZIP，再到插件页选择「导入本地插件」。");
        }
        hint->setText(message);
        hint->setVisible(!message.isEmpty());
        services.toggle->setEnabled(services.grid->count() > 0);
        features.toggle->setEnabled(features.grid->count() > 0);
    };
    if (client) {
        connect(client, &HotaruPluginRepository::Client::changed, home, rebuild);
        connect(client, &HotaruPluginRepository::Client::installed, home, [] { HotaruNativeHost::inventoryChanged(); });
        connect(client, &HotaruPluginRepository::Client::progress, hint, [hint](qint64 bytes, qint64 total) {
            hint->setText(QStringLiteral("正在下载：%1 / %2")
                .arg(PluginStore::humanSize(bytes), PluginStore::humanSize(total)));
        });
    } else {
        m_pluginStack->addWidget(home);
        connect(m_pluginStack, &QStackedWidget::currentChanged, home, [rebuild](int i) { if (!i) rebuild(); });
        HotaruNativeHost::subscribeInventory(home, rebuild);
    }
    groupsLayout->addStretch();
    rebuild();
    return home;
}

bool SettingsDialog::openNativePlugin(const QString& id, const QByteArray& target)
{
    openAt(1);
    ensurePage(1);
    auto* panel = new QWidget(m_pluginStack);
    panel->setObjectName(QStringLiteral("settingsNativeDetail"));
    panel->setAttribute(Qt::WA_StyledBackground);
    auto* page = HotaruNativeHost::createPage(id, panel, target);
    if (!page) {
        delete panel;
        return false;
    }
    if (auto* previous = m_pluginStack->currentWidget(); previous && previous != m_pluginStack->widget(0) &&
        (!m_toolsPage || previous != m_toolsPage->parentWidget())) {
        m_pluginStack->removeWidget(previous);
        previous->deleteLater();
    }
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto* toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(30, 14, 30, 0);
    auto* back = new QPushButton(QStringLiteral("返回插件首页"), panel);
    back->setObjectName(QStringLiteral("nativeDetailBack"));
    toolbar->addWidget(back);
    toolbar->addStretch();
    layout->addLayout(toolbar);
    layout->addWidget(page, 1);
    if (id == QLatin1String(kHotaruLanClipboardId)) {
        auto& store = ClipboardStore::instance();
        store.start();
        QWidget* history = nullptr;
        for (auto* frame : page->findChildren<QFrame*>()) {
            if (frame->property("hotaruClipboardHistorySlot").toBool() && qobject_cast<QVBoxLayout*>(frame->layout())) {
                history = frame;
                break;
            }
        }
        if (!history) {
            // 旧前置包没有预留历史栏；宿主仍按左右分栏显示。
            layout->removeWidget(page);
            auto* columns = new QHBoxLayout;
            columns->setContentsMargins(0, 0, 30, 0);
            columns->setSpacing(12);
            columns->addWidget(page, 1);
            history = new QFrame(panel);
            history->setObjectName(QStringLiteral("lanDetailCard"));
            auto* fallback = new QVBoxLayout(history);
            fallback->setContentsMargins(14, 12, 14, 12);
            fallback->addWidget(new QLabel(QStringLiteral("本机记录"), history));
            columns->addWidget(history, 1);
            layout->addLayout(columns, 1);
        }
        auto* form = qobject_cast<QVBoxLayout*>(history->layout());
        auto* record = new QCheckBox(QStringLiteral("在本机保留剪贴板历史"), history);
        record->setChecked(store.enabled());
        form->addWidget(record);
        auto* description = new QLabel(QStringLiteral("保存本机复制和从局域网收到的文本、图片。关闭记录后，仍能查看当前内容。"), history);
        description->setObjectName(QStringLiteral("muted"));
        description->setWordWrap(true);
        form->addWidget(description);
        auto* days = new QSpinBox(history);
        days->setRange(0, 3650);
        days->setSpecialValueText(QStringLiteral("不限天数"));
        days->setSuffix(QStringLiteral(" 天"));
        days->setValue(store.keepDays());
        auto* retention = new QHBoxLayout;
        auto* daysLabel = new QLabel(QStringLiteral("保留天数（最多 200 条）"), history);
        daysLabel->setWordWrap(true);
        daysLabel->setBuddy(days);
        retention->addWidget(daysLabel);
        retention->addWidget(days);
        retention->addStretch();
        form->addLayout(retention);
        auto* clear = new QPushButton(QStringLiteral("清空剪贴板历史"), history);
        form->addWidget(clear, 0, Qt::AlignLeft);
        form->addStretch();
        connect(record, &QCheckBox::toggled, history, [&store](bool on) { store.setEnabled(on); });
        connect(days, qOverload<int>(&QSpinBox::valueChanged), history, [&store](int value) { store.setKeepDays(value); });
        connect(clear, &QPushButton::clicked, history, [history, &store] {
            if (QMessageBox::question(history, QStringLiteral("清空历史"),
                QStringLiteral("删除本机保存的剪贴板记录和图片？"),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes)
                store.clear();
        });
    }
    connect(back, &QPushButton::clicked, this, [this, panel] {
        m_pluginStack->setCurrentIndex(0);
        m_pluginStack->removeWidget(panel);
        panel->deleteLater();
    });
    auto* style = new SettingsControlStyle(panel);
    style->setObjectName(QStringLiteral("nativeDetailControlStyle"));
    style->setAnimationsEnabled(false);
    style->applyTheme(SettingsPalette::current());
    for (auto* checkbox : panel->findChildren<QCheckBox*>())
        checkbox->setStyle(style);
    for (auto* spinner : panel->findChildren<QSpinBox*>())
        spinner->setStyle(style);
    m_pluginStack->addWidget(panel);
    m_pluginStack->setCurrentWidget(panel);
    return true;
}
ToolsPage* SettingsDialog::openCompatibilityPlugins()
{
    ensurePage(1);
    if (!m_toolsPage) {
        auto* panel = new QWidget(m_pluginStack);
        panel->setObjectName(QStringLiteral("settingsCompatibility"));
        auto* layout = new QVBoxLayout(panel);
        auto* back = new QPushButton(QStringLiteral("返回插件首页"), panel);
        layout->addWidget(back, 0, Qt::AlignLeft);
        connect(back, &QPushButton::clicked, this, [this] { m_pluginStack->setCurrentIndex(0); });
        m_toolsPage = new ToolsPage(panel);
        layout->addWidget(m_toolsPage, 1);
        m_pluginStack->addWidget(panel);
    }
    m_pluginStack->setCurrentWidget(m_toolsPage->parentWidget());
    return m_toolsPage;
}

void SettingsDialog::editQuickSearch(const QString& name)
{
    ensurePage(0);
    m_menu->setCurrentRow(0);
    if (auto* page = qobject_cast<WebsitesPage*>(m_stack->widget(0)))
        page->editEntryNamed(name);
}

// 工具都是 DLL 插件，所以这里只剩一条路：把设置页开到「工具」那一行，再展开那张卡。
// 返回 false = 那个 id 没加载起来（ToolsPage 里已经写了一条带原因的日志）。
bool SettingsDialog::showTool(const QString& id, const QString& text)
{
    m_menu->setCurrentRow(1);
    return openCompatibilityPlugins()->showTool(id, text);
}

void SettingsDialog::showEvent(QShowEvent* e)
{
    QDialog::showEvent(e);
    if (QGuiApplication::platformName() != QLatin1String("windows"))
        return;
    HWND h = reinterpret_cast<HWND>(winId());
    // 原生样式负责系统移动、缩放和窗口菜单，客户区由 WM_NCCALCSIZE 扩展到边缘。
    const LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    constexpr LONG_PTR frame = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    if ((style & frame) != frame) {
        SetWindowLongPtrW(h, GWL_STYLE, style | frame);
        SetWindowPos(h, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    if (testAttribute(Qt::WA_DontShowOnScreen))
        return;
    // 明确取消置顶并激活展示在前台
    SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(h);
}

void SettingsDialog::paintEvent(QPaintEvent*)
{
    const auto* title = findChild<QFrame*>(QStringLiteral("settingsTitleBar"), Qt::FindDirectChildrenOnly);
    const auto* rail = findChild<QFrame*>(QStringLiteral("settingsRail"), Qt::FindDirectChildrenOnly);
    QRect footer;
    if (const auto* page = m_stack->currentWidget()) {
        if (const auto* status = page->findChild<QFrame*>(QStringLiteral("settingsFooter")))
            footer = QRect(status->mapTo(this, QPoint{}), status->size());
    }
    QPainter painter(this);
    SettingsFrame::paint(painter, rect(), isMaximized() || isFullScreen() ? 0 : kFrameRadius,
                         title->geometry().bottom() + 1, rail->geometry(), footer);
}

void SettingsDialog::resizeEvent(QResizeEvent* e)
{
    QDialog::resizeEvent(e);
    update();
}

bool SettingsDialog::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    if (eventType != QByteArrayLiteral("windows_generic_MSG") || !message || !result)
        return QDialog::nativeEvent(eventType, message, result);
    const auto* msg = static_cast<const MSG*>(message);
    if (msg->message == WM_NCCALCSIZE) {
        if (msg->wParam && IsZoomed(msg->hwnd)) {
            auto* area = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);
            MONITORINFO monitor{sizeof(MONITORINFO)};
            if (GetMonitorInfoW(MonitorFromRect(&area->rgrc[0], MONITOR_DEFAULTTONEAREST), &monitor))
                area->rgrc[0] = monitor.rcWork;
        }
        *result = 0;
        return true;
    }
    if (msg->message != WM_NCHITTEST)
        return QDialog::nativeEvent(eventType, message, result);

    POINT native{static_cast<short>(LOWORD(msg->lParam)), static_cast<short>(HIWORD(msg->lParam))};
    ScreenToClient(msg->hwnd, &native);
    const qreal scale = devicePixelRatioF();
    const QPoint pos(qFloor(native.x / scale), qFloor(native.y / scale));
    const auto* title = findChild<QFrame*>(QStringLiteral("settingsTitleBar"), Qt::FindDirectChildrenOnly);
    const bool overTitle = title && title->geometry().contains(pos);
    if (overTitle && qobject_cast<QToolButton*>(title->childAt(pos - title->pos()))) {
        *result = HTCLIENT;
        return true;
    }
    if (!isMaximized() && !isFullScreen()) {
        constexpr int edge = 6;
        const int col = pos.x() < edge ? 0 : pos.x() >= width() - edge ? 2 : 1;
        const int row = pos.y() < edge ? 0 : pos.y() >= height() - edge ? 2 : 1;
        constexpr int hits[3][3] = {{HTTOPLEFT, HTTOP, HTTOPRIGHT},
                                    {HTLEFT, HTCLIENT, HTRIGHT},
                                    {HTBOTTOMLEFT, HTBOTTOM, HTBOTTOMRIGHT}};
        if (hits[row][col] != HTCLIENT) {
            *result = hits[row][col];
            return true;
        }
    }
    *result = overTitle ? HTCAPTION : HTCLIENT;
    return true;
}

void SettingsDialog::reject()
{
    // 复用 Qt 的关窗流程，避免 closeEvent 进入 reject 后再次调用 close。
    QDialog::reject();
}

void SettingsDialog::openAt(int menuRow, int tab, const QString& toolId)
{
    m_menu->setCurrentRow(qBound(0, menuRow, m_menu->count() - 1));
    // 顺序是"先展开卡、再挑 tab"：软件那两张页搬进插件之后，那个 QTabWidget 在卡片里面，
    // 反过来写就永远挑不到。
    if (menuRow == 1 && (!toolId.isEmpty() || tab >= 0)) {
        auto* tools = openCompatibilityPlugins();
        if (!toolId.isEmpty())
            tools->showTool(toolId);
    }
    if (tab >= 0) {
        QWidget* host = m_stack->widget(qBound(0, menuRow, m_stack->count() - 1));
        if (auto* tabs = host ? host->findChild<QTabWidget*>() : nullptr)
            tabs->setCurrentIndex(qBound(0, tab, tabs->count() - 1));
    }
}

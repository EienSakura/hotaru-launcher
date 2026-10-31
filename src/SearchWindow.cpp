#include "SearchWindow.h"
#include "platform/ProcInfo.h"
#include "platform/Launch.h"
#include "platform/Paths.h"

#include "CaptureHistory.h"
#include "ClipboardStore.h"
#include "DevEnv.h"
#include "EnvPath.h"
#include "FileTools.h"
#include "FileIndexer.h"
#include "SettingsDialog.h"
#include "LauncherTheme.h"
#include "SysTools.h"
#include "PluginHost.h"
#include "PinnedApps.h"
#include "LauncherGrid.h"
#include "LauncherSkin.h"
#include <QFileDialog>
#include <QWindow>
#include <QInputDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QContextMenuEvent>
#include <QSignalBlocker>
#include "Pinyin.h"
#include "Utils.h"
#include "WebShortcuts.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QApplication>
#include <QEventLoop>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFontMetrics>
#include <QFutureWatcher>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScreen>
#include <QSettings>
#include <QScrollBar>
#include <QScrollArea>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

#include <windows.h>
#include <dwmapi.h>

namespace {

enum class ActionGlyph { Gear, Folder, Pin, Check };
void drawGlyph(QPainter& p,const QRectF& box,ActionGlyph glyph)
{
    p.save();p.setRenderHint(QPainter::Antialiasing);p.translate(box.center());p.scale(box.width()/24,box.height()/24);
    p.setPen(QPen(Theme::launcherColor("text"),1.6,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p.setBrush(Qt::NoBrush);
    if (glyph==ActionGlyph::Gear) {
        p.drawEllipse(QPointF(),6.8,6.8);p.drawEllipse(QPointF(),2.6,2.6);
        for (int i=0;i<8;++i) {p.drawLine(QPointF(0,-7),QPointF(0,-9));p.rotate(45);}
    } else if (glyph==ActionGlyph::Folder) {
        QPainterPath path;path.moveTo(-9,-5);path.lineTo(-3,-5);path.lineTo(-1,-2);path.lineTo(9,-2);
        path.lineTo(9,7);path.lineTo(-9,7);path.closeSubpath();p.drawPath(path);
        p.drawLine(QPointF(0,-7),QPointF(7,-7));p.drawLine(QPointF(7,-7),QPointF(7,-4));
    } else if (glyph==ActionGlyph::Pin) {
        QPainterPath path;path.moveTo(-5,-8);path.lineTo(5,-8);path.lineTo(4,-1);path.lineTo(7,2);
        path.lineTo(-7,2);path.lineTo(-4,-1);path.closeSubpath();p.drawPath(path);p.drawLine(QPointF(0,2),QPointF(0,9));
    } else {p.drawLine(QPointF(-7,0),QPointF(-2,5));p.drawLine(QPointF(-2,5),QPointF(8,-6));}
    p.restore();
}
class GearButton : public QToolButton {
public:
    using QToolButton::QToolButton;
    void paintEvent(QPaintEvent* event) override {
        QToolButton::paintEvent(event);QPainter p(this);
        // 分层透明窗口也保留完整命中面，不能只让齿轮线条接受点击。
        p.fillRect(rect(),QColor(0,0,0,1));
        drawGlyph(p,QRectF((width()-15)/2.,(height()-15)/2.,15,15),ActionGlyph::Gear);
    }
};
class PageBar : public QWidget {
public:
    explicit PageBar(QWidget* parent):QWidget(parent) {setObjectName("hotaruPages");setMouseTracking(true);setCursor(Qt::PointingHandCursor);}
    std::function<void(int,bool)> request;
    std::function<void()> removePage;
    int page=0,count=1;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.fillRect(rect(),QColor(0,0,0,1));
        QFont f=font();f.setPointSizeF(8);p.setFont(f);p.setPen(Theme::launcherColor("muted"));
        p.drawText(QRect(26,0,68,height()),Qt::AlignCenter,QStringLiteral("%1 / %2").arg(page+1,2,10,QLatin1Char('0')).arg(count,2,10,QLatin1Char('0')));
        const int y=height()/2;
        for (int direction:{-1,1}) {
            QColor c=Theme::launcherColor("text");if ((direction<0 && !page)||(direction>0 && page==count-1)) c.setAlpha(65);
            p.setPen(QPen(c,1.3,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
            const int x=direction<0?13:105;
            p.drawLine(x-direction*2,y-4,x+direction*2,y);p.drawLine(x+direction*2,y,x-direction*2,y+4);
        }
        p.setPen(QPen(Theme::launcherColor("muted"),1.2,Qt::SolidLine,Qt::RoundCap));p.drawLine(127,y,135,y);p.drawLine(131,y-4,131,y+4);
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button()!=Qt::LeftButton || !rect().contains(e->pos()) || !request) return;
        if (e->pos().x()<26 && page>0) request(page-1,false);
        else if (e->pos().x()>=94 && e->pos().x()<118 && page<count-1) request(page+1,false);
        else if (e->pos().x()>=118) request(count,true);
    }
    void contextMenuEvent(QContextMenuEvent* e) override {
        QMenu menu(this);Theme::bindMenu(&menu);
        auto* remove=menu.addAction(QStringLiteral("删除当前页"));remove->setEnabled(count>1 && bool(removePage));
        if (menu.exec(e->globalPos())==remove && removePage) removePage();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        setToolTip(e->pos().x()>=118?QStringLiteral("新建空白页（可右键添加或跨页拖入）"):QStringLiteral("翻页 · 滚轮或 PageUp / PageDown；拖动至网格左右边缘停留可跨页"));
    }
};
QRect actionRect(const QRect& row,bool pin) {return {row.right()-(pin?43:83),row.center().y()-18,36,36};}
bool hasResultActions(const SearchResult& r) {return r.kind==SearchResult::App || r.kind==SearchResult::File || r.kind==SearchResult::PinnedApp;}

constexpr int kWindowWidth = 980;
constexpr int kRowHeight = 54;
constexpr int kSectionHeight = 28;
constexpr int kMaxVisibleRows = 12;   // 空查询的常用面板有 5 节，9 行只够看到两节

inline QIcon letterIcon(const QString& text, const QColor& color)
{
    return Utils::letterIcon(text, color);
}

QColor colorForKind(const QString& key)
{
    static const QColor palette[] = {
        QColor(63, 106, 224), QColor(46, 160, 88), QColor(214, 128, 15),
        QColor(136, 87, 229), QColor(214, 69, 69), QColor(31, 141, 160),
    };
    const size_t h = std::hash<QString>{}(key);
    return palette[h % (sizeof(palette) / sizeof(palette[0]))];
}

class ResultDelegate : public QStyledItemDelegate {
public:
    explicit ResultDelegate(SearchWindow* w) : QStyledItemDelegate(w), m_w(w) {}

    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& index) const override
    {
        const int i = index.data(Qt::UserRole).toInt();
        p->save();
        p->setRenderHint(QPainter::Antialiasing);

        if (i < 0) { // 分组标题
            QFont f = opt.font;
            f.setPointSizeF(8.5);
            f.setBold(true);
            p->setFont(f);
            p->setPen(Theme::launcherColor("sectionText"));
            p->drawText(opt.rect.adjusted(10, 0, -8, 0),
                        Qt::AlignVCenter | Qt::AlignLeft, index.data().toString());
            p->restore();
            return;
        }

        const SearchResult& r = m_w->resultAt(i);
        const QRect rr = opt.rect.adjusted(8, 2, -8, 2);
        const int actionWidth=hasResultActions(r)?88:0;

        if (opt.state & QStyle::State_Selected) {
            QPainterPath path;
            path.addRoundedRect(rr, 9, 9);
            p->fillPath(path, Theme::launcherColor("rowSel"));
        }

        if (actionWidth && (opt.state & (QStyle::State_MouseOver|QStyle::State_Selected))) {
            const QRect folder=actionRect(opt.rect,false);
            drawGlyph(*p,QRectF(folder.center().x()-9,folder.center().y()-9,18,18),ActionGlyph::Folder);
            if (m_w->canPinResult(r)) {
                const auto pin=actionRect(opt.rect,true);
                drawGlyph(*p,QRectF(pin.center().x()-9,pin.center().y()-9,18,18),
                    PinnedApps::isPinned(m_w->resultPinnedApp(r))?ActionGlyph::Check:ActionGlyph::Pin);
            }
        }

        const QRect iconRect(rr.left() + 8, rr.center().y() - 14, 28, 28);
        if (!r.icon.isNull()) {
            r.icon.paint(p, iconRect);
        } else {
            QPainterPath path;
            path.addEllipse(iconRect);
            p->fillPath(path, Theme::launcherColor("badgeBg"));
            p->setPen(Qt::white);
            p->drawText(iconRect, Qt::AlignCenter, "?");
        }

        QFont f = opt.font;
        f.setPointSizeF(10.5);
        f.setBold(true);
        p->setFont(f);
        p->setPen(Theme::launcherColor(r.actionable ? "text" : "dim"));
        const QFontMetrics tfm(f);
        p->drawText(QRect(rr.left() + 48, rr.top() + 6, rr.width() - 58-actionWidth, 20),
                    Qt::AlignVCenter,
                    tfm.elidedText(r.title, Qt::ElideRight, rr.width() - 58-actionWidth));

        f.setBold(false);
        f.setPointSizeF(8.5);
        p->setFont(f);
        p->setPen(Theme::launcherColor("muted"));
        const QFontMetrics sfm(f);
        p->drawText(QRect(rr.left() + 48, rr.top() + 29, rr.width() - 58-actionWidth, 16),
                    Qt::AlignVCenter,
                    sfm.elidedText(r.subtitle, Qt::ElideMiddle, rr.width() - 58-actionWidth));
        p->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& index) const override
    {
        const int i = index.data(Qt::UserRole).toInt();
        return {0, i < 0 ? kSectionHeight : kRowHeight};
    }

private:
    SearchWindow* m_w;
};

// 全角→半角：中文输入法在"全角"下打出来的是 ＱＱ（U+FF31），和索引里 ASCII 的 "qq"
// 一个字符都对不上，界面上就变成"搜 QQ 搜不到应用"（2026-10-12 他报的这条）。
// U+FF01–U+FF5E 是 U+0021–U+007E 各加 0xFEE0，全角空格是 U+3000。
QString halfWidth(const QString& s)
{
    bool needs = false;
    for (const QChar& c : s) {
        const ushort u = c.unicode();
        if ((u >= 0xFF01 && u <= 0xFF5E) || u == 0x3000) {
            needs = true;
            break;
        }
    }
    if (!needs)
        return s;                     // 绝大多数查询就是纯 ASCII，别每个键都重搭一遍串
    QString out;
    out.reserve(s.size());
    for (const QChar& c : s) {
        const ushort u = c.unicode();
        if (u >= 0xFF01 && u <= 0xFF5E)
            out.append(QChar(u - 0xFEE0));
        else if (u == 0x3000)
            out.append(QLatin1Char(' '));
        else
            out.append(c);
    }
    return out;
}

// 一条应用的"使用次数"记在哪个键上。以前直接用 lnkPath，而**目录扫出来的那些没有 lnk**，
// 于是它们全共用同一个空键：这台机器实测那个键涨到 43，QQEX（没有快捷方式）就靠这 43 次
// "别人家的记录"排到了 QQ 前面。没有 lnk 就用 target 当键，各记各的。
QString usageKey(const QString& lnkPath, const QString& target)
{
    return lnkPath.isEmpty() ? target : lnkPath;
}

int scoreApp(const AppEntry& a, const QString& low)
{
    const QString n = a.name.toLower();
    if (n.startsWith(low))
        return 0;
    const QStringList words = n.split(QRegularExpression("[\\s\\-_\\.]"), Qt::SkipEmptyParts);
    for (const QString& w : words)
        if (w.startsWith(low))
            return 1;
    // 拼音两档：renwu → 任务（全拼），rw → 任务/软件（首字母）。排在英文词首之后、
    // 子串之前 —— 打拼音的人要的就是那个中文名，比"名字里正好含这几个字母"更准。
    switch (Pinyin::match(a.name, low)) {
    case Pinyin::Full:
        return 2;
    case Pinyin::Initial:
        return 3;
    default:
        break;
    }
    if (n.contains(low))
        return 4;
    if (!a.target.isEmpty()
        && QFileInfo(a.target).fileName().toLower().contains(low))
        return 5;
    return -1;
}

} // namespace

SearchWindow::SearchWindow(QWidget* parent)
    : QWidget(parent)
{
    setWindowTitle(QStringLiteral("萤 Hotaru 搜索"));
    buildUi();
    loadUsage();
    m_apps = AppScanner::loadIndex();
    m_apps += SysTools::systemTools(); // Windows 内置面板/工具进索引

    connect(&FileIndexer::instance(), &FileIndexer::indexingFinished, this,
            [this](int total) {
                if (isVisible() && m_input->text().trimmed().isEmpty()) {
                    setStatus(QString("索引完成：应用 %1 · 文件 %2")
                                        .arg(m_apps.size()).arg(total));
                }
            });
}

void SearchWindow::buildUi()
{
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMinimumSize(760, 420);
    m_skin = new LauncherSkin(this);
    m_skin->lower();

    m_input = new QLineEdit(this);
    m_input->setObjectName(QStringLiteral("input"));   // --pinyin 的端到端断言靠它找到输入框
    m_input->setFixedHeight(48);
    m_input->setPlaceholderText("搜索固定软件、应用、文件或网址");
    Theme::bindLauncher(m_input,
        "QLineEdit{background:transparent;border:none;color:@editText;"
        "font-size:15pt;padding:4px 10px;}");
    m_input->installEventFilter(this);

    // 右侧齿轮：打开配置页面
    m_settingsBtn = new GearButton(this);
    m_settingsBtn->setToolTip("设置");
    m_settingsBtn->setCursor(Qt::PointingHandCursor);
    m_settingsBtn->setObjectName(QStringLiteral("hotaruSettings"));
    m_settingsBtn->setFixedSize(28, 28);
    Theme::bindLauncher(m_settingsBtn,
        "QToolButton{background:transparent;border:none;color:@groupText;font-size:20px;}"
        "QToolButton:hover{color:@editText;background:@card;border-radius:8px;}");

    auto* inputRow = m_inputRow = new QWidget(this);
    inputRow->setStyleSheet("background:transparent;");
    auto* inputLay = new QHBoxLayout(inputRow);
    inputLay->setContentsMargins(0, 0, 0, 0);
    inputLay->setSpacing(2);
    inputLay->addWidget(m_input, 1);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("list"));
    m_list->setItemDelegate(new ResultDelegate(this));
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setMouseTracking(true);
    m_list->viewport()->setMouseTracking(true);
    m_list->viewport()->installEventFilter(this);
    m_list->setVisible(false);
    Theme::bindLauncher(m_list,
        "QListWidget{background:transparent;border:none;outline:none;font-size:10.5pt;}"
        "QListWidget::item{background:transparent;}"
        "QScrollBar:vertical{background:transparent;width:6px;margin:2px;}"
        "QScrollBar::handle:vertical{background:@scrollHandle;border-radius:3px;min-height:20px;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}"
        "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}");

    m_grid = new LauncherGrid(this);
    m_grid->installEventFilter(this);
    m_homePage = m_grid;
    connect(m_grid,&LauncherGrid::addRequested,this,[this](int row,int column,bool folder) {
        addPinnedFileOrFolder({},row,column,folder);
    });
    connect(m_grid,&LauncherGrid::createGroupRequested,this,
            &SearchWindow::createPanelGroup);
    connect(m_grid,&LauncherGrid::itemMenuRequested,this,&SearchWindow::pinnedMenu);
    wireGrid(m_grid);
    m_homeScroll = new QScrollArea(this);
    m_homeScroll->setObjectName(QStringLiteral("hotaruHomeScroll"));
    m_homeScroll->setWidgetResizable(true);
    m_homeScroll->setFrameShape(QFrame::NoFrame);
    m_homeScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_homeScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_homeScroll->setWidget(m_homePage);
    Theme::bindLauncher(m_homeScroll,
        "QScrollArea{background:transparent;border:none;}"
        "QScrollBar:vertical{background:transparent;width:7px;margin:2px;}"
        "QScrollBar::handle:vertical{background:@scrollHandle;border-radius:3px;min-height:24px;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}"
        "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}");
    m_homeScroll->setVisible(false);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) { openContextMenu(m_list->mapToGlobal(pos)); });
    connect(m_list, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { activateCurrent(); });

    m_hint = new QLabel(
        "右键管理 · 搜索启动", this);
    m_hint->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_hint->setFixedHeight(28);
    Theme::bindLauncher(m_hint, "color:@sectionText;background:transparent;font-size:7.5pt;");
    auto* pages=new PageBar(this);m_pageBar=pages;
    pages->request=[this](int page,bool empty) {
        if (empty) {
            QString error;
            if (!PinnedApps::addPage(&error)) {QMessageBox::warning(this,QStringLiteral("新建页面失败"),error);return;}
            rebuildHome();
        }
        m_grid->setPage(page);
    };
    pages->removePage=[this]{deleteCurrentPage();};
    connect(m_grid,&LauncherGrid::pageChanged,this,[this,pages](int page,int count) {
        pages->page=page;pages->count=count;pages->update();
        if (m_restoringPage || m_grid->isDragging()) return;
        auto cfg=PinnedApps::config();
        const int saved=qMin(page,PinnedApps::pageCount(cfg)-1);
        if (cfg.lastPage==saved) return;
        cfg.lastPage=saved;QString error;
        if (!PinnedApps::saveConfig(cfg,&error)) setStatus(QStringLiteral("页码未保存：")+error);
    });

    resize(kWindowWidth, 640);
    refreshSkin();

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(120);
    connect(m_debounce, &QTimer::timeout, this, &SearchWindow::runSearch);
    connect(m_input, &QLineEdit::textChanged, this, [this](const QString&) {
        scheduleSearch();
    });
    connect(m_settingsBtn, &QToolButton::clicked, this, &SearchWindow::openSettings);
    qApp->installEventFilter(this);

    // 装了新软件搜不到：应用索引以前只在启动时扫一次，配置里那个"扫描间隔"也没人读。
    m_appRescan = new QTimer(this);
    m_appRescan->setSingleShot(true);
    m_appRescan->setInterval(4000);
    m_lastAutoScan.start();
    connect(m_appRescan, &QTimer::timeout, this, [this] {
        const qint64 since = m_lastAutoScan.elapsed();
        if (since < 60 * 1000) {
            // 节流只能延后、不能丢弃：丢掉的话"装完软件"这次变更就永远没人处理了
            m_appRescan->start(60 * 1000 - since + 500);
            return;
        }
        m_lastAutoScan.restart();
        startScan(true, false);
    });
    // F1 浮层抢走激活时搜索框要先站着别走：窗口一 hide，浮层的"窗口吸附"
    // 就找不到搜索框了（截图像素里有它、可鼠标就是吸不上去）。浮层一关就补收。
    m_captureHold = new QTimer(this);
    m_captureHold->setInterval(200);
    connect(m_captureHold, &QTimer::timeout, this, [this] {
        if (CaptureState::isActive())
            return;
        m_captureHold->stop();
        if (!isActiveWindow() && (!m_menu || !m_menu->isVisible()))
            hide();
    });

    const QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
    const int hours = cfg.value("index/refreshHours", 6).toInt();
    if (hours > 0) {
        m_periodicScan = new QTimer(this);
        m_periodicScan->setInterval(hours * 3600 * 1000);
        connect(m_periodicScan, &QTimer::timeout, this, [this] {
            m_lastAutoScan.restart();
            startScan(true, false);
        });
        m_periodicScan->start();
    }
    watchAppDirs();
}

// 监视快捷方式所在的目录（含子目录，深度与 AppScanner::scan 一致）。
// QFileSystemWatcher 不递归，所以开始菜单那两层树要逐个目录挂上去。
void SearchWindow::watchAppDirs()
{
    if (!m_appWatch) {
        m_appWatch = new QFileSystemWatcher(this);
        connect(m_appWatch, &QFileSystemWatcher::directoryChanged, this,
                [this](const QString& path) {
                    // 目录被删除重建后 Qt 会丢掉监视，补挂一次再防抖重扫
                    if (QDir(path).exists() && !m_appWatch->directories().contains(path))
                        m_appWatch->addPath(path);
                    m_appRescan->start();   // 变更成串到来时把重扫窗口往后推
                });
    }

    QStringList want;
    const auto collect = [&want](auto&& self, const QString& dir, int depth) -> void {
        want << dir;
        if (depth <= 0)
            return;
        QDir d(dir);
        for (const QFileInfo& fi : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
            self(self, fi.absoluteFilePath(), depth - 1);
    };
    const QString desktop = Platform::knownDir(Platform::KnownDir::UserDesktop);
    const QString pubDesktop = Platform::knownDir(Platform::KnownDir::SharedDesktop);
    const QString userProgs = Platform::knownDir(Platform::KnownDir::UserApps);
    const QString commonProgs = Platform::knownDir(Platform::KnownDir::SystemApps);
    if (!desktop.isEmpty())
        collect(collect, desktop, 1);
    if (!pubDesktop.isEmpty())
        collect(collect, pubDesktop, 0);
    if (!userProgs.isEmpty())
        collect(collect, userProgs, 10);
    if (!commonProgs.isEmpty())
        collect(collect, commonProgs, 10);

    QStringList cur = m_appWatch->directories();
    QStringList toAdd;
    for (const QString& d : std::as_const(want))
        if (!cur.contains(d, Qt::CaseInsensitive))
            toAdd << d;
    QStringList gone;
    for (const QString& d : std::as_const(cur))
        if (!QDir(d).exists())
            gone << d;
    if (!gone.isEmpty())
        m_appWatch->removePaths(gone);
    if (!toAdd.isEmpty())
        m_appWatch->addPaths(toAdd);
    Utils::log(QString("apps: watching %1 shortcut dirs (%2 new)")
                   .arg(m_appWatch->directories().size()).arg(toAdd.size()));
}

void SearchWindow::openSettings()
{
    // 必须在隐藏启动器之前确定屏幕，不能沿用设置窗口上次所在的副屏。
    QScreen* target=QGuiApplication::screenAt(frameGeometry().center());
    if (!target) target=screen();
    if (!target) target=QGuiApplication::primaryScreen();
    const QRect available=target->availableGeometry();
    MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);
    const bool haveNativeWork=GetMonitorInfo(MonitorFromWindow(reinterpret_cast<HWND>(winId()),MONITOR_DEFAULTTONEAREST),&monitor);
    Utils::log(QStringLiteral("settings: 入口触发 reuse=%1 screen=%2 available=%3,%4 %5x%6 launcher=%7,%8")
        .arg(bool(m_settingsDlg)).arg(target->name()).arg(available.x()).arg(available.y()).arg(available.width()).arg(available.height()).arg(x()).arg(y()));
    closeGroup();
    m_grid->cancelDrag();
    m_settingsOpen=true;
    hide();
    if (!m_settingsDlg) {
        auto* dlg=new SettingsDialog(nullptr);m_settingsDlg=dlg;
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        connect(dlg,&SettingsDialog::rescanRequested,this,&SearchWindow::rescan);
        connect(dlg,&SettingsDialog::hotkeyChanged,this,&SearchWindow::hotkeyChanged);
        connect(dlg,&SettingsDialog::hotkeyCaptureActive,this,&SearchWindow::hotkeyCaptureActive);
        connect(dlg,&QObject::destroyed,this,[this] {m_settingsOpen=false;refreshLauncher();});
        dlg->openAt(2,1);
        Utils::log(QStringLiteral("settings: 已创建并加载配置页"));
    }
    auto* dlg=m_settingsDlg.data();
    dlg->setWindowState(Qt::WindowNoState);
    dlg->winId();
    if (dlg->windowHandle()) dlg->windowHandle()->setScreen(target);
    if (dlg->layout()) dlg->layout()->setSizeConstraint(QLayout::SetNoConstraint);
    dlg->setMinimumSize(0,0);
    const auto fit=[dlg,available,monitor,haveNativeWork] {
        // 顶层 QWidget::move 接受外框位置；不能再加客户区边距，否则会偏移甚至越界。
        const QSize frameExtra=dlg->frameGeometry().size()-dlg->size();
        const QSize limit=(available.size()-frameExtra).expandedTo(QSize(1,1));
        dlg->setMaximumSize(limit);dlg->resize(QSize(1140,790).boundedTo(limit));
        const QSize outer=dlg->frameGeometry().size();
        dlg->move(available.topLeft()+QPoint((available.width()-outer.width())/2,(available.height()-outer.height())/2));
        if (!dlg->isVisible() || !haveNativeWork || QGuiApplication::platformName()!=QStringLiteral("windows")) return;
        // Windows 缩放后的真实可见边框可能与 Qt frameGeometry 相差数像素。
        // 用 DWM 的物理像素外框校正尺寸和位置，系统阴影不计入可用尺寸。
        const HWND handle=reinterpret_cast<HWND>(dlg->winId());
        RECT visible{},native{};
        if (FAILED(DwmGetWindowAttribute(handle,DWMWA_EXTENDED_FRAME_BOUNDS,&visible,sizeof(visible)))) return;
        const int excessWidth=qMax(0,int(visible.right-visible.left-(monitor.rcWork.right-monitor.rcWork.left)));
        const int excessHeight=qMax(0,int(visible.bottom-visible.top-(monitor.rcWork.bottom-monitor.rcWork.top)));
        if (excessWidth || excessHeight) {
            const qreal dpr=dlg->devicePixelRatioF();
            const QSize correction(qCeil(excessWidth/dpr),qCeil(excessHeight/dpr));
            dlg->setMaximumSize((limit-correction).expandedTo(QSize(1,1)));
            dlg->resize((dlg->size()-correction).expandedTo(QSize(1,1)));
            DwmGetWindowAttribute(handle,DWMWA_EXTENDED_FRAME_BOUNDS,&visible,sizeof(visible));
        }
        if (!GetWindowRect(handle,&native)) return;
        const int left=monitor.rcWork.left+(monitor.rcWork.right-monitor.rcWork.left-(visible.right-visible.left))/2;
        const int top=monitor.rcWork.top+(monitor.rcWork.bottom-monitor.rcWork.top-(visible.bottom-visible.top))/2;
        SetWindowPos(handle,nullptr,left-(visible.left-native.left),top-(visible.top-native.top),0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    };
    fit();
    dlg->showNormal();dlg->raise();dlg->activateWindow();
    fit();QTimer::singleShot(0,dlg,fit);
    HWND h=reinterpret_cast<HWND>(dlg->winId());
    ShowWindow(h,SW_RESTORE);SetForegroundWindow(h);
    // 记录 Qt 与原生矩形，便于区分命中失败、窗口隐藏和跨屏定位问题。
    QTimer::singleShot(100,dlg,[dlg] {
        RECT r{};const auto h=reinterpret_cast<HWND>(dlg->winId());GetWindowRect(h,&r);
        const auto frame=dlg->frameGeometry();
        Utils::log(QStringLiteral("settings: displayed visible=%1 active=%2 nativeVisible=%3 rect=%4,%5 %6x%7 qtFrame=%8,%9 %10x%11")
            .arg(dlg->isVisible()).arg(dlg->isActiveWindow()).arg(IsWindowVisible(h)).arg(r.left).arg(r.top).arg(r.right-r.left).arg(r.bottom-r.top)
            .arg(frame.x()).arg(frame.y()).arg(frame.width()).arg(frame.height()));
    });
}

void SearchWindow::openSettingsPage(int menuRow)
{
    openSettings();
    if (auto* dlg = qobject_cast<SettingsDialog*>(m_settingsDlg.data()))
        dlg->openAt(menuRow);
}

void SearchWindow::openTool(const QString& id, const QString& text)
{
    openSettings();
    if (auto* dlg = qobject_cast<SettingsDialog*>(m_settingsDlg.data()))
        dlg->showTool(id, text);
}

// ---------- 显示/隐藏 ----------

void SearchWindow::showLauncher(const char* why)
{
    // 设置页现在是非模态的，热键随时能把搜索框弹出来（以前模态时弹出来也打不了字）
    Utils::log(QStringLiteral("show: 呼出（要求者=%1）").arg(QString::fromUtf8(why)));
    m_hadForeground = false;   // 每一次呼出重新判：见 blurAction
    m_showLatency.start();
    m_paintLogged = false;
    const int wsBefore = Platform::workingSetMB();
    QElapsedTimer lap;
    lap.start();
    QScreen* s = QGuiApplication::screenAt(QCursor::pos());
    if (!s)
        s = QGuiApplication::primaryScreen();
    const QRect g = s->availableGeometry();
    move(g.center().x() - width() / 2, g.top() + int(g.height() * 0.20));
    m_input->clear(); // 再次呼出不保留上一次的搜索
    runSearch();
    const auto pageConfig=PinnedApps::config();
    m_grid->setPage(pageConfig.rememberPage?pageConfig.lastPage:0);
    const qint64 msLayout = lap.elapsed();
    lap.restart();
    // 被最小化过（或 Win+D 之后）单纯 show() 不会把它从最小化状态拉回来，
    // 表现同样是"按了热键没出来"，所以这里显式清掉最小化位
    setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    show();
    const qint64 msShow = lap.elapsed();
    raise();
    // 后台进程调用 SetForegroundWindow 会被 Windows 前台锁拦住，
    // 附加到当前前台线程的输入队列后再置前，保证弹出即可输入
    HWND h = reinterpret_cast<HWND>(winId());
    HWND fg = GetForegroundWindow();
    m_prevFg = reinterpret_cast<WId>(fg);   // Ctrl+Enter 粘贴要还给这个窗口
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD curThread = GetCurrentThreadId();
    const bool attached = fgThread && fgThread != curThread
                          && AttachThreadInput(curThread, fgThread, TRUE);
    SetForegroundWindow(h);
    activateWindow();
    if (attached)
        AttachThreadInput(curThread, fgThread, FALSE);
    // 只在没抢到前台时留痕：那时窗口是"已显示但在别的窗口后面"，
    // 用户视角就是"按了热键没出来"，日志里必须能看出来是这一步而不是热键没到
    if (GetForegroundWindow() != h)
        Utils::log(QString("show: 置前失败（SetForegroundWindow 没抢到，前台仍是 0x1）")
                       .arg(quintptr(GetForegroundWindow()), 0, 16));
    m_input->setFocus();
    const qint64 msFg = lap.elapsed();
    lap.restart();
    m_showTime.start();
    runSearch();
    // 只在真的慢下来时才留痕（正常是 2-3ms，每次按热键都写一行会把日志刷满）
    if (m_showLatency.elapsed() > 30)
        Utils::log(QString("show: 慢启动器 布局=%1 建窗口=%2 置前=%3 结果=%4 合计=%5ms 工作集 %6→%7MB")
                       .arg(msLayout).arg(msShow).arg(msFg).arg(lap.elapsed()).arg(m_showLatency.elapsed())
                       .arg(wsBefore).arg(Platform::workingSetMB()));
}

void SearchWindow::warmUp()
{
    if (m_warmed || isVisible())
        return;
    m_warmed = true;
    // 第一次 show() 要付 Qt 建原生窗口 + DWM 首帧 + 字体/委托首次绘制的钱（实测约 300ms），
    // 那笔钱本来全落在用户第一次按热键上。挪到所有显示器之外映射一遍再收起，界面看不到。
    const QPoint saved = pos();
    // 两道保险：位置挪到显示器之外（Qt 有时会把它夹回屏幕内），同时整窗透明度压到 0
    setWindowOpacity(0.0);
    move(-32000, -32000);
    show();
    repaint();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    // 这一行量的必须是**停在预热位置那一刻**的矩形。以前它排在 `move(saved)` 之后才读 x()/y()，
    // 读到的是恢复完的落点（本机日志一直打 (0,0 660x100) 就是这个），
    // 而"有没有被 Qt 夹回屏幕里"这件事当场就测不到 —— 被夹回来的话，那一下就是"搜索框自己闪出来"。
    const QRect parked = geometry();
    bool onScreen = false;
    for (QScreen* sc : QGuiApplication::screens())
        if (sc->geometry().intersects(parked)) { onScreen = true; break; }
    hide();
    setWindowOpacity(1.0);
    move(saved);
    Utils::log(QStringLiteral("warmup: 预热时窗口矩形=(%1,%2 %3x%4) · %5")
                   .arg(parked.x()).arg(parked.y()).arg(parked.width()).arg(parked.height())
                   .arg(onScreen ? QStringLiteral("还压在某个显示器里（只靠透明度 0 遮着）")
                                 : QStringLiteral("在所有显示器之外")));
}

void SearchWindow::toggleVisible()
{
    // 以前这里只看 isVisible()，会出"有时候按了没反应"：这个窗口失焦不自动收起，
    // 于是它完全可能正躲在别的窗口后面（用户看不见）。那种状态下再按热键，
    // 旧逻辑走的是 hide() —— 按一下没出来，得再按一遍才出来。
    // 正确的判据是"它就是当前前台窗口"才收起，否则一律呼出并置前。
    // 注意最小化那一支：实测窗口被最小化之后 GetForegroundWindow() 仍然算它自己，
    // 所以"在前台"必须同时要求"没被最小化"，否则按热键会变成把它 hide 掉（--toggle 抓到过）。
    const bool visible = isVisible();
    const bool minimized = (windowState() & Qt::WindowMinimized) != 0;
    const bool fgIsMine = GetForegroundWindow() == reinterpret_cast<HWND>(winId());
    const bool onTop = visible && !minimized && fgIsMine;
    Utils::log(QStringLiteral("hotkey: %1（visible=%2 最小化=%3 前台=%4 位置=%5,%6 %7x%8 屏幕=%9 距上次呼出=%10ms）")
                   .arg(onTop ? QStringLiteral("收起") : QStringLiteral("呼出"))
                   .arg(visible ? 1 : 0).arg(minimized ? 1 : 0).arg(fgIsMine ? 1 : 0)
                   .arg(x()).arg(y()).arg(width()).arg(height())
                   .arg(QGuiApplication::screenAt(geometry().center()) ? QStringLiteral("有")
                                                                       : QStringLiteral("无"))
                   // 这一列是用来判"两次按压被吞"的：间隔只有两三百毫秒的一对，
                   // 说明第一次还堵在消息队列里，第二次落地就成了 show 完立刻 hide（=用户看到"没出来"）
                   .arg(m_showTime.isValid() ? m_showTime.elapsed() : -1));
    if (onTop)
        hide();
    else
        showLauncher("热键");
}

void SearchWindow::updateHeight()
{
    const QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
    const bool searchOnly = !cfg.value("ui/favoritesOnEmpty", true).toBool();
    if (searchOnly && !m_searchOnly) m_gridWindowSize = size();
    const bool restoreGrid = m_searchOnly && !searchOnly;
    m_searchOnly = searchOnly;
    m_skin->setCompact(searchOnly, !m_list->isHidden() && m_list->count()>0);
    m_skin->setSurfaceVisible(true);
    m_hint->setVisible(!searchOnly);
    if (searchOnly) {
        // 收起固定面板时保留主题搜索外框，结果仍按实际行高展开。
        int resultHeight = 0;
        if (!m_list->isHidden())
            for (int row = 0; row < qMin(m_list->count(), kMaxVisibleRows); ++row)
                resultHeight += m_list->sizeHintForRow(row);
        const bool skinned = m_skin->active();
        const int chrome = skinned ? m_skin->compactHeight(1)-1 : 82;
        if (const auto* targetScreen = screen())
            resultHeight = qMin(resultHeight, qMax(kRowHeight,
                targetScreen->availableGeometry().bottom() - frameGeometry().top() + 1 - chrome));
        setMinimumSize(skinned ? qMax(760,m_gridMinimumSize.width()) : 760, 72);
        setFixedHeight(skinned ? m_skin->compactHeight(resultHeight)
                              : resultHeight > 0 ? 82 + resultHeight : 72);
    } else {
        setMaximumHeight(QWIDGETSIZE_MAX);
        setMinimumSize(m_gridMinimumSize);
        if (restoreGrid) resize(QSize(width(), m_gridWindowSize.height()).expandedTo(m_gridMinimumSize));
    }
    layoutLauncher();
    update();
}

void SearchWindow::layoutLauncher()
{
    if (!m_inputRow || !m_hint || !m_homeScroll) return;
    m_skin->setGeometry(rect());
    const bool skinned = m_skin->active();
    if (m_searchOnly) {
        if (skinned) {
            const QRect search = m_skin->compactSearchRect();
            m_inputRow->setGeometry(search.adjusted(0,0,-44,0));
            QRect content = m_skin->compactContentRect();
            const auto& surface = Theme::currentPackage().surface;
            content.setHeight(qMax(0,height()-surface.framePadding.bottom()-surface.frameMargins.bottom()-8-content.y()));
            m_list->setGeometry(content);
            m_settingsBtn->setGeometry(search.right()-27,search.center().y()-13,28,28);
        } else {
            m_inputRow->setGeometry(14, 12, width()-72, 48);
            m_list->setGeometry(14, 70, width()-28, qMax(0,height()-82));
            m_settingsBtn->setGeometry(width()-44, 22, 28, 28);
        }
        m_settingsBtn->raise();
        m_skin->lower();
        return;
    }
    const QRect search = skinned ? m_skin->searchRect() : QRect(14, 12, width()-28, 48);
    const QRect content = skinned ? m_skin->contentRect() : QRect(14, 70, width()-28, height()-110);
    m_inputRow->setGeometry(search);
    m_homeScroll->setGeometry(content);
    m_list->setGeometry(content);
    const QRect footer=skinned ? m_skin->footerRect() : QRect(22,height()-34,width()-44,28);
    const QRect themedSettings=skinned ? m_skin->settingsRect() : QRect(footer.right()-27,footer.y(),28,28);
    const QRect settings(themedSettings.center().x()-13,footer.bottom()-27,28,28);
    const int pageX=qBound(footer.x(),footer.center().x()-72,settings.left()-148);
    if (m_pageBar) {m_pageBar->setGeometry(pageX,settings.y(),144,28);m_pageBar->raise();}
    m_hint->setGeometry(footer.x(),settings.y(),qMax(0,pageX-footer.x()-8),28);
    m_settingsBtn->setGeometry(settings);
    m_settingsBtn->raise();m_hint->raise();
    if (m_groupPanel && m_groupPanel->isVisible()) {
        const QSize size=QSize(520,440).boundedTo(content.size());
        m_groupPanel->setGeometry(QRect(content.center()-QPoint(size.width()/2,size.height()/2),size));
        m_groupPanel->raise();
    }
    setStatus(m_hint->toolTip().isEmpty()?QStringLiteral("右键管理 · 搜索启动"):m_hint->toolTip());
    m_skin->lower();
}

void SearchWindow::refreshSkin()
{
    if (!m_skin) return;
    Theme::applyToLauncher(this);
    m_skin->reload();
    if (m_skin->active()) {
        const auto& surface = Theme::currentPackage().surface;
        m_gridMinimumSize = QSize(qMax(760, surface.frameMargins.left()+surface.frameMargins.right()+400),
                       qMax(420, surface.frameMargins.top()+surface.frameMargins.bottom()+surface.framePadding.top()+220));
        if (Theme::currentPackage().schemaVersion>=2) m_gridMinimumSize = surface.canvasSize;
    } else m_gridMinimumSize = QSize(760,420);
    updateHeight();
}

void SearchWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    layoutLauncher();
}

void SearchWindow::addPinnedFileOrFolder(const QString& id,int row,int column,bool folder)
{
    if (!id.isEmpty()) for (const auto& app:PinnedApps::load()) if (app.id==id) folder=PinnedApps::targetType(app)=="folder";
    const QString path=folder?QFileDialog::getExistingDirectory(this,QStringLiteral("选择磁盘文件夹"))
        :QFileDialog::getOpenFileName(this,QStringLiteral("选择程序或脚本"),{},QStringLiteral("程序与脚本 (*.exe *.lnk *.bat *.cmd)"));
    if (path.isEmpty()) return;
    QString error;
    if (id.isEmpty()) {
        if (!PinnedApps::add(PinnedApps::fromTarget(path),row,column,&error)) QMessageBox::warning(this,QStringLiteral("无法固定"),error);
    } else {
        auto cfg=PinnedApps::config();
        for (auto& app:cfg.pinnedApps) if (app.id==id) {
            const auto target=PinnedApps::fromTarget(path);app.targetPath=target.targetPath;app.targetType=target.targetType;
            app.iconPath.clear();app.workingDirectory=target.workingDirectory;
        }
        if (!PinnedApps::saveConfig(cfg,&error)) QMessageBox::warning(this,QStringLiteral("保存失败"),error);
    }
    runSearch();
}

void SearchWindow::createPanelGroup(int row, int column)
{
    if (m_groupPanel && m_groupPanel->isVisible())
        return;
    QString title;
    if (!promptRename(QStringLiteral("新建文件夹"), true, &title))
        return;
    QString error;
    if (!PinnedApps::createGroup(title, row, column, &error)) {
        QMessageBox::warning(this, QStringLiteral("无法新建面板文件夹"), error);
        return;
    }
    refreshLauncher();
}

void SearchWindow::addToGroup(int row, int column, bool folder)
{
    if (m_openGroup.isEmpty())
        return;
    const QString path = folder
        ? QFileDialog::getExistingDirectory(this, QStringLiteral("选择磁盘文件夹"))
        : QFileDialog::getOpenFileName(this, QStringLiteral("选择程序或脚本"), {},
                                       QStringLiteral("程序与脚本 (*.exe *.lnk *.bat *.cmd)"));
    if (path.isEmpty())
        return;
    auto item = PinnedApps::fromTarget(path);
    item.groupId = m_openGroup;
    QString error;
    if (!PinnedApps::add(item, row, column, &error))
        QMessageBox::warning(this, QStringLiteral("无法固定"), error);
    refreshLauncher();
}

void SearchWindow::wireGrid(LauncherGrid* grid)
{
    connect(grid,&LauncherGrid::activated,this,&SearchWindow::activatePinned);
    connect(grid,&LauncherGrid::renameRequested,this,&SearchWindow::renamePinned);
    connect(grid,&LauncherGrid::deletePageRequested,this,&SearchWindow::deleteCurrentPage);
    connect(grid,&LauncherGrid::moveRequested,this,[this,grid](const QString& id,int row,int column){applyGridDrop(grid,id,row,column);});
    connect(grid,&LauncherGrid::mergeRequested,this,[this,grid](const QString& id,const QString& target){applyGridDrop(grid,id,0,0,target);});
    if (grid==m_grid) {
        connect(grid,&LauncherGrid::dragCanceled,this,[this] {
            if (m_transferGroup.isEmpty()) return;
            const auto origin=m_transferGroup;const int page=m_transferPage,home=m_transferHomePage;
            m_transferGroup.clear();m_restoringPage=true;m_grid->setPage(home);m_restoringPage=false;
            openGroup(origin,page);
        });
    }
}

void SearchWindow::activatePinned(const QString& id)
{
    const auto cfg=PinnedApps::config();
    for (const auto& g:cfg.groups) if (g.id==id) {openGroup(id);return;}
    for (const auto& app:cfg.pinnedApps) if (app.id==id) {
        QString error;
        if (!PinnedApps::launch(app,&error)) QMessageBox::warning(this,QStringLiteral("启动失败"),error+QStringLiteral("\n右键图标可以重新定位程序。"));
        else hide();
        return;
    }
}

void SearchWindow::applyGridDrop(LauncherGrid* grid,const QString& id,int row,int column,const QString& mergeTarget)
{
    const auto before=PinnedApps::config();
    const QString scope=grid==m_groupGrid?m_openGroup:QString();
    const QString transfer=m_transferGroup;
    const int originalHome=transfer.isEmpty()?m_grid->currentPage():m_transferHomePage;
    int originalPage=0;
    for (const auto& a:before.pinnedApps) if (a.id==id) originalPage=a.gridRow/(a.groupId.isEmpty()?before.gridRows:3);
    for (const auto& g:before.groups) if (g.id==id) originalPage=g.gridRow/before.gridRows;
    const int destination=grid->currentPage();
    QString error;
    const bool ok=mergeTarget.isEmpty()?PinnedApps::moveTo(id,scope,row,column,&error):PinnedApps::merge(id,mergeTarget,&error);
    m_transferGroup.clear();
    if (!ok) {
        closeGroup();rebuildHome();
        m_restoringPage=true;m_grid->setPage(transfer.isEmpty() && scope.isEmpty()?originalPage:originalHome);m_restoringPage=false;
        if (!scope.isEmpty()) openGroup(scope,originalPage);
        else if (!transfer.isEmpty()) openGroup(transfer,m_transferPage);
        QMessageBox::warning(this,QStringLiteral("布局未保存"),error);
        return;
    }
    if (mergeTarget.isEmpty() && transfer.isEmpty()) {
        // 同一容器的移动保留正在播放的吸附动画；分组封面在关闭二级页时刷新。
        if (!scope.isEmpty()) m_groupPanel->setProperty("refreshCover",true);
        return;
    }
    // 提交后重建全部布局，才能同时更新分组封面、隐藏占位和空分组清理。
    rebuildHome();
    if (scope.isEmpty()) m_grid->setPage(destination);
    else if (m_groupGrid && !m_openGroup.isEmpty()) m_groupGrid->setPage(destination);
}

void SearchWindow::renamePinned(const QString& id)
{
    renamePinnedInteractive(id);
}

bool SearchWindow::renamePinnedInteractive(const QString& id)
{
    const auto cfg=PinnedApps::config();QString title;
    bool isGroup=false;
    for (const auto& a:cfg.pinnedApps) if (a.id==id) title=a.title;
    for (const auto& g:cfg.groups) if (g.id==id) { title=g.title; isGroup=true; }
    QString name;
    if (!promptRename(title, isGroup, &name))
        return false;
    QString error;
    if (!PinnedApps::rename(id,name,&error)) {
        QMessageBox::warning(this,QStringLiteral("重命名失败"),error);
        return false;
    }
    refreshLauncher();
    return true;
}

bool SearchWindow::promptRename(const QString& initial, bool isGroup, QString* result)
{
    if (!result)
        return false;
    QDialog dialog(this);
    dialog.setWindowTitle(isGroup ? QStringLiteral("重命名面板文件夹") : QStringLiteral("重命名固定项"));
    dialog.setMinimumWidth(500);
    dialog.setObjectName(QStringLiteral("renameDialog"));
    Theme::bind(&dialog, "QDialog#renameDialog{background:@window;} QLabel#renameTitle{color:@text;font-size:17pt;font-weight:600;} QLabel#renameHint,QLabel#renameError{color:@muted;} QLineEdit#renameEdit{background:@edit;background-color:@edit;border:1px solid @line;border-radius:8px;padding:8px 10px;font-size:12pt;color:@editText;} QLineEdit#renameEdit:focus{border:1px solid @accent;}");
    auto* layout=new QVBoxLayout(&dialog);
    layout->setContentsMargins(22,20,22,18);
    auto* heading=new QLabel(isGroup ? QStringLiteral("重命名面板文件夹") : QStringLiteral("重命名固定项"),&dialog);
    heading->setObjectName(QStringLiteral("renameTitle"));
    layout->addWidget(heading);
    auto* hint=new QLabel(QStringLiteral("只修改面板显示名，不修改源文件或磁盘目录。"),&dialog);
    hint->setObjectName(QStringLiteral("renameHint"));hint->setWordWrap(true);layout->addWidget(hint);
    auto* edit=new QLineEdit(initial,&dialog);edit->setObjectName(QStringLiteral("renameEdit"));edit->setMinimumHeight(44);edit->selectAll();layout->addWidget(edit);
    auto* errorHint=new QLabel(&dialog);errorHint->setObjectName(QStringLiteral("renameError"));layout->addWidget(errorHint);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);layout->addWidget(buttons);
    auto* ok=buttons->button(QDialogButtonBox::Ok);
    ok->setObjectName(QStringLiteral("primary"));
    ok->setText(QStringLiteral("确定"));
    auto* cancelButton = buttons->button(QDialogButtonBox::Cancel);
    cancelButton->setObjectName(QStringLiteral("secondary"));
    cancelButton->setText(QStringLiteral("取消"));
    ok->setMinimumWidth(88); cancelButton->setMinimumWidth(88);
    ok->setStyleSheet(QStringLiteral("QPushButton#primary{background:#3b82f6;color:#ffffff;border:1px solid #3b82f6;border-radius:7px;padding:6px 18px;font-weight:600;}QPushButton#primary:hover{background:#60a5fa;}QPushButton#primary:disabled{background:#64748b;color:#dbeafe;border:1px solid #64748b;}"));
    cancelButton->setStyleSheet(QStringLiteral("QPushButton#secondary{background:#334155;color:#e2e8f0;border:1px solid #64748b;border-radius:7px;padding:6px 18px;}QPushButton#secondary:hover{background:#475569;}"));
    auto validate=[edit,errorHint,ok]{const bool good=!edit->text().trimmed().isEmpty();ok->setEnabled(good);errorHint->setText(good?QString():QStringLiteral("名称不能为空。"));};
    connect(edit,&QLineEdit::textChanged,&dialog,validate);
    connect(edit,&QLineEdit::returnPressed,&dialog,[&dialog,ok]{if(ok->isEnabled()) dialog.accept();});
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&dialog,ok]{if(ok->isEnabled()) dialog.accept();});
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    validate();
    if (dialog.exec()!=QDialog::Accepted) return false;
    *result = edit->text().trimmed();
    return true;
}

void SearchWindow::deleteCurrentPage()
{
    const auto cfg=PinnedApps::config();const int page=m_grid->currentPage();
    if (PinnedApps::pageCount(cfg)<=1) return;
    bool migrate=false;
    if (PinnedApps::pageOccupied(cfg,page)) {
        const auto choice=QMessageBox::question(this,QStringLiteral("删除当前页"),QStringLiteral("是否迁移图标后删除？\n选择“否”将仅移除此页的固定配置，不删除源文件。"),QMessageBox::Yes|QMessageBox::No|QMessageBox::Cancel,QMessageBox::Yes);
        if (choice==QMessageBox::Cancel) return;
        migrate=choice==QMessageBox::Yes;
    }
    QString error;
    if (!PinnedApps::deletePage(page,migrate,&error)) {
        // 写入失败直接报错，不能再次选择删除而覆盖失败前的状态。
        if (migrate && error==QStringLiteral("其他现有页面的空格不足")) {
            if (QMessageBox::question(this,QStringLiteral("迁移失败"),QStringLiteral("迁移失败，是否保留此页？\n")+error,QMessageBox::Yes|QMessageBox::No,QMessageBox::Yes)==QMessageBox::Yes) return;
            if (!PinnedApps::deletePage(page,false,&error)) {QMessageBox::warning(this,QStringLiteral("删除失败"),error);return;}
        } else {QMessageBox::warning(this,QStringLiteral("删除失败"),error);return;}
    }
    rebuildHome();m_grid->setPage(qMin(page,PinnedApps::pageCount(PinnedApps::config())-1));
}

void SearchWindow::closeGroup()
{
    m_openGroup.clear();
    if (m_groupGrid) m_groupGrid->cancelDrag();
    if (m_groupPanel) m_groupPanel->hide();
    if (m_groupPanel && m_groupPanel->property("refreshCover").toBool()) {
        m_groupPanel->setProperty("refreshCover",false);rebuildHome();
    }
}

void SearchWindow::openGroup(const QString& id,int page)
{
    const auto cfg=PinnedApps::config();
    const auto found=std::find_if(cfg.groups.cbegin(),cfg.groups.cend(),[&](const auto& g){return g.id==id;});
    if (found==cfg.groups.cend()) {closeGroup();return;}
    if (!m_groupPanel) {
        auto* panel=new QFrame(this);m_groupPanel=panel;panel->setObjectName("hotaruGroupPanel");
        Theme::bindLauncher(panel,"QFrame#hotaruGroupPanel{background:@window;border:1px solid @accent;border-radius:16px;}");
        auto* layout=new QVBoxLayout(panel);layout->setContentsMargins(18,12,18,6);
        m_groupTitle=new QLabel(panel);m_groupTitle->setAlignment(Qt::AlignCenter);
        Theme::bindLauncher(m_groupTitle,"color:@text;background:transparent;font-size:12pt;border:none;");
        layout->addWidget(m_groupTitle);
        m_groupGrid=new LauncherGrid(panel);m_groupGrid->setObjectName("hotaruGroupGrid");m_groupGrid->setGroupMode(true);
        layout->addWidget(m_groupGrid,1);wireGrid(m_groupGrid);
        connect(m_groupGrid,&LauncherGrid::itemMenuRequested,this,&SearchWindow::pinnedMenu);
        connect(m_groupGrid,&LauncherGrid::transferRequested,this,[this](const QString& itemId,const QPoint& global) {
            const auto cfg=PinnedApps::config();
            for (const auto& app:cfg.pinnedApps) if (app.id==itemId) {
                m_transferGroup=app.groupId;m_transferPage=m_groupGrid->currentPage();m_transferHomePage=m_grid->currentPage();
                closeGroup();
                m_grid->beginExternalDrag({app,letterIcon(app.title,Theme::launcherColor("accent")),PinnedApps::targetExists(app)},global);
                return;
            }
        });
        connect(m_groupGrid, &LauncherGrid::addRequested, this,
                [this](int row, int col, bool folder) { addToGroup(row, col, folder); });
        auto* pages=new PageBar(panel);pages->setFixedSize(144,24);layout->addWidget(pages,0,Qt::AlignHCenter);
        pages->request=[this](int page,bool extra){m_groupGrid->setPage(page,extra);};
        connect(m_groupGrid,&LauncherGrid::pageChanged,pages,[pages](int page,int count){pages->page=page;pages->count=count;pages->update();});
    }
    m_openGroup=id;m_groupTitle->setText(found->title);
    QVector<LauncherGrid::Item> items;
    for (const auto& app:cfg.pinnedApps) if (app.groupId==id)
        items.append({app,letterIcon(app.title,Theme::launcherColor("accent")),PinnedApps::targetExists(app)});
    m_groupGrid->setItems(items,3,3);m_groupGrid->setPage(page);
    const QRect content=m_homeScroll->geometry();
    const QSize size=QSize(520,440).boundedTo(content.size());
    m_groupPanel->setGeometry(QRect(content.center()-QPoint(size.width()/2,size.height()/2),size));
    m_groupPanel->show();m_groupPanel->raise();m_groupGrid->setFocus();
}

void SearchWindow::pinnedMenu(const QString& id,const QPoint& globalPos)
{
    const auto cfg=PinnedApps::config();
    const auto group=std::find_if(cfg.groups.cbegin(),cfg.groups.cend(),[&](const auto& g){return g.id==id;});
    if (group!=cfg.groups.cend()) {
        QMenu menu(this);Theme::bindMenu(&menu);m_menu=&menu;
        menu.addAction(QStringLiteral("打开"),this,[this,id]{openGroup(id);});
        menu.addAction(QStringLiteral("重命名…"),this,[this,id]{renamePinned(id);});
        menu.addAction(QStringLiteral("解散"),this,[this,id] {
            QString error;if (!PinnedApps::dissolve(id,&error)) QMessageBox::warning(this,QStringLiteral("无法解散"),error);
            refreshLauncher();
        });
        menu.exec(globalPos);m_menu=nullptr;return;
    }
    const auto apps=cfg.pinnedApps;
    const auto found=std::find_if(apps.cbegin(),apps.cend(),[&](const auto& a){return a.id==id;});
    if (found==apps.cend()) return;
    const auto app=*found;
    QMenu menu(this);Theme::bindMenu(&menu);m_menu=&menu;
    const auto invoke=[this,app](bool admin) {
        QString error;
        const bool ok=admin?PinnedApps::launchAdmin(app,&error):PinnedApps::launch(app,&error);
        if (!ok) QMessageBox::warning(this,QStringLiteral("启动失败"),error);
        else hide();
    };
    menu.addAction(QStringLiteral("运行"),this,[invoke]{invoke(false);})->setEnabled(PinnedApps::targetExists(app));
    if (PinnedApps::canRunAdmin(app)) menu.addAction(QStringLiteral("以管理员身份运行"),this,[invoke]{invoke(true);});
    menu.addAction(QStringLiteral("打开所在位置"),this,[app]{Platform::reveal(PinnedApps::revealTarget(app));});
    menu.addSeparator();
    menu.addAction(QStringLiteral("重命名…"),this,[this,id]{renamePinned(id);});
    menu.addAction(QStringLiteral("属性…"),this,[this,id]{if (PinnedApps::editProperties(id,this)) runSearch();});
    if (!PinnedApps::targetExists(app)) menu.addAction(QStringLiteral("重新定位…"),this,[this,id]{addPinnedFileOrFolder(id);});
    menu.addAction(QStringLiteral("移除"),this,[this,id] {
        QString error;if (!PinnedApps::remove(id,&error)) QMessageBox::warning(this,QStringLiteral("移除失败"),error);
        runSearch();
    });
    menu.exec(globalPos);m_menu=nullptr;
}

PinnedApp SearchWindow::resultPinnedApp(const SearchResult& r) const
{
    if (r.kind==SearchResult::PinnedApp) for (const auto& app:PinnedApps::load()) if (app.id==r.target) return app;
    auto app=PinnedApps::fromTarget(r.kind==SearchResult::App && !r.extra.isEmpty()?r.extra:r.target);
    app.title=r.title;return app;
}
bool SearchWindow::canPinResult(const SearchResult& r) const
{
    return (r.kind==SearchResult::App || r.kind==SearchResult::File || r.kind==SearchResult::PinnedApp)
        && PinnedApps::supportsTarget(resultPinnedApp(r).targetPath);
}
void SearchWindow::resultAction(int index,bool pin)
{
    if (index<0 || index>=m_results.size()) return;
    const auto result=m_results[index];const auto app=resultPinnedApp(result);
    if (pin) {
        if (!canPinResult(result) || PinnedApps::isPinned(app)) return;
        QString error;
        if (!PinnedApps::add(app,-1,-1,&error)) QMessageBox::warning(this,QStringLiteral("固定失败"),error);
        m_list->viewport()->update();
    } else {
        QString path=PinnedApps::revealTarget(app);
        if (result.kind==SearchResult::App && QFileInfo::exists(result.target)) path=result.target;
        Platform::reveal(path);
    }
}

void SearchWindow::rebuildHome()
{
    const auto cfg = PinnedApps::config();
    QVector<LauncherGrid::Item> items;
    for (auto app : cfg.pinnedApps) {
        if (!app.groupId.isEmpty()) continue;
        items << LauncherGrid::Item{app,letterIcon(app.title,Theme::launcherColor("accent")),PinnedApps::targetExists(app)};
    }
    for (const auto& g:cfg.groups) {
        PinnedApp app;app.id=g.id;app.title=g.title;app.gridRow=g.gridRow;app.gridColumn=g.gridColumn;
        LauncherGrid::Item item{app,{},true,true};
        for (const auto& child:cfg.pinnedApps) if (child.groupId==g.id) item.children.append(child);
        std::sort(item.children.begin(),item.children.end(),[](const auto& a,const auto& b){return a.gridRow==b.gridRow?a.gridColumn<b.gridColumn:a.gridRow<b.gridRow;});
        items.append(item);
    }
    m_restoringPage=true;
    m_grid->setItems(items,cfg.gridColumns,cfg.gridRows,PinnedApps::pageCount(cfg));
    m_restoringPage=false;
    if (!m_openGroup.isEmpty()) openGroup(m_openGroup,m_groupGrid->currentPage());
}

void SearchWindow::setStatus(const QString& detail)
{
    if (!m_hint) return;
    m_hint->setToolTip(detail);
    QString text=detail;
    static const QRegularExpression counts(QStringLiteral("应用 (\\d+).*文件 (\\d+)"));
    const auto match=counts.match(detail);
    if (match.hasMatch()) text=QStringLiteral("索引 · %1 应用 · %2 文件").arg(match.captured(1),match.captured(2));
    else if (detail.contains("Enter") || detail.contains(QStringLiteral("呼出/隐藏"))) text=QStringLiteral("右键管理 · 搜索启动");
    m_hint->setText(m_hint->fontMetrics().elidedText(text,Qt::ElideRight,m_hint->width()));
}

void SearchWindow::setHotkeyLabel(const QString& label)
{
    if (label.isEmpty()) {
        setStatus("Enter/双击 运行 · ↑↓ 选择 · Esc 隐藏 · 右键 更多操作");
    } else {
        setStatus(QString("%1 呼出/隐藏 · Enter/双击 运行 · ↑↓ 选择 · Esc 隐藏 · 右键 更多操作")
                            .arg(label));
    }
}

void SearchWindow::paintEvent(QPaintEvent*)
{
    if (!m_paintLogged && m_showLatency.isValid()) {
        m_paintLogged = true;
        const qint64 ms = m_showLatency.elapsed();
        if (ms > 30)
            Utils::log(QStringLiteral("show: 慢启动器 首次绘制 +%1ms").arg(ms));
    }
    if (m_skin && m_skin->active()) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 13, 13);
    p.fillPath(path, Theme::launcherColor("panel"));
    p.setPen(QPen(Theme::launcherColor("panelEdge"), 1));
    p.drawPath(path);

}

SearchWindow::BlurAction SearchWindow::blurAction(bool hadForeground, bool backToPrevious,
                                                 qint64 msSinceShow, bool menuVisible, bool capturing)
{
    if (menuVisible)
        return BlurAction::StayMenuOpen;      // 自己的右键菜单开着，这条失焦不是"点走了"
    if (capturing)
        return BlurAction::WaitCaptureEnds;   // F1 截图进行中：等它结束再收（--capturehold 那一支）
    // **拿到过前台之后的失焦就是真点走了，立刻收。** 以前这里只看"离上次呼出够不够 500ms"，
    // 于是呼出后 0.5 秒内点走一律被当成激活竞争吞掉，要等 600ms 的复查才收 ——
    // 他 2026-10-21 说的"隐藏的时候会卡一下"就是这个等待（本机日志里这一支一天 35 次）。
    const bool inGrace = msSinceShow >= 0 && msSinceShow < 500;
    // 从来没拿到过前台 = 我们压根没成为前台，这条失焦不是"点走了"；
    // 拿到过、但前台**回到了呼出之前那个窗口** = 我们的激活被 Windows 撤回去（前台锁）。
    // 这两种都是 #40 立的那一支：当场 hide 就变成"按了没出来"，所以先不动，宽限一过自己复查。
    if (inGrace && (!hadForeground || backToPrevious))
        return BlurAction::IgnoreActivationRace;
    return BlurAction::HideNow;
}

bool SearchWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
#ifdef Q_OS_WIN
    if (eventType == QByteArrayLiteral("windows_generic_MSG") && message && result) {
        auto* msg = static_cast<MSG*>(message);
        if (msg->message == WM_NCHITTEST) {
            // WM_NCHITTEST 携带物理屏幕坐标；光标位置可能已变化，不能用于本条消息。
            POINT nativePos{static_cast<short>(LOWORD(msg->lParam)),static_cast<short>(HIWORD(msg->lParam))};
            ScreenToClient(reinterpret_cast<HWND>(winId()),&nativePos);
            const qreal dpr=devicePixelRatioF();
            const QPoint pos(qRound(nativePos.x/dpr),qRound(nativePos.y/dpr));
            const auto interactive=[this,pos](QWidget* widget) {
                return widget && widget->isVisible() && QRect(widget->mapTo(this,QPoint()),widget->size()).contains(pos);
            };
            // 底栏贴边后，交互控件仍优先于窗口边缘缩放区域。
            if (interactive(m_settingsBtn) || interactive(m_pageBar) || interactive(m_input)) {*result=HTCLIENT;return true;}
            constexpr int edge = 8;
            const bool left = pos.x() <= edge;
            const bool right = pos.x() >= width() - edge;
            const bool top = pos.y() <= edge;
            const bool bottom = pos.y() >= height() - edge;
            if (top && left) { *result = HTTOPLEFT; return true; }
            if (top && right) { *result = HTTOPRIGHT; return true; }
            if (bottom && left) { *result = HTBOTTOMLEFT; return true; }
            if (bottom && right) { *result = HTBOTTOMRIGHT; return true; }
            if (left) { *result = HTLEFT; return true; }
            if (right) { *result = HTRIGHT; return true; }
            if (top) { *result = HTTOP; return true; }
            if (bottom) { *result = HTBOTTOM; return true; }

            // 顶部空白边带作为标题区，输入框和设置入口仍保留正常点击行为。
            const auto areaInWindow = [this](QWidget* w) {
                return w ? QRect(w->mapTo(this, QPoint(0, 0)), w->size()) : QRect();
            };
            const QRect inputArea = areaInWindow(m_input);
            const QRect settingsArea = areaInWindow(m_settingsBtn);
            if (pos.y() < (m_skin && m_skin->active() ? m_skin->contentRect().top() : 64) && !inputArea.contains(pos)
                && !settingsArea.contains(pos)) {
                *result = HTCAPTION;
                return true;
            }
        }
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif
    return QWidget::nativeEvent(eventType, message, result);
}

bool SearchWindow::event(QEvent* e)
{
    if (e->type() == QEvent::ThemeChange && m_grid) {
        refreshSkin();
        rebuildHome();
    }

    if (e->type() == QEvent::WindowActivate)
        m_hadForeground = true;
    if (e->type() == QEvent::Hide && m_grid) {m_grid->cancelDrag();closeGroup();}
    if (e->type() == QEvent::WindowDeactivate) {
        if (m_grid) m_grid->cancelDrag();
        if (m_groupGrid) m_groupGrid->cancelDrag();
        if (QApplication::activePopupWidget() || QApplication::activeModalWidget()) return QWidget::event(e);
        // 设置页现在是非模态的，点去设置页时搜索框照样该收起（以前模态时不能这么判）
        const bool backToPrevious = m_prevFg
                                    && GetForegroundWindow() == reinterpret_cast<HWND>(m_prevFg);
        const BlurAction act = blurAction(m_hadForeground, backToPrevious,
                                          m_showTime.isValid() ? m_showTime.elapsed() : -1,
                                          m_menu && m_menu->isVisible(), CaptureState::isActive());
        Utils::log(QStringLiteral("hide: 失焦 → %1（拿到过前台=%2 距呼出=%3ms）")
                       .arg(act == BlurAction::HideNow ? QStringLiteral("立刻收")
                            : act == BlurAction::IgnoreActivationRace ? QStringLiteral("当激活竞争，600ms 后复查")
                            : act == BlurAction::WaitCaptureEnds ? QStringLiteral("等截图结束")
                            : QStringLiteral("菜单开着，不动"))
                           .arg(m_hadForeground ? 1 : 0)
                           .arg(m_showTime.isValid() ? m_showTime.elapsed() : -1));
        if (act == BlurAction::HideNow) {
            hide();
        } else if (act == BlurAction::WaitCaptureEnds) {
            if (m_captureHold && !m_captureHold->isActive())
                m_captureHold->start();   // 截图结束再收（见构造函数里的说明）
        } else if (act == BlurAction::IgnoreActivationRace) {
            // 宽限期里丢掉的这条失焦，以前没人再回头看，于是窗口能一直"可见但躲在别的程序后面"。
            // 那个状态本身没坏处，坏在它是"看不见却还活着"——下一次按热键的判据全靠它。
            // 所以宽限一过自己复查：确实不是活动窗口就收起。
            QTimer::singleShot(600, this, [this] {
                if (!isVisible() || isActiveWindow())
                    return;
                if (m_menu && m_menu->isVisible())
                    return;
                if (m_captureHold && CaptureState::isActive())
                    return;
                Utils::log(QStringLiteral("show: 失焦落在宽限里，复查后收起"));
                hide();
            });
        }
    }
    return QWidget::event(e);
}

bool SearchWindow::eventFilter(QObject* obj, QEvent* e)
{
    if (m_groupPanel && m_groupPanel->isVisible()) {
        if (e->type()==QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key()==Qt::Key_Escape) {
            if (m_groupGrid->isDragging()) m_groupGrid->cancelDrag();else closeGroup();
            return true;
        }
        if (e->type()==QEvent::MouseButtonPress && !QApplication::activePopupWidget() && !QApplication::activeModalWidget()) {
            const auto pos=static_cast<QMouseEvent*>(e)->globalPosition().toPoint();
            if (!QRect(m_groupPanel->mapToGlobal(QPoint()),m_groupPanel->size()).contains(pos)) {
                closeGroup();if (obj!=m_settingsBtn) return true;
            }
        }
    }
    if (m_list && obj==m_list->viewport() && (e->type()==QEvent::MouseButtonPress || e->type()==QEvent::MouseButtonRelease
        || e->type()==QEvent::MouseButtonDblClick || e->type()==QEvent::MouseMove)) {
        auto* mouse=static_cast<QMouseEvent*>(e);
        auto* item=m_list->itemAt(mouse->pos());
        int hit=-1;bool pin=false;
        if (item) {
            const int index=item->data(Qt::UserRole).toInt();
            if (index>=0 && index<m_results.size() && hasResultActions(m_results[index])) {
                const auto row=m_list->visualItemRect(item);
                pin=actionRect(row,true).contains(mouse->pos());
                if (actionRect(row,false).contains(mouse->pos()) || (pin && canPinResult(m_results[index]))) hit=index;
            }
        }
        if (e->type()==QEvent::MouseMove) {
            m_list->viewport()->setCursor(hit>=0?Qt::PointingHandCursor:Qt::ArrowCursor);
            m_list->viewport()->setToolTip(hit<0?QString():!pin?QStringLiteral("打开所在位置"):
                PinnedApps::isPinned(resultPinnedApp(m_results[hit]))?QStringLiteral("已固定"):QStringLiteral("固定到面板"));
        } else if (mouse->button()==Qt::LeftButton) {
            if (e->type()==QEvent::MouseButtonPress) {
                m_list->setProperty("actionPress",hit);m_list->setProperty("actionPin",pin);
                if (hit>=0) {m_list->setCurrentItem(item);return true;}
            } else {
                const int pressed=m_list->property("actionPress").isValid()?m_list->property("actionPress").toInt():-1;
                if (e->type()==QEvent::MouseButtonRelease) {
                    m_list->setProperty("actionPress",-1);
                    if (pressed>=0) {
                        if (hit==pressed && pin==m_list->property("actionPin").toBool()) resultAction(hit,pin);
                        return true;
                    }
                }
                if (hit>=0) return true;
            }
        }
    }
    if (obj==m_grid && e->type()==QEvent::KeyPress
        && static_cast<QKeyEvent*>(e)->key()==Qt::Key_Escape && !m_grid->isDragging()) {
        hide();
        return true;
    }
    if (obj == m_input && e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        if (m_homeScroll->isVisible() && (ke->key()==Qt::Key_PageUp || ke->key()==Qt::Key_PageDown)) {
            QApplication::sendEvent(m_grid,e);return true;
        }
        flushSearch();
        if ((ke->modifiers()&Qt::AltModifier) && (ke->key()==Qt::Key_P || ke->key()==Qt::Key_O)) {
            if (auto* item=m_list->currentItem()) resultAction(item->data(Qt::UserRole).toInt(),ke->key()==Qt::Key_P);
            return true;
        }
        if (m_homeScroll->isVisible() && (ke->key()==Qt::Key_Down || ke->key()==Qt::Key_Up
            || ke->key()==Qt::Key_Return || ke->key()==Qt::Key_Enter)) {
            m_grid->setFocus();
            QKeyEvent select(QEvent::KeyPress,Qt::Key_Right,Qt::NoModifier);
            QApplication::sendEvent(m_grid,&select);
            if (ke->key()==Qt::Key_Return || ke->key()==Qt::Key_Enter)
                QApplication::sendEvent(m_grid,ke);
            return true;
        }
        switch (ke->key()) {
        case Qt::Key_Escape:
            hide();
            return true;
        case Qt::Key_Up:
            moveCurrent(-1);
            return true;
        case Qt::Key_Down:
            moveCurrent(1);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            // Ctrl+Enter 在剪贴板条目上多一步"顺手粘出去"
            if ((ke->modifiers() & Qt::ControlModifier) && pasteClipToPrevious())
                return true;
            // Ctrl+Shift+Enter 快捷以管理员身份启动应用
            if ((ke->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) == (Qt::ControlModifier | Qt::ShiftModifier)) {
                const int row = m_list->currentRow();
                if (row >= 0) {
                    const int idx = m_list->item(row)->data(Qt::UserRole).toInt();
                    if (idx >= 0 && idx < m_results.size()) {
                        const SearchResult& r = m_results[idx];
                        if (r.kind == SearchResult::App) {
                            const QString target = r.extra.isEmpty() ? r.target : r.extra;
                            bumpUsage(target);
                            saveUsage();
                            hide();
                            Platform::launchAsAdmin(target);
                            return true;
                        }
                    }
                }
            }
            activateCurrent();
            return true;
        default:
            break;
        }
    }
    return QWidget::eventFilter(obj, e);
}

// ---------- 结果构建 ----------

void SearchWindow::scheduleSearch()
{
    m_debounce->start();
}

// 输入框有 120ms 的去抖：打字快一点、立刻按回车，列表还停在"少一个字母"的那一版。
// 搜 shutdown 却"什么都搜不出来"就是这么来的（他 2026-10-19 报的）—— 那一版列表是空的，
// 回车自然什么也没启动，而那 120ms 还没到。
void SearchWindow::flushSearch()
{
    if (m_debounce && m_debounce->isActive()) {
        m_debounce->stop();
        runSearch();
    }
}

void SearchWindow::refreshLauncher()
{
    if (m_debounce) m_debounce->stop();
    runSearch();
}

void SearchWindow::addSection(const QString& text)
{
    SearchResult r;
    r.kind = SearchResult::Info;
    r.actionable = false;
    r.isSection = true;
    r.title = text;
    m_results.append(r);
}

void SearchWindow::addItem(const SearchResult& r)
{
    m_results.append(r);
    if (!hasResultActions(r)) return;
    const int index=m_results.size()-1;
    const auto sequence=m_searchSeq;
    const auto pinned=resultPinnedApp(r);
    QString path=pinned.iconPath.isEmpty()?pinned.targetPath:pinned.iconPath;
    if (r.kind==SearchResult::App) path=r.extra.isEmpty()?r.target:r.extra;
    const qreal dpr=devicePixelRatioF();
    Utils::requestFileIcon(path,qCeil(28*dpr),this,[this,index,sequence,dpr](const QImage& image) {
        if (sequence!=m_searchSeq || index>=m_results.size() || image.isNull()) return;
        auto pix=QPixmap::fromImage(image);pix.setDevicePixelRatio(dpr);m_results[index].icon=QIcon(pix);m_list->viewport()->update();
    });
}

void SearchWindow::rebuildList()
{
    if (m_homeScroll)
        m_homeScroll->setVisible(false);
    m_list->clear();
    int firstActionable = -1;
    for (int i = 0; i < m_results.size(); ++i) {
        const SearchResult& r = m_results[i];
        QListWidgetItem* it = new QListWidgetItem(m_list);
        if (r.isSection) {
            it->setData(Qt::UserRole, -1);
            it->setText(r.title);
        } else {
            it->setData(Qt::UserRole, i);
        }
        if (firstActionable < 0 && r.actionable && !r.isSection)
            firstActionable = i;
    }
    if (firstActionable >= 0) {
        m_list->setCurrentRow(firstActionable);
        // setCurrentRow 会把当前行滚到视口顶部，于是第一节的表头被推出屏幕
        // （常用面板看起来少了"网站"那一节）。选中保留，滚动条拉回顶部。
        m_list->verticalScrollBar()->setValue(m_list->verticalScrollBar()->minimum());
    }
    // 空查询又没有列表（配置里关掉了"呼出时显示常用列表"）：连控件一起藏掉，
    // 不然它自己的最小高度会把窗口撑住，缩不成"只有一个搜索框"
    m_list->setVisible(!(m_list->count() == 0 && m_input->text().trimmed().isEmpty()));
    updateHeight();
}

void SearchWindow::runSearch()
{
    ++m_searchSeq;
    m_results.clear();
    m_regIssues.clear();

    const QString q = halfWidth(m_input->text()).trimmed();
    const QString low = q.toLower();

    const auto launcher = PinnedApps::config();
    if (q.isEmpty()) {
        // 每次刷新读取设置，避免呼出、清空搜索或关闭设置后又强制显示网格。
        const QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
        const bool showGrid = cfg.value("ui/favoritesOnEmpty", true).toBool();
        if (showGrid) rebuildHome();
        else { m_grid->cancelDrag(); closeGroup(); }
        m_homeScroll->setVisible(showGrid);
        m_list->setVisible(false);
        m_pageBar->setVisible(showGrid);
        updateHeight();
        return;
    }
    if (m_homeScroll)
        m_homeScroll->setVisible(false);

    m_pageBar->setVisible(false);
    // 搜索到的工具继续使用现有 DLL 插件入口。
    // 索引指的是 PluginHost::scan() 里那一条 —— 九个自带工具现在也是插件，这里没有特判。
    const auto addToolRow = [this](int i) {
        const PluginHost::Meta& td = PluginHost::scan()[i];
        SearchResult r;
        r.kind = SearchResult::Command;
        r.title = QStringLiteral("打开 %1").arg(td.name);
        r.subtitle = QStringLiteral("%1 · DLL 工具（点开才加载）").arg(td.desc);
        r.target = QStringLiteral("tool:%1").arg(td.id);
        r.icon = letterIcon(td.name, td.color);
        addItem(r);
    };

    // 插件目录里"通过检查"的那几条的下标，卡片墙和这里共用同一个顺序。
    // 没通过的不在结果里（工具页那面会单独把原因写出来），但也不能因为它就少一张卡。
    QVector<int> toolIndices;
    {
        const auto& metas = PluginHost::scan();
        for (int i = 0; i < metas.size(); ++i)
            if (metas[i].ok() && !PluginHost::isDisabled(metas[i].id))
                toolIndices << i;
    }

    // 命令区域：init / path / reg / rescan
    const QString tok0 = q.section(' ', 0, 0).toLower();
    const QString rest = q.mid(tok0.size()).trimmed();

    if (tok0 == "init") {
        handleInit(rest);
        rebuildList();
        return;
    }
    if (tok0 == "path") {
        handlePath(rest);
        rebuildList();
        return;
    }
    if (tok0 == "reg") {
        handleReg(rest);
        rebuildList();
        return;
    }

    // rescan 命令行
    if (tok0 == "rescan") {
        SearchResult r;
        r.kind = SearchResult::Command;
        r.title = "rescan — 重新扫描应用与文件索引";
        r.subtitle = "按 Enter 开始（桌面/开始菜单快捷方式 + 常用目录文件索引）";
        r.target = "rescan";
        r.icon = letterIcon("⟳", QColor(46, 160, 88));
        addItem(r);
        rebuildList();
        return;
    }

    // json 命令行（带内容）：直接把那串递给 JSON 工具那张页解析
    if (tok0 == "json" && !q.mid(tok0.size()).trimmed().isEmpty() && !PluginHost::isDisabled(QStringLiteral("json"))) {
        SearchResult r;
        r.kind = SearchResult::Command;
        r.title = "json — 把后面这串丢进 JSON 格式化";
        const QString payload = q.mid(tok0.size()).trimmed();
        r.subtitle = QStringLiteral("按 Enter 打开工具页并解析 %1 个字符").arg(payload.size());
        r.target = "tool:json";
        r.extra = payload;
        r.icon = letterIcon("{}", QColor(90, 140, 220));
        addItem(r);
        rebuildList();
        return;
    }

    // base64 命令行（带内容）：直接把那串递给编解码那张页
    if ((tok0 == "b64" || tok0 == "base64") && !q.mid(tok0.size()).trimmed().isEmpty() && !PluginHost::isDisabled(QStringLiteral("base64"))) {
        SearchResult r;
        r.kind = SearchResult::Command;
        r.title = "b64 — 把后面这串拿去解码";
        const QString payload = q.mid(tok0.size()).trimmed();
        r.subtitle = QStringLiteral("按 Enter 打开工具页并解码 %1 个字符").arg(payload.size());
        r.target = "tool:base64";
        r.extra = payload;
        r.icon = letterIcon("=", QColor(190, 130, 60));
        addItem(r);
        rebuildList();
        return;
    }

    // 剪贴板历史：clip 列出最近的条目，Enter 放回剪贴板，Ctrl+Enter 顺手粘出去
    if (tok0 == "clip" || low.contains(QStringLiteral("剪贴板"))) {
        auto& cs = ClipboardStore::instance();
        addSection(QStringLiteral("剪贴板历史"));
        if (!cs.enabled()) {
            SearchResult r;
            r.kind = SearchResult::Command;
            r.title = QStringLiteral("开启剪贴板记录");
            r.subtitle = QStringLiteral("现在没在记录（设置 → 配置 → 剪贴板 里也能关）。开启后复制过的文本和图片会被收进本地历史。");
            r.target = "clip:on";
            r.icon = letterIcon(QStringLiteral("刷"), QColor(190, 130, 60));
            addItem(r);
        } else if (cs.items().isEmpty()) {
            SearchResult r;
            r.kind = SearchResult::Command;
            r.title = QStringLiteral("还没有记录：复制点东西再回来");
            r.subtitle = QStringLiteral("上限 200 条；超过 256 KB 的文本不收");
            r.target = "clip:none";
            r.actionable = false;
            addItem(r);
        }
        int n = 0;
        for (const ClipboardStore::Item& it : cs.items()) {
            if (n >= 15)
                break;
            SearchResult r;
            r.kind = SearchResult::Command;
            r.title = ClipboardStore::summary(it);
            r.subtitle = QStringLiteral("%1 · Enter 放回剪贴板，Ctrl+Enter 直接粘贴")
                             .arg(it.when.toString("MM-dd hh:mm"));
            r.target = QStringLiteral("clip:%1").arg(n);
            QIcon ic = cs.iconFor(it);
            r.icon = ic.isNull() ? letterIcon(QStringLiteral("剪"), QColor(150, 110, 220)) : ic;
            addItem(r);
            ++n;
        }
        if (cs.enabled() && !cs.items().isEmpty()) {
            SearchResult r;
            r.kind = SearchResult::Command;
            r.title = QStringLiteral("清空剪贴板历史");
            r.subtitle = QStringLiteral("删除 %1 条记录（含图片文件）").arg(cs.items().size());
            r.target = "clip:clear";
            r.icon = letterIcon(QStringLiteral("×"), QColor(214, 69, 69));
            addItem(r);
        }
        rebuildList();
        return;
    }

    // 工具直达：卡片墙那些事，搜名字或关键词都能打开。
    // 现在九个自带工具和别人写的插件是同一张表（PluginHost::scan()），这里没有"内置"特判。
    // 三条规矩都是吃过亏才立起来的（2026-10-16 起，直达**没有"独占整页"这个特权了**，见下面那处注释；
    // 但前两条反而更要紧 —— 命中错了不再是"把别人的挤掉"，而是"第一行摆错东西"，回车就点错了）：
    // ① 命中只认开头（matchStart）—— 中段撞上是"名字里正好含这几个字母"，
    //    把它摆到第一行会把用户要的那个顶掉（搜 renwu 想要任务管理器，却被「开机启动分析」
    //    的关键词"计划任务"当成直达）；
    // ② 只有"整条查询就是这个工具名/关键词"（或关键词后面带空格接参数）才算直达，
    //    否则只算松散命中、排在末尾那一节；
    // ③ 拼音直达只认主读音（Pinyin::matchStart）—— 表里那些生僻读法没资格占第一行：
    //    搜 An 想要 Antigravity，结果「广告弹窗」的关键词"广告"有个读法 an（广 = guang,yan,an）
    //    被判成直达。
    // 网站那一节要用的东西先算出来：它得能在"工具直达"之前说话（见下面 webKeywordWins 那条）。
    const QVector<WebEntry> webs = WebShortcuts::loadWebs();
    const QString webTok = low.section(' ', 0, 0);
    // 显式配在网站上的关键字，优先级高于"工具名的首字母拼音"：配的 `gg` 就是 Google，
    // 不该被「广告弹窗」（guang gao → 首字母 gg）用拼音抢走整页。工具那边还有 `ggp`、`广告` 两条路。
    bool webKeywordWins = false;
    for (const WebEntry& w : webs)
        if (w.keys.contains(webTok) || w.name.compare(webTok, Qt::CaseInsensitive) == 0)
            webKeywordWins = true;

    // 电源那两件事（2026-10-19 他点名要的：输 sleep/休眠 就休眠，输 shutdown/关机 就关机）。
    // 中文词只认**整条相等** —— "关机后怎么恢复文件"是一句查询，前缀匹配会把它当成要点电源。
    // 英文词按"命令行形状"认：`shutdown`、`shutdown.exe`、`shutdown /s` 都是要动手，
    // `shutdown 怎么用` 是在查资料（尾巴是普通词，不是开关），不算。
    // `shut` 是同一动作的口头缩写（他 2026-10-19 点名要），走同一条尾巴规矩 ——
    // 所以 `shutter`、`shut down the pc` 这类尾巴是普通词的仍然只当查询。
    // 也不清场：应用/文件/网站那几节照常在下面列出来（直达那条 2026-10-16 吃过亏的规矩）。
    const auto powerWord = [&low](const char* word) {
        const QString w = QLatin1String(word);
        if (!low.startsWith(w))
            return false;
        const QString tail = low.mid(w.size());
        if (tail.isEmpty() || tail == QLatin1String(".exe"))
            return true;
        return tail.size() >= 2 && tail.startsWith(QLatin1Char(' '))
            && (tail.at(1) == QLatin1Char('/') || tail.at(1) == QLatin1Char('-'));
    };
    const bool wantHiber = low == QStringLiteral("休眠") || powerWord("sleep") || powerWord("hibernate");
    const bool wantOff = low == QStringLiteral("关机") || powerWord("shutdown") || powerWord("shut");
    if (wantHiber) {
        addSection(QStringLiteral("系统动作"));
        SearchResult r;
        r.kind = SearchResult::Command;
        r.title = QStringLiteral("休眠（Hibernate）");
        r.subtitle = SysTools::hibernationEnabled()
            ? QStringLiteral("按 Enter 立刻休眠这台电脑（回到桌面时东西都还在）")
            : QStringLiteral("这台机器现在没开休眠，按 Enter 会失败 —— 管理员命令行跑一次 powercfg /h on");
        r.target = QStringLiteral("power:hibernate");
        r.icon = letterIcon(QStringLiteral("眠"), QColor(120, 90, 200));
        addItem(r);
    } else if (wantOff) {
        addSection(QStringLiteral("系统动作"));
        SearchResult r;
        r.kind = SearchResult::Command;
        r.title = QStringLiteral("关机");
        r.subtitle = QStringLiteral("按 Enter 立刻关闭这台电脑 —— 这条路不问第二遍（点名要的快路径）");
        r.target = QStringLiteral("power:shutdown");
        r.icon = letterIcon(QStringLiteral("关"), QColor(214, 69, 69));
        addItem(r);
    }

    QVector<int> toolLoose;
    QVector<int> toolExact;   // 工具直达命中的那几条。**只算不发** —— 谁排第一由下面那段决定
    {
        const auto& metas = PluginHost::scan();
        if (low == QStringLiteral("工具") || low == QStringLiteral("tools")
            || low == QStringLiteral("工具箱")) {   // 想看全部就搜"工具"
            addSection(QStringLiteral("工具"));
            for (const int i : std::as_const(toolIndices))
                addToolRow(i);
            rebuildList();
            return;
        }
        for (const int i : std::as_const(toolIndices)) {
            const PluginHost::Meta& td = metas[i];
            const QString name = td.name.toLower();
            bool hitExact = (low == name);
            bool hitLoose = low.contains(name);
            for (const QString& kw : td.keywords) {
                const QString k = kw.toLower();
                if (k.isEmpty())
                    continue;
                if (low == k || low.startsWith(k + QLatin1Char(' ')))
                    hitExact = true;
                if (low.contains(k))
                    hitLoose = true;
            }
            // 拼音直达：cipanqingli / cpql 也算"就要这个工具"。工具就卡片墙那几件事，
            // 命中面比应用列表窄得多，所以首字母也当直达（应用那边首字母只排第三档）。
            if (!hitExact && Pinyin::usableQuery(low)) {
                if (Pinyin::matchStart(td.name, low) != Pinyin::None)
                    hitExact = true;
                else
                    for (const QString& kw : td.keywords)
                        if (Pinyin::matchStart(kw, low) != Pinyin::None) {
                            hitExact = true;
                            break;
                        }
            }
            if (hitExact)
                toolExact << i;
            else if (hitLoose)
                toolLoose << i;
        }
    }

    // 应用匹配。从原来"应用搜索"那一段搬到这里，是因为"谁排第一"要同时知道
    // 直达命中没有和应用命中没有 —— 在发射点之后才算就来不及了。
    struct Match { int score; int idx; };    QVector<Match> ms;
    for (int i = 0; i < m_apps.size(); ++i) {
        const int s = scoreApp(m_apps[i], low);
        if (s >= 0)
            ms.append({s, i});
    }
    std::sort(ms.begin(), ms.end(), [this](const Match& a, const Match& b) {
        if (a.score != b.score)
            return a.score < b.score;
        const int ua = m_usage.value(usageKey(m_apps[a.idx].lnkPath, m_apps[a.idx].target), 0);
        const int ub = m_usage.value(usageKey(m_apps[b.idx].lnkPath, m_apps[b.idx].target), 0);
        if (ua != ub)
            return ua > ub;
        return m_apps[a.idx].name.size() < m_apps[b.idx].name.size();
    });

    // 节的先后（他 2026-10-21 拍的）：**没命中直达时应用第一、网站第二**；
    // 命中工具直达时直达仍占第一行、回车还是它（10-19 定的一条，他这次选的是"别让位"）。
    // 电源那两条（休眠/关机）也是直达，在上面就先发 —— 那支要是让位，输 `shutdown`
    // 回车可能变成启动一个名字像 shutdown 的应用。
    // 直达**不再清场**。以前这里 `return`，后面「网站 / 网址直开 / 应用 / 文件 / 系统 /
    // 剪贴板历史」五节整个没跑到 —— 那是一条有特权的路，拿真实的 apps.json 复算过：
    // 这台机器上 51 个查询串会命中它、连带 44 个应用永远搜不出来（最刺眼的是 `windows`：
    // 「Windows 用户名」那把工具的名字本身就是 windows 开头，于是所有 Windows* 应用没了）。
    const bool toolFirst = !toolExact.isEmpty() && !webKeywordWins;
    if (toolFirst) {
        addSection(QStringLiteral("工具"));
        for (const int i : std::as_const(toolExact))
            addToolRow(i);
        // 松散命中的那几个并进**同一节**（强的在前、弱的在后），末尾那个"工具"标题就不再重复
        for (const int i : std::as_const(toolLoose))
            addToolRow(i);
        toolLoose.clear();
    }

    // 固定软件与面板文件夹使用同一节搜索结果，但保留不同 kind：名称、分类和目标路径均可匹配。
    // 真实磁盘目录可以定位/固定，面板文件夹只能打开和管理分组；固定项始终参与搜索。
    {
        struct PinnedMatch { int score = 99; PinnedApp app; };
        QVector<PinnedMatch> matches;
        for (const PinnedApp& app : PinnedApps::load()) {
            if (!app.enabled)
                continue;
            const QString title = app.title.toLower();
            const QString category = app.category.toLower();
            const QString path = app.targetPath.toLower();
            int score = -1;
            if (title.startsWith(low))
                score = 0;
            else if (title.contains(low))
                score = 1;
            else if (category.contains(low))
                score = 2;
            else if (QFileInfo(path).fileName().contains(low))
                score = 3;
            else if (path.contains(low))
                score = 4;
            if (score >= 0)
                matches.append({score, app});
        }
        std::sort(matches.begin(), matches.end(), [](const PinnedMatch& a, const PinnedMatch& b) {
            if (a.score != b.score)
                return a.score < b.score;
            return a.app.title.toLower() < b.app.title.toLower();
        });
        bool addedPinnedSection = false;
        if (!matches.isEmpty()) {
            addSection(QStringLiteral("── 固定项 ──"));
            addedPinnedSection = true;
            for (const PinnedMatch& match : matches) {
                const PinnedApp& app = match.app;
                SearchResult r;
                r.kind = SearchResult::PinnedApp;
                r.title = app.title;
                r.subtitle = app.category.isEmpty()
                    ? app.targetPath : QStringLiteral("%1 · %2").arg(app.category, app.targetPath);
                r.target = app.id;
                const QString iconPath = app.iconPath.isEmpty() ? app.targetPath : app.iconPath;
                r.icon = Utils::cachedFileIcon(iconPath);
                if (r.icon.isNull())
                    r.icon = letterIcon(app.title, QColor(30, 150, 220));
                addItem(r);
            }
        }
        for (const auto& group : PinnedApps::config().groups) {
            if (!group.title.contains(low, Qt::CaseInsensitive))
                continue;
            if (!addedPinnedSection) {
                addSection(QStringLiteral("── 固定项 ──"));
                addedPinnedSection = true;
            }
            SearchResult r;
            r.kind = SearchResult::PinnedGroup;
            r.title = group.title;
            r.subtitle = QStringLiteral("面板文件夹 · 固定面板");
            r.target = group.id;
            r.icon = QApplication::style()->standardIcon(QStyle::SP_DirIcon);
            addItem(r);
        }
    }

    if (!ms.isEmpty())
        addSection("── 应用 ──");
    const int appLimit = qMin(8, ms.size());
    for (int k = 0; k < appLimit; ++k) {
        const AppEntry& a = m_apps[ms[k].idx];
        SearchResult r;
        r.kind = SearchResult::App;
        r.title = a.name;
        r.subtitle = a.target.isEmpty() ? a.lnkPath : a.target;
        r.target = a.target;
        r.extra = a.lnkPath;
        if (!a.iconFile.isEmpty())
            r.icon = QIcon(a.iconFile);
        else if (a.category == QLatin1String("system")
                 || a.category == QLatin1String("appsfolder"))
            r.icon = Utils::letterIcon(a.name, QColor(31, 141, 160));
        addItem(r);
    }

    // 快捷搜索只取第一个完整词，后续内容不会拼接到目标地址或命令参数。
    // 兜底那条以前单独甩在整个列表最末尾，等于"网站"这一类排到了最后 —— 现在三条挨着放。
    // webs / webTok 在工具那一节之前就算好了。
    // 这一组**以前没有表头**：那几行直接跟在上一节的表头（搜 `b64` 时就是"工具"）底下，
    // 看着像工具的结果。补一个快捷搜索分组表头。
    const int webMark = m_results.size();
    for (const WebEntry& w : webs) {
        if (!w.keys.contains(webTok) && w.name.compare(webTok, Qt::CaseInsensitive) != 0)
            continue;
        SearchResult r;
        r.kind = SearchResult::QuickSearch;
        r.title = w.name;
        const QString target = w.target.isEmpty() ? w.url : w.target;
        const QString typeLabel = w.targetType == QLatin1String("folder") ? QStringLiteral("磁盘文件夹")
            : w.targetType == QLatin1String("app") ? QStringLiteral("应用或脚本") : QStringLiteral("网页");
        r.subtitle = QStringLiteral("快捷搜索 · %1 · %2").arg(typeLabel, target);
        r.target = target;
        r.extra = w.targetType;
        r.icon = w.targetType == QLatin1String("folder") ? QApplication::style()->standardIcon(QStyle::SP_DirIcon)
            : w.targetType == QLatin1String("app") ? Utils::cachedFileIcon(target) : letterIcon(w.name, colorForKind(target));
        if (r.icon.isNull()) r.icon = letterIcon(w.name, colorForKind(target));
        addItem(r);
        break;
    }
    if (WebShortcuts::looksLikeUrl(q)) {
        SearchResult r;
        r.kind = SearchResult::Web;
        r.title = WebShortcuts::normalizeUrl(q);
        r.subtitle = "在默认浏览器打开网址";
        r.target = WebShortcuts::normalizeUrl(q);
        r.icon = letterIcon("网", QColor(31, 141, 160));
        addItem(r);
    }
    // 网络搜索兜底（始终提供）
    if (low.size() >= 2) {
        SearchResult r;
        r.kind = SearchResult::Web;
        r.title = QString("用搜索引擎搜索 “%1”").arg(q);
        r.subtitle = WebShortcuts::engineSearchUrl(q);
        r.target = WebShortcuts::engineSearchUrl(q);
        r.icon = letterIcon("搜", QColor(136, 87, 229));
        addItem(r);
    }
    if (m_results.size() > webMark) {
        SearchResult h;
        h.kind = SearchResult::Info;
        h.actionable = false;
        h.isSection = true;
        h.title = QStringLiteral("── 快捷搜索 ──");
        m_results.insert(webMark, h);   // 表头补在这一组最前面（列表是 rebuildList() 时按 m_results 铺的）
    }

    if (!toolLoose.isEmpty()) {
        addSection(QStringLiteral("工具"));
        for (const int i : std::as_const(toolLoose))
            addToolRow(i);
    }

    // 文件搜索（≥2 个字符才搜，避免太泛）：丢到线程池，结果稍后插回列表
    if (low.size() >= 2)
        startFileSearch(q);

    if (m_results.isEmpty()) {
        SearchResult r;
        r.kind = SearchResult::Info;
        r.actionable = false;
        r.title = "无匹配结果";
        addItem(r);
    }
    rebuildList();
}

// 深索引有数百万条，一次扫描上百毫秒：放到线程池里跑，输入不卡；
// 同一时刻只允许一个在跑，跑完若期间又换了查询就用最新输入重来一次
void SearchWindow::startFileSearch(const QString& q)
{
    if (m_fileSearchBusy) {
        m_fileSearchRerun = true;
        return;
    }
    m_fileSearchBusy = true;
    const quint64 seq = m_searchSeq;

    auto* watcher = new QFutureWatcher<QVector<FileEntry>>(this);
    connect(watcher, &QFutureWatcher<QVector<FileEntry>>::finished, this,
            [this, watcher, seq, q] {
                const auto files = watcher->result();
                watcher->deleteLater();
                m_fileSearchBusy = false;
                if (m_fileSearchRerun) {
                    m_fileSearchRerun = false;
                    runSearch();
                    return;
                }
                if (seq != m_searchSeq || halfWidth(m_input->text()).trimmed() != q)
                    return;   // 查询已变，结果作废
                if (files.isEmpty())
                    return;

                for (int i = m_results.size() - 1; i >= 0; --i)
                    if (m_results[i].kind == SearchResult::Info
                        && m_results[i].title == QStringLiteral("无匹配结果"))
                        m_results.removeAt(i);
                int pos = m_results.size();
                // 插在"用搜索引擎搜索…"那条兜底行之前 = 应用/网站/工具之后。
                // 以前是找第一条 Web 行，于是命中网站快捷方式时文件会被顶到最上面。
                const QString fallbackUrl = WebShortcuts::engineSearchUrl(q);
                for (int i = 0; i < m_results.size(); ++i) {
                    if (m_results[i].kind == SearchResult::Web
                        && m_results[i].target == fallbackUrl) {
                        pos = i;
                        break;
                    }
                }
                SearchResult head;
                head.kind = SearchResult::Info;
                head.actionable = false;
                head.isSection = true;
                head.title = QStringLiteral("── 文件 ──");
                m_results.insert(pos++, head);
                for (const FileEntry& fe : files) {
                    SearchResult r;
                    r.kind = SearchResult::File;
                    r.title = fe.name;
                    r.subtitle = fe.path;
                    r.target = fe.path;
                    r.icon = Utils::cachedFileIcon(fe.path);
                    m_results.insert(pos++, r);
                }
                rebuildList();
            });
    watcher->setFuture(QtConcurrent::run(
        [q]() { return FileIndexer::instance().search(q, 8); }));
}

// ---------- 激活 ----------

int SearchWindow::firstActionableRow() const
{
    for (int i = 0; i < m_list->count(); ++i) {
        const int idx = m_list->item(i)->data(Qt::UserRole).toInt();
        if (idx >= 0 && idx < m_results.size() && m_results[idx].actionable
            && m_results[idx].kind != SearchResult::Info)
            return i;
    }
    return -1;
}

void SearchWindow::moveCurrent(int dir)
{
    int row = m_list->currentRow();
    if (row < 0)
        row = firstActionableRow();
    int steps = m_list->count();
    while (steps-- > 0) {
        row += dir;
        if (row < 0 || row >= m_list->count())
            return;
        const int idx = m_list->item(row)->data(Qt::UserRole).toInt();
        if (idx >= 0 && idx < m_results.size() && m_results[idx].actionable
            && m_results[idx].kind != SearchResult::Info) {
            m_list->setCurrentRow(row);
            return;
        }
    }
}

void SearchWindow::activateCurrent()
{
    flushSearch();          // 先把还压着的那一拍补上，别拿"少一个字母"的旧列表当结果
    const int row = m_list->currentRow();
    if (row < 0)
        return;
    activateRow(row);
}

QString SearchWindow::currentCommandTarget() const
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_list->count())
        return QString();
    const int idx = m_list->item(row)->data(Qt::UserRole).toInt();
    if (idx < 0 || idx >= m_results.size())
        return QString();
    const SearchResult& r = m_results[idx];
    return r.kind == SearchResult::Command ? r.target : QString();
}

// 放回剪贴板后，把输入焦点还给呼出启动器之前的那个窗口，再补一次 Ctrl+V。
// 不这么做的话"从历史里挑一条"还要再手动按一次粘贴。
bool SearchWindow::pasteClipToPrevious()
{
    const QString target = currentCommandTarget();
    if (!target.startsWith(QLatin1String("clip:")))
        return false;
    bool ok = false;
    const int n = target.mid(5).toInt(&ok);
    if (!ok)
        return false;
    ClipboardStore::instance().restore(n);
    hide();
    auto* fg = reinterpret_cast<HWND>(m_prevFg);
    if (!fg || !IsWindow(fg))
        return true;                      // 找不到原窗口就只放回剪贴板，别乱发按键
    const DWORD fgThread = GetWindowThreadProcessId(fg, nullptr);
    const DWORD cur = GetCurrentThreadId();
    const bool attached = fgThread && fgThread != cur && AttachThreadInput(cur, fgThread, TRUE);
    SetForegroundWindow(fg);
    keybd_event(VK_CONTROL, 0, 0, 0);
    keybd_event('V', 0, 0, 0);
    keybd_event('V', 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
    if (attached)
        AttachThreadInput(cur, fgThread, FALSE);
    m_prevFg = 0;
    return true;
}

void SearchWindow::activateRow(int row)
{
    if (row < 0 || row >= m_list->count())
        return;
    const int idx = m_list->item(row)->data(Qt::UserRole).toInt();
    if (idx < 0 || idx >= m_results.size())
        return;
    const SearchResult r = m_results[idx];

    switch (r.kind) {
    case SearchResult::PinnedGroup:
        openGroup(r.target);
        break;
    case SearchResult::PinnedApp: {
        const auto apps = PinnedApps::load();
        auto it = std::find_if(apps.cbegin(), apps.cend(), [&r](const PinnedApp& app) {
            return app.id == r.target;
        });
        if (it == apps.cend()) {
            setStatus(QStringLiteral("固定软件已经不存在，请到设置中重新添加。"));
            break;
        }
        QString error;
        if (!PinnedApps::launch(*it, &error)) {
            setStatus(error);
            break;
        }
        hide();
        break;
    }
    case SearchResult::App: {
        // 键和排序那边同一口径：没有 .lnk 的应用用 target 记，别挤到同一个空键上
        bumpUsage(r.extra.isEmpty() ? r.target : r.extra);
        saveUsage();
        hide();   // 先收窗再启动：带 DDE 段的 .lnk 同步执行要等握手回调（实测 729ms）
        // 优先通过 lnk 启动（保留参数/工作目录）；异步拿不到失败回执，所以只在没有 lnk 时退到目标
        Platform::launch(r.extra.isEmpty() ? r.target : r.extra, {}, {}, true);
        break;
    }
    case SearchResult::File:
    case SearchResult::Web: {   // ShellExecute 会走默认程序/默认浏览器
        // 常用面板要按"真的打开过"排序，所以文件和书签也记使用次数（"用搜索引擎搜索…"不算书签）
        if (r.kind == SearchResult::File)
            bumpUsage(QStringLiteral("file:") + r.target);
        else if (!r.title.startsWith(QStringLiteral("用搜索引擎搜索")))
            bumpUsage(QStringLiteral("web:") + r.title);
        saveUsage();
        hide();
        Platform::launch(r.target, {}, {}, true);
        break;
    }
    case SearchResult::QuickSearch: {
        bumpUsage(QStringLiteral("quick:") + r.title);
        saveUsage();
        hide();
        if (r.extra == QLatin1String("app"))
            Platform::launch(r.target, {}, {}, true);
        else if (r.extra == QLatin1String("folder"))
            Platform::reveal(r.target);
        else
            Platform::launch(WebShortcuts::normalizeUrl(r.target), {}, {}, true);
        break;
    }
    case SearchResult::Command: {
        if (r.target == "rescan") {
            setStatus("正在重新扫描，完成后会自动刷新…");
            startScan(false, true);
        } else if (r.target == QLatin1String("power:hibernate")
                   || r.target == QLatin1String("power:shutdown")) {
            // 动手之前不弹确认框（他要的就是"回车直接执行"），但**失败必须就地写在提示行上、
            // 窗口不许收** —— "按了没反应"是这程序里最难查的形状。
            const bool hib = r.target.endsWith(QLatin1String("hibernate"));
            const QString err = hib ? SysTools::hibernateNow() : SysTools::shutDownNow();
            if (err.isEmpty()) {
                Utils::log(QStringLiteral("power: %1 已叫起来（回车直达）")
                               .arg(hib ? QStringLiteral("休眠") : QStringLiteral("关机")));
                hide();
            } else {
                Utils::log(QStringLiteral("power: %1").arg(err));
                setStatus(err);
            }
        } else if (r.target.startsWith(QLatin1String("tool:"))) {
            // 工具就是 DLL 插件：先弹设置页，再展开那张卡；r.extra 有内容就顺手递给那页
            hide();
            openTool(r.target.mid(5), r.extra);
        } else if (r.target.startsWith(QLatin1String("clip:"))) {
            const QString arg = r.target.mid(5);
            if (arg == QLatin1String("on")) {
                ClipboardStore::instance().setEnabled(true);
                setStatus(QStringLiteral("已开始记录剪贴板（设置 → 配置 里可以关掉）"));
                runSearch();
            } else if (arg == QLatin1String("clear")) {
                ClipboardStore::instance().clear();
                setStatus(QStringLiteral("剪贴板历史已清空"));
                runSearch();
            } else if (arg != QLatin1String("none")) {
                ClipboardStore::instance().restore(arg.toInt());
                hide();
            }
        }
        break;
    }
    case SearchResult::PluginEntry: {
        m_input->setText("init " + r.target + " ");
        m_input->setFocus();
        break;
    }
    case SearchResult::PluginItem: {
        // r.target = 插件 id, r.extra = 组件 label
        const QString key = r.target + "|" + r.extra;
        DevItem it = m_detectItems.value(key);
        if (it.ok) {
            m_detecting.remove(r.target);
            m_detectItems.remove(key);
            runSearch();
        } else if (!it.wingetId.isEmpty()) {
            QString err;
            DevEnv::runInstall(this, it, &err);
            m_detectItems.remove(key);
            setStatus("安装完成或取消后，按 Enter 重新检测");
            runSearch();
        } else if (!it.note.isEmpty()) {
            setStatus(it.note);
        }
        break;
    }
    case SearchResult::RegIssue: {
        const int issueIdx = r.target.toInt();
        if (issueIdx < 0 || issueIdx >= m_regIssues.size())
            break;
        QString err;
        if (RegClean::deleteIssue(m_regIssues[issueIdx], &err)) {
            setStatus("已删除并备份。重新扫描中…");
        } else {
            setStatus(err);
        }
        runSearch();
        break;
    }
    case SearchResult::Info:
        break;
    }
}

// ---------- init：开发环境 ----------

void SearchWindow::handleInit(const QString& arg)
{
    const QVector<DevPlugin> plugins = DevEnv::loadPlugins();

    if (arg.isEmpty()) {
        addSection("── 开发环境插件 ──");
        for (const DevPlugin& p : plugins) {
            SearchResult r;
            r.kind = SearchResult::PluginEntry;
            r.title = p.name;
            r.subtitle = QString("init %1 · %2 个组件 · Enter 查看")
                             .arg(p.id).arg(p.items.size());
            r.target = p.id;
            r.icon = letterIcon(p.name, colorForKind(p.id));
            addItem(r);
        }
        SearchResult tip;
        tip.kind = SearchResult::Info;
        tip.actionable = false;
        tip.title = "插件定义在 %APPDATA%\\ltool\\plugins，可自行添加";
        addItem(tip);
        return;
    }

    // 按 id 精确/前缀匹配插件
    const DevPlugin* found = nullptr;
    for (const DevPlugin& p : plugins)
        if (p.id.compare(arg, Qt::CaseInsensitive) == 0)
            found = &p;
    if (!found) {
        QVector<const DevPlugin*> pref;
        for (const DevPlugin& p : plugins)
            if (p.id.startsWith(arg, Qt::CaseInsensitive))
                pref.append(&p);
        if (pref.size() == 1)
            found = pref.first();
    }
    if (!found) {
        QStringList ids;
        for (const DevPlugin& p : plugins)
            ids.append(p.id);
        SearchResult r;
        r.kind = SearchResult::Info;
        r.actionable = false;
        r.title = QString("未找到插件：%1").arg(arg);
        r.subtitle = ids.isEmpty() ? QString() : "可用插件：" + ids.join(", ");
        addItem(r);
        return;
    }

    DevPlugin p = *found;
    addSection(QString("── %1 (init %2) ──").arg(p.name, p.id));

    // 检查已有检测结果的覆盖情况：全部有结果 → 展示；部分缺失 → 展示+补检
    int cachedCount = 0;
    for (const DevItem& it : p.items) {
        if (m_detectItems.contains(p.id + "|" + it.label))
            ++cachedCount;
    }
    const bool allCached = cachedCount == p.items.size();
    const bool someCached = cachedCount > 0;

    for (const DevItem& it : p.items) {
        const QString key = p.id + "|" + it.label;
        SearchResult r;
        if (m_detectItems.contains(key)) {
            const DevItem det = m_detectItems.value(key);
            r.kind = SearchResult::PluginItem;
            r.title = it.label;
            r.target = p.id;
            r.extra = it.label;
            r.icon = letterIcon(det.ok ? "✓" : "!",
                                det.ok ? QColor(46, 160, 88) : QColor(214, 128, 15));
            if (det.ok) {
                r.subtitle = "已安装 · " + det.version;
            } else if (!det.wingetId.isEmpty()) {
                r.subtitle = QString("未安装 · Enter 用 winget 安装 (%1)").arg(det.wingetId);
            } else {
                r.subtitle = det.note.isEmpty() ? "未检测到" : det.note;
            }
        } else {
            r.kind = SearchResult::Info;
            r.actionable = false;
            r.title = it.label;
            r.subtitle = someCached ? "检测中…" : "检测中…";
        }
        addItem(r);
    }
    if (allCached) {
        SearchResult tip;
        tip.kind = SearchResult::Info;
        tip.actionable = false;
        tip.title = "Enter：已安装项重新检测 / 缺失项安装";
        addItem(tip);
        return;
    }
    if (m_detecting.contains(p.id))
        return;
    m_detecting.insert(p.id);

    const int run = ++m_detectRun;
    const QString id = p.id;
    auto future = QtConcurrent::run([p]() mutable -> QHash<QString, DevItem> {
        DevEnv::detect(&p);
        QHash<QString, DevItem> map;
        for (const DevItem& it : p.items)
            map.insert(p.id + "|" + it.label, it);
        return map;
    });
    auto* watcher = new QFutureWatcher<QHash<QString, DevItem>>(this);
    connect(watcher, &QFutureWatcher<QHash<QString, DevItem>>::finished, this,
            [this, watcher, run, id] {
                watcher->deleteLater();
                if (run != m_detectRun)
                    return;
                m_detecting.remove(id);
                const QHash<QString, DevItem> res = watcher->result();
                for (auto it = res.begin(); it != res.end(); ++it)
                    m_detectItems.insert(it.key(), it.value());
                const QString cur = m_input->text();
                if (cur.section(' ', 0, 0).compare("init", Qt::CaseInsensitive) == 0)
                    runSearch();
            });
    watcher->setFuture(future);
}

// ---------- path：环境变量 ----------

void SearchWindow::handlePath(const QString& rest)
{
    const QString t = rest.trimmed();
    if (t.startsWith("add", Qt::CaseInsensitive)) {
        QString dir = t.mid(3).trimmed();
        dir.remove('"');
        if (dir.isEmpty()) {
            SearchResult r;
            r.kind = SearchResult::Info;
            r.actionable = false;
            r.title = "用法：path add <目录>";
            addItem(r);
            return;
        }
        QString err;
        const bool ok = EnvPath::addToUserPath(dir, &err);
        SearchResult r;
        r.kind = SearchResult::Info;
        r.actionable = false;
        r.title = ok ? QString("已加入用户 PATH：%1").arg(QDir::toNativeSeparators(dir))
                     : err;
        r.subtitle = ok ? "对新开的终端/程序生效（已广播 WM_SETTINGCHANGE）" : QString();
        addItem(r);
        return;
    }
    if (t.startsWith("del", Qt::CaseInsensitive)) {
        QString dir = t.mid(3).trimmed();
        dir.remove('"');
        QString err;
        const bool ok = EnvPath::removeFromUserPath(dir, &err);
        SearchResult r;
        r.kind = SearchResult::Info;
        r.actionable = false;
        r.title = ok ? QString("已从 PATH 移除：%1").arg(dir) : err;
        addItem(r);
        return;
    }

    // path / path list：列出
    const QStringList entries = EnvPath::userPathEntries();
    addSection(QString("── 用户 PATH（共 %1 项）──").arg(entries.size()));
    for (const QString& e : entries) {
        SearchResult r;
        r.kind = SearchResult::Info;
        r.actionable = false;
        r.title = Platform::expandEnv(e);
        r.subtitle = e == r.title ? QString() : e;
        addItem(r);
    }
    SearchResult tip;
    tip.kind = SearchResult::Info;
    tip.actionable = false;
    tip.title = "path add <目录> 添加 · path del <目录> 移除 · 应用结果右键也可加入 PATH";
    addItem(tip);
}

// ---------- reg：注册表整理 ----------

void SearchWindow::handleReg(const QString& rest)
{
    const QString t = rest.trimmed();

    if (t.startsWith("clean", Qt::CaseInsensitive)) {
        // 命令行里只自动删"安全"级（启动项/缓存/App Paths/遗留键）；
        // 卸载记录、服务、文件关联、COM 这些要人去分析页看着明细勾，别一句话盲删上百个键
        QVector<RegIssue> all = RegClean::quickScan();
        QVector<RegIssue> issues;
        int held = 0;
        for (const RegIssue& it : all) {
            if (it.deletable() && it.risk == RegIssue::Risk::Safe)
                issues.append(it);
            else if (it.deletable())
                ++held;
        }
        if (issues.isEmpty()) {
            SearchResult r;
            r.kind = SearchResult::Info;
            r.actionable = false;
            r.title = held > 0
                ? QString("没有可以自动删的项（另有 %1 项属于谨慎级，去 工具 → 注册表分析 里看明细）").arg(held)
                : QString("注册表很干净，没有发现残留项");
            addItem(r);
            return;
        }
        int okCount = 0;
        QStringList fails;
        for (const RegIssue& it : issues) {
            QString err;
            if (RegClean::deleteIssue(it, &err))
                ++okCount;
            else
                fails.append(QString("%1：(%2)").arg(it.title, err));
        }
        SearchResult r;
        r.kind = SearchResult::Info;
        r.actionable = false;
        r.title = QString("已清理 %1/%2 项（每项删除前已自动备份 .reg）")
                      .arg(okCount).arg(issues.size());
        r.subtitle = QString("备份目录：%1%2")
                         .arg(QDir::toNativeSeparators(Utils::backupsDir()),
                              held > 0
                                  ? QString(" · 另有 %1 项谨慎级没动，去 工具 → 注册表分析 里勾").arg(held)
                                  : QString());
        addItem(r);
        for (const QString& f : fails) {
            SearchResult fr;
            fr.kind = SearchResult::Info;
            fr.actionable = false;
            fr.title = f;
            addItem(fr);
        }
        return;
    }

    // reg / reg scan：列出问题。搜索框里只做快的那几类，
    // COM 组件要遍历上万个键，留给「工具 → 注册表分析」那页去跑
    const QVector<RegIssue> issues = RegClean::quickScan();
    addSection("── 注册表体检 ──");
    if (issues.isEmpty()) {
        SearchResult r;
        r.kind = SearchResult::Info;
        r.actionable = false;
        r.title = "未发现无效项（卸载残留 / 启动项失效 / 缓存残留）";
        addItem(r);
        return;
    }
    SearchResult head;
    head.kind = SearchResult::Info;
    head.actionable = false;
    head.title = QString("发现 %1 个无效项 · Enter 删除单项（自动备份）· reg clean 全部清理")
                     .arg(issues.size());
    addItem(head);

    m_regIssues = issues;
    for (int i = 0; i < issues.size(); ++i) {
        const RegIssue& it = issues[i];
        SearchResult r;
        r.kind = SearchResult::RegIssue;
        r.title = it.title;
        r.subtitle = QString("[%1] %2").arg(RegClean::categoryLabel(it), it.detail);
        r.target = QString::number(i);
        r.icon = letterIcon("C", QColor(214, 69, 69));
        addItem(r);
    }
}

// ---------- 扫描 ----------

void SearchWindow::startScan(bool quiet, bool forceDeep)
{
    if (!m_scanning.testAndSetRelaxed(0, 1))
        return;
    auto future = QtConcurrent::run([]() -> QVector<AppEntry> {
        const auto list = AppScanner::scan();
        AppScanner::saveIndex(list);
        return list;
    });
    auto* watcher = new QFutureWatcher<QVector<AppEntry>>(this);
    connect(watcher, &QFutureWatcher<QVector<AppEntry>>::finished, this, [this, watcher, quiet] {
        m_apps = watcher->result();
        m_apps += SysTools::systemTools();
        watcher->deleteLater();
        m_scanning.storeRelaxed(0);
        Utils::log(QString("scan finished: %1 apps").arg(m_apps.size()));
        watchAppDirs();     // 安装器常新建子目录，扫完补挂一次监视
        if (!quiet) {
            setStatus(QString("扫描完成：应用 %1 · 文件索引进行中…").arg(m_apps.size()));
            runSearch();
        }
    });
    watcher->setFuture(future);
    FileIndexer::instance().reindexAsync(forceDeep);
    if (!quiet)
        setStatus("正在扫描桌面与开始菜单…");
}

// ---------- 使用统计 ----------

void SearchWindow::loadUsage()
{
    QFile f(Utils::dataDir() + "/usage.json");
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = o.begin(); it != o.end(); ++it) {
        // 那个空键是旧版本的记账口径留下的（所有没有 .lnk 的应用共用 ""，这台机器涨到 43 次），
        // 已经没法归属到具体哪个应用，读的时候丢掉 —— 留着它，QQEX 这种就会一直压在 QQ 前面。
        if (it.key().isEmpty())
            continue;
        m_usage.insert(it.key(), it.value().toInt());
    }
}

void SearchWindow::saveUsage()
{
    QJsonObject o;
    for (auto it = m_usage.begin(); it != m_usage.end(); ++it)
        o.insert(it.key(), it.value());
    QSaveFile f(Utils::dataDir() + "/usage.json");
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

void SearchWindow::bumpUsage(const QString& lnkPath)
{
    m_usage[lnkPath] = m_usage.value(lnkPath, 0) + 1;
}

// ---------- 右键菜单 ----------

void SearchWindow::openContextMenu(const QPoint& globalPos)
{
    auto* hit=m_list->itemAt(m_list->viewport()->mapFromGlobal(globalPos));
    if (!hit) return;
    m_list->setCurrentItem(hit);
    const int idx=hit->data(Qt::UserRole).toInt();
    if (idx<0 || idx>=m_results.size()) return;
    const SearchResult r=m_results[idx];
    if (r.kind==SearchResult::PinnedApp || r.kind==SearchResult::PinnedGroup) {
        pinnedMenu(r.target,globalPos);
        return;
    }
    m_menu = new QMenu(this);
    m_menu->setAttribute(Qt::WA_DeleteOnClose);
    m_menu->setStyleSheet(Utils::darkMenuStyle());
    connect(m_menu, &QMenu::aboutToHide, this, [this] {
        m_menu = nullptr;
        if (!isActiveWindow())
            hide();
    });

    if (r.kind == SearchResult::QuickSearch) {
        m_menu->addAction(QStringLiteral("打开目标"), this, [this, r] {
            hide();
            if (r.extra == QLatin1String("folder")) Platform::reveal(r.target);
            else if (r.extra == QLatin1String("app")) Platform::launch(r.target, {}, {}, true);
            else Platform::launch(WebShortcuts::normalizeUrl(r.target), {}, {}, true);
        });
        m_menu->addAction(QStringLiteral("编辑快捷搜索"), this, [this, r] {
            openSettings();
            if (auto* dlg = qobject_cast<SettingsDialog*>(m_settingsDlg.data()))
                dlg->editQuickSearch(r.title);
        });
        m_menu->addAction(QStringLiteral("删除快捷搜索"), this, [this, r] {
            auto entries = WebShortcuts::loadWebs();
            for (int i = entries.size() - 1; i >= 0; --i)
                if (entries[i].name == r.title) entries.removeAt(i);
            WebShortcuts::saveWebs(entries);
            runSearch();
        });
    }

    if (canPinResult(r)) {
        const bool exists=PinnedApps::isPinned(resultPinnedApp(r));
        m_menu->addAction(exists?QStringLiteral("已固定"):QStringLiteral("固定到面板"),this,[this,idx]{resultAction(idx,true);})->setEnabled(!exists);
    }
    if (r.kind == SearchResult::App) {
        const QString launchTarget = r.extra.isEmpty() ? r.target : r.extra;
        const QString where = QFileInfo::exists(r.target)?r.target:r.extra;
        m_menu->addAction(QStringLiteral("以管理员身份启动"), this, [this, launchTarget] {
            bumpUsage(launchTarget);
            saveUsage();
            hide();
            Platform::launchAsAdmin(launchTarget);
        });
        m_menu->addSeparator();
        m_menu->addAction(QStringLiteral("打开所在位置"), this, [where] { Platform::reveal(where); });
        if (!r.target.isEmpty()) {
            const QString dir = QFileInfo(r.target).absolutePath();
            m_menu->addAction(QStringLiteral("把程序目录加入 PATH"), this, [this, dir] {
                QString err;
                const bool ok = EnvPath::addToUserPath(dir, &err);
                setStatus(ok ? QStringLiteral("已加入 PATH：%1").arg(dir) : err);
            });
        }
        m_menu->addAction(QStringLiteral("复制路径"), this, [where] {
            QGuiApplication::clipboard()->setText(QDir::toNativeSeparators(where));
        });
    } else if (r.kind == SearchResult::File) {
        const QString ext = QFileInfo(r.target).suffix().toLower();
        if (ext == QLatin1String("exe") || ext == QLatin1String("bat") || ext == QLatin1String("cmd")) {
            m_menu->addAction(QStringLiteral("以管理员身份启动"), this, [this, t = r.target] {
                bumpUsage(QStringLiteral("file:") + t);
                saveUsage();
                hide();
                Platform::launchAsAdmin(t);
            });
            m_menu->addSeparator();
        }
        m_menu->addAction(QStringLiteral("打开所在文件夹"), this, [t = r.target] {
            Platform::reveal(t);
        });
        m_menu->addAction(QStringLiteral("复制路径"), this, [t = r.target] {
            QGuiApplication::clipboard()->setText(QDir::toNativeSeparators(t));
        });
    }

    const QString mainFile = (r.kind == SearchResult::App)
        ? (r.target.isEmpty() ? r.extra : r.target) : r.target;
    if ((r.kind == SearchResult::App || r.kind == SearchResult::File)
        && QFileInfo(mainFile).isFile()) {
        m_menu->addSeparator();
        m_menu->addAction(QStringLiteral("解除占用"), this, [this, mainFile] {
            const auto locks = FileTools::findLocks(mainFile);
            if (locks.isEmpty()) {
                QMessageBox::information(this, QStringLiteral("萤 Hotaru"), QStringLiteral("没有发现占用该文件的进程。"));
                return;
            }
            QStringList names;
            for (const auto& l : locks)
                names.append(QStringLiteral("%1 (pid %2)").arg(l.name).arg(l.pid));
            if (QMessageBox::question(this, QStringLiteral("解除占用"),
                    QStringLiteral("以下进程正在占用：\n%1\n结束它们？").arg(names.join(QStringLiteral("\n"))))
                == QMessageBox::Yes) {
                const QString msg = FileTools::unlock(mainFile);
                msg.isEmpty()
                    ? QMessageBox::information(this, QStringLiteral("萤 Hotaru"), QStringLiteral("已全部结束。"))
                    : QMessageBox::warning(this, QStringLiteral("萤 Hotaru"), msg);
            }
        });
        m_menu->addAction(QStringLiteral("文件粉碎"), this, [this, mainFile] {
            if (QMessageBox::warning(this, QStringLiteral("文件粉碎"),
                    QStringLiteral("将对文件进行三轮覆写并永久删除（不可恢复）：\n%1\n确定？")
                        .arg(QDir::toNativeSeparators(mainFile)),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
                return;
            const QString err = FileTools::shred(mainFile);
            err.isEmpty()
                ? QMessageBox::information(this, QStringLiteral("萤 Hotaru"), QStringLiteral("文件已粉碎。"))
                : QMessageBox::warning(this, QStringLiteral("萤 Hotaru"), err);
        });
    }
    if (m_menu->isEmpty())
        m_menu->addAction("(无可用操作)")->setEnabled(false);
    m_menu->popup(globalPos);
}

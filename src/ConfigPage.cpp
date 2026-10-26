#include "ConfigPage.h"
#include "platform/Autostart.h"
#include "ShellMenu.h"
#include "ClipboardStore.h"
#include "DevEnv.h"
#include "CaptureHistory.h"
#include "LauncherTheme.h"
#include "Utils.h"
#include "PluginStore.h"
#include "PinnedApps.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QTabWidget>
#include <QScrollArea>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QSignalBlocker>
#include <QInputDialog>
#include <QUrl>
#include <QVBoxLayout>

#include <windows.h>

#include <algorithm>

namespace {

// 微软拼音"中文输入时使用英文标点"不是 ltool 自己的设置，它是系统的一个 DWORD：
// HKCU\Software\Microsoft\InputMethod\Settings\CHS。写它 = 去
// 设置 → 时间和语言 → 中文(中国) → 微软拼音 → 常规 里点那一下，值名是本机实测出来的。
// 用 Win32 注册表 API 而不是 QSettings：后者在 Windows 上可能按自己的规则改值名大小写。
// 返回 -1 表示这台机器上没有这个值（不是微软拼音，或系统版本不一样）。
constexpr wchar_t kImeChsKey[] = L"Software\\Microsoft\\InputMethod\\Settings\\CHS";
constexpr wchar_t kImePunctValue[] = L"UseEnglishPunctuationsInChineseInputMode";

int imePunctState()
{
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kImeChsKey, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return -1;
    DWORD v = 0, size = sizeof(v), type = 0;
    const LONG r = RegQueryValueExW(hk, kImePunctValue, nullptr, &type,
                                    reinterpret_cast<LPBYTE>(&v), &size);
    RegCloseKey(hk);
    if (r != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(v))
        return -1;
    return v ? 1 : 0;
}

bool setImePunct(bool on)
{
    HKEY hk = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kImeChsKey, 0, nullptr, 0, KEY_WRITE,
                        nullptr, &hk, nullptr) != ERROR_SUCCESS)
        return false;
    const DWORD v = on ? 1u : 0u;
    const LONG r = RegSetValueExW(hk, kImePunctValue, 0, REG_DWORD,
                                  reinterpret_cast<const BYTE*>(&v), sizeof(v));
    RegCloseKey(hk);
    return r == ERROR_SUCCESS;
}

void showImeState(QCheckBox* box, QLabel* info)
{
    const int st = imePunctState();
    box->setEnabled(st >= 0);
    box->setChecked(st > 0);
    info->setText(st < 0
        ? QStringLiteral("没在系统里找到这个开关（可能用的不是微软拼音）")
        : QStringLiteral("注册表 UseEnglishPunctuationsInChineseInputMode = %1（%2）")
              .arg(st)
              .arg(st ? QStringLiteral("开") : QStringLiteral("关")));
}

// 这个目录能不能用。空串代表能用（空值 = 用默认位置，永远合法）；否则返回给人看的原因。
QString dirProblem(const QString& path)
{
    if (path.isEmpty())
        return {};
    QDir().mkpath(path);
    if (!QFileInfo::exists(path))
        return QStringLiteral("建不出来");
    const QString probe = path + "/.ltool-write-test";
    QFile f(probe);
    if (!f.open(QIODevice::WriteOnly))
        return QStringLiteral("写不进去（没权限 / 只读 / 盘满）");
    f.close();
    QFile::remove(probe);
    return {};
}

// "现在这个目录里有多少个、多大" —— 只数 ltool 自己命名的那批文件
QString dirSummary(const QString& dir, const QStringList& patterns)
{
    QDir d(dir);
    if (!d.exists())
        return QStringLiteral("（目录还没建）");
    const QFileInfoList files = d.entryInfoList(patterns, QDir::Files);
    qint64 bytes = 0;
    for (const QFileInfo& fi : files)
        bytes += fi.size();
    return QStringLiteral("%1 个 · %2 MB").arg(files.size())
        .arg(bytes / 1048576.0, 0, 'f', 1);
}

// 列表里"是目录的那几项"有几个：往里找几层这个设置只对目录有效，
// 点名加的 exe 一条都不受它影响，所以状态行要把这两个数分开写
int dirItemCount(QListWidget* list)
{
    int n = 0;
    for (int i = 0; i < list->count(); ++i)
        if (QFileInfo(list->item(i)->text()).isDir())
            ++n;
    return n;
}

// 剪贴板那一格的状态行：条数 + 保留多久一起说，别让人以为 200 条是永远不倒的。
QString clipInfoText()
{
    auto& cs = ClipboardStore::instance();
    return QString("%1 条记录 · 保留 %2 · 图片存在 clipimg\\")
        .arg(cs.items().size())
        .arg(cs.keepDays() > 0 ? QString("%1 天").arg(cs.keepDays()) : QStringLiteral("不限（只按 200 条）"));
}

} // namespace

ConfigPage::ConfigPage(QWidget* parent)
    : QWidget(parent)
{
    // 配置项越加越多，早就超出窗口高度了：整页放进滚动区，不然下面的分组看不见也点不到
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    // 2026-10-20 他点名把配置页拆成两个 tab：通用 / 外观。
    // 外观那一页眼下只有「夜间 / 日间」这一行，但它以后是所有跟长相有关的开关的落点 ——
    // 继续挤在通用那张长列表里，下次加字号/密度就得再搬一次。
    auto* tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("cfgTabs"));
    auto* scroller = new QScrollArea(tabs);
    scroller->setObjectName(QStringLiteral("cfgScroll"));
    scroller->setWidgetResizable(true);
    scroller->setFrameShape(QFrame::NoFrame);
    scroller->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget;
    body->setObjectName(QStringLiteral("cfgBody"));
    scroller->setWidget(body);
    tabs->addTab(scroller, QString::fromUtf8("通用"));
    outer->addWidget(tabs, 1);
    auto* lay = new QVBoxLayout(body);
    lay->setContentsMargins(12, 4, 4, 4);

    auto* general = new QGroupBox("通用", this);
    auto* generalLay = new QFormLayout(general);
    m_autostart = new QCheckBox("开机自动启动 Hotaru", general);
    generalLay->addRow(m_autostart);
    m_hotkey = new QKeySequenceEdit(general);
    m_hotkey->setToolTip("呼出/隐藏搜索框的全局热键（至少包含一个修饰键）");
    generalLay->addRow("呼出快捷键：", m_hotkey);
    m_installPath = new QLineEdit(general);
    m_installPath->setPlaceholderText("D:\\software");
    generalLay->addRow("软件安装路径：", m_installPath);
    m_proxy = new QLineEdit(general);
    if (const QString sys = DevEnv::systemProxyUrl(); !sys.isEmpty()) {
        m_proxy->setPlaceholderText(QString("留空 = 自动用系统代理（当前检测到 %1）").arg(sys));
        m_proxy->setToolTip("填别的地址可覆盖系统代理。\n"
                            "内置下载器一直走它；winget 要用命令行代理，需管理员先执行一次\n"
                            "  winget settings --enable ProxyCommandLineOptions\n"
                            "没启用时 Hotaru 会自动不带代理装（否则 winget 会直接拒绝整条命令）。");
    } else {
        m_proxy->setPlaceholderText("例：http://127.0.0.1:10808（GitHub 包下载慢时有用）");
        m_proxy->setToolTip("没检测到系统代理。填上本地代理后，内置下载器会走它；"
                            "winget 还需要管理员先执行 winget settings --enable ProxyCommandLineOptions "
                            "才认命令行代理。");
    }
    m_proxy->setClearButtonEnabled(true);
    generalLay->addRow("下载代理：", m_proxy);
    m_storeUrl = new QLineEdit(general);
    m_storeUrl->setPlaceholderText(PluginStore::defaultIndexUrl());
    m_storeUrl->setToolTip("插件商店要拉的那份 plugins-index.json。留空 = 用内置那条"
                            "（沿用上游 ltool v1.5.0，未测试与 Hotaru 的适配情况，需自行适配）。\n"
                            "要指到自己的测试源就填那里的地址；代理沿用上面那一行，清单和 dll 都走它。");
    m_storeUrl->setClearButtonEnabled(true);
    generalLay->addRow("插件清单地址：", m_storeUrl);
    m_storeMirrors = new QLineEdit(general);
    m_storeMirrors->setPlaceholderText(PluginStore::mirrorList());
    m_storeMirrors->setToolTip("拉插件（清单和 dll）时**先**按这里每条前缀试一遍，走直连；"
                               "都不成才轮到我们自己的下载代理，最后才是直连原始地址。\n"
                               "多个用逗号分隔，留空 = 内置那两条（这台机器实测过还活着：gh-proxy.com、ghfast.top）。\n"
                               "只有地址在 github 上才用到镜像（raw 那条和发版资产 releases/download 那条）"
                               "—— 指到自己的服务器就不套镜像。");
    m_storeMirrors->setClearButtonEnabled(true);
    generalLay->addRow("插件镜像前缀：", m_storeMirrors);
    // 默认**开**：他这句是"自动做 X"（2026-10-21「插件商店增加一个自动更新功能，一天自动更新一次」），
    // 不是"给我个按钮我按了才做"。范围是他拍的：只换已装的，清单里新出现的插件仍要点「安装」。
    m_autoUpdate = new QCheckBox("每天自动更新一次已装的插件（启动之后后台跑，不弹框）", general);
    m_autoUpdate->setToolTip("程序启动十几秒后拉一次插件清单，把**已经装着**的那几颗换成清单里更新的版本，一天只跑一次。\n"
                             "清单里新出现的插件不会自动装 —— 那仍然要你在商店里自己点。\n"
                             "商店那一页会常驻一行说上次跑的结果（换了几颗、哪几颗没成）。换下来的那颗如果这个进程还加载着，"
                             "文件当场已经换好，界面要下次启动才用新的。");
    generalLay->addRow(m_autoUpdate);
    m_engine = new QComboBox(general);
    m_engine->addItem("百度", "baidu");
    m_engine->addItem("Google", "google");
    m_engine->addItem("必应", "bing");
    m_engine->addItem("DuckDuckGo", "duckduckgo");
    generalLay->addRow("默认搜索引擎：", m_engine);
    m_favorites = new QCheckBox("呼出时显示常驻网格", general);
    m_favorites->setObjectName("showResidentGrid");
    m_favorites->setToolTip("打开：呼出时显示手动固定的软件和文件夹。\n"
                            "关闭：只显示搜索框，输入后展开搜索结果。");
    generalLay->addRow(m_favorites);

    auto* shell = new QGroupBox("资源管理器右键菜单", this);
    auto* shellLay = new QFormLayout(shell);
    m_shellMenu = new QCheckBox("在文件 / 文件夹的右键里加入 Hotaru 子菜单", shell);
    m_shellMenu->setToolTip(QStringLiteral("子菜单里是：解压到当前目录、解压到同名文件夹、压缩为 .zip、压缩为 .7z、\n"
                                          "粉碎（不可恢复）、占用分析。\n"
                                          "后端是自带的 libarchive，zip / 7z / tar / gz / bz2 / xz / zstd / iso 都能解，"
                                          "写只写 zip 和 7z。"));
    shellLay->addRow(m_shellMenu);
    m_archiveProgress = new QCheckBox("压缩 / 解压时弹出进度窗口", shell);
    m_archiveProgress->setToolTip(QStringLiteral("关闭（默认）：后台静默做完，不弹任何窗口；只有失败才弹框告诉你，"
                                                 "结果照常写进日志。\n"
                                                 "打开：弹一个进度窗口，能看到正在处理哪个文件、百分比，并且可以取消。"));
    shellLay->addRow(m_archiveProgress);
    m_shellInfo = new QLabel(shell);
    m_shellInfo->setObjectName("muted");
    m_shellInfo->setWordWrap(true);
    shellLay->addRow("当前：", m_shellInfo);

    auto* ime = new QGroupBox("微软拼音 · 中文输入时的标点", this);
    auto* imeLay = new QFormLayout(ime);
    m_imePunct = new QCheckBox("中文输入使用英文标点（, . ? ! 而不是 ，。？！）", ime);
    m_imePunct->setToolTip("改的是系统里微软拼音自己的开关，等价于去\n"
                           "设置 → 时间和语言 → 中文(中国) → 微软拼音 → 常规 → 中文使用英文标点。\n"
                           "保存后按一次 Shift 切一下中英文（或重选一次输入法）就生效。");
    imeLay->addRow(m_imePunct);
    m_imePunctInfo = new QLabel(ime);
    m_imePunctInfo->setObjectName("muted");
    m_imePunctInfo->setWordWrap(true);
    imeLay->addRow("当前：", m_imePunctInfo);

    auto* index = new QGroupBox("定时扫描（文件索引刷新）", this);
    auto* indexLay = new QFormLayout(index);
    m_deep = new QCheckBox("启用全盘深度索引（后台进行，占用更多内存）", index);
    indexLay->addRow(m_deep);
    m_refreshHours = new QSpinBox(index);
    m_refreshHours->setRange(0, 168);
    m_refreshHours->setSuffix(" 小时");
    indexLay->addRow("扫描间隔（0 = 不自动）：", m_refreshHours);
    m_maxEntries = new QSpinBox(index);
    m_maxEntries->setRange(100000, 20000000);
    m_maxEntries->setSingleStep(500000);
    m_maxEntries->setGroupSeparatorShown(true);
    indexLay->addRow("索引条目上限：", m_maxEntries);

    auto* appDirs = new QGroupBox("应用搜索的额外项目", this);
    auto* appLay = new QFormLayout(appDirs);
    // 这些是"启动器输名字要能搜到"的应用来源。两种条目：填文件夹 = 往里找若干层，
    // 只收看起来是主程序的那些 exe（子进程和命令行工具会被跳过）；点名填某个 exe = 一定要它，
    // 无条件收。扫描是重活，所以只在保存之后才跑，不随输入跑。
    m_appRoots = new QListWidget(appDirs);
    m_appRoots->setMaximumHeight(120);
    m_appRoots->setToolTip(QStringLiteral(
        "一行一个：文件夹（往里找 exe）或者某个 exe 的完整路径（直接收它）。\n"
        "填目录时，命令行工具和子进程（7z / cmake / BugReport / updater 这类）不算应用，会被跳过；\n"
        "真想要其中哪一个，用「添加应用…」点名指到那个 exe。\n"
        "开始菜单那些快捷方式不用往这儿加，本来就扫。"));
    auto* rootRow = new QHBoxLayout;
    auto* addRoot = new QPushButton("添加目录…", appDirs);
    addRoot->setObjectName(QStringLiteral("addAppDir"));
    auto* addExe = new QPushButton("添加应用…", appDirs);
    addExe->setObjectName(QStringLiteral("addAppExe"));
    addExe->setToolTip(QStringLiteral("直接挑一个 exe：它一定进应用列表，不做\u201c像不像主程序\u201d那两道判断"));
    auto* delRoot = new QPushButton("移除选中", appDirs);
    rootRow->addWidget(m_appRoots, 1);
    auto* rootBtns = new QVBoxLayout;
    rootBtns->addWidget(addRoot);
    rootBtns->addWidget(addExe);
    rootBtns->addWidget(delRoot);
    rootBtns->addStretch(1);
    rootRow->addLayout(rootBtns);
    appLay->addRow("项目列表：", rootRow);
    m_appDepth = new QSpinBox(appDirs);
    m_appDepth->setRange(0, 6);
    m_appDepth->setSuffix(QStringLiteral(" 层"));
    m_appDepth->setToolTip(QStringLiteral("只对填进来的**目录**起作用。0 = 只看这个目录本身；3 = 往里三层里的 exe 都算。"));
    appLay->addRow("往里找几层：", m_appDepth);
    m_appInfo = new QLabel(appDirs);
    m_appInfo->setObjectName("muted");
    m_appInfo->setWordWrap(true);
    appLay->addRow("当前：", m_appInfo);
    connect(addRoot, &QPushButton::clicked, this, &ConfigPage::addAppRoot);
    connect(addExe, &QPushButton::clicked, this, &ConfigPage::addAppExe);
    connect(delRoot, &QPushButton::clicked, this, &ConfigPage::removeAppRoot);

    auto* data = new QGroupBox("数据", this);
    auto* dataLay = new QFormLayout(data);
    // 截图 / 录像的存放目录：填了就用填的，留空回到 %APPDATA%\ltool 下的默认子目录。
    // 改目录只影响"以后存的文件"，已有文件不搬 —— 这行话必须写在页面上，
    // 不然人会以为文件跟过去了。
    const auto pathRow = [this, data, dataLay](QLineEdit** member, const QString& label,
                                      const QString& defaultDir, const QString& what, bool isCap) {
        auto* row = new QWidget(data);
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        auto* e = new QLineEdit(row);
        e->setPlaceholderText(QDir::toNativeSeparators(defaultDir));
        e->setToolTip(QString("%1放这里。留空 = %2").arg(what, QDir::toNativeSeparators(defaultDir)));
        auto* browse = new QPushButton("浏览…", row);
        auto* open = new QPushButton("打开", row);
        open->setToolTip(QString("在资源管理器里打开%1现在存放的目录").arg(what));
        QObject::connect(browse, &QPushButton::clicked, row, [e, label, what, defaultDir] {
            // 没填的时候从"以后会存过去的那个地方"开始挑，而不是数据目录
            const QString start = e->text().trimmed().isEmpty()
                                      ? QDir::toNativeSeparators(defaultDir) : e->text().trimmed();
            const QString got = QFileDialog::getExistingDirectory(e, QString("选%1的存放目录").arg(what), start);
            if (!got.isEmpty())
                e->setText(QDir::toNativeSeparators(got));
        });
        QObject::connect(open, &QPushButton::clicked, row, [isCap] {
            const QString dir = isCap ? Utils::capturesDir() : Utils::recordingsDir();
            QDir().mkpath(dir);
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        });
        rl->addWidget(e, 1);
        rl->addWidget(browse);
        rl->addWidget(open);
        *member = e;
        dataLay->addRow(label, row);
    };
    pathRow(&m_capDir, "截图存放：", Utils::mediaRoot() + "/captures", QStringLiteral("截图"), true);
    pathRow(&m_recDir, "录像存放：", Utils::mediaRoot() + "/recordings", QStringLiteral("录像"), false);
    m_dirInfo = new QLabel(data);
    m_dirInfo->setObjectName("muted");
    m_dirInfo->setWordWrap(true);
    dataLay->addRow("当前：", m_dirInfo);
    auto* btnRow = new QHBoxLayout;
    auto* openBtn = new QPushButton("打开数据目录", data);
    auto* clearBtn = new QPushButton("清除索引缓存", data);
    btnRow->addWidget(openBtn);
    btnRow->addWidget(clearBtn);
    btnRow->addStretch(1);
    dataLay->addRow(btnRow);

    auto* clip = new QGroupBox("剪贴板历史", this);
    auto* clipLay = new QFormLayout(clip);
    m_clip = new QCheckBox("记录我复制过的文本和图片（默认关闭）", clip);
    m_clip->setToolTip("开启后 Hotaru 会在后台读剪贴板，把内容存到\n"
                       "软件目录下的 data\\clipboard.json 和 data\\clipimg\\ 里（最多 200 条）。\n"
                       "启动器里输 clip 就能翻历史。关掉只是停止收集，已有历史保留。");
    clipLay->addRow(m_clip);
    m_clipDays = new QSpinBox(clip);
    m_clipDays->setObjectName("clipDays");
    m_clipDays->setRange(0, 3650);
    m_clipDays->setSuffix(QStringLiteral(" 天"));
    m_clipDays->setSpecialValueText(QStringLiteral("不限"));
    m_clipDays->setToolTip("两条上限取交集：超过这个天数的一律清掉，没超龄的仍然受「最多 200 条」兜底。\n"
                           "选「不限」就只按条数算。改完当场裁一次，超龄条目那张图片文件也跟着删；\n"
                           "裁掉的就是裁掉了，改回「不限」不会把它们找回来。");
    clipLay->addRow(QStringLiteral("保留天数："), m_clipDays);
    m_clipInfo = new QLabel(clip);
    m_clipInfo->setObjectName("muted");
    clipLay->addRow("当前：", m_clipInfo);
    auto* clipClear = new QPushButton("清空剪贴板历史", clip);
    clipClear->setToolTip("删除所有记录和已保存的图片文件");
    clipLay->addRow(clipClear);

    // ---- 「外观」那一页：夜间 / 日间 从通用那张长列表里搬到这里（他 2026-10-20 点名）----
    // 搬完，通用那一行「外观：」的标签就没了 —— 判据盯着"外观那一行只有一颗、而且它在
    // 外观这一页上"，别哪天又被人加回通用。
    auto* appearancePage = new QWidget(tabs);
    auto* appearance = new QGroupBox("外观", appearancePage);
    auto* appearanceLay = new QFormLayout(appearance);
    auto* appearanceLayBox = new QVBoxLayout(appearancePage);
    appearanceLayBox->setContentsMargins(12, 12, 12, 12);
    appearanceLayBox->addWidget(appearance);
    appearanceLayBox->addStretch(1);   // 这一页东西少，别把分组框拉到整页高
    // 外观：夜间 / 日间。拨一下就当场生效（Theme::setMode 会重刷调色板、两份弹层样式表，
    // 并给每个顶层窗口发一次 ThemeChange），不用按保存、也不用重启。
    auto* themeRow = new QWidget(appearance);
    auto* themeLay = new QHBoxLayout(themeRow);
    themeLay->setContentsMargins(0, 0, 0, 0);
    themeLay->setSpacing(16);
    m_themeDark = new QRadioButton(QString::fromUtf8("夜间"), themeRow);
    m_themeLight = new QRadioButton(QString::fromUtf8("日间"), themeRow);
    m_themeDark->setChecked(Theme::current() == Theme::Mode::Dark);
    m_themeLight->setChecked(Theme::current() == Theme::Mode::Light);
    themeLay->addWidget(m_themeDark);
    themeLay->addWidget(m_themeLight);
    themeLay->addStretch(1);
    themeRow->setToolTip(QStringLiteral(
        "基础主题可快捷切换夜间与日间，黑、灰、白在下方主题列表中选择。\n"
        "自带颜色的第三方主题使用包内配色；切换立即生效。插件自行绘制的内容由插件负责。"));
    appearanceLay->addRow(QStringLiteral("外观："), themeRow);
    connect(m_themeDark, &QRadioButton::toggled, this,
            [this](bool on) { if (on) {
                Theme::setMode(Theme::Mode::Dark);
                syncThemeControls();
            } });
    connect(m_themeLight, &QRadioButton::toggled, this,
            [this](bool on) { if (on) {
                Theme::setMode(Theme::Mode::Light);
                syncThemeControls();
            } });

    auto* packageRow = new QWidget(appearance);
    auto* packageLay = new QHBoxLayout(packageRow);
    packageLay->setContentsMargins(0, 0, 0, 0);
    m_themePackages = new QComboBox(packageRow);
    m_themePackages->setObjectName(QStringLiteral("themePackages"));
    auto* importTheme = new QPushButton(QStringLiteral("导入 ZIP…"), packageRow);
    auto* removeTheme = new QPushButton(QStringLiteral("删除主题"), packageRow);
    packageLay->addWidget(m_themePackages, 1);
    packageLay->addWidget(importTheme);
    packageLay->addWidget(removeTheme);
    appearanceLay->addRow(QStringLiteral("主题："), packageRow);
    m_themePackageInfo = new QLabel(appearance);
    m_themePackageInfo->setObjectName(QStringLiteral("muted"));
    m_themePackageInfo->setWordWrap(true);
    appearanceLay->addRow(QStringLiteral("说明："), m_themePackageInfo);

    auto* pinned = new QGroupBox(QStringLiteral("固定软件"), appearancePage);
    auto* pinnedLay = new QFormLayout(pinned);
    m_gridColumns = new QSpinBox(pinned);
    m_gridColumns->setRange(4, 8);
    m_gridColumns->setSuffix(QStringLiteral(" 列"));
    m_gridColumns->setObjectName(QStringLiteral("gridColumns"));
    m_gridRows = new QSpinBox(pinned);
    m_gridRows->setObjectName(QStringLiteral("gridRows"));
    m_gridRows->setRange(2,6);m_gridRows->setSuffix(QStringLiteral(" 行"));
    auto* gridSize=new QWidget(pinned);auto* gridSizeLayout=new QHBoxLayout(gridSize);
    gridSizeLayout->setContentsMargins(0,0,0,0);gridSizeLayout->addWidget(m_gridRows);gridSizeLayout->addWidget(m_gridColumns);
    pinnedLay->addRow(QStringLiteral("每页网格："), gridSize);
    m_pageStart=new QComboBox(pinned);m_pageStart->setObjectName(QStringLiteral("pageStart"));
    m_pageStart->addItem(QStringLiteral("每次打开回到首页"),false);
    m_pageStart->addItem(QStringLiteral("记住上次所在页"),true);
    pinnedLay->addRow(QStringLiteral("打开面板时："),m_pageStart);
    m_pinnedApps = new QListWidget(pinned);
    m_pinnedApps->setObjectName(QStringLiteral("pinnedApps"));
    m_pinnedApps->setMinimumHeight(120);
    m_pinnedApps->setMaximumHeight(190);
    auto* pinnedRow = new QWidget(pinned);
    auto* pinnedRowLay = new QHBoxLayout(pinnedRow);
    pinnedRowLay->setContentsMargins(0, 0, 0, 0);
    auto* addPinned = new QPushButton(QStringLiteral("添加程序…"), pinnedRow);
    auto* togglePinned = new QPushButton(QStringLiteral("启用/禁用"), pinnedRow);
    auto* editPinned = new QPushButton(QStringLiteral("编辑"), pinnedRow);
    auto* upPinned = new QPushButton(QStringLiteral("格位…"), pinnedRow);
    auto* downPinned = new QPushButton(QStringLiteral("下移"), pinnedRow);
    auto* removePinned = new QPushButton(QStringLiteral("移除"), pinnedRow);
    auto* importConfig = new QPushButton(QStringLiteral("导入配置"), pinnedRow);
    auto* exportConfig = new QPushButton(QStringLiteral("导出配置"), pinnedRow);
    pinnedRowLay->addWidget(addPinned);
    pinnedRowLay->addWidget(togglePinned);
    pinnedRowLay->addWidget(editPinned);
    pinnedRowLay->addWidget(upPinned);
    downPinned->hide();
    pinnedRowLay->addWidget(removePinned);
    pinnedRowLay->addWidget(importConfig);
    pinnedRowLay->addWidget(exportConfig);
    pinnedRowLay->addStretch(1);
    pinnedLay->addRow(QStringLiteral("列表："), m_pinnedApps);
    pinnedLay->addRow(QString(), pinnedRow);
    m_pinnedAppsInfo = new QLabel(pinned);
    m_pinnedAppsInfo->setObjectName(QStringLiteral("muted"));
    m_pinnedAppsInfo->setWordWrap(true);
    pinnedLay->addRow(QStringLiteral("说明："), m_pinnedAppsInfo);
    appearanceLayBox->addWidget(pinned);

    connect(importTheme, &QPushButton::clicked, this, &ConfigPage::importThemePackage);
    connect(removeTheme, &QPushButton::clicked, this, &ConfigPage::removeThemePackage);
    connect(m_themePackages, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &ConfigPage::activateThemePackage);
    connect(addPinned, &QPushButton::clicked, this, &ConfigPage::addPinnedApp);
    auto* addFolder=new QPushButton(QStringLiteral("添加文件夹…"),pinnedRow);
    pinnedRowLay->insertWidget(1,addFolder);
    connect(addFolder,&QPushButton::clicked,this,[this] {
        const QString path=QFileDialog::getExistingDirectory(this,QStringLiteral("选择固定文件夹"));
        if (path.isEmpty()) return;
        QString error;if (!PinnedApps::add(PinnedApps::fromTarget(path),-1,-1,&error)) m_pinnedAppsInfo->setText(error);
        else load();
    });
    connect(togglePinned, &QPushButton::clicked, this, &ConfigPage::togglePinnedApp);
    connect(editPinned, &QPushButton::clicked, this, &ConfigPage::editPinnedApp);
    connect(upPinned, &QPushButton::clicked, this, &ConfigPage::movePinnedUp);
    connect(downPinned, &QPushButton::clicked, this, &ConfigPage::movePinnedDown);
    connect(removePinned, &QPushButton::clicked, this, &ConfigPage::removePinnedApp);
    connect(importConfig, &QPushButton::clicked, this, &ConfigPage::importHotaruConfig);
    connect(exportConfig, &QPushButton::clicked, this, &ConfigPage::exportHotaruConfig);
    tabs->addTab(appearancePage, QString::fromUtf8("外观"));

    auto* save = new QPushButton("保存", this);
    save->setDefault(true);
    save->setObjectName("primary");
    auto* btnLay = new QHBoxLayout;
    btnLay->addStretch(1);
    btnLay->addWidget(save);
    lay->addWidget(general);
    lay->addWidget(shell);
    lay->addWidget(ime);
    lay->addWidget(index);
    lay->addWidget(appDirs);
    lay->addWidget(clip);
    lay->addWidget(data);
    outer->addLayout(btnLay);

    connect(save, &QPushButton::clicked, this, &ConfigPage::onSave);
    connect(openBtn, &QPushButton::clicked, this, &ConfigPage::onOpenDataDir);
    connect(clearBtn, &QPushButton::clicked, this, &ConfigPage::onClearIndexCache);
    connect(clipClear, &QPushButton::clicked, this, [this] {
        ClipboardStore::instance().clear();
        m_clipInfo->setText(QString("已清空 · 现在 %1 条").arg(ClipboardStore::instance().items().size()));
    });
    // 保留天数不等「保存」：它就是引擎的一条上限，改了当场生效才对得上那句话。
    connect(m_clipDays, qOverload<int>(&QSpinBox::valueChanged), this, [this](int days) {
        ClipboardStore::instance().setKeepDays(days);
        m_clipInfo->setText(clipInfoText());
    });
    load();
}

void ConfigPage::load()
{
    QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
    const PinnedApps::LauncherConfig hotaruCfg = PinnedApps::config();
    m_autostart->setChecked(Platform::autostartEnabled());
    m_hotkey->setKeySequence(QKeySequence(cfg.value("general/hotkey", "Alt+Space").toString()));
    m_installPath->setText(cfg.value("install/path", "D:\\software").toString());
    m_proxy->setText(cfg.value("net/proxy").toString());
    m_storeUrl->setText(cfg.value("store/indexUrl").toString());
    m_storeMirrors->setText(cfg.value("store/mirrors").toString());
    m_autoUpdate->setChecked(cfg.value("store/autoUpdate", false).toBool());
    const QString engine = cfg.value("web/engine", "baidu").toString();
    const int idx = m_engine->findData(engine);
    m_engine->setCurrentIndex(idx < 0 ? 0 : idx);
    m_favorites->setChecked(cfg.value("ui/favoritesOnEmpty", true).toBool());
    m_gridColumns->setValue(hotaruCfg.gridColumns);
    m_gridRows->setValue(hotaruCfg.gridRows);
    m_pageStart->setCurrentIndex(hotaruCfg.rememberPage?1:0);
    m_deep->setChecked(cfg.value("index/deep", true).toBool());
    m_refreshHours->setValue(cfg.value("index/refreshHours", 6).toInt());
    m_maxEntries->setValue(cfg.value("index/maxEntries", 5000000).toInt());
    m_appDepth->setValue(cfg.value("apps/depth", 3).toInt());
    m_appRoots->clear();
    for (const QString& r : cfg.value("apps/roots").toStringList())
        if (!r.trimmed().isEmpty())
            new QListWidgetItem(QDir::toNativeSeparators(r), m_appRoots);
    m_appInfo->setText(QStringLiteral("%1 项 · 其中目录 %2 个（往里 %3 层只对目录有效）· 保存之后才会重扫应用列表")
                           .arg(m_appRoots->count()).arg(dirItemCount(m_appRoots))
                           .arg(m_appDepth->value()));
    refreshPathInfo();
    m_clip->setChecked(ClipboardStore::instance().enabled());
    m_clipDays->setValue(ClipboardStore::instance().keepDays());
    m_clipInfo->setText(clipInfoText());
    m_shellMenu->setChecked(ShellMenu::intent());
    m_archiveProgress->setChecked(cfg.value("ui/archiveProgress", false).toBool());
    // 三件事分开说：勾是你想要的，注册表里有没有是另一回事 —— 提供右键那几项的
    // 「右键动作」插件被卸载/禁用了，勾还留着，但右键里此刻没有 ltool。
    m_shellInfo->setText(ShellMenu::installed()
                              ? QStringLiteral("已集成。%1").arg(ShellMenu::locationHint())
                              : ShellMenu::intent()
                                    ? QStringLiteral("你勾了，但「右键动作」插件不在了（卸载或禁用）—— "
                                                     "右键里此刻没有 Hotaru，装回来会自动写上。%1")
                                          .arg(ShellMenu::locationHint())
                                    : QStringLiteral("未集成（默认）。勾上并保存才会写入；取消勾选再保存即撤销。"));
    showImeState(m_imePunct, m_imePunctInfo);

    {
        const QSignalBlocker blocker(m_themePackages);
        m_themePackages->clear();
        const QString current = Theme::currentPackageId();
        int selected = 0;
        for (const Theme::Package& p : Theme::packages()) {
            const QString label = p.valid ? QStringLiteral("%1 · %2").arg(p.name, p.version)
                                          : QStringLiteral("%1 · 无效：%2").arg(p.id, p.error);
            m_themePackages->addItem(label, p.id);
            if (p.id == current)
                selected = m_themePackages->count() - 1;
        }
        m_themePackages->setCurrentIndex(selected);
    }
    syncThemeControls();

    m_pinnedApps->clear();
    const auto pinnedApps = hotaruCfg.pinnedApps;
    for (const PinnedApp& app : pinnedApps) {
        auto* item = new QListWidgetItem(m_pinnedApps);
        item->setData(Qt::UserRole, app.id);
        item->setText(QStringLiteral("[%1] %2\n%3")
                          .arg(app.enabled ? QStringLiteral("启用") : QStringLiteral("停用"),
                               app.title, app.targetPath));
        QString scope=QStringLiteral("主面板");
        for (const auto& group:hotaruCfg.groups) if (group.id==app.groupId) scope=QStringLiteral("分组：")+group.title;
        const int rows=app.groupId.isEmpty()?hotaruCfg.gridRows:3;
        item->setText(item->text()+QStringLiteral(" · %1 · 第 %2 页 %3 行 %4 列").arg(scope).arg(app.gridRow/rows+1).arg(app.gridRow%rows+1).arg(app.gridColumn+1));
        item->setToolTip(app.targetPath);
    }
    m_pinnedAppsInfo->setText(QStringLiteral("%1 个固定软件。支持程序、快捷方式、脚本和文件夹；隐藏项保留格位，属性中可编辑行列。")
                              .arg(pinnedApps.size()));
}

void ConfigPage::onSave()
{
    QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
    Platform::setAutostart(m_autostart->isChecked());
    if (m_shellMenu->isChecked() != ShellMenu::intent()) {
        // 只记下"你想不想要"；注册表那侧由 intent × 插件在不在 一起决定（syncWithPlugin）
        ShellMenu::setIntent(m_shellMenu->isChecked());
        const QString r = ShellMenu::syncWithPlugin();
        if (!r.isEmpty())
            m_shellInfo->setText(r);
        m_shellMenu->setChecked(ShellMenu::intent());
    }

    // 快捷键
    const QString oldHk = cfg.value("general/hotkey", "Alt+Space").toString();
    const QString newHk = m_hotkey->keySequence().toString();
    cfg.setValue("general/hotkey", newHk);
    cfg.setValue("install/path", m_installPath->text().trimmed());
    cfg.setValue("net/proxy", m_proxy->text().trimmed());
    cfg.setValue("store/indexUrl", m_storeUrl->text().trimmed());
    cfg.setValue("store/mirrors", m_storeMirrors->text().trimmed());
    cfg.setValue("store/autoUpdate", m_autoUpdate->isChecked());
    if (newHk != oldHk)
        emit hotkeyChanged(newHk);

    // 索引
    const bool deepWas = cfg.value("index/deep", true).toBool();
    const int maxWas = cfg.value("index/maxEntries", 5000000).toInt();
    const int hoursWas = cfg.value("index/refreshHours", 6).toInt();
    cfg.setValue("web/engine", m_engine->currentData().toString());
    cfg.setValue("ui/favoritesOnEmpty", m_favorites->isChecked());
    cfg.setValue("ui/archiveProgress", m_archiveProgress->isChecked());
    cfg.setValue("index/deep", m_deep->isChecked());
    cfg.setValue("index/maxEntries", m_maxEntries->value());
    cfg.setValue("index/refreshHours", m_refreshHours->value());

    PinnedApps::LauncherConfig hotaruCfg = PinnedApps::config();
    hotaruCfg.gridColumns = m_gridColumns->value();
    hotaruCfg.gridRows = m_gridRows->value();
    hotaruCfg.rememberPage = m_pageStart->currentData().toBool();
    QString hotaruError;
    if (!PinnedApps::saveConfig(hotaruCfg, &hotaruError))
    {
        m_pinnedAppsInfo->setText(hotaruError);
        const auto previous=PinnedApps::config();
        QSignalBlocker blocker(m_gridColumns);m_gridColumns->setValue(previous.gridColumns);
        m_gridRows->setValue(previous.gridRows);m_pageStart->setCurrentIndex(previous.rememberPage?1:0);
    }

    // 应用目录：整个列表 + 层数一起比，变了才重扫（扫盘是重活，不能随输入跑）
    const QStringList appRootsWas = cfg.value("apps/roots").toStringList();
    const int appDepthWas = cfg.value("apps/depth", 3).toInt();
    QStringList appRoots;
    for (int i = 0; i < m_appRoots->count(); ++i) {
        const QString r = m_appRoots->item(i)->text().trimmed();
        if (!r.isEmpty())
            appRoots << QDir::fromNativeSeparators(r);
    }
    cfg.setValue("apps/roots", appRoots);
    cfg.setValue("apps/depth", m_appDepth->value());
    bool appChanged = appDepthWas != m_appDepth->value()
                      || appRoots.size() != appRootsWas.size();
    if (!appChanged) {
        for (int i = 0; i < appRoots.size(); ++i)
            if (QString::compare(appRoots[i], QDir::fromNativeSeparators(appRootsWas[i]),
                                 Qt::CaseInsensitive) != 0)
                appChanged = true;
    }
    m_appInfo->setText(QStringLiteral("%1 项 · 其中目录 %2 个（往里 %3 层只对目录有效）· 保存之后才会重扫应用列表")
                           .arg(appRoots.size()).arg(dirItemCount(m_appRoots))
                           .arg(m_appDepth->value()));

    const bool indexChanged = deepWas != m_deep->isChecked()
                              || maxWas != m_maxEntries->value()
                              || hoursWas != m_refreshHours->value()
                              || appChanged;
    if (indexChanged)
        emit rescanRequested();

    // 存放目录：先验"能建、能写"，不行就回退到原来的值并说清楚 —— 不接受悄悄改掉
    const QString capNew = m_capDir->text().trimmed();
    const QString recNew = m_recDir->text().trimmed();
    const QString pc = dirProblem(capNew), pr = dirProblem(recNew);
    QString bad;
    if (!pc.isEmpty())
        bad = QStringLiteral("截图目录%1").arg(pc);
    if (!pr.isEmpty())
        bad += (bad.isEmpty() ? QStringLiteral("录像目录%1").arg(pr)
                              : QStringLiteral("；录像目录%1").arg(pr));
    if (!bad.isEmpty()) {
        Utils::log(QStringLiteral("paths change refused: %1").arg(bad));
        refreshPathInfo();
        m_dirInfo->setText(QStringLiteral("没改：%1。还按原来的地方存。").arg(bad));
    } else {
        const QString capOld = cfg.value("paths/captures").toString();
        const QString recOld = cfg.value("paths/recordings").toString();
        if (capNew != capOld || recNew != recOld) {
            cfg.setValue("paths/captures", capNew);
            cfg.setValue("paths/recordings", recNew);
            cfg.sync();               // 让 Utils::capturesDir() 下一次就读到新值
            CaptureHistory::reload(); // 往期列表改成从新目录读（不删任何文件）
            for (QWidget* w : QApplication::topLevelWidgets())
                if (w->inherits("PinWindow"))
                    w->update();      // 开着的置顶窗左边那条缩略图带当场换掉
            Utils::log(QStringLiteral("paths changed: captures=%1 recordings=%2")
                           .arg(Utils::capturesDir(), Utils::recordingsDir()));
        }
        refreshPathInfo();
    }

    ClipboardStore::instance().setEnabled(m_clip->isChecked());
    m_clipInfo->setText(clipInfoText());

    // 只在真的变了时才写注册表，写完回读一遍把系统里的实际值显示出来
    if (m_imePunct->isEnabled() && imePunctState() != (m_imePunct->isChecked() ? 1 : 0)) {
        const bool ok = setImePunct(m_imePunct->isChecked());
        Utils::log(QString("ime: UseEnglishPunctuationsInChineseInputMode -> %1 ok=%2 now=%3")
                       .arg(m_imePunct->isChecked() ? 1 : 0)
                       .arg(ok ? 1 : 0)
                       .arg(imePunctState()));
    }
    showImeState(m_imePunct, m_imePunctInfo);
}

void ConfigPage::addPinnedApp()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择固定软件或脚本"), QDir::rootPath(),
        QStringLiteral("程序和脚本 (*.exe *.lnk *.bat *.cmd);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext != QLatin1String("exe") && ext != QLatin1String("lnk")
        && ext != QLatin1String("bat") && ext != QLatin1String("cmd")) {
        QMessageBox::warning(this, QStringLiteral("不支持的启动目标"),
                             QStringLiteral("固定软件只支持 exe、lnk、bat 和 cmd 文件。"));
        return;
    }
    QString error;
    if (!PinnedApps::add(PinnedApps::fromTarget(path),-1,-1,&error)) {
        m_pinnedAppsInfo->setText(error);return;
    }
    load();
}

void ConfigPage::removePinnedApp()
{
    auto* item = m_pinnedApps->currentItem();
    if (!item)
        return;
    const QString id = item->data(Qt::UserRole).toString();
    QString error;
    if (!PinnedApps::remove(id, &error))
        m_pinnedAppsInfo->setText(error);
    else
        load();
}

void ConfigPage::togglePinnedApp()
{
    auto* item = m_pinnedApps->currentItem();
    if (!item)
        return;
    const QString id = item->data(Qt::UserRole).toString();
    auto apps = PinnedApps::load();
    for (PinnedApp& app : apps) {
        if (app.id == id) {
            app.enabled = !app.enabled;
            break;
        }
    }
    QString error;
    if (!PinnedApps::save(apps, &error))
        m_pinnedAppsInfo->setText(error);
    else
        load();
}

void ConfigPage::movePinnedUp() { editPinnedApp(); }
void ConfigPage::movePinnedDown() { editPinnedApp(); }

void ConfigPage::editPinnedApp()
{
    auto* item=m_pinnedApps->currentItem();if (!item) return;
    if (PinnedApps::editProperties(item->data(Qt::UserRole).toString(),this)) load();
}

void ConfigPage::importThemePackage()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("导入主题包"), QDir::homePath(),
        QStringLiteral("主题包 (*.zip);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!Theme::importPackage(path, &error)) {
        m_themePackageInfo->setText(QStringLiteral("导入失败：%1").arg(error));
        return;
    }
    load();
}

void ConfigPage::removeThemePackage()
{
    const QString id = m_themePackages->currentData().toString();
    if (id.isEmpty())
        return;
    QString error;
    if (!Theme::removePackage(id, &error))
        m_themePackageInfo->setText(error);
    else
        load();
}

void ConfigPage::activateThemePackage(int index)
{
    if (index < 0)
        return;
    const QString id = m_themePackages->itemData(index).toString();
    QString error;
    if (!Theme::activatePackage(id, &error)) {
        m_themePackageInfo->setText(QStringLiteral("切换失败：%1").arg(error));
        return;
    }
    syncThemeControls();
}

void ConfigPage::syncThemeControls()
{
    const auto package=Theme::currentPackage();
    const QSignalBlocker darkBlocker(m_themeDark), lightBlocker(m_themeLight), packageBlocker(m_themePackages);
    m_themePackages->setCurrentIndex(m_themePackages->findData(package.id));
    m_themeDark->setChecked(Theme::current()==Theme::Mode::Dark);
    m_themeLight->setChecked(Theme::current()==Theme::Mode::Light);
    m_themeDark->setEnabled(true);
    m_themeLight->setEnabled(true);
    m_themePackageInfo->setText(QStringLiteral("当前主题：%1。黑灰白为内置主题；第三方主题仅改变启动器；设置页使用独立的日间/夜间配色。")
        .arg(package.name));
}

void ConfigPage::importHotaruConfig()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("导入萤配置"), QDir::homePath(),
        QStringLiteral("JSON 配置 (*.json);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!PinnedApps::importConfig(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), error);
        return;
    }
    QString themeError;
    if (!Theme::activatePackage(PinnedApps::config().themeId, &themeError)) {
        QMessageBox::warning(this, QStringLiteral("主题未切换"), themeError);
    }
    load();
    m_pinnedAppsInfo->setText(QStringLiteral("配置已导入。"));
}

void ConfigPage::exportHotaruConfig()
{
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出萤配置"), QDir::homePath() + QStringLiteral("/hotaru-config.json"),
        QStringLiteral("JSON 配置 (*.json);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!PinnedApps::exportConfig(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        return;
    }
    m_pinnedAppsInfo->setText(QStringLiteral("配置已导出：%1").arg(path));
}

void ConfigPage::refreshPathInfo()
{
    const QString cap = Utils::capturesDir();
    const QString rec = Utils::recordingsDir();
    m_capDir->setText(QDir::toNativeSeparators(cap));
    m_recDir->setText(QDir::toNativeSeparators(rec));
    // 说明行不再重复整条路径（上面两个框里就是），只报数量和大小。
    // 截图要数两种名字：往期是 cap-*.png，手动"保存"那张是 capture-*.png
    m_dirInfo->setText(QStringLiteral("当前：截图 %1 · 录像 %2 · 置顶窗往期 %3 张\n"
                                       "改目录只影响以后存的文件，已有的文件不会搬走。")
                           .arg(dirSummary(cap, { QStringLiteral("cap-*.png"),
                                                  QStringLiteral("capture-*.png") }),
                                dirSummary(rec, { QStringLiteral("rec-*.mp4") }),
                                QString::number(CaptureHistory::count())));
}

void ConfigPage::onOpenDataDir()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(Utils::dataDir()));
}

void ConfigPage::onClearIndexCache()
{
    QFile::remove(Utils::dataDir() + "/deepindex.dat");
    QFile::remove(Utils::dataDir() + "/usnstate.dat");
    Utils::log("index cache cleared from settings");
    emit rescanRequested();
}

void ConfigPage::addAppRoot()
{
    const QString start = m_appRoots->count()
        ? m_appRoots->item(m_appRoots->count() - 1)->text()
        : QDir::rootPath();
    const QString got = QFileDialog::getExistingDirectory(this, QStringLiteral("选应用目录"), start);
    if (got.isEmpty())
        return;
    const QString nat = QDir::toNativeSeparators(got);
    for (int i = 0; i < m_appRoots->count(); ++i)
        if (m_appRoots->item(i)->text().compare(nat, Qt::CaseInsensitive) == 0)
            return;   // 同一个目录列两遍没有意义
    new QListWidgetItem(nat, m_appRoots);
}

void ConfigPage::addAppExe()
{
    // 点名加某个 exe：这一条不走"像不像主程序"那两道判断，加了就一定在应用列表里 ——
    // 目录扫描必然有误杀，这是给它留的出路（也是"这软件根本不在开始菜单"的那类东西）
    const QString last = m_appRoots->count()
        ? m_appRoots->item(m_appRoots->count() - 1)->text() : QString();
    const QFileInfo li(last);
    const QString start = last.isEmpty() ? QDir::rootPath()
                                         : (li.isDir() ? last : li.absolutePath());
    const QString got = QFileDialog::getOpenFileName(this, QStringLiteral("选要加进搜索的应用（exe）"),
                                                     start, QStringLiteral("程序 (*.exe)"));
    if (got.isEmpty())
        return;
    const QString nat = QDir::toNativeSeparators(got);
    for (int i = 0; i < m_appRoots->count(); ++i)
        if (m_appRoots->item(i)->text().compare(nat, Qt::CaseInsensitive) == 0)
            return;                       // 同一个 exe 列两遍没有意义
    new QListWidgetItem(nat, m_appRoots);
}

void ConfigPage::removeAppRoot()
{
    delete m_appRoots->currentItem();
}

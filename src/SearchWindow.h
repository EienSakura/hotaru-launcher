#pragma once
#include "core-api.h"

#include <QPointer>
#include <QWidget>
#include <QElapsedTimer>
#include <QVector>
#include <QHash>
#include <QSet>

#include "AppScanner.h"
#include "DevEnv.h"
#include "RegClean.h"
#include "PinnedApps.h"

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QLabel;
class QToolButton;
class QTimer;
class QMenu;
class QFileSystemWatcher;
class QScrollArea;
class QGridLayout;
class LauncherGrid;
class LauncherSkin;

// 搜索结果条目
struct SearchResult {
    enum Kind {
        App,          // 应用：Enter 启动，target=目标 exe（可空），extra=lnk 路径
        File,         // 文件/文件夹：Enter 打开
        Web,          // 网址/网站关键字：Enter 用默认浏览器打开
        Command,      // 命令：rescan / init / path / reg
        PinnedApp,    // 固定软件：target=配置中的 id
        PluginEntry,  // 开发环境插件入口
        PluginItem,   // 插件组件（安装/重检）
        RegIssue,     // 注册表问题（Enter 删除）
        Info,         // 纯信息行，不可激活
    };
    Kind kind = Info;
    QString title;
    QString subtitle;
    QString target;   // 语义见各 kind
    QString extra;    // 语义见各 kind
    QIcon icon;
    bool actionable = true;
    bool isSection = false;
};

class LTOOL_CORE_API SearchWindow : public QWidget {
    Q_OBJECT
public:
    explicit SearchWindow(QWidget* parent = nullptr);

    void toggleVisible();
    // why = 是谁要求的这一次呼出。三条路都能把搜索框弹出来（热键、托盘左键单击、
    // 第二个实例转交过来的 "show"），而**只有热键那条以前留日志** —— 于是"它怎么自己出来了"
    // 在现场查不到。现在每一条都必须报上名字（2026-10-21 他问的就是这个）。
    void showLauncher(const char* why);
    void warmUp();   // 启动后先映射+绘制一遍，别把这份开销留给第一次按热键

    // 失焦那一条到底该不该收：抽成静态纯函数，是为了这两支能在离屏问出来
    //（离屏**抢不到前台**，真造不出"拿到过前台之后点走"那一次，只能问谓词本身）。
    enum class BlurAction {
        HideNow,                // 立刻收
        IgnoreActivationRace,   // 先不动，宽限一过自己复查（#40 那一支）
        WaitCaptureEnds,        // F1 截图进行中，等它结束
        StayMenuOpen,           // 自己的右键菜单开着
    };
    // backToPrevious = 前台现在回到了**这次呼出之前**那个窗口（m_prevFg）。
    // 这一颗是为了不把两件事混成一件：拿到过前台之后失焦，
    //   回到原主 = 我们的激活被 Windows 撤回去（前台锁），那才是 #40 要保的那一支；
    //   去了别的窗口 = 他真的点走了，就该立刻收。
    static BlurAction blurAction(bool hadForeground, bool backToPrevious, qint64 msSinceShow,
                                 bool menuVisible, bool capturing);
    void rescan() { startScan(false, true); }        // 手动：强制全量重建
    void startInitialScan() { startScan(false, false); } // 启动：增量优先
    void setHotkeyLabel(const QString& label);
    void setStatus(const QString& detail);
    void openSettings();
    // 弹设置页 → 工具页 → 展开那个插件的页（工具都是 DLL 插件，主程序不再 new 它们）。
    // text 非空 = 把这段递给那个工具（JSON 解析 / Base64 解码那两条）。
    void openTool(const QString& id, const QString& text = QString());
    // 托盘「关于」：弹设置页并翻到那一行（行号就是左侧菜单的顺序）
    void openSettingsPage(int menuRow);

    const SearchResult& resultAt(int i) const { return m_results[i]; }
    int resultCount() const { return m_results.size(); }   // 给离屏自测用（--pinyin 数第一行是谁）
    // 把还压在 120ms 去抖里的那一拍立刻补上（回车之前必须做，见 .cpp 里 activateCurrent 的注释）。
    // 公开是为了离屏判据能量到"回车用的是新列表还是少一个字母的旧列表"。
    void flushSearch();
    // 外部配置变化后刷新固定区和当前搜索条件。
    void refreshLauncher();
    PinnedApp resultPinnedApp(const SearchResult& result) const;
    bool canPinResult(const SearchResult& result) const;
    void resultAction(int index, bool pin);

signals:
    void hotkeyChanged(const QString& sequence);

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    bool event(QEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    void buildUi();
    void scheduleSearch();
    void runSearch();
    void startFileSearch(const QString& q);   // 深索引搜索放到线程池，别卡输入
    void rebuildList();
    void rebuildHome();
    void refreshSkin();
    void layoutLauncher();
    void addPinnedTarget(const QString& id = QString(), int row=-1, int column=-1, bool folder=false);
    void pinnedMenu(const QString& id,const QPoint& globalPos);
    void renamePinned(const QString& id);
    void deleteCurrentPage();
    void openGroup(const QString& id,int page=0);
    void closeGroup();
    void wireGrid(LauncherGrid* grid);
    void activatePinned(const QString& id);
    void applyGridDrop(LauncherGrid* grid,const QString& id,int row,int column,const QString& mergeTarget=QString());
    void addSection(const QString& text);
    void addItem(const SearchResult& r);
    void activateRow(int row);
    void activateCurrent();
    void moveCurrent(int dir);
    int firstActionableRow() const;

    void startScan(bool quiet, bool forceDeep);
    void watchAppDirs();        // 监听开始菜单/桌面：装了新软件马上进索引
    QString currentCommandTarget() const;   // 当前行若是命令条目，返回它的 target
    bool pasteClipToPrevious();             // Ctrl+Enter：放回剪贴板并粘给呼出前的窗口
    void handleInit(const QString& arg);
    void handlePath(const QString& rest);
    void handleReg(const QString& rest);

    void loadUsage();
    void saveUsage();
    void bumpUsage(const QString& lnkPath);
    void openContextMenu(const QPoint& pos);
    void updateHeight();

    QLineEdit* m_input = nullptr;
    QWidget* m_inputRow = nullptr;
    LauncherSkin* m_skin = nullptr;
    LauncherGrid* m_grid = nullptr;
    QWidget* m_groupPanel = nullptr;
    LauncherGrid* m_groupGrid = nullptr;
    QLabel* m_groupTitle = nullptr;
    QString m_openGroup,m_transferGroup;
    int m_transferPage=0,m_transferHomePage=0;
    QToolButton* m_settingsBtn = nullptr;
    QListWidget* m_list = nullptr;
    QScrollArea* m_homeScroll = nullptr;
    QWidget* m_homePage = nullptr;
    QWidget* m_pageBar = nullptr;
    QLabel* m_hint = nullptr;
    QMenu* m_menu = nullptr;
    QTimer* m_debounce = nullptr;
    bool m_searchOnly = false;
    QSize m_gridWindowSize;
    QSize m_gridMinimumSize = QSize(760, 420);
    QFileSystemWatcher* m_appWatch = nullptr;   // 快捷方式目录变了就重扫应用索引
    QTimer* m_appRescan = nullptr;              // 单次防抖：一次安装会触发几十条目录变更
    QTimer* m_periodicScan = nullptr;           // index/refreshHours 承诺的定时重扫
    QTimer* m_captureHold = nullptr;            // F1 浮层开着时推迟收起，浮层一关就补收
    QElapsedTimer m_lastAutoScan;
    WId m_prevFg = 0;        // 启动器弹出前的前台窗口（剪贴板历史要粘回给它）
    quint64 m_searchSeq = 0;      // 每次搜索递增，用于丢弃过期的异步结果
    bool m_fileSearchBusy = false;
    bool m_fileSearchRerun = false;

    QVector<SearchResult> m_results;
    QVector<RegIssue> m_regIssues;
    QVector<AppEntry> m_apps;
    QHash<QString, DevItem> m_detectItems; // "id|label" -> 检测结果
    QSet<QString> m_detecting;
    QHash<QString, int> m_usage;           // lnk 路径 -> 使用次数
    int m_detectRun = 0;
    QAtomicInt m_scanning{0};
    QElapsedTimer m_showTime; // 用于忽略弹出瞬间的失焦，避免闪烁隐藏
    QElapsedTimer m_showLatency;   // 从按下呼出到画出来的分段计时（诊断"唤出慢"）
    bool m_paintLogged = false;
    bool m_warmed = false;
    // 这一次呼出**有没有真的拿到过前台**。它是"失焦"该不该收起的那道判据的输入：
    // 拿到过 = 用户真的点走了（立刻收）；从来没拿到过 = 激活竞争/前台锁没抢到（先别动，见 blurAction）。
    bool m_hadForeground = false;
    bool m_settingsOpen = false;
    bool m_restoringPage = false;
    QPointer<QWidget> m_settingsDlg;   // 设置页开着时热键不再弹搜索框
};

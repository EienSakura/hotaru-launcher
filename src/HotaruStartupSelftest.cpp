#include "platform/Autostart.h"
#include "platform/CtrlTap.h"
#include "platform/ResidentLock.h"
#include "FileIndexer.h"
#include "SearchWindow.h"
#include "SettingsDialog.h"
#include "PluginHost.h"
#include "PluginStore.h"
#include "Utils.h"
#include <QSettings>
#include <QDir>
#include <QKeyEvent>
#include "SysTools.h"
#include "WebShortcuts.h"
#include <QCoreApplication>
#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QFrame>
#include <QGridLayout>
#include <QTabBar>
#include <QToolButton>
#include <QMessageBox>
#include <QTimer>
#include <QPointer>
#include <QFileInfo>
#include <QLineEdit>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QThread>
#include <QThreadPool>
#include <QUuid>
#include <cstdio>
#include <memory>
#include <vector>

int runHotaruStartupSelftest()
{
    int failures = 0;
    const auto check = [&](const char* name, bool ok) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };
    struct Event { unsigned key; bool down; unsigned time; bool injected = false; };
    const auto count = [](std::initializer_list<Event> events) {
        CtrlTap tap;
        int hits = 0;
        for (const auto& e : events)
            hits += tap.event(e.key, e.down, e.injected, e.time);
        return hits;
    };
    const unsigned l = CtrlTap::left, r = CtrlTap::right;
    check("完整双击在第二次抬起触发", count({{l,1,1},{l,0,31},{l,1,101},{l,0,131}}) == 1);
    check("第二次按下不提前触发", count({{l,1,1},{l,0,31},{l,1,101}}) == 0);
    check("连续 Ctrl+C 不触发", count({{l,1,1},{'C',1,10},{'C',0,20},{l,0,31},
        {l,1,101},{'C',1,110},{'C',0,120},{l,0,131}}) == 0);
    check("单按后接 Ctrl+V 不触发", count({{l,1,1},{l,0,31},{l,1,101},{'V',1,110},{'V',0,120},{l,0,131}}) == 0);
    check("长按不触发", count({{l,1,1},{l,0,301},{l,1,401},{l,0,431}}) == 0);
    check("自动重复不计次数", count({{l,1,1},{l,1,11},{l,1,21},{l,0,31}}) == 0);
    check("左右交叠不触发", count({{l,1,1},{r,1,10},{l,0,20},{r,0,31}}) == 0);
    check("不同侧点按不配对", count({{l,1,1},{l,0,31},{r,1,101},{r,0,131}}) == 0);
    check("超时不触发", count({{l,1,1},{l,0,31},{l,1,332},{l,0,351}}) == 0);
    check("注入取消候选", count({{l,1,1},{l,0,31},{l,1,101,true},{l,0,131,true}}) == 0);
    check("300ms 和 250ms 边界", count({{l,1,1},{l,0,251},{l,1,551},{l,0,801}}) == 1);
    check("迁移只操作隔离的模拟注册表", Platform::autostartSelftest());
    check("中文路径快照增量文件名及目录配额", FileIndexer::searchSelftest());
    const auto key = QStringLiteral("Hotaru.test.") + QUuid::createUuid().toString(QUuid::Id128);
    std::vector<std::unique_ptr<QProcess>> children;
    for (int i = 0; i < 8; ++i) {
        auto child = std::make_unique<QProcess>();
        child->start(QCoreApplication::applicationFilePath(), {"--hotaru-instance-probe", key});
        children.push_back(std::move(child));
    }
    int owners = 0, existing = 0;
    for (auto& child : children) {
        if (!child->waitForFinished(8000)) { child->kill(); child->waitForFinished(); continue; }
        if (child->exitStatus() != QProcess::NormalExit) continue;
        const auto out = child->readAllStandardOutput().trimmed();
        owners += out == "owner";
        existing += out == "existing";
    }
    check("八进程并发只有一个锁拥有者", owners == 1 && existing == 7);
    return failures ? 1 : 0;
}

int runHotaruSearchSelftest()
{
    int failures = 0;
    const auto check = [&](const char* name, bool ok) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    const auto tools = SysTools::systemTools();
    AppEntry calculator;
    for (const auto& tool : tools)
        if (SysTools::aliasMatches(tool, QStringLiteral("calc"))) {
            calculator = tool;
            break;
        }
    check("计算器来自实际系统目录", QFileInfo(calculator.target).isAbsolute()
          && QFileInfo::exists(calculator.target));
    AppEntry duplicate = calculator;
    duplicate.name = QStringLiteral("calc");
    duplicate.category = QStringLiteral("startmenu");
    AppScanner::saveIndex({duplicate});

    WebEntry quick;
    quick.name = QStringLiteral("我的计算器快捷搜索");
    quick.keys = {QStringLiteral("calc")};
    quick.url = QStringLiteral("https://example.invalid/calculator");
    check("写入隔离的快捷搜索规则", WebShortcuts::saveWebs({quick}));

    SearchWindow window;
    auto* input = window.findChild<QLineEdit*>(QStringLiteral("input"));
    if (!input)
        return 1;
    const auto query = [&](const QString& text) {
        input->setText(text);
        window.flushSearch();
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
    };
    query(QStringLiteral("calc"));
    int calcRows = 0;
    bool shortcutFirst = false;
    bool sawAction = false;
    for (int i = 0; i < window.resultCount(); ++i) {
        const auto& row = window.resultAt(i);
        if (row.isSection || !row.actionable)
            continue;
        if (!sawAction) {
            shortcutFirst = row.kind == SearchResult::QuickSearch || row.title == quick.name;
            sawAction = true;
        }
        if (row.kind == SearchResult::App && row.target == calculator.target)
            ++calcRows;
    }
    check("精确快捷搜索是首个可执行结果", shortcutFirst);
    check("同路径的系统应用和开始菜单副本只显示一项", calcRows == 1);
    query(QStringLiteral("cmd.exe"));
    bool cmdFirst = false;
    for (int i = 0; i < window.resultCount(); ++i) {
        const auto& row = window.resultAt(i);
        if (!row.isSection && row.actionable) {
            cmdFirst = row.kind == SearchResult::App
                && QFileInfo(row.target).fileName().compare(QStringLiteral("cmd.exe"), Qt::CaseInsensitive) == 0;
            break;
        }
    }
    check("系统程序扩展名别名排在网页兜底之前", cmdFirst);

    for (const auto& word : {QStringLiteral("clip"), QStringLiteral("jtb"),
             QStringLiteral("json {}"), QStringLiteral("b64 Zg=="), QStringLiteral("windows"),
             QStringLiteral("reg clean"), QStringLiteral("init java"), QStringLiteral("path add X"),
             QStringLiteral("sleep"), QStringLiteral("shutdown /s")}) {
        query(word);
        bool intercepted = false;
        for (int i = 0; i < window.resultCount(); ++i) {
            const auto& row = window.resultAt(i);
            intercepted |= row.target.startsWith(QLatin1String("clip:")) ||
                row.target.startsWith(QLatin1String("power:")) || row.target.startsWith(QLatin1String("tool:")) ||
                row.title.startsWith(QStringLiteral("当前剪贴板 ·"));
        }
        check((QStringLiteral("未注册的命令按普通搜索：") + word).toUtf8().constData(), !intercepted);
    }
    query(QStringLiteral("@"));
    bool noPeerMessage = false;
    for (int i = 0; i < window.resultCount(); ++i)
        noPeerMessage |= window.resultAt(i).title == QStringLiteral("没有可发送的已配对设备");
    check("直属文件插件未加载时 @ 保持普通搜索", !noPeerMessage);
    auto* list = window.findChild<QListWidget*>(QStringLiteral("list"));
    input->setText(QStringLiteral("旧查询"));
    window.flushSearch();
    input->setText(QStringLiteral("calc"));
    window.flushSearch();
    int selected = -1;
    for (int i = 0; i < window.resultCount(); ++i)
        if (window.resultAt(i).kind == SearchResult::App && window.resultAt(i).target == calculator.target)
            selected = i;
    if (list && selected >= 0)
        list->setCurrentRow(selected);
    for (int i = 0; i < 2; ++i) {
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
    }
    check("旧查询的异步返回不重置当前选择", list && selected >= 0 && list->currentRow() == selected);

    SettingsDialog dialog;
    dialog.openAt(1);
    const auto findButton = [&dialog](const QString& title) -> QPushButton* {
        for (auto* button : dialog.findChildren<QPushButton*>())
            if (button->text() == title)
                return button;
        return nullptr;
    };
    auto* tabs = dialog.findChild<QTabBar*>(QStringLiteral("pluginCategoryTabs"));
    auto* pluginHome = tabs ? tabs->parentWidget() : nullptr;
    const auto groups = dialog.findChildren<QToolButton*>(QStringLiteral("pluginGroupToggle"));
    QToolButton* services = nullptr;
    QToolButton* features = nullptr;
    for (auto* group : groups) {
        if (group->text() == QStringLiteral("基础服务")) services = group;
        if (group->text() == QStringLiteral("功能插件")) features = group;
    }
    const auto twoColumnCards = [pluginHome](QToolButton* group, int expected) {
        if (!group) return false;
        const auto cards = group->parentWidget()->findChildren<QFrame*>(QStringLiteral("pluginCard"));
        int visible = 0;
        for (auto* card : cards) {
            if (!card->isVisibleTo(pluginHome)) continue;
            const auto* badge = card->findChild<QLabel*>(QStringLiteral("hotaruNativeBadge"));
            if (badge && badge->text() != QStringLiteral("Hotaru直属")) return false;
            auto* grid = qobject_cast<QGridLayout*>(card->parentWidget()->layout());
            if (!grid || grid->indexOf(card) < 0) return false;
            int row, column, rowSpan, columnSpan;
            grid->getItemPosition(grid->indexOf(card), &row, &column, &rowSpan, &columnSpan);
            if (row != visible / 2 || column != visible % 2 || rowSpan != 1 || columnSpan != 1)
                return false;
            ++visible;
        }
        return visible == expected;
    };
    const auto visibleCards = [&](int serviceCount, int featureCount) {
        return twoColumnCards(services, serviceCount) && twoColumnCards(features, featureCount);
    };
    const auto installedCount = [](QToolButton* group) {
        return group ? group->parentWidget()->findChildren<QFrame*>(QStringLiteral("pluginCard")).size() : 0;
    };
    const auto serviceCount = installedCount(services), featureCount = installedCount(features);
    check("插件主页面保留官方仓库和本地导入入口，不显示来源切换栏",
        findButton(QStringLiteral("官方仓库")) && findButton(QStringLiteral("导入本地插件")) &&
        !dialog.findChild<QTabBar*>(QStringLiteral("pluginSourceTabs")) &&
        !dialog.findChild<QDialog*>(QStringLiteral("pluginRepositoryDialog")));
    check("全部默认展开两个分类，组内用两列卡片",
        tabs && tabs->count() == 3 && tabs->tabText(0) == QStringLiteral("全部") &&
        tabs->tabText(1) == QStringLiteral("基础服务") && tabs->tabText(2) == QStringLiteral("功能插件") &&
        services && features && services->isVisibleTo(pluginHome) && features->isVisibleTo(pluginHome) &&
        services->arrowType() == Qt::DownArrow && features->arrowType() == Qt::DownArrow &&
        visibleCards(serviceCount, featureCount));
    if (tabs && services && features) {
        services->toggle();
        bool collapseWorks = services->arrowType() == Qt::RightArrow && visibleCards(0, featureCount);
        features->toggle();
        collapseWorks = collapseWorks && features->arrowType() == Qt::RightArrow && visibleCards(0, 0);
        tabs->setCurrentIndex(1);
        collapseWorks = collapseWorks && !services->isVisibleTo(pluginHome) && visibleCards(serviceCount, 0);
        tabs->setCurrentIndex(2);
        collapseWorks = collapseWorks && !features->isVisibleTo(pluginHome) && visibleCards(0, featureCount);
        tabs->setCurrentIndex(0);
        collapseWorks = collapseWorks && visibleCards(0, 0);
        services->toggle();
        collapseWorks = collapseWorks && visibleCards(serviceCount, 0);
        features->toggle();
        check("分类可独立折叠，单独标签完整显示，返回全部保留折叠状态",
            collapseWorks && visibleCards(serviceCount, featureCount));
    } else {
        check("分类可独立折叠，单独标签完整显示，返回全部保留折叠状态", false);
    }
    auto* compatibility = findButton(QStringLiteral("ltool 兼容插件"));
    check("插件首页可进入兼容子页", compatibility != nullptr);
    if (compatibility)
        compatibility->click();
    check("兼容子页有返回插件首页的入口", findButton(QStringLiteral("返回插件首页")) != nullptr);
    if (QCoreApplication::arguments().contains(QStringLiteral("--compat-fixtures"))) {
        // 测试入口的数据目录已经隔离；插件目录由调用方预先布置，全程只读。
        PluginHost::prepareShortcuts();
        const auto hasTarget = [&](const QString& target) {
            for (int i = 0; i < window.resultCount(); ++i)
                if (window.resultAt(i).target == target) return true;
            return false;
        };
        query(QStringLiteral("b64 Zg=="));
        check("实际加载的 Base64 注册带参搜索", hasTarget(QStringLiteral("tool:base64")));
        query(QStringLiteral("windows"));
        check("已禁用的用户名插件不注册直达", !hasTarget(QStringLiteral("tool:user")));
        query(QStringLiteral("reg"));
        check("无效注册表插件不注册直达", !hasTarget(QStringLiteral("tool:registry")));
        query(QStringLiteral("clipboard"));
        check("遗留剪贴板 DLL 不扫描或加载", !PluginHost::meta(QStringLiteral("clipboard")) &&
            !PluginHost::plugin(QStringLiteral("clipboard")) && !hasTarget(QStringLiteral("tool:clipboard")));
        query(QStringLiteral("b64 Zg=="));
        if (list && hasTarget(QStringLiteral("tool:base64"))) {
            QSettings config(QDir(Utils::dataDir()).filePath(QStringLiteral("config.ini")), QSettings::IniFormat);
            config.setValue(QStringLiteral("store/pendingRemove"), QStringList{QStringLiteral("ltool-base64.dll")});
            config.sync();
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QCoreApplication::sendEvent(input, &enter);
            check("旧结果执行前拒绝待卸载插件并刷新", !hasTarget(QStringLiteral("tool:base64")));
        } else {
            check("待卸载测试能选中实际结果", false);
        }
    }

    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        bool servicesStopped = false;
        QPointer<SettingsDialog> settings;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, qApp, [] { QCoreApplication::exit(3); });
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, &window, [&] {
            servicesStopped = !settings;
        });
        QTimer::singleShot(0, &window, [&] {
            window.openSettings();
            for (auto* widget : QApplication::topLevelWidgets()) {
                if (auto* candidate = qobject_cast<SettingsDialog*>(widget); candidate && candidate->isVisible())
                    settings = candidate;
            }
            check("设置窗口打开后可发起退出", bool(settings));
            if (!settings) { QCoreApplication::exit(3); return; }
            QDialog modal(settings);
            modal.setModal(true);
            modal.show();
            window.requestQuit();
            window.requestQuit();
            check("有模态窗口时暂不弹退出确认", !settings->findChild<QMessageBox*>());
            modal.close();
            QTimer::singleShot(220, &window, [&] {
                auto* prompt = settings ? settings->findChild<QMessageBox*>() : nullptr;
                check("模态窗口关闭后出现退出确认", prompt && prompt->isVisible());
                if (!prompt) { QCoreApplication::exit(3); return; }
                QStringList labels;
                for (const auto* button : prompt->buttons()) labels << button->text();
                check("退出确认只显示是否退出及确认取消", prompt->text() == QStringLiteral("是否退出 Hotaru？") &&
                    labels.size() == 2 && labels.contains(QStringLiteral("确认")) && labels.contains(QStringLiteral("取消")));
                window.requestQuit();
                check("重复退出只复用一个弹窗", settings->findChildren<QMessageBox*>().size() == 1);
                prompt->defaultButton()->click();
                check("取消后保留设置窗口", settings && settings->isVisible());
                QTimer::singleShot(0, &window, [&] {
                    window.requestQuit();
                    if (!settings) { QCoreApplication::exit(3); return; }
                    for (auto* box : settings->findChildren<QMessageBox*>()) {
                        if (!box->isVisible()) continue;
                        for (auto* button : box->buttons()) {
                            if (box->buttonRole(button) == QMessageBox::AcceptRole) {
                                button->click();
                                return;
                            }
                        }
                    }
                    QCoreApplication::exit(3);
                });
            });
        });
        timeout.start(3500);
        const int exitCode = QCoreApplication::exec();
        check("确认退出先销毁设置，再退出应用", exitCode == 0 && servicesStopped);
    }
    return failures ? 1 : 0;
}

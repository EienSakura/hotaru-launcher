#include "platform/Autostart.h"
#include "platform/CtrlTap.h"
#include "platform/ResidentLock.h"
#include "FileIndexer.h"
#include "SearchWindow.h"
#include "SettingsDialog.h"
#include "SysTools.h"
#include "WebShortcuts.h"
#include <QCoreApplication>
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
    auto* compatibility = findButton(QStringLiteral("ltool 兼容插件"));
    check("插件首页可进入兼容子页", compatibility != nullptr);
    if (compatibility)
        compatibility->click();
    check("兼容子页有返回插件首页的入口", findButton(QStringLiteral("返回插件首页")) != nullptr);
    return failures ? 1 : 0;
}

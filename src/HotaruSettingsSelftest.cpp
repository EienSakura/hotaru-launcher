#include "BuildChannel.h"
#include "ConfigPage.h"
#include "HotaruNativeHost.h"
#include "HotaruPluginRepository.h"
#include "InstallLogDialog.h"
#include "LauncherGrid.h"
#include "LauncherTheme.h"
#include "PinnedApps.h"
#include "PluginHost.h"
#include "SearchWindow.h"
#include "SettingsDialog.h"
#include "SilentInstallLog.h"
#include "Utils.h"
#include "WebShortcuts.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QFocusFrame>
#include <QFrame>
#include <QGroupBox>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryFile>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

#include <windows.h>

namespace {

bool hasSmoothFrame(const QPixmap& shot)
{
    const QImage image = shot.toImage();
    if (!image.hasAlphaChannel()
        || image.pixelColor(image.width() / 2, 1).alpha() != 255
        || image.pixelColor(image.width() / 2, image.height() - 2).alpha() != 255)
        return false;
    const int depth = qRound(12 * shot.devicePixelRatio());
    for (bool right : {false, true}) {
        for (bool bottom : {false, true}) {
            const int x = right ? image.width() - 1 : 0;
            const int y = bottom ? image.height() - 1 : 0;
            if (image.pixelColor(x, y).alpha() != 0)
                return false;
            bool blended = false;
            for (int dy = 1; dy < depth; ++dy) {
                for (int dx = 1; dx < depth; ++dx) {
                    const int alpha = image.pixelColor(x + (right ? -dx : dx), y + (bottom ? -dy : dy)).alpha();
                    blended |= alpha > 0 && alpha < 255;
                }
            }
            if (!blended)
                return false;
        }
    }
    return true;
}

bool hasNativeRoundedFrame(HWND hwnd, const QSize& size)
{
    if (!hwnd || size.isEmpty())
        return false;
    const int right = size.width() - 1;
    const int bottom = size.height() - 1;
    const HRGN region = CreateRectRgn(0, 0, 0, 0);
    if (!region)
        return false;
    const bool rounded = GetWindowRgn(hwnd, region) == COMPLEXREGION
        && !PtInRegion(region, 0, 0) && !PtInRegion(region, right, 0)
        && !PtInRegion(region, 0, bottom) && !PtInRegion(region, right, bottom)
        && PtInRegion(region, right / 2, 0) && PtInRegion(region, right / 2, bottom)
        && PtInRegion(region, 0, bottom / 2) && PtInRegion(region, right, bottom / 2);
    DeleteObject(region);
    return rounded;
}

int runLauncherPopupAudit(const QString& package)
{
    if (QGuiApplication::platformName() != QLatin1String("windows"))
        return 1;

    QString error;
    if (!Theme::importPackage(package, &error)) {
        std::printf("FAIL 导入测试主题：%s\n", qPrintable(error));
        return 1;
    }
    const auto groupId = QStringLiteral("popup-audit-group");
    auto fixture = PinnedApps::config();
    auto pin = PinnedApps::fromTarget(QCoreApplication::applicationFilePath());
    pin.title = QStringLiteral("固定软件测试");
    pin.gridRow = 0;
    pin.gridColumn = 0;
    fixture.pinnedApps = {pin};
    fixture.groups = {{groupId, QStringLiteral("测试分组"), 0, 1}};
    fixture.pages = 1;
    fixture.lastPage = 0;
    if (!PinnedApps::saveConfig(fixture, &error)) {
        std::printf("FAIL 保存测试格位：%s\n", qPrintable(error));
        return 1;
    }

    // 保留真实右键入口和弹出流程，但在 Show 时拦住 HWND 上屏。
    class HidePopups final : public QObject {
    public:
        QPointer<QMenu> shown;

    private:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (event->type() == QEvent::Show) {
                if (auto* menu = qobject_cast<QMenu*>(watched)) {
                    menu->setAttribute(Qt::WA_DontShowOnScreen);
                    shown = menu;
                }
            }
            return false;
        }
    } hidePopups;
    qApp->installEventFilter(&hidePopups);
    QApplication::setEffectEnabled(Qt::UI_AnimateMenu, false);

    SearchWindow launcher;
    launcher.setAttribute(Qt::WA_DontShowOnScreen);
    launcher.show();
    launcher.refreshLauncher();
    auto* grid = launcher.findChild<LauncherGrid*>(QStringLiteral("hotaruGrid"));
    auto* pages = launcher.findChild<QWidget*>(QStringLiteral("hotaruPages"));
    if (!grid || !pages)
        return 1;

    int failures = 0;
    const auto shots = QDir::tempPath() + QStringLiteral("/launcher-popups");
    QDir().mkpath(shots);
    const auto clickMenu = [&](QWidget* owner, const QPoint& pos, const char* name, const QStringList& expected) {
        bool complete = false;
        hidePopups.shown.clear();
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &launcher, [&] {
            auto* menu = hidePopups.shown.data();
            if (!menu)
                return;
            const auto hwnd = reinterpret_cast<HWND>(menu->internalWinId());
            QStringList actions;
            for (auto* action : menu->actions())
                if (!action->isSeparator() && menu->rect().contains(menu->actionGeometry(action)))
                    actions.append(action->text());
            complete = hwnd && !IsWindowVisible(hwnd) && actions == expected
                && hasNativeRoundedFrame(hwnd, menu->size() * menu->devicePixelRatioF())
                && menu->grab().save(shots + QLatin1Char('/') + QLatin1String(name) + QStringLiteral(".png"));
            if (!complete)
                std::printf("  %s: %s\n", name, qPrintable(actions.join(QStringLiteral(" / "))));
            menu->hide();
        });
        timer.start(0);
        QContextMenuEvent click(QContextMenuEvent::Mouse, pos, owner->mapToGlobal(pos));
        QCoreApplication::sendEvent(owner, &click);
        QCoreApplication::processEvents();
        std::printf("%s %s\n", complete ? "ok" : "FAIL", name);
        failures += !complete;
    };
    clickMenu(grid, grid->cellRect(0, 2).center(), "grid-empty",
              {QStringLiteral("添加程序或脚本…"), QStringLiteral("添加磁盘文件夹…"),
               QStringLiteral("新建面板文件夹"), QStringLiteral("删除当前页")});
    clickMenu(grid, grid->cellRect(0, 0).center(), "pinned-app",
              {QStringLiteral("运行"), QStringLiteral("以管理员身份运行"), QStringLiteral("打开所在位置"),
               QStringLiteral("重命名…"), QStringLiteral("属性…"), QStringLiteral("移除")});
    clickMenu(grid, grid->cellRect(0, 1).center(), "group",
              {QStringLiteral("打开"), QStringLiteral("重命名…"), QStringLiteral("解散")});
    clickMenu(pages, pages->rect().center(), "pages", {QStringLiteral("删除当前页")});
    emit grid->activated(groupId);
    if (auto* group = launcher.findChild<LauncherGrid*>(QStringLiteral("hotaruGroupGrid"))) {
        clickMenu(group, group->cellRect(0, 0).center(), "group-empty",
                  {QStringLiteral("添加程序或脚本…"), QStringLiteral("添加磁盘文件夹…"), QStringLiteral("删除当前页")});
    } else {
        ++failures;
    }
    auto* input = launcher.findChild<QLineEdit*>(QStringLiteral("input"));
    auto* list = launcher.findChild<QListWidget*>(QStringLiteral("list"));
    if (!input || !list)
        return failures + 1;
    input->setText(pin.title);
    launcher.flushSearch();
    for (int row = 0; row < list->count(); ++row) {
        auto* item = list->item(row);
        const int index = item->data(Qt::UserRole).toInt();
        if (index >= 0 && launcher.resultAt(index).kind == SearchResult::PinnedApp) {
            clickMenu(list->viewport(), list->visualItemRect(item).center(), "search-pinned",
                      {QStringLiteral("运行"), QStringLiteral("以管理员身份运行"), QStringLiteral("打开所在位置"),
                       QStringLiteral("重命名…"), QStringLiteral("属性…"), QStringLiteral("移除")});
            std::printf("launcher popups: %d failures (%s)\n", failures, qPrintable(shots));
            return failures;
        }
    }
    std::puts("FAIL 找不到测试用固定软件搜索结果");
    return failures + 1;
}

int runSurfaceAudit(const QStringList& args)
{
    int bad = 0;
    if (QGuiApplication::platformName() != QLatin1String("offscreen"))
        return 1;
    const auto check = [&bad](const QString& label, bool pass) {
        std::printf("%s %s\n", pass ? "ok" : "FAIL", label.toUtf8().constData());
        if (!pass)
            ++bad;
    };
    const QString shots = QDir::tempPath() + QStringLiteral("/settings-surfaces");
    QDir().mkpath(shots);
    if (args.contains(QStringLiteral("--settings-repository-fixture"))) {
        const QString folder = HotaruPluginRepository::root();
        if (!QDir().mkpath(folder) || !QFile::copy(qEnvironmentVariable("HOTARU_SETTINGS_CATALOG_FIXTURE"),
                QDir(folder).filePath(QStringLiteral("catalog.json")))) {
            std::puts("FAIL 无法加载官方目录测试夹具");
            return 1;
        }
    }
    WebShortcuts::ensureDefaultWebs();
    auto fixture = PinnedApps::config();
    auto item = PinnedApps::fromTarget(QCoreApplication::applicationFilePath());
    item.title = QStringLiteral("Hotaru 开发版");
    item.gridRow = 0;
    item.gridColumn = 0;
    fixture.pinnedApps.append(item);
    auto hidden = PinnedApps::fromTarget(QCoreApplication::applicationDirPath());
    hidden.title = QStringLiteral("开发版目录");
    hidden.enabled = false;
    hidden.gridRow = 0;
    hidden.gridColumn = 1;
    fixture.pinnedApps.append(hidden);
    check(QStringLiteral("设置截图夹具可写入"), PinnedApps::saveConfig(fixture));
    const bool lanFixture = args.contains(QStringLiteral("--settings-lan-fixture"));
    if (lanFixture)
        HotaruNativeHost::start();
    for (const auto mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::setMode(mode);
        const auto suffix = mode == Theme::Mode::Dark ? QStringLiteral("-dark.png") : QStringLiteral("-light.png");
        SettingsDialog window;
        window.resize(1140, 790);
        window.show();
        QCoreApplication::processEvents();
        const auto* initial = window.focusWidget();
        check(QStringLiteral("设置页打开时焦点落在导航"), initial && initial->objectName() == "menuList");
        const auto shot = [&](QWidget* widget, const QString& name) {
            widget->grab();
            QCoreApplication::processEvents();
            return widget->grab().save(shots + "/" + name + suffix);
        };
        const auto captureCombo = [&](QWidget* parent, const char* name) {
            auto* combo = parent->findChild<QComboBox*>(QLatin1String(name));
            if (!combo) {
                check(QLatin1String(name), false);
                return;
            }
            combo->setFocus(Qt::TabFocusReason);
            combo->showPopup();
            QCoreApplication::processEvents();
            auto* list = combo->view();
            auto* popup = list->window();
            const QRect control(combo->mapToGlobal(QPoint()), combo->size());
            const QRect listRect(list->mapToGlobal(QPoint()), list->size());
            const QRect lastRow = list->visualRect(combo->model()->index(combo->count() - 1, 0));
            const QRect screen = popup->screen()->availableGeometry();
            const int expectedLeft = qBound(screen.left(), control.left(),
                qMax(screen.left(), screen.right() + 1 - listRect.width()));
            const bool saved = shot(popup, QStringLiteral("dropdown-") + QLatin1String(name));
            const bool fit = popup != combo->window() && list->isVisible()
                && listRect.left() == expectedLeft && listRect.width() >= qMin(screen.width(), control.width())
                && (listRect.top() >= control.bottom() || listRect.bottom() <= control.top())
                && lastRow.isValid() && list->viewport()->rect().contains(lastRow);
            if (!fit) {
                std::printf("  control=%d,%d %dx%d list=%d,%d %dx%d last=%d,%d %dx%d viewport=%dx%d\n",
                    control.x(), control.y(), control.width(), control.height(),
                    listRect.x(), listRect.y(), listRect.width(), listRect.height(),
                    lastRow.x(), lastRow.y(), lastRow.width(), lastRow.height(),
                    list->viewport()->width(), list->viewport()->height());
            }
            check(QLatin1String(name), fit && saved);
            const auto mask = popup->mask();
            check(QStringLiteral("展开列表保留上下圆角"), mask.contains(popup->rect().center())
                && !mask.contains(popup->rect().topLeft()) && !mask.contains(popup->rect().topRight())
                && !mask.contains(popup->rect().bottomLeft()) && !mask.contains(popup->rect().bottomRight()));
            if (QLatin1String(name) == QLatin1String("settingsSearchEngine")) {
                auto* host = combo->window();
                auto combined = host->grab();
                QPainter painter(&combined);
                painter.drawPixmap(host->mapFromGlobal(listRect.topLeft()), popup->grab());
                painter.end();
                check(QStringLiteral("下拉触发器和展开列表同时截图"),
                    combined.save(shots + "/dropdown-expanded" + suffix));
            }
            const int selected = combo->currentIndex();
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QCoreApplication::sendEvent(list, &escape);
            check(QStringLiteral("Esc 取消下拉选择"), !list->isVisible() && combo->currentIndex() == selected);
            if (list->isVisible())
                combo->hidePopup();

            if (QLatin1String(name) == QLatin1String("themePackages")) {
                const int count = combo->count();
                for (int i = 0; i < 12; ++i)
                    combo->addItem(QStringLiteral("测试主题 %1").arg(i + 1));
                combo->showPopup();
                QCoreApplication::processEvents();
                list->scrollToBottom();
                const QRect end = list->visualRect(combo->model()->index(combo->count() - 1, 0));
                check(QStringLiteral("较长主题列表可滚动至最后一项"), list->verticalScrollBar()->maximum() > 0
                    && end.isValid() && list->viewport()->rect().contains(end));
                combo->hidePopup();
                while (combo->count() > count)
                    combo->removeItem(combo->count() - 1);
            }
        };
        const auto captureModal = [&](const QString& dialogName, const QString& file, auto open) {
            bool passed = false;
            const auto capture = [&] {
                for (auto* top : QApplication::topLevelWidgets()) {
                    auto* dialog = qobject_cast<QDialog*>(top);
                    if (!dialog || !dialog->isVisible()
                        || (dialog->objectName() != dialogName && dialog->windowTitle() != dialogName))
                        continue;
                    auto* close = dialog->findChild<QToolButton*>(QStringLiteral("hotaruModalClose"));
                    auto* header = dialog->findChild<QFrame*>(QStringLiteral("hotaruModalHeader"));
                    passed = close && header && dialog->property("hotaruModalDialog").toBool()
                        && shot(dialog, file) && hasSmoothFrame(dialog->grab());
                    if (dialogName == QStringLiteral("卸载插件")) {
                        auto* box = qobject_cast<QMessageBox*>(dialog);
                        passed &= box && box->text().contains(QLatin1String(kHotaruLanCoreId))
                            && box->text().contains(QLatin1String(kHotaruLanClipboardId))
                            && box->text().contains(QLatin1String(kHotaruLanFilesId))
                            && box->defaultButton() == box->button(QMessageBox::Cancel);
                    }
                    if (dialogName == QLatin1String("quickSearchDialog"))
                        captureCombo(dialog, "quickType");
                    if (dialogName == QLatin1String("pluginRepositoryDialog")
                        && args.contains(QStringLiteral("--settings-repository-fixture"))) {
                        QPushButton* install = nullptr;
                        for (auto* action : dialog->findChildren<QPushButton*>(QStringLiteral("pluginCatalogInstall"))) {
                            if (action->isEnabled()) {
                                install = action;
                                break;
                            }
                        }
                        check(QStringLiteral("正式目录夹具显示直属徽标"),
                            install && dialog->findChild<QLabel*>(QStringLiteral("hotaruNativeBadge")));
                        if (install) {
                            bool confirmationSeen = false;
                            QTimer cancel;
                            cancel.setSingleShot(true);
                            QObject::connect(&cancel, &QTimer::timeout, dialog, [&] {
                                auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                                if (!box || box == dialog)
                                    return;
                                auto* chrome = box->findChild<QToolButton*>(QStringLiteral("hotaruModalClose"));
                                confirmationSeen = box->windowTitle() == QStringLiteral("确认安装插件")
                                    && box->windowFlags().testFlag(Qt::FramelessWindowHint) && chrome
                                    && box->defaultButton() == box->button(QMessageBox::Cancel)
                                    && shot(dialog, QStringLiteral("official-catalog"))
                                    && shot(box, QStringLiteral("modal-install-confirm"));
                                if (chrome)
                                    chrome->click();
                                else
                                    box->close();
                            });
                            cancel.start(0);
                            install->click();
                            check(QStringLiteral("实际安装确认使用圆角标题栏，默认取消"), confirmationSeen);
                        }
                    }
                    if (close)
                        close->click();
                    else
                        dialog->reject();
                    return;
                }
            };
            QTimer timer;
            timer.setSingleShot(true);
            QObject::connect(&timer, &QTimer::timeout, &window, capture);
            timer.start(0);
            open();
            if (timer.isActive()) {
                timer.stop();
                capture();
            }
            check(dialogName, passed);
        };
        const auto click = [&window](const char* name) {
            if (auto* button = window.findChild<QPushButton*>(QLatin1String(name)))
                button->click();
        };
        window.openAt(0);
        check(QStringLiteral("快捷搜索截图"), shot(&window, QStringLiteral("shortcuts")));
        captureModal(QStringLiteral("quickSearchDialog"), QStringLiteral("modal-quick-search"),
                     [&] { click("quickAddButton"); });
        window.openAt(1);
        check(QStringLiteral("插件截图"), shot(&window, QStringLiteral("plugins")));
        if (lanFixture) {
            QPushButton* uninstall = nullptr;
            for (auto* button : window.findChildren<QPushButton*>()) {
                if (button->text() == QStringLiteral("卸载") && button->isVisible() && button->isEnabled()) {
                    uninstall = button;
                    break;
                }
            }
            check(QStringLiteral("可通过插件卡片打开卸载确认"), uninstall);
            if (uninstall) {
                captureModal(QStringLiteral("卸载插件"), QStringLiteral("modal-uninstall-plugin"),
                             [uninstall] { uninstall->click(); });
                check(QStringLiteral("关闭卸载确认后仍可卸载，操作未提交"), uninstall->isEnabled());
            }
        }
        captureModal(QStringLiteral("pluginRepositoryDialog"), QStringLiteral("modal-plugin-repository"),
                     [&] { click("openPluginRepository"); });
        window.openAt(1, 0);
        check(QStringLiteral("兼容插件截图"), shot(&window, QStringLiteral("plugins-compatibility")));
        window.openAt(2, 0);
        check(QStringLiteral("通用截图"), shot(&window, QStringLiteral("general")));
        check(QStringLiteral("去掉快捷跳转行，启动与快捷键设置仍在"),
            window.findChildren<QPushButton*>(QStringLiteral("settingsJump")).isEmpty()
            && window.findChild<QPushButton*>(QStringLiteral("hotkeyCaptureButton")));
        const bool providers = args.contains(QStringLiteral("--settings-compatibility-fixture"));
        const auto* shell = window.findChild<QGroupBox*>(QStringLiteral("settingsShellGroup"));
        const QLabel* installRoot = nullptr;
        auto* openData = window.findChild<QPushButton*>(QStringLiteral("settingsOpenData"));
        auto* clearIndex = window.findChild<QPushButton*>(QStringLiteral("settingsClearIndex"));
        for (auto* label : window.findChildren<QLabel*>())
            if (label->text() == QStringLiteral("第三方软件安装根目录："))
                installRoot = label;
        check(QStringLiteral("兼容设置跟随可用插件显示"),
            shell && installRoot && shell->isVisible() == providers && installRoot->isVisible() == providers);
        auto* roots = window.findChild<QListWidget*>(QStringLiteral("settingsAppRoots"));
        auto* addDir = window.findChild<QPushButton*>(QStringLiteral("addAppDir"));
        auto* addExe = window.findChild<QPushButton*>(QStringLiteral("addAppExe"));
        auto* remove = window.findChild<QPushButton*>(QStringLiteral("removeAppRoot"));
        check(QStringLiteral("项目列表与三个按钮保留间距"), roots && addDir && addExe && remove
            && addDir->x() - roots->geometry().right() >= 12
            && addExe->y() - addDir->geometry().bottom() >= 8
            && remove->y() - addExe->geometry().bottom() >= 8);
        check(QStringLiteral("数据操作按钮保留间距"), openData && clearIndex
            && clearIndex->x() - openData->geometry().right() >= 8);
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("cfgTabs"));
        auto* scroll = tabs ? qobject_cast<QScrollArea*>(tabs->currentWidget()) : nullptr;
        if (scroll && roots && clearIndex) {
            scroll->ensureWidgetVisible(roots, 16, 48);
            check(QStringLiteral("应用搜索额外项目截图"), shot(&window, QStringLiteral("general-app-roots")));
            scroll->ensureWidgetVisible(clearIndex);
            check(QStringLiteral("数据与兼容设置截图"), shot(&window, QStringLiteral("general-data")));
            scroll->verticalScrollBar()->setValue(0);
        }
        captureCombo(&window, "settingsSearchEngine");
        captureModal(QStringLiteral("hotkeyCaptureDialog"), QStringLiteral("modal-hotkey"),
                     [&] { click("hotkeyCaptureButton"); });
        window.openAt(2, 1);
        check(QStringLiteral("外观截图"), shot(&window, QStringLiteral("appearance")));
        auto* pinned = window.findChild<QListWidget*>(QStringLiteral("pinnedApps"));
        if (pinned && pinned->count() >= 2) {
            if (auto* appearance = tabs ? qobject_cast<QScrollArea*>(tabs->currentWidget()) : nullptr)
                appearance->ensureWidgetVisible(pinned);
            auto* first = pinned->itemWidget(pinned->item(0));
            auto* second = pinned->itemWidget(pinned->item(1));
            pinned->setCurrentRow(0);
            QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
            QCoreApplication::sendEvent(pinned, &down);
            const bool secondSelected = second && second->property("selected").toBool();
            QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
            QCoreApplication::sendEvent(pinned, &up);
            const auto* enabled = first ? first->findChild<QLabel*>(QStringLiteral("pinStatus")) : nullptr;
            const auto* disabled = second ? second->findChild<QLabel*>(QStringLiteral("pinStatus")) : nullptr;
            check(QStringLiteral("固定软件选中样式跟随条目，启用和停用铭牌垂直居中"),
                secondSelected && first->property("selected").toBool() && !second->property("selected").toBool()
                && enabled && disabled && enabled->property("active").toBool()
                && !disabled->property("active").toBool()
                && enabled->height() < first->height()
                && qAbs(enabled->geometry().center().y() - first->rect().center().y()) <= 1
                && disabled->height() < second->height()
                && qAbs(disabled->geometry().center().y() - second->rect().center().y()) <= 1);
            check(QStringLiteral("固定软件选中态截图"), shot(&window, QStringLiteral("appearance-pinned")));
        } else {
            check(QStringLiteral("固定软件测试夹具已显示"), false);
        }
        captureCombo(&window, "themePackages");
        captureCombo(&window, "pageStart");
        auto* footer = window.findChild<QFrame*>(QStringLiteral("settingsFooter"));
        auto* segments = window.findChild<QTabBar*>(QStringLiteral("settingsSegments"));
        check(QStringLiteral("保存状态栏铺满配置内容区，分段标签等宽"),
            footer && footer->x() == 0 && footer->width() == footer->parentWidget()->width()
            && segments && segments->tabRect(0).width() >= 70
            && segments->tabRect(0).width() == segments->tabRect(1).width());
        captureModal(QStringLiteral("addPinDialog"), QStringLiteral("modal-add-pin"),
                     [&] { click("pinnedAddButton"); });
        captureModal(QStringLiteral("pinPropertiesDialog"), QStringLiteral("modal-pin-properties"),
                     [&] { click("pinEditButton"); });
        window.openAt(3);
        check(QStringLiteral("关于截图"), shot(&window, QStringLiteral("about")));
        if (lanFixture) {
            for (const auto* id : {kHotaruLanCoreId, kHotaruLanClipboardId, kHotaruLanFilesId}) {
                const QString pluginId = QLatin1String(id);
                const bool opened = window.openNativePlugin(pluginId);
                QCoreApplication::processEvents();
                auto* back = window.findChild<QPushButton*>(QStringLiteral("nativeDetailBack"));
                auto* panel = back ? back->parentWidget() : nullptr;
                int cards = 0;
                if (panel) {
                    for (auto* card : panel->findChildren<QFrame*>(QStringLiteral("lanDetailCard")))
                        cards += card->isVisible();
                }
                check(QStringLiteral("直属插件详情 ") + pluginId, opened && cards == 3 && back->isVisible()
                    && shot(&window, pluginId));
                if (back)
                    back->click();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
        }
        captureModal(QStringLiteral("installLogDialog"), QStringLiteral("modal-install-log"),
                     [] { SilentInstallLog::showLog(); });
        captureModal(QStringLiteral("foregroundInstallLogDialog"), QStringLiteral("modal-foreground-install"),
                     [&] {
            InstallLogDialog log(&window, QStringLiteral("示例工具"), QStringLiteral("winget install"), {}, {});
            log.exec();
        });
        QMessageBox confirm(QMessageBox::Question, QStringLiteral("删除当前页"),
            QStringLiteral("是否迁移图标后删除？"), QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, &window);
        confirm.setDefaultButton(QMessageBox::Yes);
        confirm.show();
        check(QStringLiteral("标准确认窗保留默认按钮和三种操作"),
            confirm.buttons().size() == 3 && confirm.defaultButton() == confirm.button(QMessageBox::Yes)
            && confirm.property("hotaruModalDialog").toBool() && shot(&confirm, QStringLiteral("modal-confirm")));
        confirm.close();
        {
            SearchWindow search;
            if (auto* input = search.findChild<QLineEdit*>(QStringLiteral("input"))) {
                search.show();
                input->setText(QStringLiteral("cmd"));
                search.flushSearch();
                check(QStringLiteral("搜索结果标题与路径截图"),
                    search.resultCount() > 0 && shot(&search, QStringLiteral("search-result-spacing")));
                auto* list = search.findChild<QListWidget*>(QStringLiteral("list"));
                QListWidgetItem* item = nullptr;
                for (int row = 0; list && row < list->count(); ++row) {
                    auto* candidate = list->item(row);
                    const int index = candidate->data(Qt::UserRole).toInt();
                    if (index >= 0 && search.canPinResult(search.resultAt(index))) {
                        item = candidate;
                        break;
                    }
                }
                check(QStringLiteral("搜索结果包含固定与定位操作"), item);
                if (item) {
                    list->setCurrentItem(item);
                    list->scrollToItem(item);
                    auto* viewport = list->viewport();
                    const auto row = list->visualItemRect(item);
                    const QPoint folder(row.right() - 66, row.center().y() - 1);
                    const QPoint pin(row.right() - 26, folder.y());
                    const auto pixel = [viewport](const QPoint& position) {
                        const auto image = viewport->grab();
                        const auto sample = (position - QPoint(0, 12)) * image.devicePixelRatio();
                        return image.toImage().pixelColor(sample);
                    };
                    const auto hover = [viewport](const QPoint& position) {
                        QMouseEvent move(QEvent::MouseMove, position, viewport->mapToGlobal(position),
                            Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                        QCoreApplication::sendEvent(viewport, &move);
                    };
                    const auto idleFolder = pixel(folder);
                    const auto idlePin = pixel(pin);
                    hover(folder);
                    check(QStringLiteral("悬浮定位按钮只显示定位底色"),
                        pixel(folder) != idleFolder && pixel(pin) == idlePin && shot(&search, QStringLiteral("search-hover-folder")));
                    hover(pin);
                    check(QStringLiteral("悬浮固定按钮只显示固定底色"),
                        pixel(folder) == idleFolder && pixel(pin) != idlePin && shot(&search, QStringLiteral("search-hover-pin")));
                    QEvent leave(QEvent::Leave);
                    QCoreApplication::sendEvent(viewport, &leave);
                    check(QStringLiteral("移开鼠标时两个操作恢复"),
                        pixel(folder) == idleFolder && pixel(pin) == idlePin);
                }
            }
        }
    }
    if (lanFixture)
        HotaruNativeHost::stop();
    std::printf("settings surfaces: %d failures (%s)\n", bad, shots.toUtf8().constData());
    return bad;
}

int runChromeAudit(bool windowsBackend)
{
    int bad = 0;
    SettingsDialog dialog;
    dialog.setAttribute(Qt::WA_DontShowOnScreen);
    dialog.openAt(2, 1);
    dialog.resize(1140, 790);
    dialog.show();
    auto* titleBar = dialog.findChild<QFrame*>(QStringLiteral("settingsTitleBar"));
    auto* close = dialog.findChild<QToolButton*>(QStringLiteral("settingsClose"));
    auto* maximize = dialog.findChild<QToolButton*>(QStringLiteral("settingsMaximize"));
    auto* combo = dialog.findChild<QComboBox*>(QStringLiteral("themePackages"));
    const auto check = [&bad](const char* label, bool pass) {
        std::printf("%s %s\n", pass ? "ok" : "FAIL", label);
        if (!pass)
            ++bad;
    };
    check("设置窗拥有 40 像素标题栏和圆角窗口形状", titleBar && titleBar->height() == 40
        && dialog.testAttribute(Qt::WA_TranslucentBackground)
        && dialog.windowFlags().testFlag(Qt::FramelessWindowHint));
    check("最小化按钮没有抢占初始焦点", dialog.focusWidget()
        && dialog.focusWidget()->objectName() != QLatin1String("settingsMinimize"));
    if (windowsBackend && titleBar && close) {
        const HWND hwnd = reinterpret_cast<HWND>(dialog.winId());
        const auto hit = [&](const QPoint& p) {
            const qreal scale = dialog.devicePixelRatioF();
            POINT screen{qRound(p.x() * scale), qRound(p.y() * scale)};
            ClientToScreen(hwnd, &screen);
            return SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(screen.x, screen.y));
        };
        check("Windows 设置窗句柄保持隐藏，标题区、按钮区与缩放区命中正确",
            !IsWindowVisible(hwnd) && hit(QPoint(230, 21)) == HTCAPTION
                && hit(close->mapTo(&dialog, close->rect().center())) == HTCLIENT
                && hit(QPoint(1, dialog.height() / 2)) == HTLEFT
                && hit(QPoint(dialog.width() / 2, dialog.height() - 2)) == HTBOTTOM);
        RECT frame{};
        POINT client{0, 0};
        check("Windows 客户区从窗口外缘开始，系统标题栏没有占据额外高度",
            GetWindowRect(hwnd, &frame) && ClientToScreen(hwnd, &client)
                && frame.left == client.x && frame.top == client.y);
    } else if (!windowsBackend && maximize) {
        maximize->click();
        check("最大化显示直角", dialog.isMaximized()
            && dialog.grab().toImage().pixelColor(0, 0).alpha() == 255);
        maximize->click();
        check("还原恢复圆角", !dialog.isMaximized() && hasSmoothFrame(dialog.grab()));
    }
    const QString shots = QDir::tempPath() + QStringLiteral("/settings-chrome");
    QDir().mkpath(shots);
    for (const auto mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::setMode(mode);
        const QString name = mode == Theme::Mode::Dark ? QStringLiteral("dark") : QStringLiteral("light");
        // 先绘制让控件挂上延迟创建的焦点框，再抓取窗口。
        dialog.grab();
        QCoreApplication::processEvents();
        const auto shot = dialog.grab();
        check("标题栏和外观页截图可保存", shot.save(shots + "/" + name + ".png"));
        check("设置窗四个圆角透明且边缘抗锯齿", hasSmoothFrame(shot));
        if (windowsBackend) {
            const auto checkModalFrame = [&](QDialog& modal, const char* label, const QString& file) {
                modal.setAttribute(Qt::WA_DontShowOnScreen);
                modal.ensurePolished();
                modal.resize(modal.sizeHint().expandedTo(QSize(240, 160)));
                // 保持 HWND 隐藏，同时检查实际窗口区域和 Qt 绘制。
                if (qobject_cast<QMessageBox*>(&modal)) {
                    // Windows 的消息框显示路径可能直接显示 HWND，此处只触发布局。
                    modal.winId();
                    QResizeEvent layoutEvent(modal.size(), modal.size());
                    QCoreApplication::sendEvent(&modal, &layoutEvent);
                } else {
                    modal.show();
                }
                const HWND hwnd = reinterpret_cast<HWND>(modal.internalWinId());
                const auto* header = modal.findChild<QFrame*>(QStringLiteral("hotaruModalHeader"));
                RECT bounds{};
                POINT client{};
                const auto modalShot = modal.grab();
                const bool headerFits = header && header->width() == modal.width() - 2;
                const bool smooth = hasSmoothFrame(modalShot);
                const bool rounded = hasNativeRoundedFrame(hwnd, modal.size() * modal.devicePixelRatioF());
                const bool aligned = GetWindowRect(hwnd, &bounds) && ClientToScreen(hwnd, &client)
                    && client.x == bounds.left && client.y == bounds.top;
                const bool valid = headerFits && !IsWindowVisible(hwnd) && smooth && rounded && aligned;
                if (!valid)
                    std::printf("  标题=%d Qt 圆角=%d Windows 圆角=%d 客户区=%d\n", headerFits, smooth, rounded, aligned);
                check(label, valid);
                check("Windows 二级窗截图可保存", modalShot.save(shots + "/" + file + "-" + name + ".png"));
            };
            QMessageBox modal(QMessageBox::Question, QStringLiteral("确认操作"),
                QStringLiteral("保留当前设置吗？"), QMessageBox::Yes | QMessageBox::No, &dialog);
            modal.setDefaultButton(QMessageBox::No);
            checkModalFrame(modal, "Windows 确认框的原生区域裁去四角", QStringLiteral("modal"));
            QInputDialog input(&dialog);
            input.setLabelText(QStringLiteral("请输入名称"));
            checkModalFrame(input, "Windows 输入框的原生区域裁去四角", QStringLiteral("modal-input"));
            QDialog custom(&dialog);
            auto* form = new QVBoxLayout(&custom);
            form->addWidget(new QLabel(QStringLiteral("弹窗内容"), &custom));
            Theme::bindModal(&custom, true);
            checkModalFrame(custom, "Windows 自绘弹窗的原生区域裁去四角", QStringLiteral("modal-custom"));
            const auto original = custom.size();
            custom.resize(original + QSize(36, 20));
            check("Windows 自绘弹窗缩放后仍保留四个圆角",
                custom.size() != original && hasNativeRoundedFrame(reinterpret_cast<HWND>(custom.internalWinId()),
                                                                    custom.size() * custom.devicePixelRatioF()));
            for (const bool launcher : {false, true}) {
                QMenu menu(&dialog);
                menu.setAttribute(Qt::WA_DontShowOnScreen);
                if (launcher)
                    Theme::bindLauncherMenu(&menu);
                else
                    Theme::bindMenu(&menu);
                // Qt 可在排完菜单项之前先创建 HWND，第一次 Show 仍可能读到旧尺寸。
                menu.resize(100, 30);
                const HWND hwnd = reinterpret_cast<HWND>(menu.winId());
                auto* action = menu.addAction(QStringLiteral("打开所在位置"));
                menu.addSeparator();
                menu.addAction(QStringLiteral("复制路径"));
                menu.addAction(QStringLiteral("以管理员身份启动"));
                menu.popup(dialog.mapToGlobal(QPoint(250, 80)));
                menu.setActiveAction(action);
                const auto row = menu.actionGeometry(action);
                const QPixmap preview = menu.grab();
                const QImage image = preview.toImage();
                const auto scale = preview.devicePixelRatio();
                const auto corner = image.pixelColor(row.topLeft() * scale);
                const auto inset = image.pixelColor((row.topLeft() + QPoint(7, 5)) * scale);
                const auto accent = launcher ? Theme::launcherColor("accent") : Theme::color("accent");
                check(launcher ? "启动器右键菜单外缘和选中项都有圆角" : "标准菜单外缘和选中项都有圆角",
                    !IsWindowVisible(hwnd) && hasNativeRoundedFrame(hwnd, menu.size() * scale) && hasSmoothFrame(preview)
                    && corner != accent && inset == accent);
                check("Windows 菜单截图可保存", preview.save(shots + (launcher ? "/launcher-menu-" : "/base-menu-") + name + ".png"));
                menu.hide();
            }
        }
        if (mode == Theme::Mode::Dark && combo && !shot.isNull()) {
            const auto img = shot.toImage();
            const qreal scale = shot.devicePixelRatio();
            bool bordersClean = true;
            for (const auto* name : {"themePackages", "settingsImportTheme", "settingsRemoveTheme",
                                     "gridRows", "gridColumns", "applyGrid", "pageStart",
                                     "settingsPinConfigActions"}) {
                const auto* control = dialog.findChild<QWidget*>(QLatin1String(name));
                if (!control) {
                    bordersClean = false;
                    continue;
                }
                const auto p = control->mapTo(&dialog, QPoint(0, 0));
                for (int dy : {-2, -1, 0, 1}) {
                    int bright = 0;
                    for (int i = 8; i < control->width() - 8; ++i)
                        bright += qGray(img.pixel(qRound((p.x() + i) * scale), qRound((p.y() + dy) * scale))) > 185;
                    bordersClean &= bright < control->width() / 5;
                }
            }
            check("夜间下拉、数值和按钮外没有 Windows 白色长线", bordersClean);
            const auto frames = dialog.findChildren<QFocusFrame*>();
            check("渲染后实际存在 Qlementine 控件的焦点框", std::any_of(
                frames.cbegin(), frames.cend(), [combo](const QFocusFrame* frame) {
                    return frame->widget() == combo;
                }));
        }
    }
    if (close)
        close->click();
    check("自绘关闭按钮关闭设置窗", close && !dialog.isVisible());
    std::printf("settings chrome: %d failures  (%s)\n", bad, shots.toUtf8().constData());
    return bad;
}

int runConfigChecks()
{
    int bad = 0;
    const auto appPalette = qApp->palette();
    const auto appFont = qApp->font();
    auto* const appStyle = qApp->style();
    ConfigPage page;
    const auto labels = page.findChildren<QLabel*>();
    const auto rowCount = [&labels](const QString& name) {
        int n = 0;
        for (const QLabel* l : labels)
            if (l->text().trimmed() == name)
                ++n;
        return n;
    };
    const auto hasRow = [&rowCount](const QString& name) { return rowCount(name) > 0; };
    const auto say = [&bad](const char* what, bool ok, const QString& got) {
        std::printf("%s %s：%s\n", ok ? "ok  " : "FAIL", what, got.toUtf8().constData());
        if (!ok)
            ++bad;
        std::fflush(stdout);
    };
    QCoreApplication::processEvents();
    auto* configStyle = page.findChild<QStyle*>(QStringLiteral("QlementineStyle"));
    auto* checkbox = page.findChild<QCheckBox*>(QStringLiteral("showResidentGrid"));
    const auto indicator = QStyle::PM_IndicatorWidth;
    say("配置开关实际走 Qlementine 指标，应用样式、调色板和字体保持隔离",
        configStyle && checkbox && checkbox->style()->pixelMetric(indicator, nullptr, checkbox)
            == configStyle->pixelMetric(indicator, nullptr, checkbox)
            && qApp->style() == appStyle && qApp->palette() == appPalette && qApp->font() == appFont,
        configStyle ? QString::fromLatin1(configStyle->metaObject()->className()) : QStringLiteral("缺少局部样式"));
    const int proxy = rowCount(QStringLiteral("下载代理："));
    say("「下载代理」只有一行", proxy == 1, QStringLiteral("数到 %1 行").arg(proxy));
    const int installRoot = rowCount(QStringLiteral("第三方软件安装根目录："));
    say("安装路径明确为第三方软件安装根目录", installRoot == 1,
        QStringLiteral("数到 %1 行").arg(installRoot));
    auto* hotkeyButton = page.findChild<QPushButton*>(QStringLiteral("hotkeyCaptureButton"));
    auto* hotkeyMode = page.findChild<QComboBox*>(QStringLiteral("hotkeyMode"));
    say("快捷键使用单一二级录入按钮",
        hotkeyButton != nullptr && hotkeyMode == nullptr,
        QStringLiteral("录入按钮=%1，旧模式框=%2")
            .arg(hotkeyButton ? 1 : 0).arg(hotkeyMode ? 1 : 0));
    const int roots = rowCount(QStringLiteral("扫描根目录："));
    say("没生效过的「扫描根目录」已拿掉", roots == 0, QStringLiteral("数到 %1 行").arg(roots));
    const bool hasList = hasRow(QStringLiteral("项目列表："));
    const bool hasDepth = hasRow(QStringLiteral("往里找几层："));
    say("应用那一栏在（列表 + 层数）", hasList && hasDepth,
        QStringLiteral("项目列表=%1 往里找几层=%2").arg(hasList ? 1 : 0).arg(hasDepth ? 1 : 0));
    int addExeBtn = 0;
    int addDirBtn = 0;
    for (QPushButton* b : page.findChildren<QPushButton*>()) {
        if (b->objectName() == QStringLiteral("addAppExe"))
            ++addExeBtn;
        if (b->objectName() == QStringLiteral("addAppDir"))
            ++addDirBtn;
    }
    say("「添加应用…」和「添加目录…」各只有一个", addExeBtn == 1 && addDirBtn == 1,
        QStringLiteral("应用=%1 目录=%2").arg(addExeBtn).arg(addDirBtn));
    const auto lists = page.findChildren<QListWidget*>();
    say("列表控件真的建出来了", !lists.isEmpty(),
        QStringLiteral("%1 个 QListWidget").arg(lists.size()));
    // 上面那两条只数得到"控件建没建"，数不出"摆没摆进布局"。应用目录那一栏就是这么
    // 在页面上隐身的：分组框 new 出来时认了 this 当父件，却从没 addWidget 到那一列，
    // 于是标签 findChild 找得到、--cfgcheck 一片绿，界面上却一行都没有（2026-10-12 他报的）。
    // 所以这条把"布局里能走到"当成结构不变量，逐个分组框验一遍，谁没挂上就点谁的名。
    QSet<const QWidget*> laidOut;
    std::function<void(QLayout*)> walk;
    // 「访问一个控件」这件事只写一遍：它自己的布局 + 两种"内容不是布局项"的容器
    //（滚动区的 viewport 件、tab 的每一页）。上一版这里只有 walk() 一条递归，
    // 配置页拆成 通用/外观 两个 tab 之后当场把 7 个分组框全报成"没摆进布局" ——
    // 因为 tab 的第一页本身就是那颗 QScrollArea，从它走只能拿到它自己的 layout()。
    std::function<void(QWidget*)> visit = [&](QWidget* w) {
        if (!w || laidOut.contains(w))
            return;
        laidOut.insert(w);
        walk(w->layout());
        if (auto* sc = qobject_cast<QScrollArea*>(w))   // 滚动区里那层不是布局项
            if (sc->widget())
                visit(sc->widget());
        if (auto* tw = qobject_cast<QTabWidget*>(w))    // tab 的非当前页也取不到布局项
            for (int i = 0; i < tw->count(); ++i)
                visit(tw->widget(i));
    };
    walk = [&](QLayout* l) {
        if (!l)
            return;
        for (int i = 0; i < l->count(); ++i) {
            QLayoutItem* it = l->itemAt(i);
            if (!it)
                continue;
            if (QWidget* w = it->widget())
                visit(w);
            else if (QLayout* sub = it->layout())
                walk(sub);
        }
    };
    visit(&page);
    QString orphans;
    int groups = 0;
    bool hasIme = false;
    for (const QGroupBox* g : page.findChildren<QGroupBox*>()) {
        ++groups;
        hasIme |= g->title().startsWith(QStringLiteral("微软拼音"));
        if (!laidOut.contains(g))
            orphans += QStringLiteral("「%1」").arg(g->title());
    }
    say("每个分组框都真的摆在布局里（没摆=界面上看不见）", orphans.isEmpty(),
        orphans.isEmpty() ? QStringLiteral("%1 个分组框全在布局里").arg(groups) : orphans);
    say("微软拼音分组已移除", !hasIme, QString());
    // ---- 2026-10-20 他把配置页拆成「通用 / 外观」两个 tab，日间夜间那颗搬进外观 ----
    // 这三条盯的是搬家的形状，不是"有没有 tab"：
    // ① 就两个、名字对得上（多一个少一个都算没照做）；
    // ② 那两颗单选**在外观这一页上**（搬一半 = 界面上找不到，比不搬更糟）；
    // ③ 通用那一页不许长出外观的东西、外观页只放外观和固定软件设置 ——
    //    这条是防"以后加开关的人随手往外观页里塞下载代理"那种回潮。
    {
        auto* tabs = page.findChild<QTabWidget*>(QStringLiteral("cfgTabs"));
        const QStringList tabNames = [&] {
            QStringList n;
            if (tabs)
                for (int i = 0; i < tabs->count(); ++i)
                    n << tabs->tabText(i);
            return n;
        }();
        say("配置页正好两个 tab，顺序是 通用/外观",
            tabNames.join(QStringLiteral(" / ")) == QStringLiteral("通用 / 外观"),
            tabNames.isEmpty() ? QStringLiteral("没找到 cfgTabs 那颗 QTabWidget")
                               : tabNames.join(QStringLiteral(" / ")));
        QWidget* generalPage = tabs && tabs->count() > 0 ? tabs->widget(0) : nullptr;
        QWidget* lookPage = tabs && tabs->count() > 1 ? tabs->widget(1) : nullptr;
        const auto radios = page.findChildren<QRadioButton*>();
        int onLook = 0, elsewhere = 0;
        for (const QRadioButton* r : radios) {
            if (lookPage && lookPage->isAncestorOf(const_cast<QRadioButton*>(r)))
                ++onLook;
            else
                ++elsewhere;
        }
        say("夜间/日间那两颗单选都在「外观」这一页上（不在通用那一页）",
            radios.size() == 2 && onLook == 2 && elsewhere == 0,
            QStringLiteral("找到 %1 颗 · 在外观页 %2 颗 · 别处 %3 颗")
                .arg(radios.size()).arg(onLook).arg(elsewhere));
        int generalGroups = 0, lookGroups = 0;
        QStringList lookTitles;
        for (QGroupBox* g : page.findChildren<QGroupBox*>()) {
            if (generalPage && generalPage->isAncestorOf(g))
                ++generalGroups;
            if (lookPage && lookPage->isAncestorOf(g)) {
                ++lookGroups;
                lookTitles << g->title();
            }
        }
        const auto* pinnedSection = lookPage ? lookPage->findChild<QFrame*>(QStringLiteral("pinnedSection")) : nullptr;
        say("外观页独立排列「界面」「启动器布局」「固定软件」",
            generalGroups == groups - 2 && lookGroups == 2 && pinnedSection && laidOut.contains(pinnedSection)
                && lookTitles == QStringList({QStringLiteral("界面"), QStringLiteral("启动器布局")}),
            QStringLiteral("通用页 %1 颗 · 外观页 %2 颗（%3）· 整页共 %4 颗")
                .arg(generalGroups).arg(lookGroups)
                .arg(lookTitles.join(QStringLiteral(" / "))).arg(groups));
        auto* packages = page.findChild<QComboBox*>(QStringLiteral("themePackages"));
        auto* pinned = page.findChild<QListWidget*>(QStringLiteral("pinnedApps"));
        say("外观页可以选择内置主题并管理固定软件列表",
            lookPage && packages && pinned && lookPage->isAncestorOf(packages)
                && lookPage->isAncestorOf(pinned)
                && packages->findData(QStringLiteral("hotaru-black")) >= 0,
            QStringLiteral("主题选择器=%1 · 固定软件列表=%2")
                .arg(packages ? packages->count() : 0).arg(pinned ? 1 : 0));
    }

    // 「语言与环境」已剥离为独立插件，配置页不再塞入该分组框
    {
        QStringList titles;
        for (const QGroupBox* g : page.findChildren<QGroupBox*>())
            if (g->title().contains(QStringLiteral("语言")))
                titles << g->title();
        say("配置页不再有「语言」分组框（已剥离至插件）", titles.isEmpty(),
            titles.isEmpty() ? QStringLiteral("已剥离至插件") : titles.join(QStringLiteral("；")));
    }

    // 设置窗那一面：菜单只剩四行，而且每一行点开都真建出一张页。
    // 这条盯的是"删掉四行菜单之后有没有留下点不开的空行"——残留入口就是没删干净。
    {
        SettingsDialog dlg;
        dlg.setAttribute(Qt::WA_DontShowOnScreen, true);
        auto* menu = dlg.findChild<QListWidget*>(QStringLiteral("menuList"));
        QStringList names;
        if (menu) {
            for (int i = 0; i < menu->count(); ++i)
                names << menu->item(i)->text();
        }
        say("菜单正好四行、顺序是 快捷搜索/插件/配置/关于",
            names.join(QStringLiteral(" / ")) == QStringLiteral("快捷搜索 / 插件 / 配置 / 关于"),
            names.join(QStringLiteral(" / ")));
        auto* stack = dlg.findChild<QStackedWidget*>();
        int built = 0;
        for (int i = 0; menu && i < menu->count(); ++i) {
            menu->setCurrentRow(i);            // currentRowChanged → ensurePage，和真人点同一条路
            QCoreApplication::processEvents();
            QWidget* cur = stack ? stack->currentWidget() : nullptr;
            if (cur && cur->findChildren<QWidget*>().size() > 1)
                ++built;
        }
        say("每一行点开都不是一块空面板", built == (menu ? menu->count() : 0),
            QStringLiteral("有内容的 %1 / 共 %2 行").arg(built).arg(menu ? menu->count() : 0));

        // 图标这一条确认四行都不再使用首字图标。底块颜色从图标自己身上取，
        // 这样和 Utils::letterIcon(名字, 同一块色) 比才是公道的比法 —— 不然换个色就永远不等。
        const auto imgOf = [](const QIcon& i) { return i.pixmap(48, 48).toImage(); };
        const auto tileColor = [](const QImage& im) {
            QMap<QRgb, int> hist;
            for (int y = 0; y < im.height(); ++y)
                for (int x = 0; x < im.width(); ++x)
                    if (qAlpha(im.pixel(x, y)) > 200)
                        ++hist[QRgb(im.pixel(x, y))];
            QRgb best = 0;
            int n = -1;
            for (auto it = hist.constBegin(); it != hist.constEnd(); ++it)
                if (it.value() > n) { n = it.value(); best = it.key(); }
            return QColor(best);
        };
        QStringList vectorIcons, lettered;
        for (int i = 0; menu && i < menu->count(); ++i) {
            const QImage im = imgOf(menu->item(i)->icon());
            const QImage asLetter = imgOf(Utils::letterIcon(menu->item(i)->text(), tileColor(im)));
            (im == asLetter ? lettered : vectorIcons) << menu->item(i)->text();
        }
        say("设置侧栏四行都使用独立矢量图标",
            vectorIcons.size() == (menu ? menu->count() : 0) && lettered.isEmpty(),
            QStringLiteral("矢量=%1 文字=%2").arg(vectorIcons.join(QStringLiteral("、")),
                                                   lettered.join(QStringLiteral("、"))));
        // 改外观的东西要能看见，不能只剩一条断言：四颗图标拼成一条，铺在菜单那块深色底上存成 PNG。
        // 数断言能证明"它不是第一个字"，证明不了"它看得出来是个袋子"。
        {
            const QString shots = QDir::tempPath() + QStringLiteral("/ltool-cfgshots");
            QDir().mkpath(shots);
            const int n = menu ? menu->count() : 0;
            QPixmap strip(48 * n, 64);
            strip.fill(QColor(30, 32, 38));
            QPainter sp(&strip);
            for (int i = 0; i < n; ++i)
                sp.drawPixmap(48 * i + 8, 8, menu->item(i)->icon().pixmap(40, 40));
            sp.end();
            const QString file = shots + QStringLiteral("/menu-icons.png");
            say("四颗菜单图标存成了一张 PNG（肉眼复核用）", strip.save(file, "PNG"),
                QStringLiteral("%1 · %2x%3").arg(file).arg(strip.width()).arg(strip.height()));
        }
    }
    // 悬停说明（tooltip）的深色：他报的是"夜间模式鼠标悬停说明，灰字压白底完全看不见字"。
    // 量不了像素 —— 离屏看不到 windows11 风格那层**系统浅底**（2026-10-17 那次菜单判据就栽在
    // 这儿，见 captureselftest 里 makeStyledMenu 那段注释），所以量的是"真挂上去的那条规则里
    // 到底写了什么"：挂在 qApp 上（tooltip 是独立顶层窗口，挂在某个窗口上管不到它），
    // 并且底色和字色两头都写死、亮差够大。撤掉 main() 里那句 setStyleSheet，这两条立刻红。
    {
        const QString css = qApp->styleSheet();
        say("悬停说明的样式挂在 qApp 上（不是某个窗口）",
            css.contains(QLatin1String("QToolTip")), QStringLiteral("qApp 样式表长度=%1").arg(css.size()));
        const int from = css.indexOf(QLatin1String("QToolTip"));
        const QString block = from < 0 ? QString()
                                       : css.mid(from, css.indexOf(QLatin1Char('}'), from) - from);
        const auto hexOf = [block](const QString& prop) {
            const QRegularExpression re(QStringLiteral("%1:\\s*#([0-9a-fA-F]{6})").arg(prop));
            const auto m = re.match(block);
            return m.hasMatch() ? QColor(QStringLiteral("#") + m.captured(1)) : QColor();
        };
        const QColor bg = hexOf(QStringLiteral("background"));
        const QColor fg = hexOf(QStringLiteral("color"));
        say("那条规则里字比底色亮出一大截（看得清）",
            bg.isValid() && fg.isValid() && fg.lightness() - bg.lightness() > 120,
            QStringLiteral("底=%1 字=%2 亮差=%3")
                .arg(bg.name(), fg.name())
                .arg(bg.isValid() && fg.isValid() ? fg.lightness() - bg.lightness() : -999));
    }
    // ---- 夜间 / 日间（2026-10-20 他点名要配置页那颗切换）----
    // 这套判据钉的是"切换真的换了颜色、而且可逆、而且落盘"，不是"有个控件在那"。
    {
        const int themeRows = rowCount(QStringLiteral("界面配色"));
        const auto radios = page.findChildren<QRadioButton*>();
        QStringList radioNames;
        int checked = 0;
        for (const QRadioButton* r : radios) {
            radioNames << r->text();
            if (r->isChecked())
                ++checked;
        }
        say("「界面配色」只有一行，提供两张预览卡",
            themeRows == 1 && radioNames.size() == 2,
            QStringLiteral("行数=%1 单选=%2").arg(themeRows).arg(radioNames.join(QStringLiteral("/"))));
        say("默认是夜间（这一版之前的行为，不该被切换功能改掉）",
            Theme::current() == Theme::Mode::Dark && checked == 1,
            QStringLiteral("current=%1 选中数=%2").arg(Theme::name(Theme::current())).arg(checked));

        const QColor darkWindow = Theme::color("window");
        const QString darkTip = qApp->styleSheet();
        // 拨到日间
        Theme::setMode(Theme::Mode::Light);
        const QColor lightWindow = Theme::color("window");
        const QColor palWindow = qApp->palette().color(QPalette::Window);
        say("切到日间：current 跟着变，调色板也真的换了",
            Theme::current() == Theme::Mode::Light && palWindow == lightWindow
                && lightWindow != darkWindow,
            QStringLiteral("夜底=%1 日底=%2 调色板=%3").arg(darkWindow.name(), lightWindow.name(),
                                                           palWindow.name()));
        {
            const QColor text = Theme::color("text");
            const int delta = qAbs(text.lightness() - lightWindow.lightness());
            say("日间那份正文压在窗口底上够亮差（和夜间同一条口径 >120）",
                delta > 120, QStringLiteral("底=%1 字=%2 亮差=%3")
                                  .arg(lightWindow.name(), text.name()).arg(delta));
        }
        say("切到日间：挂在 qApp 上的悬停说明跟着换底色（那一份是独立顶层窗口，只能靠它）",
            !qApp->styleSheet().isEmpty() && qApp->styleSheet() != darkTip
                && qApp->styleSheet().contains(Theme::color("field").name().mid(1),
                                               Qt::CaseInsensitive),
            QStringLiteral("新样式表长度=%1").arg(qApp->styleSheet().size()));
        {
            QSettings cfg(Utils::dataDir() + "/config.ini", QSettings::IniFormat);
            const QString stored = cfg.value("ui/theme").toString();
            say("切换是落盘的（重开程序还认），不是只改内存",
                stored.compare(QLatin1String("light"), Qt::CaseInsensitive) == 0,
                QStringLiteral("config.ini ui/theme=%1").arg(stored));
        }
        // 反向：拨回夜间，一切得回到原值 —— 不然就是单向棘轮
        Theme::setMode(Theme::Mode::Dark);
        say("拨回夜间：底色/样式表一字不差回到原来那一份（不是单向的）",
            Theme::color("window") == darkWindow && qApp->styleSheet() == darkTip,
            QStringLiteral("底=%1 样式表回到原样=%2")
                .arg(Theme::color("window").name())
                .arg(qApp->styleSheet() == darkTip ? 1 : 0));
        // expand() 那一步：认得的名字换成值，认不出的原样留着（写错 token 时看得见）
        {
            const QString probe = Theme::expand("@window and @nope");
            say("样式表里的 @token 换成值；写错的名字原样留着，不许静默变空",
                probe == QStringLiteral("%1 and @nope").arg(Theme::color("window").name()),
                QStringLiteral("得到 %1").arg(probe));
        }
        // 前缀陷阱：这张表里有 5 对"一个是另一个的前缀"的角色
        //（card/cardLine、accent/accentUp/accentDown、group/groupText、
        //  disabled/disabledLine、border/borderHi/borderHiUp）。
        // 按表顺序挨个 replace 的话，`@cardLine` 会先被 `@card` 吃掉，剩下一个
        // `<card 的值>Line` 混在 QSS 里 —— 界面上是"边框颜色不对"，日志里一个字都没有。
        {
            const QString probe = Theme::expand(
                QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8")
                    .arg(QStringLiteral("@card"), QStringLiteral("@cardLine"),
                         QStringLiteral("@accent"), QStringLiteral("@accentUp"),
                         QStringLiteral("@group"), QStringLiteral("@groupText"),
                         QStringLiteral("@border"), QStringLiteral("@borderHi")));
            const QString want = QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8")
                .arg(Theme::token("card"), Theme::token("cardLine"),
                     Theme::token("accent"), Theme::token("accentUp"),
                     Theme::token("group"), Theme::token("groupText"),
                     Theme::token("border"), Theme::token("borderHi"));
            say("长名字的角色不被它的短前缀抢走（@cardLine 不等于 @card 拼上 \"Line\"）",
                probe == want && !probe.contains(QLatin1String("LineLine"))
                    && !probe.contains(QLatin1String("UpUp")),
                QStringLiteral("得到 %1").arg(probe));
        }
        // Theme::bind 的那份样式表要能在切换时自己重挂，而且**子控件**也得收到通知 ——
        // 样式表挂在具体控件身上，只给顶层窗口发事件的话，配置页里的控件全是老颜色。
        {
            auto* win = new QWidget(nullptr);
            win->setObjectName(QStringLiteral("themeBindProbe"));
            auto* inner = new QLabel(QStringLiteral("探针"), win);
            auto* lay = new QVBoxLayout(win);
            lay->addWidget(inner);
            Theme::bind(inner, "color: @text; background: @window;");
            const QString nightQss = inner->styleSheet();
            Theme::setMode(Theme::Mode::Light);
            const QString dayQss = inner->styleSheet();
            const bool followsMode = nightQss != dayQss
                && dayQss.contains(Theme::color("text").name(), Qt::CaseInsensitive)
                && dayQss.contains(Theme::color("window").name(), Qt::CaseInsensitive)
                && nightQss.contains(QStringLiteral("#1b1c20"), Qt::CaseInsensitive);
            Theme::setMode(Theme::Mode::Dark);
            const bool reversible = inner->styleSheet() == nightQss;
            delete win;
            say("挂在子控件上的那份样式表（Theme::bind）会跟着切换重挂，不是只有顶层窗口变脸",
                followsMode && reversible,
                QStringLiteral("夜间=%1 日间=%2 拨回来=%3")
                    .arg(nightQss, dayQss).arg(reversible ? QStringLiteral("原样")
                                                          : QStringLiteral("没回来")));
        }
        // 最后这一条不看样式表文本，看**渲出来的像素**：两种模式各截一张配置页存成 PNG，
        // 再量整张的平均亮度。为什么要多这一条 —— 前面那些判据量的是"字符串里写的是哪个色号"，
        // 而样式表写得对、画出来还是黑的是可能的（那份 QSS 没挂到这个控件上、或者控件用的是
        // 调色板不是样式表）。2026-10-20 加切换时就是把颜色从 380 处字面量搬进角色名，
        // 这类"搬漏一处"只有像素能抓到。
        {
            const QString shots = QDir::tempPath() + QStringLiteral("/ltool-cfgshots");
            QDir().mkpath(shots);
            const auto savedLauncher = PinnedApps::config();
            auto previewLauncher = savedLauncher;
            for (int column = 0; column < 2; ++column) {
                PinnedApp sample;
                sample.id = QStringLiteral("settings-preview-%1").arg(column);
                sample.title = column == 0 ? QStringLiteral("Hotaru 开发版") : QStringLiteral("测试目录");
                sample.targetPath = column == 0 ? QCoreApplication::applicationFilePath() : Utils::dataDir();
                sample.enabled = column == 0;
                sample.gridRow = 0;
                sample.gridColumn = column;
                previewLauncher.pinnedApps.append(sample);
            }
            say("设置截图固定项夹具可写入", PinnedApps::saveConfig(previewLauncher), QString());
            // 渲的是**整扇设置窗**（左边菜单 + 右边那张页），不是光一张 ConfigPage ——
            // 2026-10-20 他回"太丑了太难受了"看的就是这个整体，只渲单页会漏掉
            // 菜单那一列和页眉跟内容之间的配色关系。
            SettingsDialog dlg;
            dlg.setAttribute(Qt::WA_DontShowOnScreen, true);
            dlg.resize(1140, 790);
            if (auto* menu = dlg.findChild<QListWidget*>(QStringLiteral("menuList")))
                menu->setCurrentRow(2);   // 「配置」那一行，走的是真人点的那条路
            QCoreApplication::processEvents();
            const auto meanLightness = [&dlg](const QString& file) {
                const QPixmap pm = dlg.grab();
                const QImage img = pm.toImage().convertToFormat(QImage::Format_RGB32);
                pm.save(file, "PNG");
                long long sum = 0, n = 0;
                for (int y = 0; y < img.height(); y += 4) {
                    const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
                    for (int x = 0; x < img.width(); x += 4, ++line) {
                        sum += qGray(*line);
                        ++n;
                    }
                }
                return n > 0 ? int(sum / n) : -1;
            };
            Theme::setMode(Theme::Mode::Dark);
            const int night = meanLightness(shots + QStringLiteral("/theme-night.png"));
            Theme::setMode(Theme::Mode::Light);
            const int day = meanLightness(shots + QStringLiteral("/theme-day.png"));
            // 再补一张「外观」那一页单独的图：拆 tab 之后只截第一页就等于没截到新东西，
            // 而"这一页是不是空得只剩一行"必须用眼睛过一遍，不能只靠上面那条数分组框。
            if (auto* tw = dlg.findChild<QTabWidget*>(QStringLiteral("cfgTabs"))) {
                tw->setCurrentIndex(1);
                QCoreApplication::processEvents();
                dlg.grab().save(shots + QStringLiteral("/theme-look-tab.png"), "PNG");
                Theme::setMode(Theme::Mode::Dark);
                dlg.grab().save(shots + QStringLiteral("/theme-look-tab-dark.png"), "PNG");
                tw->setCurrentIndex(0);
            }
            Theme::setMode(Theme::Mode::Dark);
            PinnedApps::saveConfig(savedLauncher);
            say("两种模式各渲一整扇设置窗，日间那张整张平均亮度明显更亮（量的是像素不是字符串）",
                night >= 0 && day - night > 60,
                QStringLiteral("夜间平均灰度=%1 日间=%2 差=%3 · PNG 在 %4")
                    .arg(night).arg(day).arg(day - night).arg(shots));
        }
        // 启动器那扇是 WA_TranslucentBackground + 无边框 —— 也就是说"搜索框在哪儿"
        // 这件事**只由那一圈 1px 描边**说给他看。2026-10-20 他第一句反馈就是
        // "这个白色不好，完全看不清搜索框的边框"：日间那份底是纯白、描边是黑压 26/255，
        // 两者差 1.15:1，等于没有。所以这条量的就是渲染出来的**描边那一行 vs 紧贴里面的底**，
        // 按 WCAG 1.4.11（非文本对比度 ≥3:1）判，两个模式各判一次。
        // 这条不读 Theme 里的任何常量，读的是 grab() 出来的像素 —— 换个画法它就红。
        {
            auto* launcher = new SearchWindow;
            const QString shots = QDir::tempPath() + QStringLiteral("/ltool-cfgshots");
            launcher->resize(560, 300);
            // 亮度/对比度的算法在 Theme 里只有一份（原来这里抄过一份局部 lambda，
            // 剪贴板那张卡也要问同样的问题，就来第二处 —— 搬进 Theme 收掉）。
            const auto edgeContrast = [launcher](const QString& file, QString* why) {
                const QPixmap pm = launcher->grab();
                const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
                pm.save(file, "PNG");
                // 沿**左边缘那一列**、竖着中间那一行取点：路径是 QRectF(0.5, 0.5, …) 配
                // 1px 画笔 → 描边正好盖满第 0 列（上下那两行不行，那里有 13px 圆角，
                // 第一版我从顶行取，日间那颗底一变成不透明就"描边和底同一行"了）。
                // Format_ARGB32 非预乘，所以第 0 列的 r/g/b 就是描边本身的颜色；
                // 往里第 8 列过了布局那 14px 左边距的空白区，是纯窗口底。
                const int cy = img.height() / 2;
                if (qAlpha(img.pixel(0, cy)) == 0 || qAlpha(img.pixel(8, cy)) == 0) {
                    *why = QStringLiteral("第 0 列或第 8 列在这一行是空的（窗口没画满？）");
                    return -1.0;
                }
                const QRgb e = img.pixel(0, cy);
                const QRgb f = img.pixel(8, cy);
                *why = QStringLiteral("描边=%1 底=%2").arg(QColor(e).name(), QColor(f).name());
                return Theme::contrast(e, f);
            };
            Theme::setMode(Theme::Mode::Dark);
            QString whyN;
            const double cn = edgeContrast(shots + QStringLiteral("/launcher-night.png"), &whyN);
            Theme::setMode(Theme::Mode::Light);
            QString whyD;
            const double cd = edgeContrast(shots + QStringLiteral("/launcher-day.png"), &whyD);
            Theme::setMode(Theme::Mode::Dark);
            delete launcher;
            // 日间按 WCAG 1.4.11（非文本对比度）要 ≥3:1；夜间那颗 1.86:1 是这一版一直以来
            // 的样子，他这次点的是"这个白色不好"、没说要改夜里 —— 所以夜里这条只钉
            // "别再变糊"，不拿 3:1 去动他用了很久的默认外观。
            say("启动器那圈描边和它自己的底差得开（无边框窗，看得见边=看得见框）："
                "日间 ≥3:1、夜间只钉住不比现在这份更糊",
                cd >= 3.0 && cn >= 1.8,
                QStringLiteral("夜间 %1:1（%2）· 日间 %3:1（%4）")
                    .arg(cn, 0, 'f', 2).arg(whyN).arg(cd, 0, 'f', 2).arg(whyD));
        }
    }
    {
        const auto available = [](const QString& id) {
            const auto* meta = PluginHost::meta(id);
            return meta && meta->ok() && !PluginHost::isDisabled(id);
        };
        const bool captureProvider = available(QStringLiteral("capture"));
        const bool recordingProvider = available(QStringLiteral("record"));
        const QString ini = Utils::dataDir() + "/config.ini";
        QTemporaryFile invalidDir(Utils::dataDir() + "/invalid-media-XXXXXX");
        const bool hasFile = invalidDir.open();
        {
            // 给未启用的功能留下一个已保存、但不能作为目录使用的值。
            QSettings cfg(ini, QSettings::IniFormat);
            if (!captureProvider)
                cfg.setValue(QStringLiteral("paths/captures"), invalidDir.fileName());
            if (!recordingProvider)
                cfg.setValue(QStringLiteral("paths/recordings"), invalidDir.fileName());
            cfg.sync();
        }
        SettingsDialog dialog;
        dialog.setAttribute(Qt::WA_DontShowOnScreen,
                            QGuiApplication::platformName() != QLatin1String("offscreen"));
        dialog.resize(1140, 790);
        if (auto* menu = dialog.findChild<QListWidget*>("menuList"))
            menu->setCurrentRow(2);
        auto* tabs = dialog.findChild<QTabWidget*>("cfgTabs");
        auto* rows = dialog.findChild<QSpinBox*>("gridRows");
        auto* columns = dialog.findChild<QSpinBox*>("gridColumns");
        auto* applyGrid = dialog.findChild<QPushButton*>("applyGrid");
        auto* depth = dialog.findChild<QSpinBox*>("settingsAppDepth");
        auto* applyIndex = dialog.findChild<QPushButton*>("applyIndex");
        auto* proxyEdit = dialog.findChild<QLineEdit*>("settingsDownloadProxy");
        auto* captures = dialog.findChild<QLineEdit*>("settingsCapturePath");
        auto* recordings = dialog.findChild<QLineEdit*>("settingsRecordingPath");
        auto* applyPaths = dialog.findChild<QPushButton*>("applyPaths");
        auto* openData = dialog.findChild<QPushButton*>("settingsOpenData");
        auto* clearIndex = dialog.findChild<QPushButton*>("settingsClearIndex");
        auto* favorites = dialog.findChild<QCheckBox*>("showResidentGrid");
        auto* discard = dialog.findChild<QPushButton*>("discardSettings");
        auto* startPage = dialog.findChild<QComboBox*>("pageStart");
        const bool ready = tabs && rows && columns && applyGrid && depth && applyIndex
            && proxyEdit && captures && recordings && applyPaths && openData && clearIndex
            && favorites && discard && startPage;
        say("自动保存与分组应用入口齐全", ready, QString());
        if (ready) {
            dialog.show();
            dialog.activateWindow();
            QCoreApplication::processEvents();
            tabs->setCurrentIndex(1);
            const auto original = PinnedApps::config();
            rows->setValue(original.gridRows == 6 ? 5 : original.gridRows + 1);
            startPage->setCurrentIndex(original.rememberPage ? 0 : 1);
            QMetaObject::invokeMethod(startPage, "activated", Q_ARG(int, startPage->currentIndex()));
            tabs->setCurrentIndex(0);
            const auto read = [&ini](const QString& key, const QVariant& fallback = {}) {
                return QSettings(ini, QSettings::IniFormat).value(key, fallback);
            };
            const bool newFavorites = !favorites->isChecked();
            favorites->click();
            say("切换开关和页面策略即时保存，网格仍为草稿",
                read("ui/favoritesOnEmpty", !newFavorites).toBool() == newFavorites
                    && PinnedApps::config().rememberPage != original.rememberPage
                    && PinnedApps::config().gridRows == original.gridRows && applyGrid->isEnabled(), QString());

            int rescans = 0;
            QObject::connect(&dialog, &SettingsDialog::rescanRequested, &dialog, [&rescans] { ++rescans; });
            const int oldDepth = read("apps/depth", 3).toInt();
            depth->setValue(oldDepth == 6 ? 5 : oldDepth + 1);
            tabs->setCurrentIndex(1);
            applyGrid->click();
            say("应用网格保留索引草稿，且没有重扫",
                PinnedApps::config().gridRows == rows->value() && !applyGrid->isEnabled()
                    && read("apps/depth", 3).toInt() == oldDepth && rescans == 0, QString());
            tabs->setCurrentIndex(0);
            applyIndex->click();
            applyIndex->click();
            say("应用索引只重扫一次", read("apps/depth").toInt() == depth->value() && rescans == 1, QString());

            auto fixture = PinnedApps::config();
            PinnedApp hidden;
            hidden.id = QStringLiteral("hidden-outer-column");
            hidden.targetPath = QCoreApplication::applicationFilePath();
            hidden.title = QStringLiteral("隐藏夹具");
            hidden.enabled = false;
            hidden.gridRow = 0;
            hidden.gridColumn = fixture.gridColumns - 1;
            fixture.pinnedApps.append(hidden);
            const bool prepared = PinnedApps::saveConfig(fixture);
            tabs->setCurrentIndex(1);
            columns->setValue(fixture.gridColumns - 1);
            applyGrid->click();
            say("缩列冲突保留隐藏项与原列数", prepared && columns->value() == fixture.gridColumns
                && PinnedApps::config().gridColumns == fixture.gridColumns
                && PinnedApps::config().pinnedApps.size() == fixture.pinnedApps.size(), QString());
            tabs->setCurrentIndex(0);

            const bool hasMedia = captureProvider || recordingProvider;
            say("截图和录像目录分别跟随插件启用，数据操作始终可用",
                captures->isVisible() == captureProvider && recordings->isVisible() == recordingProvider
                && applyPaths->isVisible() == hasMedia && openData->isVisible() && clearIndex->isVisible(),
                QStringLiteral("截图=%1 录像=%2").arg(captureProvider).arg(recordingProvider));
            const QString captureKey = QStringLiteral("paths/captures");
            const QString recordingKey = QStringLiteral("paths/recordings");
            const auto originalCapture = read(captureKey);
            const auto originalRecording = read(recordingKey);
            auto* availablePath = captureProvider ? captures : recordings;
            if (hasMedia) {
                const auto originalText = availablePath->text();
                availablePath->setText(invalidDir.fileName());
                applyPaths->click();
                say("不可作为目录的路径恢复原值", hasFile && !applyPaths->isEnabled()
                    && availablePath->text() == originalText && read(captureKey) == originalCapture
                    && read(recordingKey) == originalRecording, QString());
            }
            const QString nextCapture = Utils::dataDir() + "/new-captures";
            const QString nextRecording = Utils::dataDir() + "/new-recordings";
            captures->setText(captureProvider ? nextCapture : invalidDir.fileName() + "/draft");
            recordings->setText(recordingProvider ? nextRecording : invalidDir.fileName() + "/draft");
            applyPaths->click();
            say("应用媒体目录只写入可见项，隐藏路径保持原值",
                hasFile && !applyPaths->isEnabled()
                && read(captureKey) == (captureProvider ? QVariant(nextCapture) : originalCapture)
                && read(recordingKey) == (recordingProvider ? QVariant(nextRecording) : originalRecording), QString());

            const auto editProxy = [&](const QString& value) {
                proxyEdit->setFocus();
                proxyEdit->selectAll();
                QKeyEvent text(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, value);
                QApplication::sendEvent(proxyEdit, &text);
            };
            editProxy(QStringLiteral("http://localhost:8888"));
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(proxyEdit, &enter);
            say("按 Enter 后文本立即写入", read("net/proxy").toString() == "http://localhost:8888", QString());
            editProxy(QStringLiteral("http://localhost:8889"));
            if (proxyEdit->hasFocus()) {
                favorites->setFocus();
            } else {
                QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
                QApplication::sendEvent(proxyEdit, &focusOut);
            }
            QCoreApplication::processEvents();
            const QString savedProxy = read("net/proxy").toString();
            say("正常失焦提交文本", savedProxy == "http://localhost:8889",
                QStringLiteral("已存=%1，窗口活动=%2，输入框焦点=%3")
                    .arg(savedProxy).arg(dialog.isActiveWindow()).arg(proxyEdit->hasFocus()));

            if (hasMedia)
                availablePath->setText(QDir(Utils::dataDir()).filePath("media-draft"));
            editProxy(QStringLiteral("http://localhost:8890"));
            const QPointF center(discard->rect().center());
            const QPointF global(discard->mapToGlobal(center.toPoint()));
            QMouseEvent press(QEvent::MouseButtonPress, center, global, Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, center, global, Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(discard, &press);
            QApplication::sendEvent(discard, &release);
            QCoreApplication::processEvents();
            say("鼠标放弃恢复未应用草稿，保留自动保存结果",
                read("net/proxy").toString() == savedProxy && proxyEdit->text() == savedProxy
                    && !applyPaths->isEnabled() && favorites->isChecked() == newFavorites, QString());
            const QString nativeIni = QDir::toNativeSeparators(ini);
            const auto* iniPath = reinterpret_cast<LPCWSTR>(nativeIni.utf16());
            const DWORD attrs = GetFileAttributesW(iniPath);
            const bool readOnly = attrs != INVALID_FILE_ATTRIBUTES
                && SetFileAttributesW(iniPath, attrs | FILE_ATTRIBUTE_READONLY);
            if (readOnly) {
                favorites->click();
                SetFileAttributesW(iniPath, attrs);
            }
            say("INI 写入失败时恢复开关及已保存值", readOnly
                && favorites->isChecked() == newFavorites
                && read("ui/favoritesOnEmpty", !newFavorites).toBool() == newFavorites, QString());
            editProxy(QStringLiteral("http://localhost:8891"));
            dialog.close();
            QCoreApplication::processEvents();
            say("关闭设置窗不会因失焦提交文本", read("net/proxy").toString() == savedProxy, QString());
            ConfigPage reopened;
            const auto* reopenedProxy = reopened.findChild<QLineEdit*>("settingsDownloadProxy");
            const auto* reopenedFavorites = reopened.findChild<QCheckBox*>("showResidentGrid");
            say("重建设置页回读已保存值", reopenedProxy && reopenedFavorites
                && reopenedProxy->text() == savedProxy && reopenedFavorites->isChecked() == newFavorites, QString());
            const auto* pinActions = reopened.findChild<QPushButton*>(QStringLiteral("pinActions"));
            QAction* enablePin = nullptr;
            if (pinActions && pinActions->menu()) {
                for (auto* action : pinActions->menu()->actions())
                    if (action->text() == QStringLiteral("启用"))
                        enablePin = action;
            }
            if (enablePin)
                enablePin->trigger();
            const auto pinnedAfter = PinnedApps::load();
            say("固定项行内操作作用于该条目并保存", enablePin && pinnedAfter.size() == 1
                && pinnedAfter.first().id == hidden.id && pinnedAfter.first().enabled, QString());
        }
    }
    {
        const QString previous = Theme::currentPackageId();
        Theme::setMode(Theme::Mode::Dark);
        QMenu launcher;
        QMenu standard;
        Theme::bindLauncherMenu(&launcher);
        Theme::bindMenu(&standard);
        for (auto* menu : {&launcher, &standard}) {
            menu->addAction(QStringLiteral("打开"));
            menu->addSeparator();
            menu->addAction(QStringLiteral("重命名"));
            menu->addAction(QStringLiteral("删除当前页"))->setEnabled(false);
        }
        const QString standardQss = standard.styleSheet();
        const bool white = Theme::activatePackage(QStringLiteral("hotaru-white"));
        say("夜间基础主题搭配白色皮肤时，启动器右键使用浅色且标准菜单仍为夜间",
            white && launcher.styleSheet().contains(Theme::launcherToken("field"))
                && Theme::launcherColor("field").lightness() > 180
                && launcher.styleSheet() != standardQss && standard.styleSheet() == standardQss,
            QStringLiteral("启动器=%1 标准=%2").arg(Theme::launcherToken("field"), Theme::token("field")));
        launcher.adjustSize();
        launcher.setActiveAction(launcher.actions().first());
        launcher.grab().save(QDir::tempPath() + QStringLiteral("/ltool-cfgshots/white-launcher-menu.png"), "PNG");
        Theme::activatePackage(previous);
    }
    std::printf("cfgcheck: 失败 %d 项\n", bad);
    std::fflush(stdout);
    return bad;
}

} // namespace

int runHotaruSettingsSelftest(const QStringList& args)
{
    const int popup = args.indexOf(QStringLiteral("--launcher-popup-audit"));
    if (popup >= 0)
        return runLauncherPopupAudit(args.value(popup + 1));
    if (args.contains(QStringLiteral("--settings-surface-audit")))
        return runSurfaceAudit(args);
    if (args.contains(QStringLiteral("--settings-chrome-only")))
        return runChromeAudit(QGuiApplication::platformName() == QLatin1String("windows"));
    return runConfigChecks();
}

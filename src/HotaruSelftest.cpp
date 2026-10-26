#include "AboutPage.h"
#include "Archives.h"
#include "LauncherGrid.h"
#include "LauncherSkin.h"
#include "PinnedApps.h"
#include "PluginStore.h"
#include "SearchWindow.h"
#include "SettingsDialog.h"
#include "LauncherTheme.h"
#include "Utils.h"
#include "platform/FileMeta.h"
#include <QApplication>
#include <QScreen>
#include <QInputDialog>
#include <QAbstractButton>
#include <QCryptographicHash>
#include <QBuffer>
#include <QComboBox>
#include <QCheckBox>
#include <QDir>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QMessageBox>
#include <QMovie>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QScrollBar>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolButton>
#include <QTimer>
#include <archive.h>
#include <archive_entry.h>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#include <shobjidl.h>
#include <shlguid.h>
#endif

namespace {
// 合成拖动不抢占用户桌面前台；失焦隐藏行为由原有搜索回归单独验证。
class TestSearchWindow : public SearchWindow {
public:
    bool retainForSyntheticInput=true;
protected:
    bool eventFilter(QObject* watched,QEvent* event) override {
        // 原生后端抓取鼠标后会补发真实光标移动，不能覆盖合成拖动坐标。
        if (retainForSyntheticInput && qobject_cast<LauncherGrid*>(watched) && event->spontaneous()
            && (event->type()==QEvent::MouseMove || event->type()==QEvent::MouseButtonPress
                || event->type()==QEvent::MouseButtonRelease || event->type()==QEvent::UngrabMouse)) return true;
        return SearchWindow::eventFilter(watched,event);
    }
    bool event(QEvent* event) override {
        if (retainForSyntheticInput && event->type()==QEvent::WindowDeactivate) return true;
        return SearchWindow::event(event);
    }
};
void spin(int ms = 220)
{
    QEventLoop loop;
    QTimer::singleShot(ms,&loop,&QEventLoop::quit);
    loop.exec();
}

bool waitForFile(const QString& path)
{
    QElapsedTimer deadline;deadline.start();
    while (!QFileInfo::exists(path) && deadline.elapsed()<5000) spin(25);
    return QFileInfo::exists(path);
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data)==data.size();
}

bool writeZip(const QString& path, const QMap<QString,QByteArray>& files)
{
    archive* zip=archive_write_new();
    archive_write_set_format_zip(zip);
    bool ok=archive_write_open_filename_w(zip,path.toStdWString().c_str())==ARCHIVE_OK;
    for (auto it=files.begin();ok && it!=files.end();++it) {
        archive_entry* entry=archive_entry_new();
        const auto name=it.key().toUtf8();
        archive_entry_set_pathname_utf8(entry,name.constData());
        archive_entry_set_filetype(entry,AE_IFREG);
        archive_entry_set_perm(entry,0644);
        archive_entry_set_size(entry,it.value().size());
        ok=archive_write_header(zip,entry)==ARCHIVE_OK
            && archive_write_data(zip,it.value().constData(),it.value().size())==it.value().size();
        archive_entry_free(entry);
    }
    archive_write_close(zip); archive_write_free(zip);
    return ok;
}

void mouse(QWidget& target,QEvent::Type type,const QPoint& pos,Qt::MouseButton button,Qt::MouseButtons buttons)
{
    QMouseEvent event(type,QPointF(pos),QPointF(target.mapToGlobal(pos)),button,buttons,Qt::NoModifier);
    QApplication::sendEvent(&target,&event);
}

QJsonObject basicManifest()
{
    return {{"id","test-skin"},{"name",QStringLiteral("自测皮肤")},{"version","1.0.0"},
            {"minAppVersion","1.0.0"},{"stylesheet","theme.qss"},{"assetRoot","assets"}};
}

QImage renderFrame(LauncherSkin& skin)
{
    QImage image(skin.size(),QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    // 只检查外框像素，避免角色、输入框或桌面背景掩盖多余边线。
    skin.render(&image,QPoint(),QRegion(),QWidget::RenderFlags());
    return image;
}

bool transparentBelow(const QImage& image,int top)
{
    for (int y=top;y<image.height();++y)
        for (int x=0;x<image.width();++x)
            if (qAlpha(image.pixel(x,y))!=0) return false;
    return true;
}

bool movieDecorationVisible(QWidget& host,QMovie* movie)
{
    for (auto* label:host.findChildren<QLabel*>())
        if (label->movie()==movie) return label->isVisibleTo(&host);
    return false;
}
}

// 独立沙箱和合成素材；不要求安装任何 DLL 插件或第三方皮肤。
int runHotaruSelftest(QApplication& app, bool preview, const QString& localPackage)
{
    std::setvbuf(stdout,nullptr,_IONBF,0);
    int failures=0;
    const auto check=[&](const char* label,bool ok) {
        std::printf("%s %s\n",ok?"ok":"FAIL",label);
        if (!ok) ++failures;
    };
    check("宿主统一使用Hotaru应用名称",app.applicationName()==QStringLiteral("Hotaru")
        && app.applicationDisplayName()==QStringLiteral("萤 Hotaru"));
    {
        AboutPage about;
        auto* update=about.findChild<QPushButton*>("checkUpdate");
        auto* download=about.findChild<QPushButton*>("downloadUpdate");
        auto* install=about.findChild<QPushButton*>("installUpdate");
        check("独立更新渠道未配置时禁止上游覆盖升级",update && !update->isEnabled()
            && download && !download->isEnabled() && install && !install->isEnabled());
        QMessageBox legacy(QMessageBox::Information,QStringLiteral("ltool"),QStringLiteral("兼容旧插件提示"));
        legacy.ensurePolished();
        check("旧插件标准弹窗使用宿主品牌",legacy.windowTitle()==app.applicationDisplayName());
    }
    const QString root=Utils::dataDir();
    QDir().mkpath(root);
    check("全新固定区为空且默认黑色",PinnedApps::load().isEmpty() && Theme::currentPackageId()=="hotaru-black");
    check("插件自动更新默认关闭",!PluginStore::autoUpdateEnabled());
    check("三个基础主题均内置",Theme::packages().size()==3);
    QString error;
    for (const QString& id : {QStringLiteral("hotaru-black"),QStringLiteral("hotaru-grey"),QStringLiteral("hotaru-white")}) {
        check("基础主题切换保存",Theme::activatePackage(id,&error) && PinnedApps::config().themeId==id);
        check("基础主题不能删除",!Theme::removePackage(id,&error));
    }
    auto config=PinnedApps::config(); config.themeId="hotaru-blue"; PinnedApps::saveConfig(config);
    check("默认固定网格为四行八列并回首页",config.gridRows==4 && config.gridColumns==8 && !config.rememberPage);
    check("旧内置蓝色主题迁移为黑色",Theme::currentPackageId()=="hotaru-black" && PinnedApps::config().themeId=="hotaru-black");

    // 完整页逐格检查边界和点击，覆盖不能整除行列数的尺寸。
    {
        QWidget host;
        LauncherGrid geometryGrid(&host);
        QVector<LauncherGrid::Item> cells;
        for (int i=0;i<64;++i) {
            PinnedApp item;item.id=QString::number(i);item.title=item.id;
            item.gridRow=i/8;item.gridColumn=i%8;item.enabled=true;
            cells << LauncherGrid::Item{item,{},true};
        }
        geometryGrid.setItems(cells,8,4);
        QString clicked;
        QObject::connect(&geometryGrid,&LauncherGrid::activated,&host,[&](const QString& id){clicked=id;});
        for (const QSize size : {QSize(666,400),QSize(732,400),QSize(841,547)}) {
            geometryGrid.resize(size);
            bool symmetric=true,hit=true,contiguous=true;
            for (int page=0;page<2;++page) {
                geometryGrid.setPage(page);
                const QRect firstCell=geometryGrid.cellRect(page*4,0),lastCell=geometryGrid.cellRect(page*4+3,7);
                symmetric &= firstCell.left()==size.width()-1-lastCell.right()
                    && firstCell.top()==size.height()-1-lastCell.bottom();
                for (int row=0;row<4;++row) for (int col=0;col<8;++col) {
                    const QRect cell=geometryGrid.cellRect(page*4+row,col);
                    if (col<7) contiguous &= cell.right()+1==geometryGrid.cellRect(page*4+row,col+1).left();
                    if (row<3) contiguous &= cell.bottom()+1==geometryGrid.cellRect(page*4+row+1,col).top();
                    for (const QPoint point : {cell.center(),cell.topLeft(),cell.topRight(),cell.bottomLeft(),cell.bottomRight()}) {
                        clicked.clear();
                        mouse(geometryGrid,QEvent::MouseButtonPress,point,Qt::LeftButton,Qt::LeftButton);
                        mouse(geometryGrid,QEvent::MouseButtonRelease,point,Qt::LeftButton,Qt::NoButton);
                        hit &= clicked==QString::number((page*4+row)*8+col);
                    }
                }
            }
            check("完整页网格四边留白对称且格位连续",symmetric && contiguous);
            check("非整除尺寸下两页所有格位中心和边角命中一致",hit);
        }
    }

    QVector<AppEntry> searchApps;
    for (int i=0;i<25;++i) searchApps << AppEntry{QStringLiteral("qq 搜索结果 %1 ").arg(i)+QString(180,QLatin1Char('x')),
        root+QStringLiteral("/qq/%1.exe").arg(i),{},root+QStringLiteral("/qq/%1.lnk").arg(i),{},"desktop"};
    AppScanner::saveIndex(searchApps);
    TestSearchWindow window;
    check("搜索窗口使用Hotaru标题",window.windowTitle()==QStringLiteral("萤 Hotaru 搜索"));
    window.resize(902,650);
    window.show();
    window.refreshLauncher();
    auto* grid=window.findChild<LauncherGrid*>();
    check("首页不自动加入扫描软件或插件",grid && grid->itemIds().isEmpty());
    {
        QSettings prefs(root+"/config.ini",QSettings::IniFormat);
        const QVariant previous=prefs.value("ui/favoritesOnEmpty");
        auto* residentInput=window.findChild<QLineEdit*>();
        auto* pageBar=window.findChild<QWidget*>("hotaruPages");
        const QSize expandedSize=window.size();
        prefs.setValue("ui/favoritesOnEmpty",false);prefs.sync();
        window.showLauncher("关闭常驻网格回归");spin(30);
        check("关闭呼出网格时固定区和页码同时隐藏",!grid->isVisible() && !pageBar->isVisible());
        check("关闭网格后收起整个下方面板仅留搜索条",window.height()==72
            && window.rect().contains(residentInput->mapTo(&window,residentInput->rect().bottomRight())));
        {
            TestSearchWindow restarted;restarted.showLauncher("紧凑呼出回归");
            check("重建窗口仍仅显示搜索条",restarted.height()==72
                && !restarted.findChild<LauncherGrid*>()->isVisible());
            restarted.hide();
        }
        if (preview) {
            const QString shots=QDir::tempPath()+"/hotaru-previews";QDir().mkpath(shots);
            window.grab().save(shots+"/resident-off.png");
        }
        residentInput->setText("qq");window.refreshLauncher();
        check("关闭网格仍可搜索",window.findChild<QListWidget*>()->isVisible()
            && window.findChild<QListWidget*>()->count()>0);
        check("搜索结果展开且不恢复常驻面板",window.height()>72 && !grid->isVisible() && !pageBar->isVisible());
        residentInput->clear();window.refreshLauncher();
        check("清空搜索继续遵守关闭网格设置",!grid->isVisible() && !pageBar->isVisible());
        check("清空搜索后窗口重新收起",window.height()==72);
        prefs.setValue("ui/favoritesOnEmpty",true);prefs.sync();window.refreshLauncher();
        check("保存设置后刷新立即恢复常驻网格",grid->isVisible() && pageBar->isVisible());
        check("开启网格恢复原来的面板尺寸",window.size()==expandedSize);
        window.hide();window.showLauncher("开启常驻网格回归");
        check("再次呼出按配置显示网格",grid->isVisible() && pageBar->isVisible());
        if (previous.isValid()) prefs.setValue("ui/favoritesOnEmpty",previous);
        else prefs.remove("ui/favoritesOnEmpty");
        prefs.sync();
    }
    {
        const auto initialMode=Theme::current();
        const QString initialPackage=Theme::currentPackageId();
        for (const auto mode : {Theme::Mode::Light,Theme::Mode::Dark}) {
            Theme::setMode(mode);
            Theme::activatePackage(mode==Theme::Mode::Light?"hotaru-black":"hotaru-white",&error);
            const auto basePalette=app.palette();const auto baseQss=app.styleSheet();
            QMessageBox message(QMessageBox::Question,QStringLiteral("删除当前页"),
                QStringLiteral("是否迁移图标后删除？"),QMessageBox::Yes|QMessageBox::No|QMessageBox::Cancel,&window);
            message.setInformativeText(QStringLiteral("所有操作只修改面板配置，不修改源文件。"));
            message.show();spin(40);
            bool readable=message.property("hotaruBaseDialog").toBool();
            const auto contrast=[&](const QWidget* widget,QPalette::ColorRole text,QPalette::ColorRole background) {
                const auto pal=widget->palette();
                const auto ratio=[&](QPalette::ColorGroup group) {
                    // QLabel 背景透明，需与弹窗实际底色合成，不能把透明色当黑色。
                    const QColor bg=Theme::over(pal.color(group,background),message.palette().color(group,QPalette::Window));
                    return Theme::contrast(Theme::over(pal.color(group,text),bg).rgb(),bg.rgb());
                };
                const bool ok=ratio(QPalette::Active)>=4.5 && ratio(QPalette::Inactive)>=4.5;
                if (!ok) std::printf("contrast %s/%s: active=%s/%s inactive=%s/%s\n",widget->metaObject()->className(),
                    widget->objectName().toUtf8().constData(),pal.color(QPalette::Active,text).name().toUtf8().constData(),
                    pal.color(QPalette::Active,background).name().toUtf8().constData(),pal.color(QPalette::Inactive,text).name().toUtf8().constData(),
                    pal.color(QPalette::Inactive,background).name().toUtf8().constData());
                return ok;
            };
            for (auto* label:message.findChildren<QLabel*>())
                if (!label->text().isEmpty()) readable &= contrast(label,QPalette::WindowText,QPalette::Window);
            for (auto* button:message.buttons()) readable &= contrast(button,QPalette::ButtonText,QPalette::Button);
            check("明暗反差皮肤下确认框正文和按钮均清晰",readable);
            if (preview) {
                const QString shots=QDir::tempPath()+"/hotaru-previews";QDir().mkpath(shots);
                message.grab().save(shots+(mode==Theme::Mode::Light?"/dialog-light.png":"/dialog-dark.png"));
            }
            message.hide();
            QInputDialog rename(&window);rename.setLabelText(QStringLiteral("显示名称："));rename.setTextValue(QStringLiteral("新的面板名称"));
            rename.show();spin(30);
            auto* edit=rename.findChild<QLineEdit*>();
            check("重命名输入框隔离启动器配色",rename.property("hotaruBaseDialog").toBool()
                && edit && contrast(edit,QPalette::Text,QPalette::Base));
            rename.hide();
            check("提示样式不污染应用基础主题",app.palette()==basePalette && app.styleSheet()==baseQss);
            Theme::setMode(mode==Theme::Mode::Light?Theme::Mode::Dark:Theme::Mode::Light);
            message.show();spin(30);
            check("已创建提示框切换基础模式后同步刷新",message.palette().color(QPalette::WindowText)==Theme::color("text")
                && message.palette().color(QPalette::Window)==Theme::color("window"));
            message.hide();
        }
        Theme::setMode(initialMode);Theme::activatePackage(initialPackage,&error);
    }
    auto* settingsButton=window.findChild<QToolButton*>("hotaruSettings");
    check("设置入口没有被其他控件遮挡",settingsButton && window.childAt(settingsButton->geometry().center())==settingsButton);
    mouse(*settingsButton,QEvent::MouseButtonPress,settingsButton->rect().center(),Qt::LeftButton,Qt::LeftButton);
    mouse(*settingsButton,QEvent::MouseButtonRelease,settingsButton->rect().center(),Qt::LeftButton,Qt::NoButton);
    spin();
    QPointer<SettingsDialog> openedSettings;
    for (auto* widget:QApplication::topLevelWidgets())
        if (auto* dialog=qobject_cast<SettingsDialog*>(widget);dialog && dialog->isVisible()) openedSettings=dialog;
    check("点击齿轮实际打开设置",openedSettings && !window.isVisible());
    check("设置窗口使用Hotaru标题",openedSettings && openedSettings->windowTitle()==QStringLiteral("萤 Hotaru 设置"));
    check("齿轮直达配置并有分页设置",openedSettings && openedSettings->findChild<QSpinBox*>("gridRows")
        && openedSettings->findChild<QComboBox*>("pageStart") && openedSettings->findChild<QTabWidget*>()->currentIndex()==1);
    if (openedSettings) {
        const auto* showGrid=openedSettings->findChild<QCheckBox*>("showResidentGrid");
        check("呼出网格设置使用约定文案",showGrid && showGrid->text()==QStringLiteral("呼出时显示常驻网格"));
        bool removedToggle=true;
        for (auto* toggle:openedSettings->findChildren<QCheckBox*>())
            removedToggle &= toggle->text()!=QStringLiteral("在首页显示固定软件");
        check("设置移除重复的首页固定软件开关",removedToggle);
        const QRect available=window.screen()->availableGeometry();
        check("设置实际窗口位于启动器屏幕且不越界",available.contains(openedSettings->frameGeometry()));
        openedSettings->move(available.x(),available.y()-2000);openedSettings->hide();
        window.openSettings();spin();
        check("复用隐藏设置时纠正旧副屏坐标",openedSettings->isVisible() && available.contains(openedSettings->frameGeometry()));
        openedSettings->showMinimized();window.openSettings();spin();
        check("最小化设置可还原并定位",!openedSettings->isMinimized() && available.contains(openedSettings->frameGeometry()));
        openedSettings->close();
    }
    spin();
    if (QGuiApplication::platformName()=="windows") {
        for (auto* target:QGuiApplication::screens()) {
            const QRect bounds=target->availableGeometry();
            window.move(bounds.center()-QPoint(window.width()/2,window.height()/2));window.show();spin(60);
            const auto launcherMonitor=MonitorFromWindow(reinterpret_cast<HWND>(window.winId()),MONITOR_DEFAULTTONEAREST);
            mouse(*settingsButton,QEvent::MouseButtonPress,settingsButton->rect().center(),Qt::LeftButton,Qt::LeftButton);
            mouse(*settingsButton,QEvent::MouseButtonRelease,settingsButton->rect().center(),Qt::LeftButton,Qt::NoButton);spin();
            QPointer<SettingsDialog> actual;
            for (auto* widget:QApplication::topLevelWidgets()) if (auto* dlg=qobject_cast<SettingsDialog*>(widget);dlg && dlg->isVisible()) actual=dlg;
            check("原生双屏设置同屏可见且矩形在可用区",actual && target->availableGeometry().contains(actual->frameGeometry()) && actual->screen()==target);
            if (actual) {
                const HWND handle=reinterpret_cast<HWND>(actual->winId());
                MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);GetMonitorInfo(launcherMonitor,&monitor);
                RECT visible{};
                using FrameBounds=HRESULT (WINAPI*)(HWND,DWORD,PVOID,DWORD);
                const auto frameBounds=reinterpret_cast<FrameBounds>(GetProcAddress(GetModuleHandleW(L"dwmapi.dll"),"DwmGetWindowAttribute"));
                const bool nativeBounds=frameBounds && SUCCEEDED(frameBounds(handle,9,&visible,sizeof(visible)));
                const bool inside=nativeBounds && visible.left>=monitor.rcWork.left && visible.top>=monitor.rcWork.top
                    && visible.right<=monitor.rcWork.right && visible.bottom<=monitor.rcWork.bottom;
                check("Win32设置窗口可见外框同屏且不超工作区",IsWindowVisible(handle) && !IsIconic(handle)
                    && MonitorFromWindow(handle,MONITOR_DEFAULTTONEAREST)==launcherMonitor && inside);
            }
            if (actual) actual->close();spin();
        }
        window.showLauncher("分组回归");
    }
    window.show();
    {
        // 所有源文件均为沙箱夹具；别名操作逐字节核对源文件和启动字段。
        const auto initial=PinnedApps::config();
        PinnedApps::LauncherConfig layout;
        for (int i=0;i<12;++i) {
            const auto path=root+QStringLiteral("/分组回归/%1.exe").arg(i);writeFile(path,"source-bytes");
            auto a=PinnedApps::fromTarget(path);a.id="layout-"+QString::number(i);a.gridRow=i/8;a.gridColumn=i%8;
            a.arguments="--test original";layout.pinnedApps.append(a);
        }
        PinnedApps::saveConfig(layout);
        const auto bytes=[](const QString& path) {QFile f(path);if (!f.open(QIODevice::ReadOnly)) return QByteArray();return f.readAll();};
        const auto original=layout.pinnedApps[0];const auto sourceBytes=bytes(original.targetPath);
        check("显示别名只修改配置且允许重名",PinnedApps::rename(original.id,"  我的显示名  ",&error)
            && PinnedApps::load()[0].title==QStringLiteral("我的显示名") && bytes(original.targetPath)==sourceBytes
            && PinnedApps::load()[0].targetPath==original.targetPath && PinnedApps::load()[0].arguments==original.arguments
            && PinnedApps::load()[0].workingDirectory==original.workingDirectory);
        check("拒绝空白名称并保留显示名",!PinnedApps::rename(original.id,"   ",&error) && PinnedApps::load()[0].title==QStringLiteral("我的显示名"));
        auto missing=layout.pinnedApps[1];QFile::remove(missing.targetPath);
        check("失效目标仍能单独重命名",PinnedApps::rename(missing.id,"失效别名",&error));
        PinnedApps::saveConfig(layout);window.refreshLauncher();grid->setPage(0);spin();
        const QPoint source=grid->cellRect(0,0).center(),target=grid->cellRect(0,1).center();
        mouse(*grid,QEvent::MouseButtonPress,source,Qt::LeftButton,Qt::LeftButton);
        mouse(*grid,QEvent::MouseMove,target,Qt::NoButton,Qt::LeftButton);spin(180);
        check("未达到停留阈值不出现合并预览",!grid->mergePreview());
        mouse(*grid,QEvent::MouseButtonRelease,target,Qt::LeftButton,Qt::NoButton);
        check("快速拖到占用格禁止合并",PinnedApps::config().groups.isEmpty());
        mouse(*grid,QEvent::MouseButtonPress,source,Qt::LeftButton,Qt::LeftButton);
        mouse(*grid,QEvent::MouseMove,target,Qt::NoButton,Qt::LeftButton);spin(560);
        check("图标中心停留显示合并预览",grid->mergePreview());
        mouse(*grid,QEvent::MouseButtonRelease,target,Qt::LeftButton,Qt::NoButton);spin();
        auto grouped=PinnedApps::config();
        check("松手创建独立分组且原格留空",grouped.groups.size()==1 && grouped.pinnedApps[0].groupId==grouped.groups[0].id
            && grouped.pinnedApps[1].groupId==grouped.groups[0].id && grouped.groups[0].gridColumn==1 && PinnedApps::firstFreeCell(grouped)==QPoint(0,0));
        if (!grouped.groups.isEmpty()) {
            const QString groupId=grouped.groups[0].id;
            check("分组显示名可单独修改",PinnedApps::rename(groupId,"  我的分组  ",&error) && PinnedApps::config().groups[0].title==QStringLiteral("我的分组"));
            check("分组禁止嵌套",!PinnedApps::merge(groupId,"layout-2",&error));
            for (int i=2;i<10;++i) check("加入分组保持隐藏项占位",PinnedApps::merge("layout-"+QString::number(i),groupId,&error));
            auto withHidden=PinnedApps::config();withHidden.pinnedApps[2].enabled=false;PinnedApps::saveConfig(withHidden);
            window.refreshLauncher();grid->setPage(0);
            const auto groupCell=grid->cellRect(0,1).center();
            mouse(*grid,QEvent::MouseButtonPress,groupCell,Qt::LeftButton,Qt::LeftButton);
            mouse(*grid,QEvent::MouseButtonRelease,groupCell,Qt::LeftButton,Qt::NoButton);spin();
            auto* panel=window.findChild<QWidget*>("hotaruGroupPanel");
            auto* children=window.findChild<LauncherGrid*>("hotaruGroupGrid");
            check("点击分组打开二级3乘3分页弹层",panel && panel->isVisible() && children && children->pageCount()==2 && !children->itemIds().contains("layout-2"));
            if (children && panel) {
                children->setPage(1);check("二级页分页不显示其他页图标",children->itemIds().size()==1);
                children->setPage(0);QKeyEvent right(QEvent::KeyPress,Qt::Key_Right,Qt::NoModifier);QApplication::sendEvent(children,&right);
                QTimer::singleShot(30,&window,[] {
                    for (auto* widget:QApplication::topLevelWidgets()) if (auto* dialog=qobject_cast<QInputDialog*>(widget)) {dialog->setTextValue(QStringLiteral("组内新别名"));dialog->accept();}
                });
                QKeyEvent f2(QEvent::KeyPress,Qt::Key_F2,Qt::NoModifier);QApplication::sendEvent(children,&f2);
                check("F2修改组内显示名不改源文件",PinnedApps::load()[0].title==QStringLiteral("组内新别名") && bytes(original.targetPath)==sourceBytes);
                auto* searchInput=window.findChild<QLineEdit*>("input");
                searchInput->setText(QStringLiteral("组内新别名"));window.flushSearch();
                bool foundChild=false;
                for (int i=0;i<window.resultCount();++i) foundChild|=window.resultAt(i).kind==SearchResult::PinnedApp && window.resultAt(i).target==original.id;
                check("搜索包含分组内别名且判重跨组",foundChild && PinnedApps::isPinned(original));
                searchInput->clear();window.flushSearch();spin();
                children->setPage(0);
                // 二级页自由空位与跨页拖动沿用相同状态机。
                auto childStart=children->cellRect(0,1).center();
                mouse(*children,QEvent::MouseButtonPress,childStart,Qt::LeftButton,Qt::LeftButton);
                mouse(*children,QEvent::MouseMove,QPoint(children->width()-10,childStart.y()),Qt::NoButton,Qt::LeftButton);
                QElapsedTimer turnWait;turnWait.start();while (children->currentPage()!=1 && turnWait.elapsed()<2000) spin(25);
                const auto childEnd=children->cellRect(3,2).center();
                mouse(*children,QEvent::MouseMove,childEnd,Qt::NoButton,Qt::LeftButton);
                mouse(*children,QEvent::MouseButtonRelease,childEnd,Qt::LeftButton,Qt::NoButton);
                check("二级页跨页移动保存且原格留空",PinnedApps::load()[0].groupId==groupId && PinnedApps::load()[0].gridRow==3 && PinnedApps::load()[0].gridColumn==2);
                PinnedApps::moveTo(original.id,groupId,0,1,&error);window.refreshLauncher();spin();children->setPage(0);
                const QString exported=root+"/layout-export.json";
                check("导出导入包含页面分组及子项布局",PinnedApps::exportConfig(exported,&error) && PinnedApps::importConfig(exported,&error)
                    && PinnedApps::config().groups.size()==1 && PinnedApps::load()[0].groupId==groupId && PinnedApps::config().pages==1);
                // 原生窗口需先处理导入后的布局和焦点事件，再开始下一段合成拖动。
                spin();
                // 子项在空格自由移动，取消拖出不应写入配置。
                const auto beforeTransfer=bytes(PinnedApps::configPath());
                auto start=children->cellRect(0,1).center();
                mouse(*children,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
                const QPoint outside(-30,children->height()/2);
                mouse(*children,QEvent::MouseMove,outside,Qt::NoButton,Qt::LeftButton);
                QElapsedTimer transferWait;transferWait.start();
                while (!grid->isDragging() && children->isDragging() && transferWait.elapsed()<2000) spin(25);
                check("拖出弹层停留转为主面板拖动",grid->isDragging() && !panel->isVisible());
                QKeyEvent cancel(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(grid,&cancel);spin(40);
                check("跨容器拖动取消恢复分组关系和原页",!grid->isDragging() && panel->isVisible() && bytes(PinnedApps::configPath())==beforeTransfer);
                start=children->cellRect(0,1).center();
                mouse(*children,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
                mouse(*children,QEvent::MouseMove,outside,Qt::NoButton,Qt::LeftButton);
                transferWait.restart();while (!grid->isDragging() && children->isDragging() && transferWait.elapsed()<2000) spin(25);
                check("再次拖出分组完成鼠标抓取交接",grid->isDragging() && !panel->isVisible());
                const auto destination=grid->cellRect(0,0).center();
                mouse(*grid,QEvent::MouseMove,destination,Qt::NoButton,Qt::LeftButton);
                mouse(*grid,QEvent::MouseButtonRelease,destination,Qt::LeftButton,Qt::NoButton);spin();
                if (!PinnedApps::load()[0].groupId.isEmpty() || PinnedApps::load()[0].gridColumn!=0)
                    std::printf("transfer-state main=%d child=%d panel=%d row=%d col=%d group=%s\n",grid->isDragging(),children->isDragging(),panel->isVisible(),PinnedApps::load()[0].gridRow,PinnedApps::load()[0].gridColumn,PinnedApps::load()[0].groupId.toUtf8().constData());
                check("组内图标拖到主面板空格",PinnedApps::load()[0].groupId.isEmpty() && PinnedApps::load()[0].gridColumn==0 && PinnedApps::config().groups.size()==1);
            }
            check("解散使用现有空格且不压紧原项",PinnedApps::dissolve(groupId,&error) && PinnedApps::config().groups.isEmpty());
            check("分组重命名不影响磁盘目录",!QFileInfo::exists(root+"/新建文件夹"));
        }
        window.refreshLauncher();
        auto emptyPages=PinnedApps::LauncherConfig();emptyPages.pages=3;emptyPages.lastPage=2;PinnedApps::saveConfig(emptyPages);
        check("新建空白页可重读保持",PinnedApps::config().pages==3 && PinnedApps::pageCount(PinnedApps::config())==3);
        check("空页删除修正页数和记忆页",PinnedApps::deletePage(1,false,&error) && PinnedApps::config().pages==2 && PinnedApps::config().lastPage==1);
        check("至少保留一页",PinnedApps::deletePage(0,false,&error) && !PinnedApps::deletePage(0,false,&error));
        PinnedApps::saveConfig(layout);PinnedApps::addPage();PinnedApps::move("layout-0",4,6);
        auto hidden=PinnedApps::config();hidden.pinnedApps[1].enabled=false;PinnedApps::saveConfig(hidden);
        check("删页迁移保留隐藏占位且按首个空格落下",PinnedApps::deletePage(1,true,&error) && PinnedApps::load()[0].gridRow==0 && PinnedApps::load()[0].gridColumn==0 && !PinnedApps::load()[1].enabled);
        check("删页移除仅删除固定配置",PinnedApps::addPage(&error) && PinnedApps::move("layout-0",4,0,&error)
            && PinnedApps::deletePage(1,false,&error) && bytes(original.targetPath)==sourceBytes && PinnedApps::load().size()==11);
        // 满页迁移、隐藏占位、分组整体迁移与保存失败分别验证。
        auto full=PinnedApps::LauncherConfig();full.pages=2;
        for (int i=0;i<33;++i) {auto a=original;a.id="full-"+QString::number(i);a.gridRow=i/8;a.gridColumn=i%8;a.enabled=i%2==0;full.pinnedApps.append(a);}
        PinnedApps::saveConfig(full);const auto fullBytes=bytes(PinnedApps::configPath());
        check("容量不足不增加页且完整保留配置",!PinnedApps::deletePage(1,true,&error) && bytes(PinnedApps::configPath())==fullBytes);
        for (const bool keep:{true,false}) {
            PinnedApps::saveConfig(full);window.refreshLauncher();grid->setPage(1);
            QVector<QMessageBox::StandardButton> answers{QMessageBox::Yes,keep?QMessageBox::Yes:QMessageBox::No};
            QTimer responder;responder.setInterval(20);
            QObject::connect(&responder,&QTimer::timeout,&window,[&] {
                for (auto* widget:QApplication::topLevelWidgets()) if (auto* box=qobject_cast<QMessageBox*>(widget);box && box->isVisible() && !answers.isEmpty()) {
                    const auto answer=answers.takeFirst();if (box->button(answer)) box->button(answer)->click();return;
                }
            });responder.start();grid->deletePageRequested();responder.stop();
            check(keep?"迁移失败默认保留此页":"迁移失败可明确移除此页配置",PinnedApps::config().pages==(keep?2:1) && PinnedApps::load().size()==(keep?33:32));
        }
        PinnedApps::saveConfig(layout);PinnedApps::merge("layout-0","layout-1");
        const auto groupId=PinnedApps::config().groups[0].id;
        PinnedApps::addPage();PinnedApps::move(groupId,4,2);
        check("分组带子项整体迁移且组内坐标不变",PinnedApps::deletePage(1,true,&error) && PinnedApps::config().groups.size()==1 && PinnedApps::load()[0].groupId==groupId && PinnedApps::load()[0].gridRow==0);
        PinnedApps::remove("layout-1");check("单项分组继续保留",PinnedApps::config().groups.size()==1);
        PinnedApps::remove("layout-0");check("移除最后子项自动删除空分组",PinnedApps::config().groups.isEmpty());
#ifdef Q_OS_WIN
        PinnedApps::saveConfig(full);
        const auto beforeFailure=bytes(PinnedApps::configPath());
        const auto native=QDir::toNativeSeparators(PinnedApps::configPath());
        const DWORD attributes=GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
        SetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()),attributes|FILE_ATTRIBUTE_READONLY);
        check("删页写入失败保留原配置",!PinnedApps::deletePage(1,false,&error) && bytes(PinnedApps::configPath())==beforeFailure);
        check("重命名写入失败保留原配置",!PinnedApps::rename("full-0","不可保存",&error) && bytes(PinnedApps::configPath())==beforeFailure);
        SetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()),attributes);
#endif
        auto cramped=full;cramped.pages=1;cramped.pinnedApps.resize(31);
        const QString crampedId="cramped-group";cramped.groups.append({crampedId,QStringLiteral("保留分组"),3,7});
        for (int i=0;i<2;++i) {auto child=original;child.id="cramped-"+QString::number(i);child.groupId=crampedId;child.gridRow=0;child.gridColumn=i;cramped.pinnedApps.append(child);}
        PinnedApps::saveConfig(cramped);const auto crampedBytes=bytes(PinnedApps::configPath());
        check("解散容量不足时完整保留分组",!PinnedApps::dissolve(crampedId,&error) && bytes(PinnedApps::configPath())==crampedBytes);
        PinnedApps::saveConfig(initial);window.refreshLauncher();grid->setPage(0);window.show();
    }
    for (int i=0;i<5;++i) {
        const QString path=root+QStringLiteral("/程序 空格/%1.exe").arg(i);
        writeFile(path,"test");
        auto item=PinnedApps::fromTarget(path,i);
        item.id=QString::number(i); item.title=QStringLiteral("测试程序 %1").arg(i);
        item.category=(i%2==0)?QStringLiteral("分类甲"):QStringLiteral("分类乙");
        config.pinnedApps << item;
    }
    config.themeId="hotaru-black"; config.pinnedApps[2].enabled=false;
    PinnedApps::saveConfig(config);
    window.refreshLauncher();
    check("仅显示手动添加且启用的程序",grid->itemIds()==QStringList{"0","1","3","4"});
    check("添加程序正确识别 exe 类型",PinnedApps::load().front().targetType=="exe");
    int launches=0; QObject::connect(grid,&LauncherGrid::activated,&app,[&](const QString&){++launches;});
    QPoint first=grid->itemRect(0).center(),second=grid->itemRect(1).center();
    const auto drag=[&](const QPoint& from,const QPoint& to) {
        mouse(*grid,QEvent::MouseButtonPress,from,Qt::LeftButton,Qt::LeftButton);
        mouse(*grid,QEvent::MouseMove,to,Qt::NoButton,Qt::LeftButton);
        check("超过阈值开始拖动",grid->isDragging());
        mouse(*grid,QEvent::MouseButtonRelease,to,Qt::LeftButton,Qt::NoButton);spin();
    };
    drag(first,second);
    check("占用格禁止放置且不启动",launches==0 && PinnedApps::load()[0].gridColumn==0);
    drag(first,grid->itemRect(2).center());
    check("隐藏项保留格位且不能覆盖",PinnedApps::load()[0].gridColumn==0 && !PinnedApps::move("0",0,2,&error));
    const QPoint empty=grid->cellRect(1,3).center();
    drag(first,empty);
    check("自由格位保存且原格留空",launches==0 && PinnedApps::load()[0].gridRow==1 && PinnedApps::load()[0].gridColumn==3
        && PinnedApps::firstFreeCell(PinnedApps::config())==QPoint(0,0));
    window.refreshLauncher();
    check("重新加载保持位置",grid->itemRect(0)==grid->cellRect(1,3));
    first=grid->itemRect(0).center();
    mouse(*grid,QEvent::MouseButtonPress,first,Qt::LeftButton,Qt::LeftButton);
    mouse(*grid,QEvent::MouseMove,grid->cellRect(1,1).center(),Qt::NoButton,Qt::LeftButton);
    QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(grid,&escape);
    mouse(*grid,QEvent::MouseButtonRelease,second,Qt::LeftButton,Qt::NoButton);
    check("Esc 恢复原格且不启动",PinnedApps::load()[0].gridRow==1 && PinnedApps::load()[0].gridColumn==3 && launches==0);
    drag(first,QPoint(-5,-5));
    check("区域外松手保持格位",PinnedApps::load()[0].gridRow==1 && PinnedApps::load()[0].gridColumn==3);
#ifdef Q_OS_WIN
    // 真实写入失败：让沙箱配置文件只读，随后恢复属性；不能覆盖用户数据。
    const QString configNative=QDir::toNativeSeparators(PinnedApps::configPath());
    const DWORD oldAttributes=GetFileAttributesW(reinterpret_cast<LPCWSTR>(configNative.utf16()));
    const bool readOnly=SetFileAttributesW(reinterpret_cast<LPCWSTR>(configNative.utf16()),oldAttributes|FILE_ATTRIBUTE_READONLY)!=FALSE;
    QTimer::singleShot(50,&window,[] {for (auto* widget:QApplication::topLevelWidgets()) if (auto* message=qobject_cast<QMessageBox*>(widget)) message->accept();});
    drag(grid->itemRect(0).center(),grid->cellRect(1,1).center());
    SetFileAttributesW(reinterpret_cast<LPCWSTR>(configNative.utf16()),oldAttributes);
    check("保存失败恢复原格位",readOnly && PinnedApps::load()[0].gridRow==1 && PinnedApps::load()[0].gridColumn==3
        && grid->itemRect(0)==grid->cellRect(1,3));
#endif
    auto valid= PinnedApps::config();auto invalidConfig=valid;invalidConfig.pinnedApps<<invalidConfig.pinnedApps.front();
    check("重复 ID 保存被拒绝且原配置保留",!PinnedApps::saveConfig(invalidConfig,&error) && PinnedApps::load().size()==5);
    invalidConfig=valid;invalidConfig.pinnedApps[0].gridRow=0;invalidConfig.pinnedApps[0].gridColumn=2;
    check("重复格位保存被拒绝",!PinnedApps::saveConfig(invalidConfig,&error));
    invalidConfig=valid;invalidConfig.pinnedApps.removeAt(1);PinnedApps::saveConfig(invalidConfig);
    check("删除不补位",PinnedApps::load().front().gridRow==1 && PinnedApps::load().front().gridColumn==3);
    PinnedApps::saveConfig(valid);
    auto* input=window.findChild<QLineEdit*>("input");input->setText(QStringLiteral("分类甲"));window.refreshLauncher();
    check("固定项搜索统一显示列表",window.resultCount()>0 && grid->itemIds()==QStringList{"0","1","3","4"});
    bool pinnedFound=false;
    for (int i=0;i<window.resultCount();++i) if (window.resultAt(i).kind==SearchResult::PinnedApp) pinnedFound=true;
    check("分类搜索包含固定项",pinnedFound);
    input->clear();window.refreshLauncher();
    check("启动器不再显示分类控件",window.findChild<QComboBox*>()==nullptr);
    check("设置使用无文字矢量按钮",window.findChildren<QToolButton*>().size()==1 && window.findChild<QToolButton*>()->text().isEmpty());
    PinnedApps::move("4",1,1,&error);
    for (int columns=4;columns<=8;++columns) {
        auto cfg=PinnedApps::config();cfg.gridColumns=columns;
        check("4 至 8 列可保存",PinnedApps::saveConfig(cfg,&error));window.refreshLauncher();
        window.resize(760,500);QApplication::processEvents();
        check("列数变化不移动已有行列",PinnedApps::load()[0].gridRow==1 && PinnedApps::load()[0].gridColumn==3
            && grid->cellRect(0,columns-1).right()<grid->width());
    }
    PinnedApps::move("0",1,7,&error);
    auto reduced=PinnedApps::config();reduced.gridColumns=4;
    check("减少列数保护右侧项",!PinnedApps::saveConfig(reduced,&error) && PinnedApps::config().gridColumns==8);
    PinnedApps::move("0",1,3,&error);window.refreshLauncher();
    const auto beforePages=PinnedApps::config();
    check("跨页格位可保存",PinnedApps::move("4",4,5,&error));window.refreshLauncher();
    check("固定面板无滚动且页面只显示本页",grid->pageCount()==2 && !grid->itemIds().contains("4")
        && window.findChild<QScrollArea*>("hotaruHomeScroll")->verticalScrollBarPolicy()==Qt::ScrollBarAlwaysOff
        && window.findChild<QScrollArea*>("hotaruHomeScroll")->verticalScrollBar()->maximum()==0);
    grid->setPage(1);
    check("翻页显示相应固定项且页码落盘",grid->itemIds()==QStringList{"4"} && PinnedApps::config().lastPage==1);
    auto* pageBar=window.findChild<QWidget*>("hotaruPages");
    mouse(*pageBar,QEvent::MouseButtonRelease,QPoint(13,14),Qt::LeftButton,Qt::NoButton);
    check("底栏上一页可用",grid->currentPage()==0);
    QKeyEvent nextPage(QEvent::KeyPress,Qt::Key_PageDown,Qt::NoModifier);QApplication::sendEvent(grid,&nextPage);
    check("PageDown 翻页",grid->currentPage()==1);
    window.retainForSyntheticInput=true;
    window.hide();window.showLauncher("分页首页自测");
    check("回首页模式每次呼出重置",grid->currentPage()==0 && PinnedApps::config().lastPage==0);
    auto remember=PinnedApps::config();remember.rememberPage=true;PinnedApps::saveConfig(remember);
    grid->setPage(1);window.hide();window.showLauncher("分页记忆自测");
    check("记忆模式恢复上次页",grid->currentPage()==1 && PinnedApps::config().lastPage==1);
    {
        TestSearchWindow restarted;restarted.showLauncher("重建窗口页码自测");
        check("重新创建窗口从磁盘恢复页码",restarted.findChild<LauncherGrid*>()->currentPage()==1);
        restarted.hide();
    }
    window.show();spin();
    grid->setPage(0);
    const auto waitForGridPage=[&](int page) {
        QElapsedTimer deadline;deadline.start();
        while (grid->isDragging() && grid->currentPage()!=page && deadline.elapsed()<2000) spin(25);
    };
    const QPoint source=grid->itemRect(0).center();
    mouse(*grid,QEvent::MouseButtonPress,source,Qt::LeftButton,Qt::LeftButton);
    mouse(*grid,QEvent::MouseMove,QPoint(grid->width()-10,source.y()),Qt::NoButton,Qt::LeftButton);waitForGridPage(1);
    check("拖动边缘停留跨页",grid->isDragging() && grid->currentPage()==1);
    const auto pageTarget=grid->cellRect(4,4).center();
    mouse(*grid,QEvent::MouseMove,pageTarget,Qt::NoButton,Qt::LeftButton);
    mouse(*grid,QEvent::MouseButtonRelease,pageTarget,Qt::LeftButton,Qt::NoButton);spin();
    check("跨页拖动保存且不误启动",PinnedApps::load()[0].gridRow==4 && PinnedApps::load()[0].gridColumn==4 && launches==0);
#ifdef Q_OS_WIN
    const auto failedMoveSource=grid->itemRect(0).center();
    mouse(*grid,QEvent::MouseButtonPress,failedMoveSource,Qt::LeftButton,Qt::LeftButton);
    mouse(*grid,QEvent::MouseMove,QPoint(10,failedMoveSource.y()),Qt::NoButton,Qt::LeftButton);waitForGridPage(0);
    SetFileAttributesW(reinterpret_cast<LPCWSTR>(configNative.utf16()),oldAttributes|FILE_ATTRIBUTE_READONLY);
    QTimer::singleShot(50,&window,[] {for (auto* widget:QApplication::topLevelWidgets()) if (auto* message=qobject_cast<QMessageBox*>(widget)) message->accept();});
    const auto failedTarget=grid->cellRect(0,0).center();
    mouse(*grid,QEvent::MouseMove,failedTarget,Qt::NoButton,Qt::LeftButton);
    mouse(*grid,QEvent::MouseButtonRelease,failedTarget,Qt::LeftButton,Qt::NoButton);
    SetFileAttributesW(reinterpret_cast<LPCWSTR>(configNative.utf16()),oldAttributes);spin();
    check("跨页保存失败恢复原页原格",grid->currentPage()==1 && PinnedApps::load()[0].gridRow==4 && launches==0);
#endif
    const auto moved=grid->itemRect(0).center();
    mouse(*grid,QEvent::MouseButtonPress,moved,Qt::LeftButton,Qt::LeftButton);
    mouse(*grid,QEvent::MouseMove,QPoint(10,moved.y()),Qt::NoButton,Qt::LeftButton);waitForGridPage(0);
    QApplication::sendEvent(grid,&escape);
    check("跨页拖动取消回到原页原格",grid->currentPage()==1 && PinnedApps::load()[0].gridRow==4);
    mouse(*pageBar,QEvent::MouseButtonRelease,QPoint(131,14),Qt::LeftButton,Qt::NoButton);
    check("可创建空白页",grid->currentPage()==2 && grid->itemIds().isEmpty());
    const QString newPageTarget=root+"/new-page.exe";writeFile(newPageTarget,"test");
    auto appended=PinnedApps::fromTarget(newPageTarget);appended.id="new-page-item";
    check("新页空格添加保持全局坐标",PinnedApps::add(appended,8,2,&error));window.refreshLauncher();
    check("新页新增项不压紧旧格",grid->currentPage()==2 && grid->itemIds()==QStringList{"new-page-item"});
    auto fewerRows=PinnedApps::config();fewerRows.gridRows=2;PinnedApps::saveConfig(fewerRows);window.refreshLauncher();
    check("调整行数保留全局格位且所有项仍可翻页访问",PinnedApps::load()[0].gridRow==4 && grid->pageCount()==5);
    PinnedApps::saveConfig(beforePages);window.refreshLauncher();grid->setPage(0);
    const QString folder=root+QStringLiteral("/固定文件夹");QDir().mkpath(folder);
    const auto folderApp=PinnedApps::fromTarget(folder);
    check("文件夹不能提权",!PinnedApps::canRunAdmin(folderApp));
    check("目录可固定而普通文件不可固定",PinnedApps::supportsTarget(folder) && folderApp.targetType=="folder"
        && !PinnedApps::supportsTarget(root+"/ordinary.txt"));
    check("新增使用第一个空格",PinnedApps::add(folderApp,-1,-1,&error) && PinnedApps::load().back().gridRow==0 && PinnedApps::load().back().gridColumn==0);
    check("相同启动信息禁止重复固定",PinnedApps::isPinned(folderApp) && !PinnedApps::add(folderApp,-1,-1,&error));
    auto cleanup= PinnedApps::config();cleanup.pinnedApps.removeLast();PinnedApps::saveConfig(cleanup);
    QFile legacyFile(PinnedApps::configPath());check("旧配置夹具可读取",legacyFile.open(QIODevice::ReadOnly));
    auto legacy=QJsonDocument::fromJson(legacyFile.readAll()).object();legacyFile.close();
    auto legacyItems=legacy.value("pinnedApps").toArray();
    for (int i=0;i<legacyItems.size();++i) {auto object=legacyItems[i].toObject();object.remove("gridRow");object.remove("gridColumn");legacyItems[i]=object;}
    legacy.insert("pinnedApps",legacyItems);writeFile(PinnedApps::configPath(),QJsonDocument(legacy).toJson());
    check("旧配置按原顺序迁移包含隐藏项",PinnedApps::load()[2].gridColumn==2 && !PinnedApps::load()[2].enabled);
    check("迁移配置可重新读取",legacyFile.open(QIODevice::ReadOnly));
    check("迁移结果只需一次并落盘",QJsonDocument::fromJson(legacyFile.readAll()).object().value("pinnedApps").toArray()[0].toObject().contains("gridRow"));legacyFile.close();
    QFile oldDisplay(PinnedApps::configPath());check("旧显示字段夹具可读取",oldDisplay.open(QIODevice::ReadOnly));
    auto oldDisplayJson=QJsonDocument::fromJson(oldDisplay.readAll()).object();oldDisplay.close();
    oldDisplayJson.insert("showPinnedApps",false);writeFile(PinnedApps::configPath(),QJsonDocument(oldDisplayJson).toJson());
    input->clear();window.refreshLauncher();
    check("旧首页显示字段不再隐藏固定项",!grid->itemIds().isEmpty());
    input->setText(QStringLiteral("测试程序"));window.refreshLauncher();
    bool foundPinned=false;
    for (int i=0;i<window.resultCount();++i) foundPinned |= window.resultAt(i).kind==SearchResult::PinnedApp;
    check("旧首页显示字段不再隐藏固定搜索结果",foundPinned);
    PinnedApps::saveConfig(PinnedApps::config());
    const auto hidden=PinnedApps::config();
    input->setText("qq");window.show();window.refreshLauncher();spin();
    auto* results=window.findChild<QListWidget*>("list");
    check("长结果出现纵向滚动",results->count()>10 && results->verticalScrollBar()->maximum()>0);
    for (const QSize size : {QSize(1100,800),QSize(760,420),QSize(902,650)}) {
        window.resize(size);window.show();spin();
        check("搜索缩放不产生底部横向滚动",results->horizontalScrollBarPolicy()==Qt::ScrollBarAlwaysOff
            && !results->horizontalScrollBar()->isVisible() && results->horizontalScrollBar()->maximum()==0);
    }
    int resultIndex=-1;
    for (int i=0;i<window.resultCount();++i) if (window.resultAt(i).kind==SearchResult::App
        && window.canPinResult(window.resultAt(i))) {resultIndex=i;break;}
    check("应用结果提供固定入口",resultIndex>=0);
    if (resultIndex>=0) {
        const auto beforeCount=PinnedApps::load().size();
        results->setCurrentRow(resultIndex);results->scrollToItem(results->item(resultIndex));spin();
        const QRect rowRect=results->visualItemRect(results->item(resultIndex));
        const QPoint pinPoint(rowRect.right()-26,rowRect.center().y());
        mouse(*results->viewport(),QEvent::MouseButtonPress,pinPoint,Qt::LeftButton,Qt::LeftButton);
        mouse(*results->viewport(),QEvent::MouseButtonRelease,pinPoint,Qt::LeftButton,Qt::NoButton);
        check("搜索按钮可命中且保持查询",PinnedApps::load().size()==beforeCount+1 && input->text()=="qq");
        window.resultAction(resultIndex,true);
        check("再次固定不重复添加",PinnedApps::load().size()==beforeCount+1);
        auto duplicateConfig=PinnedApps::config();duplicateConfig.pinnedApps.back().enabled=false;PinnedApps::saveConfig(duplicateConfig);
        check("隐藏项也参与判重",PinnedApps::isPinned(window.resultPinnedApp(window.resultAt(resultIndex))));
        auto variant=window.resultPinnedApp(window.resultAt(resultIndex));variant.arguments="--other";
        check("不同启动参数可单独固定",!PinnedApps::isPinned(variant));
        SearchResult document;document.kind=SearchResult::File;document.target=root+"/note.txt";
        check("普通文档没有固定按钮",!window.canPinResult(document));
        int doubles=0;auto connection=QObject::connect(results,&QListWidget::itemDoubleClicked,&app,[&]{++doubles;});
        mouse(*results->viewport(),QEvent::MouseButtonDblClick,pinPoint,Qt::LeftButton,Qt::LeftButton);
        check("操作按钮双击不会激活结果行",doubles==0);QObject::disconnect(connection);
        duplicateConfig.pinnedApps.removeLast();PinnedApps::saveConfig(duplicateConfig);
    }
    input->clear();window.refreshLauncher();

    const QString scripts=root+QStringLiteral("/脚本 空格");QDir().mkpath(scripts);
    for (const QString& type : {QStringLiteral("bat"),QStringLiteral("cmd")}) {
        const QString path=scripts+"/test."+type;
        writeFile(path,QString("@echo off\r\necho %1> %1-marker.txt\r\n").arg(type).toUtf8());
        auto launch= PinnedApps::fromTarget(path,0);
        const bool started=PinnedApps::launch(launch,&error);
        check(type=="bat"?"含空格和中文路径 bat 可启动":"含空格和中文路径 cmd 可启动",
              started && waitForFile(scripts+"/"+type+"-marker.txt"));
    }
    PinnedApp missing=PinnedApps::fromTarget(scripts+"/missing.exe",0);
    check("失效路径保留配置并明确拒绝启动",!PinnedApps::targetExists(missing) && !PinnedApps::launch(missing,&error) && !error.isEmpty());
#ifdef Q_OS_WIN
    const QString systemCmd=qEnvironmentVariable("SystemRoot")+QStringLiteral("/System32/cmd.exe");
    auto exe=PinnedApps::fromTarget(systemCmd,0);
    exe.workingDirectory=scripts;exe.arguments=QStringLiteral("/d /c echo exe>exe-marker.txt");
    const bool exeStarted=PinnedApps::launch(exe,&error);
    check("exe 参数与工作目录正确传递",exeStarted && waitForFile(scripts+"/exe-marker.txt"));
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    IShellLinkW* link=nullptr;
    bool linkSaved=false;
    const QString shortcut=scripts+QStringLiteral("/快捷方式.lnk");
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link)))) {
        const auto target=QDir::toNativeSeparators(systemCmd).toStdWString();
        const auto cwd=QDir::toNativeSeparators(scripts).toStdWString();
        link->SetPath(target.c_str());link->SetWorkingDirectory(cwd.c_str());
        link->SetArguments(L"/d /c echo lnk>lnk-marker.txt");
        IPersistFile* file=nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
            linkSaved=SUCCEEDED(file->Save(QDir::toNativeSeparators(shortcut).toStdWString().c_str(),TRUE));
            file->Release();
        }
        link->Release();
    }
    const bool linkStarted=linkSaved && PinnedApps::launch(PinnedApps::fromTarget(shortcut,0),&error);
    check("lnk 通过 Windows 关联启动",linkStarted && waitForFile(scripts+"/lnk-marker.txt"));
    const auto resolved=Platform::shortcutInfo(shortcut);
    check("快捷方式参数工作目录可解析",linkSaved && resolved.arguments.contains("lnk-marker")
        && QDir::fromNativeSeparators(resolved.workingDirectory)==QDir::fromNativeSeparators(scripts));
    check("快捷方式定位真实程序",QDir::fromNativeSeparators(PinnedApps::revealTarget(PinnedApps::fromTarget(shortcut)))==QDir::fromNativeSeparators(systemCmd));
    {
        const auto saved=PinnedApps::config();
        QFile originalLink(shortcut);const bool readOriginal=originalLink.open(QIODevice::ReadOnly);const auto contents=originalLink.readAll();originalLink.close();
        auto linkApp=PinnedApps::fromTarget(shortcut);PinnedApps::add(linkApp);
        const bool renamed=PinnedApps::rename(linkApp.id,QStringLiteral("快捷方式面板别名"),&error);
        const auto after=Platform::shortcutInfo(shortcut);
        QFile checkLink(shortcut);const bool readResult=checkLink.open(QIODevice::ReadOnly);
        check("快捷方式别名不改源字节名称和启动信息",readOriginal && readResult && renamed && checkLink.readAll()==contents && QFileInfo::exists(shortcut)
            && after.target==resolved.target && after.arguments==resolved.arguments && after.workingDirectory==resolved.workingDirectory && after.iconPath==resolved.iconPath && after.iconIndex==resolved.iconIndex);
        checkLink.close();PinnedApps::saveConfig(saved);
    }
    bool iconReady=false;QImage icon;
    Utils::requestFileIcon(shortcut,96,&window,[&](const QImage& image){icon=image;iconReady=true;});
    QElapsedTimer iconWait;iconWait.start();while (!iconReady && iconWait.elapsed()<5000) spin(10);
    check("后台提取快捷方式高清图标",iconReady && !icon.isNull() && icon.width()>=48);
    check("Windows 图标保留透明通道",icon.hasAlphaChannel() && qAlpha(icon.pixel(0,0))<255);
    if (SUCCEEDED(initialized)) CoUninitialize();
#endif

    // 使用合成 PNG/GIF 验证公开 ZIP 接口，第三方素材不是测试前提。
    QImage frame(200,160,QImage::Format_ARGB32);frame.fill(QColor("#184878"));
    const QString imagePath=root+"/frame.png";frame.save(imagePath);
    QFile image(imagePath);check("合成图片可读取",image.open(QIODevice::ReadOnly));
    QMap<QString,QByteArray> files{{"theme.qss","QLineEdit#input { color: @text; }"},{"assets/frame.png",image.readAll()},
        {"assets/character.gif",QByteArray::fromBase64("R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7")}};
    QJsonObject manifest=basicManifest();
    manifest.insert("palette",QJsonObject{{"window","#09204a"},{"text","#ffffff"},{"editText","#b7f1ff"}});
    manifest.insert("surface",QJsonObject{{"canvasSize",QJsonArray{902,650}},{"frame","frame.png"},
        {"frameMargins",QJsonArray{20,20,20,20}},{"searchRect",QJsonArray{40,40,700,48}},
        {"contentRect",QJsonArray{40,110,822,450}},
        {"decorations",QJsonArray{QJsonObject{{"image","character.gif"},{"anchor","top-right"},
            {"offset",QJsonArray{-5,5}},{"size",QJsonArray{40,40}}},
            QJsonObject{{"image","character.gif"},{"anchor","bottom-left"},
                {"offset",QJsonArray{0,0}},{"size",QJsonArray{20,20}}}}}});
    const auto pack=[&](const QJsonObject& spec,const QMap<QString,QByteArray>& extras=QMap<QString,QByteArray>{}) {
        auto entries=files;entries.insert("theme.json",QJsonDocument(spec).toJson());
        for (auto it=extras.begin();it!=extras.end();++it)entries.insert(it.key(),it.value());
        const QString path=root+"/fixture.zip";writeZip(path,entries);return path;
    };
    Theme::setMode(Theme::Mode::Light);
    SettingsDialog settings;
    settings.ensurePolished();
    check("设置导航禁用横向滚动",settings.findChild<QListWidget*>("menuList")->horizontalScrollBarPolicy()==Qt::ScrollBarAlwaysOff);
    const auto settingsPalette=settings.palette();const QString settingsQss=settings.styleSheet();
    const auto appPalette=app.palette();const QString appQss=app.styleSheet();
    const QString settingsWindowColor=Theme::token("window");
    check("合法 ZIP 安装并立即应用",Theme::importPackage(pack(manifest),&error) && Theme::currentPackageId()=="test-skin");
    check("外部主题不改变设置和全局配色",Theme::current()==Theme::Mode::Light
        && Theme::token("window")==settingsWindowColor && settings.palette()==settingsPalette
        && settings.styleSheet()==settingsQss && app.palette()==appPalette && app.styleSheet()==appQss
        && Theme::launcherToken("window")=="#09204a");
    auto* skin=window.findChild<LauncherSkin*>("hotaruSkin");
    spin();
    check("外框和 GIF 实际加载",skin && skin->active() && skin->findChildren<QMovie*>().size()==2
        && skin->findChild<QMovie*>()->isValid());
    {
        QSettings prefs(root+"/config.ini",QSettings::IniFormat);
        input->clear();window.refreshLauncher();window.show();
        const QSize original=window.size();
        prefs.setValue("ui/favoritesOnEmpty",false);prefs.sync();window.refreshLauncher();spin();
        const int emptyHeight=skin->compactHeight();
        check("旧版外框包关闭网格仍显示皮肤",skin->isVisible() && !grid->isVisible()
            && window.height()==emptyHeight && window.height()<original.height());
        check("空紧凑外框不预留底框高度",emptyHeight==88);
        auto* bottomMovie=skin->findChildren<QMovie*>().last();
        check("空搜索状态底部装饰隐藏并暂停",bottomMovie->state()==QMovie::Paused
            && !movieDecorationVisible(window,bottomMovie));
        LauncherSkin frameOnly(nullptr);frameOnly.resize(original);frameOnly.setCompact(true);
        const QImage emptyFrame=renderFrame(frameOnly);
        check("空紧凑态仅绘制顶部且下方完全透明",qAlpha(emptyFrame.pixel(10,10))>0
            && transparentBelow(emptyFrame,88));
        frameOnly.setCompact(true,true);
        check("有搜索结果才绘制完整结果区底框",!transparentBelow(renderFrame(frameOnly),88));
        frameOnly.setCompact(true);
        frameOnly.reload();
        check("清空和重载后不残留结果底框",transparentBelow(renderFrame(frameOnly),88));
        frameOnly.setCompact(false);
        check("完整面板仍绘制原底框",!transparentBelow(renderFrame(frameOnly),88));
        const auto* compactGear=window.findChild<QToolButton*>("hotaruSettings");
        check("皮肤搜索框与设置按钮位于可见区且不重叠",window.rect().contains(compactGear->geometry())
            && window.rect().contains(input->mapTo(&window,input->rect().bottomRight()))
            && !QRect(input->mapTo(&window,QPoint()),input->size()).intersects(compactGear->geometry())
            && window.childAt(compactGear->geometry().center())==compactGear);
        input->setText("qq");window.refreshLauncher();
        check("紧凑搜索结果展开仍保留外框",skin->isVisible() && window.height()>emptyHeight
            && window.rect().contains(results->geometry()));
        check("合成主题搜索展开恢复底部GIF显示和播放",bottomMovie->state()==QMovie::Running
            && movieDecorationVisible(window,bottomMovie));
        input->clear();window.refreshLauncher();
        check("清空搜索收起结果但保留皮肤",skin->isVisible() && window.height()==emptyHeight);
        check("清空搜索再次隐藏暂停底部GIF",bottomMovie->state()==QMovie::Paused
            && !movieDecorationVisible(window,bottomMovie));
        skin->reload();window.refreshLauncher();
        check("紧凑状态重载主题保持皮肤布局",skin->isVisible() && window.height()==emptyHeight);
        input->setText("qq");window.refreshLauncher();skin->reload();
        bottomMovie=skin->findChildren<QMovie*>().last();
        check("搜索结果状态重载主题仍显示播放底部GIF",bottomMovie->state()==QMovie::Running
            && movieDecorationVisible(window,bottomMovie));
        input->clear();window.refreshLauncher();
        prefs.setValue("ui/favoritesOnEmpty",true);prefs.sync();window.refreshLauncher();
        check("恢复网格后原主题尺寸不变",skin->isVisible() && window.size()==original);
    }
    window.resize(902,650);QApplication::processEvents();
    check("设计坐标按基础尺寸还原",skin->searchRect()==QRect(40,40,700,48));
    window.resize(1100,800);QApplication::processEvents();
    check("外框缩放后内容区留在窗口内",window.rect().contains(skin->contentRect()));
    check("已安装用户主题保留选择",Theme::activatePackage("test-skin",&error) && PinnedApps::config().themeId=="test-skin");
    check("当前动画主题重复导入可替换",Theme::importPackage(pack(manifest),&error)
        && Theme::currentPackageId()=="test-skin" && skin->findChildren<QMovie*>().size()==2);
    check("替换主题也不改变设置配色",settings.styleSheet()==settingsQss && app.palette()==appPalette);
    check("切换基础主题释放旧动画",Theme::activatePackage("hotaru-grey",&error) && skin->findChildren<QMovie*>().isEmpty());
    Theme::activatePackage("test-skin",&error);
    check("当前第三方主题删除后恢复黑色",Theme::removePackage("test-skin",&error) && Theme::currentPackageId()=="hotaru-black");
    QJsonObject old=basicManifest();old.insert("id","qss-only");
    check("兼容仅 QSS 主题",Theme::importPackage(pack(old),&error) && !skin->active());
    check("第三方 QSS 只挂在启动器",app.styleSheet()==appQss && !window.styleSheet().isEmpty());
    Theme::activatePackage("hotaru-black",&error);
    QJsonObject v2=manifest;v2.insert("schemaVersion",2);
    auto v2surface=v2.value("surface").toObject();
    v2surface.insert("footerRect",QJsonArray{40,580,822,40});
    v2surface.insert("settingsRect",QJsonArray{822,580,40,40});v2.insert("surface",v2surface);
    QImage fullFrame(902,650,QImage::Format_ARGB32);fullFrame.fill(QColor("#184878"));
    QByteArray fullBytes;QBuffer buffer(&fullBytes);buffer.open(QIODevice::WriteOnly);fullFrame.save(&buffer,"PNG");
    const QMap<QString,QByteArray> fullResource{{"assets/frame.png",fullBytes}};
    check("v2 画布和完整资源可安装",Theme::importPackage(pack(v2,fullResource),&error));
    auto cropSurface=v2surface;
    cropSurface.insert("decorations",QJsonArray{QJsonObject{{"image","frame.png"},{"anchor","top-left"},
        {"offset",QJsonArray{1,1}},{"size",QJsonArray{20,20}},{"sourceRect",QJsonArray{20,20,20,20}}}});
    auto cropped=v2;cropped.insert("surface",cropSurface);
    check("sourceRect 按裁切区域验证比例",Theme::importPackage(pack(cropped,fullResource),&error)
        && Theme::currentPackage().surface.decorations[0].sourceRect==QRect(20,20,20,20));
    cropSurface.insert("decorations",QJsonArray{QJsonObject{{"image","frame.png"},{"anchor","top-left"},
        {"offset",QJsonArray{1,1}},{"size",QJsonArray{20,20}},{"sourceRect",QJsonArray{900,640,20,20}}}});
    cropped.insert("surface",cropSurface);
    check("拒绝越界源区域",!Theme::importPackage(pack(cropped,fullResource),&error));
    Theme::importPackage(pack(v2,fullResource),&error);
    for (const QSize size : {QSize(902,650),QSize(1100,800),QSize(1300,900)}) {
        window.resize(size);window.show();spin();
        auto* gear=window.findChild<QToolButton*>("hotaruSettings");
        const auto decor=skin->decorationRect(0);
        check("v2 缩放后设置和装饰保持安全区",skin->footerRect().contains(gear->geometry())
            && !skin->contentRect().intersects(gear->geometry()) && window.rect().contains(decor)
            && !decor.intersects(skin->searchRect()) && !decor.intersects(skin->contentRect())
            && !decor.intersects(skin->footerRect()));
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName()=="windows") {
            POINT pt{qRound(gear->geometry().center().x()*window.devicePixelRatioF()),qRound(gear->geometry().center().y()*window.devicePixelRatioF())};
            ClientToScreen(reinterpret_cast<HWND>(window.winId()),&pt);
            const auto hit=SendMessageW(reinterpret_cast<HWND>(window.winId()),WM_NCHITTEST,0,MAKELPARAM(pt.x,pt.y));
            check("缩放后的原生设置命中使用消息坐标",hit==HTCLIENT);
        }
#endif
    }
    auto badV2=v2;badV2.insert("schemaVersion",3);
    check("拒绝未知清单版本",!Theme::importPackage(pack(badV2,fullResource),&error));
    check("拒绝 v2 画布与原图尺寸不一致",!Theme::importPackage(pack(v2),&error));
    auto badSurface=v2surface;badSurface.remove("footerRect");badV2=v2;badV2.insert("surface",badSurface);
    check("拒绝 v2 缺少页脚",!Theme::importPackage(pack(badV2,fullResource),&error));
    badSurface=v2surface;badSurface.insert("settingsRect",QJsonArray{40,110,40,40});badV2=v2;badV2.insert("surface",badSurface);
    check("拒绝设置侵入内容区",!Theme::importPackage(pack(badV2,fullResource),&error));
    badSurface=v2surface;badSurface.insert("decorations",QJsonArray{QJsonObject{{"image","character.gif"},{"anchor","top-left"},
        {"reference","window"},{"offset",QJsonArray{40,40}},{"size",QJsonArray{40,40}}}});badV2=v2;badV2.insert("surface",badSurface);
    check("拒绝装饰覆盖搜索区",!Theme::importPackage(pack(badV2,fullResource),&error));
    auto overlapDecoration=QJsonObject{{"image","character.gif"},{"anchor","top-left"},
        {"offset",QJsonArray{40,110}},{"size",QJsonArray{40,40}}};
    auto overlapSurface=v2surface;overlapSurface.insert("decorations",QJsonArray{overlapDecoration});
    auto overlapTheme=v2;overlapTheme.insert("surface",overlapSurface);
    check("缺省禁止装饰覆盖内容区",!Theme::importPackage(pack(overlapTheme,fullResource),&error));
    overlapDecoration.insert("allowContentOverlap",true);
    overlapSurface.insert("decorations",QJsonArray{overlapDecoration});overlapTheme.insert("surface",overlapSurface);
    check("显式允许装饰覆盖内容区",Theme::importPackage(pack(overlapTheme,fullResource),&error)
        && Theme::currentPackage().surface.decorations[0].allowContentOverlap);
    window.resize(902,650);window.show();window.refreshLauncher();spin();
    auto* contentDecorations=window.findChild<QWidget*>("hotaruContentDecorations");
    check("内容装饰穿透至固定区",contentDecorations && contentDecorations->testAttribute(Qt::WA_TransparentForMouseEvents)
        && window.childAt(skin->decorationRect(0).center())==grid);
    overlapDecoration.insert("allowContentOverlap",QStringLiteral("true"));
    overlapSurface.insert("decorations",QJsonArray{overlapDecoration});overlapTheme.insert("surface",overlapSurface);
    check("覆盖开关必须为布尔值",!Theme::importPackage(pack(overlapTheme,fullResource),&error));
    overlapDecoration.insert("allowContentOverlap",true);overlapDecoration.insert("offset",QJsonArray{40,40});
    overlapSurface.insert("decorations",QJsonArray{overlapDecoration});overlapTheme.insert("surface",overlapSurface);
    check("允许覆盖内容仍不能覆盖搜索区",!Theme::importPackage(pack(overlapTheme,fullResource),&error));
    overlapDecoration.insert("offset",QJsonArray{40,580});
    overlapSurface.insert("decorations",QJsonArray{overlapDecoration});overlapTheme.insert("surface",overlapSurface);
    check("允许覆盖内容仍不能覆盖页脚",!Theme::importPackage(pack(overlapTheme,fullResource),&error));
    auto badResources=fullResource;badResources.insert("theme.qss","QLineEdit { min-width: 1600px; }");
    check("拒绝 v2 QSS 再次改变几何",!Theme::importPackage(pack(v2,badResources),&error));
    Theme::activatePackage("hotaru-black",&error);
    auto invalid=manifest;invalid.insert("minAppVersion","99.0.0");
    check("拒绝需要更高版本的主题",!Theme::importPackage(pack(invalid),&error));
    check("拒绝可执行资源",!Theme::importPackage(pack(manifest,{{"assets/run.exe","MZ"}}),&error));
    check("拒绝目录穿越",!Theme::importPackage(pack(manifest,{{"../escape.txt","unsafe"}}),&error)
        && !QFileInfo::exists(root+"/hotaru/themes/escape.txt"));
    check("拒绝绝对路径",!Theme::importPackage(pack(manifest,{{"D:/escape.txt","unsafe"}}),&error));
    check("拒绝包内折返路径",!Theme::importPackage(pack(manifest,{{"assets/../escape.png","unsafe"}}),&error));
    check("拒绝样式表外部路径",!Theme::importPackage(pack(manifest,{{"theme.qss","QWidget { image: url(https://example.invalid/a.png); }"}}),&error));
    auto surface=manifest.value("surface").toObject();surface.insert("frame","missing.png");invalid=manifest;invalid.insert("surface",surface);
    check("拒绝缺失图片",!Theme::importPackage(pack(invalid),&error));
    check("拒绝非法 JSON",!Theme::importPackage(pack(manifest,{{"theme.json","{"}}),&error));
    check("拒绝损坏 GIF",!Theme::importPackage(pack(manifest,{{"assets/character.gif","not an image"}}),&error));
    check("无效导入不改变当前主题",Theme::currentPackageId()=="hotaru-black");

    if (preview) {
        const QString shots=QDir::tempPath()+"/hotaru-previews";QDir().mkpath(shots);
        auto empty=PinnedApps::config();empty.pinnedApps.clear();empty.gridColumns=8;empty.gridRows=4;PinnedApps::saveConfig(empty);
        for (const QString& id : {QStringLiteral("hotaru-black"),QStringLiteral("hotaru-grey"),QStringLiteral("hotaru-white")}) {
            Theme::activatePackage(id,&error);window.resize(902,650);window.show();window.refreshLauncher();spin();
            window.grab().save(shots+"/"+id+".png");
        }
        if (!localPackage.isEmpty()) {
            check("本地蓝色包可通过统一接口安装",Theme::importPackage(localPackage,&error));
            window.resize(902,650);window.show();window.refreshLauncher();spin(900);
            window.grab().save(shots+"/blue-empty.png");
            {
                QSettings prefs(root+"/config.ini",QSettings::IniFormat);
                const QSize original=window.size();
                prefs.setValue("ui/favoritesOnEmpty",false);prefs.sync();window.refreshLauncher();spin();
                auto* decorations=window.findChild<QWidget*>("hotaruContentDecorations");
                check("蓝色皮肤关闭网格保留搜索外框和顶部动画",window.height()==skin->compactHeight()
                    && window.height()<original.height() && skin->isVisible() && decorations && decorations->isVisible()
                    && !grid->isVisible() && skin->findChildren<QMovie*>().size()==2
                    && skin->findChildren<QMovie*>()[0]->state()==QMovie::Running
                    && skin->findChildren<QMovie*>()[1]->state()==QMovie::Paused);
                check("紧凑蓝色顶部角色不裁切",window.rect().contains(skin->decorationRect(0)));
                check("蓝色空搜索条不包含中段和底框高度",window.height()==146);
                LauncherSkin frameOnly(nullptr);frameOnly.resize(original);frameOnly.setCompact(true);
                check("蓝色空搜索条下方及左右突出块完全透明",transparentBelow(renderFrame(frameOnly),146));
                window.grab().save(shots+"/blue-resident-off.png");
                input->setText("qq");window.refreshLauncher();spin();
                check("蓝色紧凑搜索结果与外框均可见",skin->isVisible() && results->isVisible()
                    && window.rect().contains(results->geometry()) && !grid->isVisible());
                auto* bottomMovie=skin->findChildren<QMovie*>().last();
                const QRect overlap=skin->decorationRect(1).intersected(results->geometry());
                check("蓝色搜索展开底部角色可见且鼠标穿透",movieDecorationVisible(window,bottomMovie)
                    && bottomMovie->state()==QMovie::Running && window.rect().contains(skin->decorationRect(1))
                    && !overlap.isEmpty() && window.childAt(overlap.center())==results->viewport());
                const int firstFrame=bottomMovie->currentFrameNumber();
                QElapsedTimer animationWait;animationWait.start();
                while (bottomMovie->currentFrameNumber()==firstFrame && animationWait.elapsed()<1500) spin(25);
                check("蓝色搜索结果中的底部GIF持续换帧",bottomMovie->currentFrameNumber()!=firstFrame);
                window.grab().save(shots+"/blue-resident-off-search.png");
                input->clear();window.refreshLauncher();spin();
                check("蓝色搜索清空后保留紧凑皮肤且暂停底部GIF",skin->isVisible() && window.height()==skin->compactHeight()
                    && bottomMovie->state()==QMovie::Paused && !movieDecorationVisible(window,bottomMovie));
                prefs.setValue("ui/favoritesOnEmpty",true);prefs.sync();window.refreshLauncher();spin();
                check("蓝色网格重新开启恢复外框与面板尺寸",skin->isVisible() && window.size()==original && grid->isVisible());
            }
            const auto movieCount=skin->findChildren<QMovie*>().size();
            check("蓝色包两组动画均运行",movieCount==2 && skin->findChildren<QMovie*>()[0]->state()==QMovie::Running
                && skin->findChildren<QMovie*>()[1]->state()==QMovie::Running);
            auto* blueGear=window.findChild<QToolButton*>("hotaruSettings");
            check("蓝色主题设置入口未被遮挡",window.childAt(blueGear->geometry().center())==blueGear);
            mouse(*blueGear,QEvent::MouseButtonPress,blueGear->rect().center(),Qt::LeftButton,Qt::LeftButton);
            mouse(*blueGear,QEvent::MouseButtonRelease,blueGear->rect().center(),Qt::LeftButton,Qt::NoButton);spin();
            QPointer<SettingsDialog> blueSettings;
            for (auto* widget:QApplication::topLevelWidgets()) if (auto* dialog=qobject_cast<SettingsDialog*>(widget);dialog && dialog->isVisible()) blueSettings=dialog;
            check("蓝色主题齿轮实际打开设置并可重开",blueSettings && blueSettings->findChild<QSpinBox*>("gridRows"));
            if (blueSettings) blueSettings->close();spin();window.show();
            auto demo=PinnedApps::config();demo.pinnedApps=hidden.pinnedApps;
            const QStringList icons={qEnvironmentVariable("SystemRoot")+"/explorer.exe",qEnvironmentVariable("SystemRoot")+"/System32/cmd.exe",
                qEnvironmentVariable("SystemRoot")+"/System32/Taskmgr.exe"};
            for (int i=0;i<demo.pinnedApps.size();++i) demo.pinnedApps[i].iconPath=icons[i%icons.size()];
            PinnedApps::saveConfig(demo);window.refreshLauncher();spin(650);
            for (auto* movie:skin->findChildren<QMovie*>()) {movie->setPaused(true);movie->jumpToFrame(movie->frameCount()>12?12:0);}
            window.grab().save(shots+"/blue-demo.png");
            check("蓝色左下裁切区域固定",Theme::currentPackage().surface.decorations[1].sourceRect==QRect(37,1,91,149)
                && skin->decorationRect(1).left()==23);
            const auto beforeFullGrid=PinnedApps::config();
            auto fullGrid=beforeFullGrid;fullGrid.pinnedApps.clear();
            for (int i=0;i<32;++i) {
                auto item=demo.pinnedApps.front();item.id=QStringLiteral("full-%1").arg(i);
                item.title=QStringLiteral("格位 %1").arg(i+1);item.enabled=true;
                item.gridRow=i/8;item.gridColumn=i%8;item.iconPath=icons[i%icons.size()];
                fullGrid.pinnedApps << item;
            }
            check("完整四行八列预览配置有效",PinnedApps::saveConfig(fullGrid,&error));
            window.refreshLauncher();grid->setPage(0);spin(650);
            for (const QSize size : {QSize(902,650),QSize(980,650),QSize(1103,797)}) {
                window.resize(size);spin();
                const QRect content=skin->contentRect();
                const QRect gridBounds(grid->mapTo(&window,grid->cellRect(0,0).topLeft()),
                    grid->mapTo(&window,grid->cellRect(3,7).bottomRight()));
                check("蓝色网格整体居中且左右边距相等",qAbs(content.left()-(window.width()-1-content.right()))<=1
                    && qAbs(gridBounds.left()-(window.width()-1-gridBounds.right()))<=1);
                const QRect overlap=skin->decorationRect(1).intersected(gridBounds);
                check("满格时角色覆盖但不拦截格位点击",grid->itemIds().size()==32 && !overlap.isEmpty()
                    && window.childAt(overlap.center())==grid && !skin->footerRect().intersects(gridBounds));
                window.grab().save(shots+QStringLiteral("/blue-full-%1x%2.png").arg(size.width()).arg(size.height()));
            }
            PinnedApps::saveConfig(beforeFullGrid);window.resize(980,650);window.refreshLauncher();spin();
            window.grab().save(shots+"/blue-demo-980.png");
            for (auto* movie:skin->findChildren<QMovie*>()) movie->setPaused(false);
            // 保存真实绘制过程，检查鼠标跟随、虚线空格和松手吸附。
            const QPoint a=grid->itemRect(0).center(), b=grid->cellRect(1,3).center();
            mouse(*grid,QEvent::MouseButtonPress,a,Qt::LeftButton,Qt::LeftButton);
            mouse(*grid,QEvent::MouseMove,b+QPoint(18,12),Qt::NoButton,Qt::LeftButton);
            spin(45); window.grab().save(shots+"/blue-drag-moving.png");
            mouse(*grid,QEvent::MouseButtonRelease,b+QPoint(18,12),Qt::LeftButton,Qt::NoButton);
            spin(45); window.grab().save(shots+"/blue-drag-settling.png");
            spin(); window.grab().save(shots+"/blue-drag-finished.png");
            const auto beforeGroupShot=PinnedApps::config();
            if (beforeGroupShot.pinnedApps.size()>=2) {
                const auto first=beforeGroupShot.pinnedApps[0].id,second=beforeGroupShot.pinnedApps[1].id;
                PinnedApps::merge(first,second,&error);
                const auto cfg=PinnedApps::config();
                if (!cfg.groups.isEmpty()) {
                    for (int i=0;i<10;++i) {auto child=demo.pinnedApps.front();child.id=QStringLiteral("preview-child-%1").arg(i);child.title=QStringLiteral("分组软件 %1").arg(i+1);child.groupId=cfg.groups[0].id;child.gridRow=(i+2)/3;child.gridColumn=(i+2)%3;
                        auto update=PinnedApps::config();update.pinnedApps.append(child);PinnedApps::saveConfig(update);}
                    window.refreshLauncher();grid->setPage(0);spin(350);window.grab().save(shots+"/blue-group-cover.png");
                    const auto point=grid->cellRect(cfg.groups[0].gridRow,cfg.groups[0].gridColumn).center();
                    mouse(*grid,QEvent::MouseButtonPress,point,Qt::LeftButton,Qt::LeftButton);mouse(*grid,QEvent::MouseButtonRelease,point,Qt::LeftButton,Qt::NoButton);spin(350);
                    window.grab().save(shots+"/blue-group-page1.png");
                    if (auto* childGrid=window.findChild<LauncherGrid*>("hotaruGroupGrid")) {childGrid->setPage(1);spin(250);window.grab().save(shots+"/blue-group-page2.png");}
                    QKeyEvent dismiss(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(grid,&dismiss);
                }
            }
            PinnedApps::saveConfig(beforeGroupShot);window.refreshLauncher();
            const auto beforePageShot=PinnedApps::config();
            PinnedApps::move(beforePageShot.pinnedApps[0].id,4,2,&error);window.refreshLauncher();grid->setPage(1);spin();
            window.grab().save(shots+"/blue-page-2.png");
            PinnedApps::saveConfig(beforePageShot);window.refreshLauncher();grid->setPage(0);
            window.resize(1100,800);spin();window.grab().save(shots+"/blue-large.png");
            input->setText("qq");window.resize(902,650);window.show();window.refreshLauncher();spin();
            // 原始动画含缩回边框的帧，预览选择展开帧，便于人工检查角色与结果区的间距。
            for (auto* movie : skin->findChildren<QMovie*>()) {
                movie->setPaused(true);
                movie->jumpToFrame(movie->frameCount()>12 ? 12 : 0);
            }
            window.grab().save(shots+"/blue-search.png");
            for (auto* movie : skin->findChildren<QMovie*>()) movie->setPaused(false);
            check("蓝色搜索无横向滚动",results->verticalScrollBar()->maximum()>0
                && results->horizontalScrollBar()->maximum()==0 && !results->horizontalScrollBar()->isVisible());
            settings.openAt(2,1);settings.show();spin();
            settings.grab().save(shots+"/settings-with-blue.png");
            Theme::activatePackage("hotaru-white",&error);
            spin();settings.grab().save(shots+"/settings-with-white.png");
            check("蓝色切白色后设置页背景保持",settings.styleSheet()==settingsQss && app.palette()==appPalette);
            settings.hide();input->clear();window.refreshLauncher();
            check("真实动画主题切换后全部释放",skin->findChildren<QMovie*>().isEmpty());
            check("本地蓝色主题可删除",Theme::removePackage("huochat-blue-local",&error));
        }
        std::printf("previews: %s\n",shots.toUtf8().constData());
    }
    window.show();
    QKeyEvent closeGrid(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
    QApplication::sendEvent(grid,&closeGrid);
    check("固定区获得焦点后 Esc 仍可隐藏窗口",!window.isVisible());
    std::printf("hotarutest: 失败 %d 项\n",failures);
    return failures?1:0;
}

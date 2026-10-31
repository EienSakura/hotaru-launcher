#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QPoint>

class QWidget;

// 固定在萤启动器首页上的软件或脚本。
struct PinnedApp {
    QString id;
    QString title;
    QString targetType;
    QString targetPath;
    QString arguments;
    QString workingDirectory;
    QString iconPath;
    QString category;
    QString groupId;
    bool enabled = true;
    int sortOrder = 0;
    int gridRow = -1;
    int gridColumn = -1;
};

namespace PinnedApps {

struct Group {
    QString id;
    QString title;
    int gridRow = 0;
    int gridColumn = 0;
};

struct LauncherConfig {
    QString themeId = QStringLiteral("hotaru-black");
    int gridColumns = 8;
    int gridRows = 4;
    bool rememberPage = false;
    int lastPage = 0;
    int pages = 1;
    QVector<PinnedApp> pinnedApps;
    QVector<Group> groups;
};

QString configPath();
LauncherConfig config();
bool saveConfig(const LauncherConfig& config, QString* error = nullptr);
bool importConfig(const QString& path, QString* error = nullptr);
bool exportConfig(const QString& path, QString* error = nullptr);
QVector<PinnedApp> load();
bool save(const QVector<PinnedApp>& apps, QString* error = nullptr);
// 所有入口共用格位校验；隐藏项同样占用位置。
QPoint firstFreeCell(const LauncherConfig& config);
// gridRow 为跨页的全局行号；调整每页行数不移动或丢失任何固定项。
int pageCount(const LauncherConfig& config);
bool move(const QString& id, int row, int column, QString* error = nullptr);
bool moveTo(const QString& id, const QString& groupId, int row, int column, QString* error = nullptr);
bool rename(const QString& id, const QString& title, QString* error = nullptr);
bool remove(const QString& id, QString* error = nullptr);
bool merge(const QString& sourceId, const QString& targetId, QString* error = nullptr);
bool dissolve(const QString& groupId, QString* error = nullptr);
bool addPage(QString* error = nullptr);
bool deletePage(int page, bool migrate, QString* error = nullptr);
bool pageOccupied(const LauncherConfig& config, int page);
bool add(PinnedApp app, int row = -1, int column = -1, QString* error = nullptr);
// 创建固定面板内部的图标分组；它不是磁盘文件夹，不包含真实路径。
bool createGroup(const QString& title, int row, int column, QString* error = nullptr);
bool isPinned(const PinnedApp& app);
bool supportsTarget(const QString& path);
bool editProperties(const QString& id, QWidget* parent, QString* error = nullptr);
bool launchAdmin(const PinnedApp& app, QString* error = nullptr);
bool canRunAdmin(const PinnedApp& app);
QString revealTarget(const PinnedApp& app);

PinnedApp fromTarget(const QString& path, int order = 0);
QString targetType(const PinnedApp& app);
bool targetExists(const PinnedApp& app);
bool launch(const PinnedApp& app, QString* error = nullptr);

} // namespace PinnedApps

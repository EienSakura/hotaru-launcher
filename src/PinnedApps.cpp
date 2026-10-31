#include "PinnedApps.h"

#include "Utils.h"
#include "Theme.h"
#include "platform/Launch.h"
#include "platform/FileMeta.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QMessageBox>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QStringList>
#include <QDateTime>
#include <QUuid>
#include <QSet>

#include <algorithm>

namespace {

QString str(const QJsonObject& o, const char* key)
{
    return o.value(QLatin1String(key)).toString();
}

QString deriveTargetType(const PinnedApp& app)
{
    if (QFileInfo(app.targetPath).isDir()) return QStringLiteral("folder");
    const QString ext = QFileInfo(app.targetPath).suffix().toLower();
    if (ext == QLatin1String("lnk"))
        return QStringLiteral("lnk");
    if (ext == QLatin1String("bat"))
        return QStringLiteral("bat");
    if (ext == QLatin1String("cmd"))
        return QStringLiteral("cmd");
    if (ext == QLatin1String("exe"))
        return QStringLiteral("exe");
    return ext.isEmpty() ? QStringLiteral("file") : ext;
}

QJsonObject toJson(const PinnedApp& app)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), app.id);
    o.insert(QStringLiteral("title"), app.title);
    o.insert(QStringLiteral("targetType"), app.targetType);
    o.insert(QStringLiteral("targetPath"), app.targetPath);
    o.insert(QStringLiteral("arguments"), app.arguments);
    o.insert(QStringLiteral("workingDirectory"), app.workingDirectory);
    o.insert(QStringLiteral("iconPath"), app.iconPath);
    o.insert(QStringLiteral("category"), app.category);
    o.insert(QStringLiteral("groupId"), app.groupId);
    o.insert(QStringLiteral("enabled"), app.enabled);
    o.insert(QStringLiteral("sortOrder"), app.sortOrder);
    o.insert(QStringLiteral("gridRow"), app.gridRow);
    o.insert(QStringLiteral("gridColumn"), app.gridColumn);
    return o;
}

PinnedApp fromJson(const QJsonObject& o, int fallbackOrder)
{
    PinnedApp app;
    app.id = str(o, "id");
    if (app.id.isEmpty())
        app.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    app.title = str(o, "title");
    app.targetType = str(o, "targetType");
    app.targetPath = str(o, "targetPath");
    app.arguments = str(o, "arguments");
    app.workingDirectory = str(o, "workingDirectory");
    app.iconPath = str(o, "iconPath");
    app.category = str(o, "category");
    app.groupId = str(o, "groupId");
    app.enabled = o.value(QStringLiteral("enabled")).toBool(true);
    app.sortOrder = o.value(QStringLiteral("sortOrder")).toInt(fallbackOrder);
    app.gridRow = o.value(QStringLiteral("gridRow")).toInt(-1);
    app.gridColumn = o.value(QStringLiteral("gridColumn")).toInt(-1);
    if (app.title.isEmpty())
        app.title = QFileInfo(app.targetPath).completeBaseName();
    if (app.targetType.isEmpty())
        app.targetType = deriveTargetType(app);
    return app;
}

QVector<PinnedApp> appsFromJson(const QJsonArray& array)
{
    QVector<PinnedApp> out;
    out.reserve(array.size());
    for (int i = 0; i < array.size(); ++i)
        if (array[i].isObject())
            out.append(fromJson(array[i].toObject(), i));
    std::sort(out.begin(), out.end(), [](const PinnedApp& a, const PinnedApp& b) {
        return a.sortOrder < b.sortOrder;
    });
    return out;
}

QJsonArray appsToJson(const QVector<PinnedApp>& apps)
{
    QJsonArray array;
    for (int i = 0; i < apps.size(); ++i) {
        PinnedApp copy = apps[i];
        copy.sortOrder = i;
        array.append(toJson(copy));
    }
    return array;
}

QJsonObject configToJson(const PinnedApps::LauncherConfig& cfg)
{
    QJsonObject root;
    root.insert(QStringLiteral("themeId"), cfg.themeId.isEmpty()
        ? QStringLiteral("hotaru-black") : cfg.themeId);
    root.insert(QStringLiteral("gridColumns"), qBound(4, cfg.gridColumns, 8));
    root.insert(QStringLiteral("gridRows"), cfg.gridRows);
    root.insert(QStringLiteral("rememberPage"), cfg.rememberPage);
    root.insert(QStringLiteral("lastPage"), cfg.lastPage);
    root.insert(QStringLiteral("pages"), cfg.pages);
    root.insert(QStringLiteral("layoutVersion"), 2);
    QJsonArray groups;
    for (const auto& g : cfg.groups)
        groups.append(QJsonObject{{"id",g.id},{"title",g.title},{"gridRow",g.gridRow},{"gridColumn",g.gridColumn}});
    root.insert(QStringLiteral("groups"), groups);
    root.insert(QStringLiteral("pinnedApps"), appsToJson(cfg.pinnedApps));
    return root;
}

bool normalize(PinnedApps::LauncherConfig& cfg, QString* error)
{
    const auto fail = [&](const QString& message) { if (error) *error=message; return false; };
    if (cfg.gridColumns<4 || cfg.gridColumns>8) return fail(QStringLiteral("网格列数必须为 4 至 8"));
    if (cfg.gridRows<2 || cfg.gridRows>6) return fail(QStringLiteral("每页行数必须为 2 至 6"));
    QSet<QString> ids;
    if (cfg.pages<1 || cfg.pages>9999/cfg.gridRows+1) return fail(QStringLiteral("页面数量超出范围"));
    QSet<QString> groupIds;
    QSet<QString> cells;
    const auto key=[](const QString& group,int row,int column) {return group+":"+QString::number(row)+":"+QString::number(column);};
    for (const auto& g:cfg.groups) {
        if (g.id.isEmpty() || ids.contains(g.id) || g.title.trimmed().isEmpty()) return fail(QStringLiteral("分组 ID 重复或名称为空"));
        if (g.gridRow<0 || g.gridRow>9999 || g.gridColumn<0 || g.gridColumn>=cfg.gridColumns) return fail(QStringLiteral("分组格位越界，请先移动右侧分组"));
        const auto cell=key({},g.gridRow,g.gridColumn);
        if (cells.contains(cell)) return fail(QStringLiteral("分组格位已被占用"));
        ids.insert(g.id);groupIds.insert(g.id);cells.insert(cell);
    }
    for (const auto& app : cfg.pinnedApps) {
        if (app.id.isEmpty() || ids.contains(app.id)) return fail(QStringLiteral("固定项 ID 为空或重复"));
        ids.insert(app.id);
        if (!app.groupId.isEmpty() && !groupIds.contains(app.groupId)) return fail(QStringLiteral("固定项所属分组不存在"));
        if (!QStringList{"exe","lnk","bat","cmd","folder"}.contains(PinnedApps::targetType(app)))
            return fail(QStringLiteral("固定项仅支持程序、快捷方式、脚本和文件夹"));
        if (app.gridRow==-1 && app.gridColumn==-1) continue;
        if (app.gridRow<0 || app.gridRow>9999 || app.gridColumn<0 || app.gridColumn>=(app.groupId.isEmpty()?cfg.gridColumns:3))
            return fail(QStringLiteral("格位超出范围；减少列数前请先移动右侧固定项（含隐藏项）"));
        const auto cell=key(app.groupId,app.gridRow,app.gridColumn);
        if (cells.contains(cell)) return fail(QStringLiteral("目标格位已被占用（含隐藏项），请先选择空格"));
        cells.insert(cell);
    }
    // 只为没有坐标的旧配置分配格位，已有坐标永不压紧。
    for (auto& app : cfg.pinnedApps) if (app.gridRow==-1 && app.gridColumn==-1) {
        int index=0;
        const int columns=app.groupId.isEmpty()?cfg.gridColumns:3;
        while (cells.contains(key(app.groupId,index/columns,index%columns))) ++index;
        app.gridRow=index/columns; app.gridColumn=index%columns;
        cells.insert(key(app.groupId,app.gridRow,app.gridColumn));
    }
    cfg.pages=PinnedApps::pageCount(cfg);
    cfg.lastPage=qBound(0,cfg.lastPage,PinnedApps::pageCount(cfg)-1);
    return true;
}

bool configFromJson(const QJsonObject& o, PinnedApps::LauncherConfig& cfg, QString* error)
{
    const QString themeId = o.value(QStringLiteral("themeId")).toString();
    if (!themeId.isEmpty())
        cfg.themeId = themeId;
    if (o.contains(QStringLiteral("gridColumns")))
        cfg.gridColumns = qBound(4, o.value(QStringLiteral("gridColumns")).toInt(8), 8);
    cfg.gridRows=qBound(2,o.value("gridRows").toInt(4),6);
    cfg.rememberPage=o.value("rememberPage").toBool(false);
    cfg.lastPage=o.value("lastPage").toInt(0);
    cfg.pages=o.value("pages").toInt(1);
    if (o.contains("groups") && !o.value("groups").isArray()) {if (error) *error=QStringLiteral("groups 必须是数组");return false;}
    for (const auto& value:o.value("groups").toArray()) {
        if (!value.isObject()) {if (error) *error=QStringLiteral("分组格式无效");return false;}
        const auto g=value.toObject();
        cfg.groups.append({g.value("id").toString(),g.value("title").toString(),g.value("gridRow").toInt(-1),g.value("gridColumn").toInt(-1)});
    }
    if (o.contains(QStringLiteral("pinnedApps"))
        && !o.value(QStringLiteral("pinnedApps")).isArray()) {
        if (error)
            *error = QStringLiteral("pinnedApps 必须是数组");
        return false;
    }
    if (o.value(QStringLiteral("pinnedApps")).isArray()) {
        cfg.pinnedApps = appsFromJson(o.value(QStringLiteral("pinnedApps")).toArray());
        for (const PinnedApp& app : cfg.pinnedApps) {
            const QString type = app.targetType.isEmpty() ? deriveTargetType(app) : app.targetType;
            if (type != QLatin1String("exe") && type != QLatin1String("lnk")
                && type != QLatin1String("bat") && type != QLatin1String("cmd") && type != QLatin1String("folder")) {
                if (error)
                    *error = QStringLiteral("固定软件包含不支持的启动类型：%1").arg(type);
                return false;
            }
        }
    }
    return normalize(cfg,error);
}

} // namespace

namespace PinnedApps {

int pageCount(const LauncherConfig& cfg)
{
    int pages=qMax(1,cfg.pages);
    for (const auto& item:cfg.pinnedApps) if (item.groupId.isEmpty()) pages=qMax(pages,item.gridRow/qBound(2,cfg.gridRows,6)+1);
    for (const auto& group:cfg.groups) pages=qMax(pages,group.gridRow/qBound(2,cfg.gridRows,6)+1);
    return pages;
}

QPoint firstFreeCell(const LauncherConfig& cfg)
{
    QSet<qint64> occupied;
    for (const auto& app : cfg.pinnedApps) if (app.groupId.isEmpty()) occupied.insert(qint64(app.gridRow)*8+app.gridColumn);
    for (const auto& g:cfg.groups) occupied.insert(qint64(g.gridRow)*8+g.gridColumn);
    for (int row=0;row<10000;++row) for (int col=0;col<cfg.gridColumns;++col)
        if (!occupied.contains(qint64(row)*8+col)) return {col,row};
    return {-1,-1};
}

bool move(const QString& id, int row, int column, QString* error)
{
    const auto cfg=config();
    for (const auto& app:cfg.pinnedApps) if (app.id==id) return moveTo(id,app.groupId,row,column,error);
    return moveTo(id,{},row,column,error);
}

bool moveTo(const QString& id,const QString& groupId,int row,int column,QString* error)
{
    auto cfg=config();
    for (auto& app : cfg.pinnedApps) if (app.id==id) {
        const QString oldGroup = app.groupId;
        app.groupId=groupId;app.gridRow=row; app.gridColumn=column;
        if (!oldGroup.isEmpty() && oldGroup != groupId
            && std::none_of(cfg.pinnedApps.cbegin(), cfg.pinnedApps.cend(),
                            [&](const auto& child) { return child.groupId == oldGroup; })) {
            cfg.groups.erase(std::remove_if(cfg.groups.begin(), cfg.groups.end(),
                                             [&](const auto& group) { return group.id == oldGroup; }),
                                             cfg.groups.end());
        }
        return saveConfig(cfg,error);
    }
    for (auto& group:cfg.groups) if (group.id==id && groupId.isEmpty()) {
        group.gridRow=row;group.gridColumn=column;return saveConfig(cfg,error);
    }
    if (error) *error=QStringLiteral("固定项已经不存在，请刷新后重试");
    return false;
}

bool rename(const QString& id,const QString& title,QString* error)
{
    const auto name=title.trimmed();
    if (name.isEmpty()) {if (error) *error=QStringLiteral("显示名称不能为空");return false;}
    auto cfg=config();
    for (auto& app:cfg.pinnedApps) if (app.id==id) {app.title=name;return saveConfig(cfg,error);}
    for (auto& g:cfg.groups) if (g.id==id) {g.title=name;return saveConfig(cfg,error);}
    if (error) *error=QStringLiteral("固定项已不存在");return false;
}

bool remove(const QString& id,QString* error)
{
    auto cfg=config();
    const auto oldSize=cfg.pinnedApps.size();
    cfg.pinnedApps.erase(std::remove_if(cfg.pinnedApps.begin(),cfg.pinnedApps.end(),[&](const auto& a){return a.id==id;}),cfg.pinnedApps.end());
    if (cfg.pinnedApps.size()==oldSize) {if (error) *error=QStringLiteral("固定项已不存在");return false;}
    QSet<QString> usedGroups;
    for (const auto& app : cfg.pinnedApps)
        if (!app.groupId.isEmpty())
            usedGroups.insert(app.groupId);
    cfg.groups.erase(std::remove_if(cfg.groups.begin(), cfg.groups.end(),
                                    [&](const auto& group) { return !usedGroups.contains(group.id); }),
                     cfg.groups.end());
    return saveConfig(cfg,error);
}

bool addPage(QString* error)
{
    auto cfg=config();cfg.pages=pageCount(cfg)+1;
    return saveConfig(cfg,error);
}

bool pageOccupied(const LauncherConfig& cfg,int page)
{
    for (const auto& a:cfg.pinnedApps) if (a.groupId.isEmpty() && a.gridRow/cfg.gridRows==page) return true;
    for (const auto& g:cfg.groups) if (g.gridRow/cfg.gridRows==page) return true;
    return false;
}

bool deletePage(int page,bool migrate,QString* error)
{
    auto cfg=config();const int count=pageCount(cfg);
    if (count<=1 || page<0 || page>=count) {if (error) *error=QStringLiteral("至少保留一页");return false;}
    struct Entry {QString id;int row,column;bool group;};
    QVector<Entry> entries;
    for (const auto& a:cfg.pinnedApps) if (a.groupId.isEmpty() && a.gridRow/cfg.gridRows==page) entries.append({a.id,a.gridRow,a.gridColumn,false});
    for (const auto& g:cfg.groups) if (g.gridRow/cfg.gridRows==page) entries.append({g.id,g.gridRow,g.gridColumn,true});
    std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.row==b.row?a.column<b.column:a.row<b.row;});
    if (migrate) {
        QSet<qint64> used;
        for (const auto& a:cfg.pinnedApps) if (a.groupId.isEmpty()) used.insert(qint64(a.gridRow)*8+a.gridColumn);
        for (const auto& g:cfg.groups) used.insert(qint64(g.gridRow)*8+g.gridColumn);
        QVector<QPoint> free;
        for (int row=0;row<count*cfg.gridRows;++row) if (row/cfg.gridRows!=page)
            for (int col=0;col<cfg.gridColumns;++col) if (!used.contains(qint64(row)*8+col)) free.append({col,row});
        if (free.size()<entries.size()) {if (error) *error=QStringLiteral("其他现有页面的空格不足");return false;}
        for (int i=0;i<entries.size();++i) {
            if (entries[i].group) {for (auto& g:cfg.groups) if (g.id==entries[i].id) {g.gridRow=free[i].y();g.gridColumn=free[i].x();}}
            else {for (auto& a:cfg.pinnedApps) if (a.id==entries[i].id) {a.gridRow=free[i].y();a.gridColumn=free[i].x();}}
        }
    } else {
        QSet<QString> removedGroups;
        for (const auto& e:entries) if (e.group) removedGroups.insert(e.id);
        cfg.pinnedApps.erase(std::remove_if(cfg.pinnedApps.begin(),cfg.pinnedApps.end(),[&](const auto& a) {
            return removedGroups.contains(a.groupId) || (a.groupId.isEmpty() && a.gridRow/cfg.gridRows==page);
        }),cfg.pinnedApps.end());
        cfg.groups.erase(std::remove_if(cfg.groups.begin(),cfg.groups.end(),[&](const auto& g){return removedGroups.contains(g.id);}),cfg.groups.end());
    }
    for (auto& a:cfg.pinnedApps) if (a.groupId.isEmpty() && a.gridRow/cfg.gridRows>page) a.gridRow-=cfg.gridRows;
    for (auto& g:cfg.groups) if (g.gridRow/cfg.gridRows>page) g.gridRow-=cfg.gridRows;
    cfg.pages=count-1;
    cfg.lastPage=qBound(0,cfg.lastPage-(cfg.lastPage>page?1:0),cfg.pages-1);
    return saveConfig(cfg,error);
}

bool merge(const QString& sourceId,const QString& targetId,QString* error)
{
    auto cfg=config();
    auto source=std::find_if(cfg.pinnedApps.begin(),cfg.pinnedApps.end(),[&](const auto& a){return a.id==sourceId;});
    if (source==cfg.pinnedApps.end() || sourceId==targetId) {if (error) *error=QStringLiteral("分组不能嵌套或合并到自身");return false;}
    QString groupId;
    for (const auto& g:cfg.groups) if (g.id==targetId) groupId=g.id;
    if (groupId.isEmpty()) {
        auto target=std::find_if(cfg.pinnedApps.begin(),cfg.pinnedApps.end(),[&](const auto& a){return a.id==targetId;});
        if (target==cfg.pinnedApps.end() || !target->groupId.isEmpty() || !target->enabled) {if (error) *error=QStringLiteral("合并目标无效");return false;}
        groupId=QUuid::createUuid().toString(QUuid::WithoutBraces);
        cfg.groups.append({groupId,QStringLiteral("新建文件夹"),target->gridRow,target->gridColumn});
        target->groupId=groupId;target->gridRow=0;target->gridColumn=0;
    }
    if (source->groupId==groupId) {if (error) *error=QStringLiteral("图标已在此分组内");return false;}
    QSet<int> used;
    for (const auto& a:cfg.pinnedApps) if (a.groupId==groupId) used.insert(a.gridRow*3+a.gridColumn);
    int cell=0;while (used.contains(cell)) ++cell;
    source->groupId=groupId;source->gridRow=cell/3;source->gridColumn=cell%3;
    return saveConfig(cfg,error);
}

bool dissolve(const QString& groupId,QString* error)
{
    auto cfg=config();const int count=pageCount(cfg);
    if (std::none_of(cfg.groups.cbegin(),cfg.groups.cend(),[&](const auto& g){return g.id==groupId;})) {
        if (error) *error=QStringLiteral("分组已不存在");return false;
    }
    cfg.groups.erase(std::remove_if(cfg.groups.begin(),cfg.groups.end(),[&](const auto& g){return g.id==groupId;}),cfg.groups.end());
    QVector<int> children;
    for (int i=0;i<cfg.pinnedApps.size();++i) if (cfg.pinnedApps[i].groupId==groupId) children.append(i);
    std::sort(children.begin(),children.end(),[&](int a,int b) {
        const auto& x=cfg.pinnedApps[a];const auto& y=cfg.pinnedApps[b];return x.gridRow==y.gridRow?x.gridColumn<y.gridColumn:x.gridRow<y.gridRow;
    });
    for (const int i:children) {
        const auto cell=firstFreeCell(cfg);
        if (cell.y()<0 || cell.y()>=count*cfg.gridRows) {if (error) *error=QStringLiteral("现有页面空格不足，分组已保留");return false;}
        auto& a=cfg.pinnedApps[i];a.groupId.clear();a.gridRow=cell.y();a.gridColumn=cell.x();
    }
    return saveConfig(cfg,error);
}

static QString normalizedPath(const QString& path)
{
    if (path.isEmpty()) return {};
    auto result=QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath()));
#ifdef Q_OS_WIN
    result=result.toCaseFolded();
#endif
    return result;
}

bool isPinned(const PinnedApp& app)
{
    for (const auto& existing : load())
        if (normalizedPath(existing.targetPath)==normalizedPath(app.targetPath)
            && existing.arguments==app.arguments
            && normalizedPath(existing.workingDirectory)==normalizedPath(app.workingDirectory)) return true;
    return false;
}

bool supportsTarget(const QString& path)
{
    return QFileInfo(path).isDir() || QStringList{"exe","lnk","bat","cmd"}.contains(QFileInfo(path).suffix().toLower());
}

bool add(PinnedApp app, int row, int column, QString* error)
{
    if (!supportsTarget(app.targetPath)) {
        if (error) *error=QStringLiteral("只支持程序、快捷方式、脚本和文件夹");
        return false;
    }
    if (isPinned(app)) { if (error) *error=QStringLiteral("该启动项已固定（可能已隐藏）"); return false; }
    auto cfg=config();
    const QPoint cell=(row<0 && column<0)?firstFreeCell(cfg):QPoint(column,row);
    app.gridRow=cell.y(); app.gridColumn=cell.x();
    app.sortOrder=cfg.pinnedApps.size();
    cfg.pinnedApps << app;
    return saveConfig(cfg,error);
}

bool createGroup(const QString& title, int row, int column, QString* error)
{
    const QString name = title.trimmed();
    if (name.isEmpty()) {
        if (error) *error = QStringLiteral("面板文件夹名称不能为空");
        return false;
    }
    auto cfg = config();
    const QPoint cell = (row < 0 || column < 0)
        ? firstFreeCell(cfg) : QPoint(column, row);
    if (cell.x() < 0 || cell.y() < 0 || cell.x() >= cfg.gridColumns
        || cell.y() > 9999) {
        if (error) *error = QStringLiteral("面板文件夹格位超出范围");
        return false;
    }
    for (const auto& group : cfg.groups) {
        if (group.gridRow == cell.y() && group.gridColumn == cell.x()) {
            if (error) *error = QStringLiteral("目标格位已被占用（含隐藏项），请先选择空格");
            return false;
        }
    }
    for (const auto& app : cfg.pinnedApps) {
        if (app.groupId.isEmpty() && app.gridRow == cell.y() && app.gridColumn == cell.x()) {
            if (error) *error = QStringLiteral("目标格位已被占用（含隐藏项），请先选择空格");
            return false;
        }
    }
    cfg.groups.append({QUuid::createUuid().toString(QUuid::WithoutBraces), name,
                       cell.y(), cell.x()});
    return saveConfig(cfg, error);
}

QString revealTarget(const PinnedApp& app)
{
    if (targetType(app)==QLatin1String("lnk")) {
        const auto link=Platform::shortcutInfo(app.targetPath);
        if (QFileInfo::exists(link.target)) return link.target;
    }
    return app.targetPath;
}

bool canRunAdmin(const PinnedApp& app)
{
    if (!targetExists(app)) return false;
    QString type=targetType(app);
    if (type==QLatin1String("lnk")) type=QFileInfo(Platform::shortcutInfo(app.targetPath).target).suffix().toLower();
    return QStringList{"exe","bat","cmd"}.contains(type);
}

bool launchAdmin(const PinnedApp& app, QString* error)
{
    if (!canRunAdmin(app)) {
        if (error) *error=QStringLiteral("该目标无法以管理员身份运行");
        return false;
    }
    QString path=app.targetPath,args=app.arguments,cwd=app.workingDirectory;
    if (targetType(app)==QLatin1String("lnk")) {
        const auto link=Platform::shortcutInfo(path);
        if (!link.target.isEmpty()) {
            path=link.target;args=link.arguments+(args.isEmpty()?QString():QLatin1Char(' ')+args);
            if (cwd.isEmpty()) cwd=link.workingDirectory;
        }
    }
    if (QStringList{"bat","cmd"}.contains(QFileInfo(path).suffix().toLower())) {
        args=QStringLiteral("/d /s /c \"\"%1\"%2\"").arg(QDir::toNativeSeparators(path),args.isEmpty()?QString():QLatin1Char(' ')+args);
        path=qEnvironmentVariable("SystemRoot")+QStringLiteral("/System32/cmd.exe");
    }
    const bool ok=Platform::launchAsAdmin(path,args,cwd);
    if (!ok && error) *error=QStringLiteral("管理员启动失败或已取消授权");
    return ok;
}

bool editProperties(const QString& id, QWidget* parent, QString* error)
{
    auto cfg=config();
    auto found=std::find_if(cfg.pinnedApps.begin(),cfg.pinnedApps.end(),[&](const auto& a){return a.id==id;});
    if (found==cfg.pinnedApps.end()) return false;
    QDialog dialog(parent); dialog.setWindowTitle(QStringLiteral("固定项属性"));
    Theme::bindDialog(&dialog);
    dialog.setMinimumWidth(540);
    auto* form=new QFormLayout(&dialog);
    const auto field=[&](const QString& name,const QString& value) {
        auto* input=new QLineEdit(value,&dialog); form->addRow(name,input); return input;
    };
    auto* title=field(QStringLiteral("名称"),found->title);
    auto* target=field(QStringLiteral("目标路径"),found->targetPath);
    auto* args=field(QStringLiteral("启动参数"),found->arguments);
    auto* cwd=field(QStringLiteral("工作目录"),found->workingDirectory);
    auto* icon=field(QStringLiteral("图标路径（留空自动提取）"),found->iconPath);
    const int rows=found->groupId.isEmpty()?cfg.gridRows:3,columns=found->groupId.isEmpty()?cfg.gridColumns:3;
    auto* page=new QSpinBox(&dialog);page->setRange(1,10000/rows);page->setValue(found->gridRow/rows+1);
    auto* row=new QSpinBox(&dialog); row->setRange(1,rows);row->setValue(found->gridRow%rows+1);
    auto* column=new QSpinBox(&dialog);column->setRange(1,columns);column->setValue(found->gridColumn+1);
    form->addRow(QStringLiteral("页（从 1 开始）"),page);
    form->addRow(QStringLiteral("页内行（从 1 开始）"),row);form->addRow(QStringLiteral("列（从 1 开始）"),column);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
    form->addRow(buttons);QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    bool saved=false;
    QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        auto updated=config();
        auto item=std::find_if(updated.pinnedApps.begin(),updated.pinnedApps.end(),[&](const auto& a){return a.id==id;});
        if (item==updated.pinnedApps.end()) { dialog.reject();return; }
        if (title->text().trimmed().isEmpty()) {QMessageBox::warning(&dialog,QStringLiteral("名称无效"),QStringLiteral("显示名称不能为空"));return;}
        if (!supportsTarget(target->text())) { QMessageBox::warning(&dialog,QStringLiteral("目标无效"),QStringLiteral("请选择程序、脚本或文件夹"));return; }
        item->title=title->text().trimmed();item->targetPath=target->text().trimmed();item->targetType.clear();
        item->targetType=targetType(*item);item->arguments=args->text();item->workingDirectory=cwd->text().trimmed();
        item->iconPath=icon->text().trimmed();item->gridRow=(page->value()-1)*rows+row->value()-1;item->gridColumn=column->value()-1;
        QString detail;
        if (!saveConfig(updated,&detail)) { QMessageBox::warning(&dialog,QStringLiteral("无法保存"),detail);return; }
        saved=true;dialog.accept();
    });
    dialog.exec();
    Q_UNUSED(error)
    return saved;
}

QString configPath()
{
    return Utils::dataDir() + QStringLiteral("/hotaru/config.json");
}

LauncherConfig config()
{
    LauncherConfig cfg;
    const QString path = configPath();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (!QFileInfo::exists(path))
            saveConfig(cfg, nullptr); // 首次启动就落盘一份可编辑的默认配置
        return cfg;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    f.close(); // Windows 替换文件前必须先释放读取句柄。
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        const QString detail = pe.error != QJsonParseError::NoError
            ? pe.errorString() : QStringLiteral("顶层 JSON 不是对象");
        Utils::log(QStringLiteral("hotaru: 固定软件配置损坏：%1").arg(detail));
        const QString backup = path + QStringLiteral(".corrupt-")
            + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmss"))
            + QStringLiteral(".json");
        if (QFile::copy(path, backup))
            Utils::log(QStringLiteral("hotaru: 已保留损坏配置副本：%1").arg(backup));
        saveConfig(cfg, nullptr);
        return cfg;
    }
    QString configError;
    if (!configFromJson(doc.object(), cfg, &configError)) {
        Utils::log(QStringLiteral("hotaru: 配置字段无效：%1").arg(configError));
        QFile::copy(path,path+QStringLiteral(".corrupt-")
            +QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmsszzz"))+QStringLiteral(".json"));
        cfg=LauncherConfig();
        saveConfig(cfg, nullptr);
    } else {
        const auto array=doc.object().value("pinnedApps").toArray();
    if (!doc.object().contains("pages") || !doc.object().contains("groups") || !doc.object().contains("gridRows") || !doc.object().contains("rememberPage") || !doc.object().contains("lastPage")
            || std::any_of(array.cbegin(),array.cend(),[](const QJsonValue& a) {
            return !a.toObject().contains("gridRow") || !a.toObject().contains("gridColumn");
        })) {
            const auto backup=path+QStringLiteral(".layout-v1-")+QDateTime::currentDateTimeUtc().toString("yyyyMMdd-hhmmsszzz")+QStringLiteral(".json");
            if (QFile::copy(path,backup)) saveConfig(cfg,nullptr);
            else Utils::log(QStringLiteral("hotaru: 布局迁移备份失败，保留原文件"));
        }
    }
    return cfg;
}

bool saveConfig(const LauncherConfig& input, QString* error)
{
    auto cfg=input;
    if (!normalize(cfg,error)) return false;
    const QString path = configPath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error)
            *error = QStringLiteral("无法创建固定软件配置目录");
        return false;
    }
    const QJsonObject root = configToJson(cfg);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("无法写入固定软件配置：%1").arg(path);
        return false;
    }
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (f.write(data) != data.size()) {
        if (error)
            *error = QStringLiteral("固定软件配置写入不完整：%1").arg(path);
        return false;
    }
    if (!f.commit()) {
        if (error)
            *error = QStringLiteral("固定软件配置保存失败：%1").arg(path);
        return false;
    }
    return true;
}

bool importConfig(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取配置文件：%1").arg(path);
        return false;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    f.close();
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("配置 JSON 无效：%1").arg(
                pe.error != QJsonParseError::NoError
                    ? pe.errorString() : QStringLiteral("顶层 JSON 不是对象"));
        return false;
    }
    LauncherConfig cfg;
    if (!configFromJson(doc.object(), cfg, error))
        return false;
    return saveConfig(cfg, error);
}

bool exportConfig(const QString& path, QString* error)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("无法写入配置文件：%1").arg(path);
        return false;
    }
    const QByteArray data = QJsonDocument(configToJson(config())).toJson(QJsonDocument::Indented);
    if (f.write(data) != data.size() || !f.commit()) {
        if (error)
            *error = QStringLiteral("配置导出失败：%1").arg(path);
        return false;
    }
    return true;
}

QVector<PinnedApp> load()
{
    return config().pinnedApps;
}

bool save(const QVector<PinnedApp>& apps, QString* error)
{
    LauncherConfig cfg = config();
    cfg.pinnedApps = apps;
    return saveConfig(cfg, error);
}

PinnedApp fromTarget(const QString& path, int order)
{
    const QFileInfo fi(path);
    PinnedApp app;
    app.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    app.title = fi.completeBaseName();
    if (fi.isDir()) app.title=fi.fileName();
    app.targetPath = QDir::toNativeSeparators(fi.absoluteFilePath());
    app.targetType = deriveTargetType(app);
    app.workingDirectory = fi.absolutePath();
    if (app.targetType==QLatin1String("lnk")) app.workingDirectory.clear();
    app.iconPath = app.targetPath;
    app.sortOrder = order;
    app.category = QStringLiteral("软件");
    return app;
}

QString targetType(const PinnedApp& app)
{
    return app.targetType.isEmpty() ? deriveTargetType(app) : app.targetType;
}

bool targetExists(const PinnedApp& app)
{
    return QFileInfo::exists(app.targetPath);
}

bool launch(const PinnedApp& app, QString* error)
{
    if (!targetExists(app)) {
        if (error)
            *error = QStringLiteral("目标不存在：%1").arg(app.targetPath);
        return false;
    }
    const QString type = targetType(app);
    if (type != QLatin1String("exe") && type != QLatin1String("lnk")
        && type != QLatin1String("bat") && type != QLatin1String("cmd") && type != QLatin1String("folder")) {
        if (error)
            *error = QStringLiteral("不支持的启动类型：%1").arg(type);
        return false;
    }
    if (type == QLatin1String("bat") || type == QLatin1String("cmd")) {
        QString command = QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(app.targetPath));
        if (!app.arguments.trimmed().isEmpty())
            command += QLatin1Char(' ') + app.arguments.trimmed();
        QProcess process;
        process.setProgram(QStringLiteral("cmd.exe"));
        process.setWorkingDirectory(app.workingDirectory);
#ifdef Q_OS_WIN
        // cmd 使用自己的引号规则，不能让 QProcess 的 CRT 参数转义再插入反斜杠。
        process.setNativeArguments(QStringLiteral("/d /s /c \"%1\"").arg(command));
#else
        process.setArguments({QStringLiteral("/d"),QStringLiteral("/c"),command});
#endif
        const bool ok = process.startDetached();
        if (!ok && error)
            *error = QStringLiteral("脚本启动失败：%1").arg(app.targetPath);
        return ok;
    }
    const bool ok = Platform::launch(app.targetPath, app.arguments,
                                     app.workingDirectory, true);
    if (!ok && error)
        *error = QStringLiteral("启动失败：%1").arg(app.targetPath);
    return ok;
}

} // namespace PinnedApps

#pragma once

// 启动器专用主题类型，保持与上游插件使用的基础配色接口分离。
#include "Theme.h"
#include <QVector>
#include <QMap>
#include <QMargins>
#include <QRect>
#include <QSize>

namespace Theme {

// 可安装的主题包。内置主题没有 rootPath，用户主题的 rootPath 指向
// `<数据目录>/hotaru/themes/<id>`。
struct Decoration {
    QString image;
    QString anchor;
    QString reference = QStringLiteral("window");
    QPoint offset;
    QSize size;
    QRect sourceRect; // 缺省整图；GIF 所有帧共用同一个源区域
    bool allowContentOverlap = false; // 显式允许覆盖内容区；装饰始终鼠标穿透
};

struct Surface {
    QSize canvasSize;
    QString frame;
    QMargins framePadding;
    QMargins frameMargins;
    QRect searchRect;
    QRect contentRect;
    QRect footerRect;
    QRect settingsRect;
    QVector<Decoration> decorations;
    bool enabled() const { return !frame.isEmpty(); }
};

struct Package {
    int schemaVersion = 1;
    QString id;
    QString name;
    QString version;
    QString author;
    QString rootPath;
    QString stylesheet;
    QString assetRoot;
    QMap<QString, QString> palette;
    Surface surface;
    bool builtin = false;
    bool valid = false;
    QString error;
};

QVector<Package> packages();
Package currentPackage();
QString currentPackageId();
bool activatePackage(const QString& id, QString* error = nullptr);
bool importPackage(const QString& zipPath, QString* error = nullptr);
bool removePackage(const QString& id, QString* error = nullptr);
QString assetPath(const QString& relativePath);
QString packageStylesheet();
// 第三方颜色和 QSS 仅作用于启动器；设置与插件继续使用基础日间/夜间模式。
QString launcherToken(const char* role);
QColor launcherColor(const char* role);
QString expandLauncher(const QString& qssWithTokens);
void bindLauncher(QWidget* w, const char* qssWithTokens);
void applyToLauncher(QWidget* w);

} // namespace Theme

#include "HotaruNativeHost.h"
#include "Archives.h"
#include "BuildChannel.h"
#include "PluginHost.h"
#include "Utils.h"
#include "hotaru-native-plugin.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QPluginLoader>
#include <QDirIterator>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <utility>

namespace HotaruNativeHost {
namespace {
HotaruNativePlugin* plugin = nullptr;
HotaruNativePluginV2* pluginV2 = nullptr;
QPluginLoader* loader = nullptr;

struct Subscriber {
    QPointer<QObject> context;
    std::function<void()> callback;
};
QVector<Subscriber> subscribers;

void notifyChanged()
{
    subscribers.erase(std::remove_if(subscribers.begin(), subscribers.end(), [](const Subscriber& item) {
        return !item.context || !item.callback;
    }), subscribers.end());
    const auto snapshot = subscribers;
    for (const auto& item : snapshot)
        if (item.context && item.callback)
            item.callback();
}

QString nativePluginDir()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(
        BuildChannel::development ? QStringLiteral("native-plugins-dev")
                                  : QStringLiteral("native-plugins"));
}

bool copyFileAtomically(const QString& source, const QString& destination, QString& error)
{
    if (QFileInfo(source).canonicalFilePath().isEmpty()) {
        error = QStringLiteral("插件文件不存在或路径无效");
        return false;
    }
    QDir().mkpath(QFileInfo(destination).absolutePath());
    const QString temp = destination + QStringLiteral(".new-%1").arg(QCoreApplication::applicationPid());
    QFile::remove(temp);
    if (!QFile::copy(source, temp)) {
        error = QStringLiteral("无法复制插件文件");
        return false;
    }
    QFile::remove(destination);
    if (!QFile::rename(temp, destination)) {
        QFile::remove(temp);
        error = QStringLiteral("无法切换插件文件");
        return false;
    }
    return true;
}

QStringList packageDlls(const QString& root)
{
    QStringList files;
    QDirIterator it(root, {QStringLiteral("*.dll")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files << it.next();
    return files;
}
}

void start(std::function<void(QString)> fileOffer)
{
#if defined(HOTARU_LAN_PREVIEW) && HOTARU_LAN_PREVIEW
    if (plugin)
        return;
    const auto path = QDir(QCoreApplication::applicationDirPath())
                          .filePath(QStringLiteral("native-preview/HotaruLan.dll"));
    if (!QFileInfo(path).isFile())
        return;
    loader = new QPluginLoader(path, qApp);
    const auto meta = loader->metaData();
    const auto data = meta.value("MetaData").toObject();
    const auto ids = data.value("Ids").toArray();
    if (meta.value("IID").toString() == QLatin1String(HOTARU_NATIVE_PLUGIN_IID) &&
        data.value("Api").toInt() == kHotaruNativePluginApi &&
        data.value("Channel").toString() == QLatin1String("development-preview") &&
        ids == QJsonArray{QStringLiteral("lan-clipboard"), QStringLiteral("lan-files")}) {
        plugin = qobject_cast<HotaruNativePlugin*>(loader->instance());
        if (plugin && plugin->start(Utils::dataDir())) {
            plugin->setFileOfferNotifier(std::move(fileOffer));
            pluginV2 = qobject_cast<HotaruNativePluginV2*>(loader->instance());
            if (pluginV2)
                pluginV2->setChangedNotifier(notifyChanged);
            return;
        }
        plugin = nullptr;
    }
    Utils::log(QStringLiteral("直属局域网预览模块无法加载：%1").arg(loader->errorString()));
    delete std::exchange(loader, nullptr);
#else
    Q_UNUSED(fileOffer);
#endif
}

void stop()
{
    if (plugin) {
        plugin->stop();
        plugin = nullptr;
    }
    pluginV2 = nullptr;
    if (loader)
        loader->unload();
    delete std::exchange(loader, nullptr);
    subscribers.clear();
}

QString state(const QString& id) { return plugin ? plugin->state(id) : QString{}; }

QWidget* createPage(const QString& id, QWidget* parent)
{
    return plugin ? plugin->createPage(id, parent) : nullptr;
}

QVector<PluginInfo> plugins()
{
    QVector<PluginInfo> result;
    const auto add = [&result](const QString& id, const QString& name,
                               const QString& desc) {
        PluginInfo info;
        info.id = id;
        info.name = name;
        info.description = desc;
        info.installed = plugin != nullptr;
        info.enabled = pluginV2 && pluginV2->enabled(id);
        info.verified = false;
        info.state = state(id);
        result.push_back(info);
    };
    add(QStringLiteral("lan-clipboard"), QStringLiteral("局域网剪贴板共享"),
        QStringLiteral("在已配对设备间同步新复制的文本和图片。"));
    add(QStringLiteral("lan-files"), QStringLiteral("局域网文件分享"),
        QStringLiteral("选择要发送的文件或文件夹，由对方确认接收。"));
    return result;
}

bool setEnabled(const QString& id, bool enabledValue, QString& error)
{
    if (!pluginV2) {
        error = QStringLiteral("当前直属插件不支持卡片启停");
        return false;
    }
    const bool ok = pluginV2->setEnabled(id, enabledValue, error);
    if (ok)
        notifyChanged();
    return ok;
}

QVector<HotaruNativePeer> peers(int feature)
{
    return pluginV2 ? pluginV2->peers(feature) : QVector<HotaruNativePeer>{};
}

HotaruClipboardSnapshot clipboardSnapshot()
{
    return pluginV2 ? pluginV2->clipboardSnapshot() : HotaruClipboardSnapshot{};
}

bool sendFiles(const QByteArray& peerId, const QStringList& paths, QString& error)
{
    if (!pluginV2) {
        error = QStringLiteral("局域网文件分享尚未加载");
        return false;
    }
    return pluginV2->sendFiles(peerId, paths, error);
}

QString clipboardSearchAlias()
{
    return pluginV2 ? pluginV2->clipboardSearchAlias() : QStringLiteral("jtb");
}

bool setClipboardSearchAlias(const QString& alias, QString& error)
{
    if (!pluginV2) {
        error = QStringLiteral("局域网剪贴板尚未加载");
        return false;
    }
    return pluginV2->setClipboardSearchAlias(alias, error);
}

void subscribeChanged(QObject* context, std::function<void()> callback)
{
    if (!context || !callback)
        return;
    subscribers.push_back({context, std::move(callback)});
    QObject::connect(context, &QObject::destroyed, qApp, [context] {
        subscribers.erase(std::remove_if(subscribers.begin(), subscribers.end(),
            [context](const Subscriber& s) { return s.context == context; }), subscribers.end());
    });
}

bool importPackage(const QString& path, QString& error)
{
    if (!BuildChannel::development) {
        error = QStringLiteral("正式版不允许导入未验证的本地插件");
        return false;
    }
    const QFileInfo input(path);
    if (!input.isFile()) {
        error = QStringLiteral("插件文件不存在");
        return false;
    }
    QTemporaryDir staging;
    if (!staging.isValid()) {
        error = QStringLiteral("无法创建插件暂存目录");
        return false;
    }
    QStringList dlls;
    const QString suffix = input.suffix().toLower();
    if (suffix == QLatin1String("dll")) {
        const QString staged = QDir(staging.path()).filePath(input.fileName());
        if (!QFile::copy(input.absoluteFilePath(), staged)) {
            error = QStringLiteral("无法复制插件到暂存目录");
            return false;
        }
        dlls << staged;
    } else if (suffix == QLatin1String("zip")) {
        const auto extracted = Archive::extract(input.absoluteFilePath(), staging.path(), nullptr, nullptr);
        if (!extracted.ok) {
            error = extracted.error.isEmpty() ? QStringLiteral("插件 ZIP 解压失败") : extracted.error;
            return false;
        }
        dlls = packageDlls(staging.path());
    } else {
        error = QStringLiteral("请选择 DLL 或 ZIP 插件包");
        return false;
    }
    if (dlls.isEmpty()) {
        error = QStringLiteral("插件包中没有 DLL");
        return false;
    }
    const QString candidate = dlls.first();
    QPluginLoader probe(candidate);
    const auto meta = probe.metaData();
    const QString iid = meta.value(QStringLiteral("IID")).toString();
    const auto data = meta.value(QStringLiteral("MetaData")).toObject();
    QString destinationDir;
    QString destinationName = QFileInfo(candidate).fileName();
    if (iid == QLatin1String(HOTARU_NATIVE_PLUGIN_IID)) {
        destinationDir = nativePluginDir();
        const auto ids = data.value(QStringLiteral("Ids")).toArray();
        if (ids.isEmpty()) {
            error = QStringLiteral("直属插件缺少 Ids 清单");
            return false;
        }
    } else if (iid == QLatin1String(LTOOL_PLUGIN_IID)) {
        destinationDir = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("ltool-plugins-dev"));
    } else {
        error = QStringLiteral("不是 Hotaru 直属插件或 ltool API v4 插件");
        return false;
    }
    const QString destination = QDir(destinationDir).filePath(destinationName);
    if (QFileInfo::exists(destination)) {
        error = QStringLiteral("同名插件已存在，请先移除旧版本");
        return false;
    }
    if (!copyFileAtomically(candidate, destination, error))
        return false;
    if (iid == QLatin1String(LTOOL_PLUGIN_IID))
        PluginHost::reload();
    return true;
}
}

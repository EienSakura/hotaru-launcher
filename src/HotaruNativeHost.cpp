#include "HotaruNativeHost.h"
#include "ClipboardStore.h"
#include <QClipboard>
#include <QMimeData>
#include <QJsonArray>
#include <QListWidget>
#include <QKeyEvent>
#include "platform/PluginNames.h"
#include "HotaruPluginRepository.h"
#include "PluginStore.h"
#include <QSaveFile>
#include <QSettings>
#include "BuildChannel.h"
#include "PluginHost.h"
#include "SearchWindow.h"
#include "SettingsDialog.h"
#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QToolButton>
#include "Utils.h"
#include "hotaru-native-plugin.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPointer>
#include <QPluginLoader>
#include <QTemporaryDir>
#include <QSet>
#include <QScopedValueRollback>
#include <QThread>

#include <algorithm>
#include <memory>
#include <utility>

namespace HotaruNativeHost {
namespace Repo = HotaruPluginRepository;
namespace {
struct Runtime {
    QPluginLoader* loader = nullptr;
    HotaruNativePlugin* entry = nullptr;
    bool running = false;
};
QMap<QString, Runtime> native;
QSet<QString> pendingRemoved;
Repo::Inventory runtimeInventory;
std::function<void(QString)> fileOfferNotifier;
bool hostStarted = false;

QString enabledKey(const QString& id) { return QStringLiteral("plugins/native/") + id + QStringLiteral("/enabled"); }
QSettings settings() { return QSettings(Utils::dataDir() + QStringLiteral("/config.ini"), QSettings::IniFormat); }
QString testPluginDirectory;
bool removed(const QString& id)
{
    return pendingRemoved.contains(id);
}

HotaruNativePlugin* plugin = nullptr;
HotaruNativePluginV2* pluginV2 = nullptr;
HotaruLanPackage* core = nullptr;
QVector<QPluginLoader*> loaders;
QSet<QString> loaded;

struct Subscriber {
    QPointer<QObject> context;
    std::shared_ptr<std::function<void()>> callback;
};
QVector<Subscriber> subscribers;
QVector<Subscriber> inventorySubscribers;
void notifyChanged();
void notifyInventory()
{
    const auto current = Repo::active(), desired = Repo::desired();
    const auto previous = pendingRemoved;
    pendingRemoved.clear();
    for (auto it = current.cbegin(); it != current.cend(); ++it)
        if (!desired.contains(it.key())) pendingRemoved.insert(it.key());
    auto order = Repo::loadOrder(current);
    std::reverse(order.begin(), order.end());
    for (const auto& id : order) {
        if (!pendingRemoved.contains(id) || previous.contains(id)) continue;
        QString error;
        if (loaded.contains(id) && pluginV2) pluginV2->setEnabled(id, false, error);
        if (native.value(id).running) {
            native[id].entry->stop();
            native[id].running = false;
        }
        if (!error.isEmpty()) Utils::log(error);
    }
    for (const auto& item : inventorySubscribers)
        if (item.context && item.callback) (*item.callback)();
    notifyChanged();
}

void notifyChanged()
{
    ClipboardStore::instance().setAvailable(enabled(QLatin1String(kHotaruLanClipboardId)));
    subscribers.erase(std::remove_if(subscribers.begin(), subscribers.end(), [](const Subscriber& item) {
        return !item.context || !item.callback;
    }), subscribers.end());
    const auto snapshot = subscribers;
    for (const auto& item : snapshot)
        if (item.context && item.callback)
            (*item.callback)();
}

QString nativePluginDir()
{
    if (!testPluginDirectory.isEmpty())
        return testPluginDirectory;
    return QDir(QCoreApplication::applicationDirPath()).filePath(
        BuildChannel::development ? QStringLiteral("native-plugins-dev")
                                  : QStringLiteral("native-plugins"));
}

struct Package {
    const char* id;
    const char* file;
    int feature;
};
constexpr Package packages[] = {
    {kHotaruLanCoreId, "HotaruLan.dll", 0},
    {kHotaruLanClipboardId, "lan-clipboard.dll", kHotaruFeatureClipboard},
    {kHotaruLanFilesId, "lan-files.dll", kHotaruFeatureFiles},
};

const Package* metadataPackage(const QJsonObject& meta)
{
    if (meta.value("IID").toString() != QLatin1String(HOTARU_LAN_PACKAGE_IID))
        return nullptr;
    const auto data = meta.value("MetaData").toObject();
    if (data.value("Api").toInt() != kHotaruLanPackageApi ||
        (data.value("Channel").toString() != QLatin1String("development-preview") &&
         data.value("Channel").toString() != QLatin1String("preview") &&
         data.value("Channel").toString() != QLatin1String("stable")))
        return nullptr;
    for (const auto& spec : packages)
        if (data.value("Id").toString() == QLatin1String(spec.id) && data.value("Requires").isString() &&
            data.value("Requires").toString() == QLatin1String(spec.feature ? kHotaruLanCoreId : "") &&
            (spec.feature || data.value("Protocol").toString() == QLatin1String("HL02")))
            return &spec;
    return nullptr;
}

bool installed(const Package& package)
{
    const QFileInfo file(QDir(nativePluginDir()).filePath(QLatin1String(package.file)));
    if (!file.isFile() || file.isSymLink())
        return false;
    QPluginLoader probe(file.absoluteFilePath());
    return metadataPackage(probe.metaData()) == &package;
}

void unloadRuntime()
{
    if (plugin) plugin->stop();
    plugin = nullptr;
    pluginV2 = nullptr;
    core = nullptr;
    loaded.clear();
    auto order = Repo::loadOrder(runtimeInventory);
    std::reverse(order.begin(), order.end());
    for (const auto& id : order) {
        const auto runtime = native.take(id);
        if (runtime.running) runtime.entry->stop();
        if (runtime.loader) { runtime.loader->unload(); delete runtime.loader; }
    }
    for (auto* loader : loaders) { loader->unload(); delete loader; }
    loaders.clear();
    runtimeInventory.clear();
}

bool startLan(const Repo::Inventory& items, bool trial, const std::function<void(QString)>& fileOffer, QString& error)
{
    const bool managed = Repo::active().contains(QLatin1String(kHotaruLanCoreId))
        || std::any_of(items.cbegin(), items.cend(), [](const auto& item) {
            return item.entry.iid == HOTARU_LAN_PACKAGE_IID;
        });
    const auto pathFor = [&](const Package& spec) -> QString {
        const auto id = QString::fromLatin1(spec.id);
        if (managed) return items.contains(id) ? Repo::dllPath(items[id]) : QString{};
        if (testPluginDirectory.isEmpty()) {
#if !HOTARU_LAN_PREVIEW
            return {};
#else
            // 一旦接管过这一组，卸载后不再回退到预装副本。
            if (settings().value("plugins/native/lanManaged").toBool()) return {};
#endif
        }
        return installed(spec) ? QDir(nativePluginDir()).filePath(QLatin1String(spec.file)) : QString{};
    };
    if (pathFor(packages[0]).isEmpty()) return true;
    int features = 0;
    for (const auto& spec : packages) {
        const auto path = pathFor(spec);
        if (path.isEmpty()) continue;
        auto* loader = new QPluginLoader(path, qApp);
        loader->setLoadHints(QLibrary::LoadHints{});
        auto* object = loader->instance();
        auto* package = qobject_cast<HotaruLanPackage*>(object);
        if (!package || package->id() != QLatin1String(spec.id) || package->feature() != spec.feature) {
            Utils::log(QStringLiteral("直属插件无法加载：%1").arg(QLatin1String(spec.id)));
            delete loader;
            if (trial || !spec.feature) {
                error = QStringLiteral("无法加载 %1").arg(QLatin1String(spec.id));
                return false;
            }
            continue;
        }
        if (!spec.feature) {
            auto* entry = qobject_cast<HotaruNativePlugin*>(object);
            auto* v2 = qobject_cast<HotaruNativePluginV2*>(object);
            if (!entry || !v2 || !entry->start(Utils::dataDir())) {
                delete loader;
                error = QStringLiteral("局域网连接初始化失败");
                return false;
            }
            core = package;
            plugin = entry;
            pluginV2 = v2;
            plugin->setFileOfferNotifier(fileOffer);
            if (auto* receiver = qobject_cast<HotaruLanClipboardReceive*>(object))
                receiver->setClipboardReceiveNotifier([](const QMimeData& mime) {
                    ClipboardStore::instance().recordReceived(mime);
                });
        }
        features |= spec.feature;
        loaded.insert(QLatin1String(spec.id));
        loaders.push_back(loader);
    }
    if (core) {
        core->setAvailableFeatures(features);
        pluginV2->setChangedNotifier(notifyChanged);
    }
    return true;
}

bool loadNative(const Repo::Inventory& items, bool trial, const std::function<void(QString)>& fileOffer, QString& error)
{
    unloadRuntime();
    runtimeInventory = items;
    if (!startLan(items, trial, fileOffer, error)) {
        if (trial) { unloadRuntime(); return false; }
    }
    for (const auto& id : Repo::loadOrder(items)) {
        const auto& item = items[id];
        if (item.entry.iid != HOTARU_NATIVE_PLUGIN_IID) continue;
        const bool on = settings().value(enabledKey(id)).toBool();
        if (!on && !(trial && item.source == "official")) continue;
        auto* loader = new QPluginLoader(Repo::dllPath(item), qApp);
        auto* entry = qobject_cast<HotaruNativePlugin*>(loader->instance());
        bool dependencyReady = true;
        for (auto dep = item.entry.requiresPackages.cbegin(); dep != item.entry.requiresPackages.cend(); ++dep)
            dependencyReady &= enabled(dep.key());
        const bool run = on && dependencyReady;
        if (!entry || (run && !entry->start(Utils::dataDir()))) {
            error = QStringLiteral("插件加载或初始化失败：%1").arg(id);
            if (entry && run) entry->stop();
            loader->unload();
            delete loader;
            if (trial) { unloadRuntime(); return false; }
            continue;
        }
        native.insert(id, {loader, entry, run});
    }
    return error.isEmpty();
}
}

void start(std::function<void(QString)> fileOffer)
{
    fileOfferNotifier = std::move(fileOffer);
    pendingRemoved.clear();
    if (!testPluginDirectory.isEmpty()) {
        QString error;
        loadNative({}, false, fileOfferNotifier, error);
    } else {
        const auto previous = Repo::active();
        Repo::activate([&](const auto& items, bool trial, QString& error) {
            return loadNative(items, trial, fileOfferNotifier, error);
        });
        const auto current = Repo::active();
        auto config = settings();
        for (auto it = previous.cbegin(); it != previous.cend(); ++it)
            if (!current.contains(it.key())) config.remove(enabledKey(it.key()));
        if (current.contains(QLatin1String(kHotaruLanCoreId)))
            config.setValue("plugins/native/lanManaged", true);
    }
    hostStarted = true;
    notifyChanged();
}

bool tryActivateFirstInstall(QString& error)
{
    // 不卸载已加载的 DLL，也不接管已有的预装插件。
    if (!hostStarted || !testPluginDirectory.isEmpty() || !runtimeInventory.isEmpty()
        || !loaders.isEmpty() || !native.isEmpty() || !Repo::active().isEmpty() || !Repo::pending())
        return false;
    Repo::activate([](const auto& items, bool trial, QString& failure) {
        return loadNative(items, trial, fileOfferNotifier, failure);
    });
    error = Repo::activationError();
    if (!error.isEmpty()) return false;
    if (Repo::active().contains(QLatin1String(kHotaruLanCoreId)))
        settings().setValue("plugins/native/lanManaged", true);
    return !Repo::active().isEmpty() && !Repo::pending();
}

void stop()
{
    hostStarted = false;
    ClipboardStore::instance().setAvailable(false);
    unloadRuntime();
    fileOfferNotifier = {};
    subscribers.clear();
    inventorySubscribers.clear();
    pendingRemoved.clear();
}

QString state(const QString& id)
{
    if (removed(id)) return QStringLiteral("待卸载");
    if (plugin && loaded.contains(id)) return plugin->state(id);
    const auto r = native.value(id);
    return r.running ? r.entry->state(id) : QString{};
}
bool enabled(const QString& id)
{
    if (removed(id)) return false;
    if (pluginV2 && loaded.contains(id)) return pluginV2->enabled(id);
    if (!native.value(id).running) return false;
    const auto e = runtimeInventory.value(id).entry;
    for (auto it = e.requiresPackages.cbegin(); it != e.requiresPackages.cend(); ++it)
        if (!enabled(it.key())) return false;
    return true;
}
bool hasPendingFileOffer() { return core && enabled(QLatin1String(kHotaruLanFilesId)) && core->hasPendingFileOffer(); }

QWidget* createPage(const QString& id, QWidget* parent, const QByteArray& target)
{
    if (removed(id)) return nullptr;
    if (core && loaded.contains(id)) return core->createFeaturePage(id, target, parent);
    const auto runtime = native.value(id);
    return enabled(id) && runtime.entry ? runtime.entry->createPage(id, parent) : nullptr;
}

QVector<PluginInfo> plugins()
{
    QVector<PluginInfo> result;
    const auto current = Repo::active();
    const auto desired = Repo::desired();
    const bool bundledAvailable = !testPluginDirectory.isEmpty() ||
        (BuildChannel::development && HOTARU_LAN_PREVIEW && !settings().value("plugins/native/lanManaged").toBool());
    const bool coreInstalled = (bundledAvailable && installed(packages[0])) || desired.contains(QLatin1String(kHotaruLanCoreId));
    const auto add = [&](const Package& spec, const QString& name,
                               const QString& desc) {
        PluginInfo info;
        info.id = QLatin1String(spec.id);
        if (spec.feature) info.dependency = QLatin1String(kHotaruLanCoreId);
        info.name = name;
        info.description = desc;
        const auto item = desired.value(info.id, current.value(info.id));
        const bool bundled = bundledAvailable && installed(spec) && !desired.contains(QLatin1String(kHotaruLanCoreId));
        info.installed = bundled || !item.source.isEmpty();
        info.loaded = loaded.contains(info.id);
        info.source = item.source.isEmpty() && bundled ? QStringLiteral("bundled") : item.source;
        info.version = item.entry.version;
        info.preview = item.source != "official" || item.entry.channel == "preview";
        info.pending = Repo::pending() && (current.value(info.id).folder != desired.value(info.id).folder);
        info.removable = !item.source.isEmpty() || (bundled && !spec.feature);
        info.enabled = enabled(info.id);
        info.dependencyReady = !spec.feature || enabled(QLatin1String(kHotaruLanCoreId));
        info.verified = (info.source == "official" && (runtimeInventory.contains(info.id) || !current.contains(info.id))) || info.source == "bundled";
        info.state = info.pending ? (desired.contains(info.id) ? QStringLiteral("待重启") : QStringLiteral("待卸载")) : spec.feature && !coreInstalled ? QStringLiteral("请先安装局域网连接")
                   : info.installed && !info.loaded ? QStringLiteral("已安装 · 重启后加载")
                   : info.installed && !info.dependencyReady ? QStringLiteral("请先启用局域网连接")
                   : state(info.id);
        result.push_back(info);
    };
    add(packages[0], QStringLiteral("局域网连接"),
        QStringLiteral("管理设备配对和连接。剪贴板共享与文件分享需要先安装此插件。"));
    add(packages[1], QStringLiteral("局域网剪贴板共享"),
        QStringLiteral("在已配对设备间同步新复制的文本和图片。"));
    add(packages[2], QStringLiteral("局域网文件分享"),
        QStringLiteral("选择要发送的文件或文件夹，由对方确认接收。"));
    auto all = current;
    for (auto it = desired.cbegin(); it != desired.cend(); ++it) all[it.key()] = it.value();
    for (const auto& item : all) {
        const auto& e = item.entry;
        if (e.iid != HOTARU_NATIVE_PLUGIN_IID) continue;
        PluginInfo info;
        info.id = e.id; info.name = e.name; info.description = e.description; info.version = e.version;
        info.installed = true; info.loaded = runtimeInventory.contains(e.id); info.enabled = enabled(e.id);
        info.source = item.source; info.verified = item.source == "official" && (runtimeInventory.contains(e.id) || !current.contains(e.id)); info.preview = e.channel == "preview";
        info.category = e.category; info.removable = true;
        if (!e.requiresPackages.isEmpty()) info.dependency = e.requiresPackages.firstKey();
        for (auto it = e.requiresPackages.cbegin(); it != e.requiresPackages.cend(); ++it)
            info.dependencyReady &= enabled(it.key());
        info.pending = Repo::pending() && current.value(e.id).folder != desired.value(e.id).folder;
        info.state = removed(e.id) ? QStringLiteral("待卸载") : info.pending ? QStringLiteral("待重启") :
            info.enabled ? state(e.id) : info.loaded ? QStringLiteral("已关闭") : QStringLiteral("加载失败");
        result << info;
    }
    return result;
}

bool setEnabled(const QString& id, bool enabledValue, QString& error)
{
    if (removed(id)) {
        error = QStringLiteral("插件已安排卸载");
        return false;
    }
    if (runtimeInventory.contains(id) && runtimeInventory[id].entry.iid == HOTARU_NATIVE_PLUGIN_IID) {
        const auto item = runtimeInventory[id];
        if (enabledValue) {
            for (auto it = item.entry.requiresPackages.cbegin(); it != item.entry.requiresPackages.cend(); ++it)
                if (!enabled(it.key())) {
                    error = QStringLiteral("请先启用 %1").arg(it.key()); return false;
                }
            auto r = native.value(id);
            if (!r.running) {
                if (!r.loader) {
                    if (!Repo::validateInventory(runtimeInventory, error)) return false;
                    r.loader = new QPluginLoader(Repo::dllPath(item), qApp);
                    r.entry = qobject_cast<HotaruNativePlugin*>(r.loader->instance());
                }
                if (!r.entry || !r.entry->start(Utils::dataDir())) {
                    if (r.entry) r.entry->stop();
                    if (r.loader && !native.contains(id)) { r.loader->unload(); delete r.loader; }
                    error = QStringLiteral("插件启动失败：%1").arg(id); return false;
                }
                r.running = true; native[id] = r;
            }
        } else {
            // 插件页可能仍由调用者持有，保留 DLL 直到宿主退出。
            auto order = Repo::loadOrder(runtimeInventory);
            std::reverse(order.begin(), order.end());
            QSet<QString> paused{id};
            for (const auto& key : order)
                for (auto it = runtimeInventory[key].entry.requiresPackages.cbegin(); it != runtimeInventory[key].entry.requiresPackages.cend(); ++it)
                    if (paused.contains(it.key())) paused.insert(key);
            for (const auto& key : order) {
                if (!paused.contains(key) || !native.value(key).running) continue;
                native[key].entry->stop(); native[key].running = false;
            }
        }
        auto config = settings();
        config.setValue(enabledKey(id), enabledValue);
        config.sync();
        notifyChanged();
        return true;
    }
    if (!pluginV2 || !loaded.contains(id)) {
        error = QStringLiteral("请先安装插件及其前置，并重启 Hotaru");
        return false;
    }
    const bool ok = pluginV2->setEnabled(id, enabledValue, error);
    if (ok && !enabledValue) {
        auto order = Repo::loadOrder(runtimeInventory);
        std::reverse(order.begin(), order.end());
        for (const auto& key : order)
            if (native.value(key).running && !enabled(key)) {
                native[key].entry->stop(); native[key].running = false;
            }
    }
    if (ok) notifyChanged();
    return ok;
}

QVector<HotaruNativePeer> peers(int feature)
{
    const auto id = QLatin1String(feature == kHotaruFeatureFiles ? kHotaruLanFilesId : kHotaruLanClipboardId);
    return enabled(id) ? pluginV2->peers(feature) : QVector<HotaruNativePeer>{};
}

HotaruClipboardSnapshot clipboardSnapshot()
{
    return enabled(QLatin1String(kHotaruLanClipboardId)) ? pluginV2->clipboardSnapshot() : HotaruClipboardSnapshot{};
}

bool sendFiles(const QByteArray& peerId, const QStringList& paths, QString& error)
{
    if (!enabled(QLatin1String(kHotaruLanFilesId))) {
        error = QStringLiteral("局域网文件分享未启用");
        return false;
    }
    return pluginV2->sendFiles(peerId, paths, error);
}

QString clipboardSearchAlias()
{
    return enabled(QStringLiteral("lan-clipboard")) ? pluginV2->clipboardSearchAlias() : QString{};
}

bool setClipboardSearchAlias(const QString& alias, QString& error)
{
    if (!pluginV2 || !loaded.contains(QLatin1String(kHotaruLanClipboardId))) {
        error = QStringLiteral("局域网剪贴板尚未加载");
        return false;
    }
    return pluginV2->setClipboardSearchAlias(alias, error);
}

void subscribeChanged(QObject* context, std::function<void()> callback)
{
    if (!context || !callback)
        return;
    subscribers.push_back({context, std::make_shared<std::function<void()>>(std::move(callback))});
    QObject::connect(context, &QObject::destroyed, qApp, [context] {
        subscribers.erase(std::remove_if(subscribers.begin(), subscribers.end(),
            [context](const Subscriber& s) { return s.context == context; }), subscribers.end());
    });
}

void inventoryChanged() { notifyInventory(); }

void subscribeInventory(QObject* context, std::function<void()> callback)
{
    inventorySubscribers.erase(std::remove_if(inventorySubscribers.begin(), inventorySubscribers.end(),
        [](const Subscriber& item) { return !item.context; }), inventorySubscribers.end());
    inventorySubscribers.push_back({context, std::make_shared<std::function<void()>>(std::move(callback))});
}

bool importPackage(const QString& path, QString& error, const ConfirmReplace& confirm)
{
    const auto parent = testPluginDirectory.isEmpty() ? Repo::root() + "/staging" : testPluginDirectory;
    if (!QDir().mkpath(parent) || QFileInfo(parent).isJunction() || QFileInfo(parent).isSymLink()
        || QFileInfo(Repo::root()).isJunction() || QFileInfo(Repo::root()).isSymLink()) {
        error = QStringLiteral("无法创建插件暂存目录");
        return false;
    }
    QTemporaryDir stage(parent + "/import-XXXXXX");
    Repo::Entry entry;
    if (!stage.isValid()) { error = QStringLiteral("无法创建插件暂存目录"); return false; }
    if (!Repo::readLocalPackage(path, stage.path(), entry, error)) return false;
    if (!testPluginDirectory.isEmpty()) {
        const auto meta = QPluginLoader(stage.filePath(entry.dll())).metaData();
        const auto* spec = metadataPackage(meta);
        if (!spec) return false;
        if (spec->feature && !installed(packages[0])) {
            error = QStringLiteral("请先安装前置插件"); return false;
        }
        if (!QFile::copy(stage.filePath(entry.dll()), QDir(nativePluginDir()).filePath(QLatin1String(spec->file)))) {
            error = QStringLiteral("同 ID 插件已存在"); return false;
        }
        return true;
    }
    if (entry.iid == LTOOL_PLUGIN_IID) {
        if (PluginHost::isRetired(entry.id)) {
            error = QStringLiteral("旧剪贴板插件已由 Hotaru 剪贴板替代"); return false;
        }
        const auto dir = PluginHost::pluginsDir();
        if (!QDir().mkpath(dir)) { error = QStringLiteral("无法创建兼容插件目录"); return false; }
        const auto file = Platform::pluginFileName(entry.id);
        QString target = QDir(dir).filePath(file) + ".off";
        for (const auto& existing : QDir(dir).entryInfoList({"*.dll", "*.dll.off"}, QDir::Files)) {
            const auto meta = QPluginLoader(existing.absoluteFilePath()).metaData().value("MetaData").toObject();
            if (meta.value("Id").toString().compare(entry.id, Qt::CaseInsensitive) != 0 &&
                existing.absoluteFilePath().compare(QDir(dir).filePath(file), Qt::CaseInsensitive) != 0 &&
                existing.absoluteFilePath().compare(target, Qt::CaseInsensitive) != 0) continue;
            const auto oldVersion = meta.value("Version").toString("1.0.0");
            if (!confirm) { error = QStringLiteral("同 ID 插件已存在"); return false; }
            if (!confirm(QStringLiteral("替换 %1 %2 → %3（本地导入）？").arg(entry.id, oldVersion, entry.version))) return false;
            target = existing.absoluteFilePath();
            const auto liveName = existing.fileName().endsWith(".off") ? existing.fileName().chopped(4) : existing.fileName();
            if (PluginStore::pendingRemoves().contains(liveName)) {
                error = QStringLiteral("请先重启完成兼容插件卸载"); return false;
            }
            error = PluginStore::stageLocalReplacement(stage.filePath(entry.dll()), target);
            if (error.isEmpty()) notifyInventory();
            return error.isEmpty();
        }
        // 首次安装先落为 .off，未启用前不进入兼容加载器。
        QFile from(stage.filePath(entry.dll()));
        QSaveFile out(target);
        if (!from.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly)) {
            error = QStringLiteral("无法暂存兼容插件"); return false;
        }
        while (!from.atEnd()) {
            const auto bytes = from.read(64 * 1024);
            if (bytes.isEmpty() || out.write(bytes) != bytes.size()) {
                error = QStringLiteral("无法写入兼容插件"); return false;
            }
        }
        if (!out.commit()) { error = QStringLiteral("无法替换兼容插件，请关闭占用后重试"); return false; }
        if (target.endsWith(".off")) PluginHost::reload();
        notifyInventory();
        return true;
    }
    const auto existing = Repo::desired().value(entry.id);
    bool replace = false;
    if (!existing.source.isEmpty()) {
        if (!confirm) { error = QStringLiteral("同 ID 插件已存在"); return false; }
        QString change = QStringLiteral("替换 %1 %2（%3）→ %4（本地导入）？").arg(entry.id, existing.entry.version,
            existing.source == "official" ? QStringLiteral("官方仓库") : QStringLiteral("本地导入"), entry.version);
        if (existing.source == "official" && entry.iid == HOTARU_LAN_PACKAGE_IID)
            change += QStringLiteral("\n此操作将切换整个局域网插件组。请先导入前置，再导入功能包。");
        if (!confirm(change)) return false;
        replace = true;
    } else if (entry.iid == HOTARU_LAN_PACKAGE_IID && loaded.contains(entry.id) &&
               !runtimeInventory.contains(entry.id) && !Repo::desired().contains(QLatin1String(kHotaruLanCoreId))) {
        if (!confirm || !confirm(QStringLiteral("切换开发预装局域网插件组到本地包 %1？").arg(entry.version))) return false;
    }
    if (!Repo::queueLocal(entry, stage.path(), replace, error)) return false;
    notifyInventory();
    return true;
}

bool uninstall(const QString& id, QStringList& affected, bool confirm, QString& error)
{
    const bool bundled = loaded.contains(id) && !runtimeInventory.contains(id)
        && id == QLatin1String(kHotaruLanCoreId) && !Repo::desired().contains(id);
    if (bundled) {
        affected.clear();
        for (const auto& p : packages)
            if (loaded.contains(QLatin1String(p.id))) affected << QLatin1String(p.id);
        if (!confirm) return true;
        auto config = settings();
        config.setValue("plugins/native/lanManaged", true);
        config.sync();
        if (config.status() != QSettings::NoError) {
            error = QStringLiteral("无法保存预装插件卸载状态"); return false;
        }
    } else {
        if (!Repo::remove(id, affected, confirm, error)) return false;
        if (!confirm) return true;
    }
    if (bundled)
        for (auto it = affected.crbegin(); it != affected.crend(); ++it)
            if (pluginV2) pluginV2->setEnabled(*it, false, error);
    notifyInventory();
    return true;
}

bool previewPackageSelftest(const QStringList& archives, QString& error)
{
    if (archives.size() != 3 || core) {
        error = QStringLiteral("需要按前置、剪贴板、文件分享提供三个包");
        return false;
    }
    if (BuildChannel::development && installed(packages[0])) {
        start();
        const auto bundled = plugins();
        const bool clean = bundled.size() == 3 && std::all_of(bundled.cbegin(), bundled.cend(), [](const auto& item) {
            return item.installed && item.loaded && !item.enabled;
        });
        stop();
        if (!clean) {
            error = QStringLiteral("开发包预装插件未全部加载或默认状态不是关闭");
            return false;
        }
    }
    QTemporaryDir fixture;
    if (!fixture.isValid())
        return false;
    const QScopedValueRollback directory(testPluginDirectory, fixture.path());
    const auto fail = [&error](const QString& message) {
        error = message;
        return false;
    };
    if (importPackage(archives[1], error) || !error.contains(QStringLiteral("前置插件")))
        return fail(QStringLiteral("功能包先于前置安装时没有被拒绝"));
    const auto renamed = QDir(fixture.path()).filePath(QStringLiteral("renamed-test-package.zip"));
    if (!QFile::copy(archives[0], renamed))
        return fail(QStringLiteral("无法准备更名导入夹具"));
    if (!importPackage(renamed, error) || !importPackage(archives[1], error) ||
        !importPackage(archives[2], error))
        return false;
    const auto corePath = QDir(fixture.path()).filePath(QLatin1String(packages[0].file));
    QFile original(corePath);
    if (!original.open(QIODevice::ReadOnly))
        return false;
    const auto before = original.readAll();
    original.close();
    if (importPackage(archives[0], error) || !original.open(QIODevice::ReadOnly) || before != original.readAll())
        return fail(QStringLiteral("重复安装未保留原 DLL"));
    original.close();
    start();
    const auto id = QLatin1String(kHotaruLanCoreId);
    const auto clip = QLatin1String(kHotaruLanClipboardId);
    const auto files = QLatin1String(kHotaruLanFilesId);
    const auto infos = plugins();
    if (infos.size() != 3 || !std::all_of(infos.cbegin(), infos.cend(), [](const PluginInfo& info) {
            return info.installed && info.loaded && !info.enabled;
        }) || setEnabled(clip, true, error)) {
        stop();
        return fail(QStringLiteral("安装后的加载状态或前置启用检查不正确"));
    }
    QJsonArray history;
    for (int i = 0; i < 38; ++i) {
        history.append(QJsonObject{{"when", QDateTime::currentDateTime().addSecs(-i).toString(Qt::ISODate)},
            {"hash", QString::number(i)}, {"text", QStringLiteral("记录 %1").arg(i)}, {"lan", i % 2 == 1}});
    }
    QFile historyFile(QDir(Utils::dataDir()).filePath(QStringLiteral("clipboard.json")));
    if (!QDir().mkpath(Utils::dataDir()) || !historyFile.open(QIODevice::WriteOnly) ||
        historyFile.write(QJsonDocument(history).toJson()) < 0) {
        stop();
        return fail(QStringLiteral("不能写入剪贴板历史夹具"));
    }
    historyFile.close();
    bool ok = false;
    {
        SearchWindow window;
        auto* input = window.findChild<QLineEdit*>(QStringLiteral("input"));
        if (input) {
            const auto special = [&](const QString& text) {
                input->clear();
                input->setText(text);
                window.flushSearch();
                for (int i = 0; i < window.resultCount(); ++i) {
                    const auto& row = window.resultAt(i);
                    if (row.title.startsWith(QStringLiteral("当前剪贴板 ·")) || row.kind == SearchResult::NativePeer ||
                        row.title == QStringLiteral("没有可发送的已配对设备"))
                        return true;
                }
                return false;
            };
            const auto clipboardRows = [&](const char* prefix) {
                int count = 0;
                for (int i = 0; i < window.resultCount(); ++i)
                    count += window.resultAt(i).target.startsWith(QLatin1String(prefix));
                return count;
            };
            auto* list = window.findChild<QListWidget*>(QStringLiteral("list"));
            const auto activate = [&](const QString& target) {
                if (!list) return false;
                for (int i = 0; i < window.resultCount(); ++i) {
                    if (window.resultAt(i).target != target) continue;
                    list->setCurrentRow(i);
                    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                    QCoreApplication::sendEvent(input, &enter);
                    return true;
                }
                return false;
            };
            const auto verifyHistory = [&] {
                const auto require = [&error](bool ok, const QString& step) {
                    if (!ok) error = QStringLiteral("剪贴板历史回归：") + step;
                    return ok;
                };
                auto& store = ClipboardStore::instance();
                if (!require(store.enabled(), QStringLiteral("新配置默认开启记录")))
                    return false;
                store.setEnabled(false);
                QApplication::clipboard()->setText(QStringLiteral("当前测试内容"));
                if (!require(special(QStringLiteral("clip")) && clipboardRows("clip:live:") == 1 &&
                    clipboardRows("clip:item:") == 5, QStringLiteral("首屏当前项与五条历史")) ||
                    !require(activate(QStringLiteral("clip:more")) && clipboardRows("clip:item:") == 30, QStringLiteral("打开二级页")) ||
                    !require(activate(QStringLiteral("clip:next")) && clipboardRows("clip:item:") == 38, QStringLiteral("加载下一批")))
                    return false;
                input->setText(QStringLiteral("局域网"));
                window.flushSearch();
                if (!require(clipboardRows("clip:item:") == 19, QStringLiteral("来源筛选"))) return false;
                auto* back = window.findChild<QToolButton*>(QStringLiteral("clipHistoryBack"));
                if (!require(back && !back->isHidden(), QStringLiteral("完整记录有可见返回按钮")))
                    return false;
                back->click();
                if (!require(back->isHidden() && input->text() == QStringLiteral("clip") &&
                             activate(QStringLiteral("clip:more")), QStringLiteral("按钮返回首屏并可再次打开")))
                    return false;
                QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QCoreApplication::sendEvent(list, &escape);
                if (!require(back->isHidden() && input->text() == QStringLiteral("clip") &&
                             activate(QStringLiteral("clip:item:text:1")), QStringLiteral("列表获得焦点后 Esc 返回并可恢复历史")))
                    return false;
                const auto* mime = QApplication::clipboard()->mimeData();
                if (!require(mime && mime->text() == QStringLiteral("记录 1") &&
                    mime->hasFormat(QLatin1String(kHotaruClipboardRestored)) &&
                    mime->hasFormat(QLatin1String(kHotaruClipboardReceived)), QStringLiteral("恢复标记")))
                    return false;
                auto* privateMime = new QMimeData;
                privateMime->setText(QStringLiteral("不应显示"));
                privateMime->setData(QLatin1String(kHotaruClipboardPrivate), "1");
                QApplication::clipboard()->setMimeData(privateMime);
                if (!require(special(QStringLiteral("clip")) && clipboardRows("clip:live:") == 1,
                             QStringLiteral("私密项不替换已观察的本机内容")))
                    return false;
                const auto receive = [](const QString& text) {
                    auto* mime = new QMimeData;
                    mime->setText(text);
                    mime->setData(QLatin1String(kHotaruClipboardReceived), "1");
                    QApplication::clipboard()->setMimeData(mime);
                };
                QMimeData delivered;
                delivered.setText(QStringLiteral("来自另一台设备"));
                delivered.setData(QLatin1String(kHotaruClipboardReceived), "1");
                // 模拟接收桥没有系统剪贴板变更通知的路径。
                store.recordReceived(delivered);
                QCoreApplication::processEvents();
                if (!require(!store.enabled() && store.items().size() == 38 &&
                             special(QStringLiteral("clip")) && clipboardRows("clip:live:") == 2,
                             QStringLiteral("关闭记录时同时显示本机、局域网和历史")))
                    return false;
                const auto live = store.liveItems();
                if (!require(live.size() == 2 && !live[0].item.fromLan && live[1].item.fromLan &&
                             live[0].item.text == QStringLiteral("当前测试内容"),
                             QStringLiteral("远端写入不会覆盖本机快照")))
                    return false;
                const auto localTarget = QStringLiteral("clip:live:") + ClipboardStore::liveId(live[0].item);
                if (!require(activate(localTarget) && QApplication::clipboard()->text() == live[0].item.text &&
                             QApplication::clipboard()->mimeData()->hasFormat(QLatin1String(kHotaruClipboardRestored)),
                             QStringLiteral("从融合列表恢复本机内容")))
                    return false;
                QApplication::clipboard()->setText(QStringLiteral("来自另一台设备"));
                if (!require(special(QStringLiteral("clip")) && clipboardRows("clip:live:") == 1,
                             QStringLiteral("本机与局域网同内容合并")))
                    return false;
                store.setEnabled(true);
                receive(QStringLiteral("新的局域网记录"));
                QCoreApplication::processEvents();
                if (!require(store.items().size() == 39 && store.items().front().fromLan &&
                             special(QStringLiteral("clip")) && clipboardRows("clip:live:") == 2 &&
                             clipboardRows("clip:item:") == 5,
                             QStringLiteral("局域网记录入库且首屏去重")))
                    return false;
                const auto stable = ClipboardStore::stableId(store.items().front());
                if (!require(store.restore(stable), QStringLiteral("按稳定标识恢复")))
                    return false;
                QCoreApplication::processEvents();
                if (!require(store.items().size() == 39, QStringLiteral("恢复不产生历史")))
                    return false;
                store.clear();
                if (!require(special(QStringLiteral("clip")) && activate(QStringLiteral("clip:more")) &&
                             clipboardRows("clip:item:") == 0 && !back->isHidden(), QStringLiteral("没有历史也可打开及返回完整记录")))
                    return false;
                back->click();
                SettingsDialog settings;
                if (!require(settings.openNativePlugin(QLatin1String(kHotaruLanClipboardId)),
                             QStringLiteral("打开剪贴板设置")))
                    return false;
                for (auto* frame : settings.findChildren<QFrame*>()) {
                    if (!frame->property("hotaruClipboardHistorySlot").toBool())
                        continue;
                    const auto rows = frame->parentWidget()->findChildren<QHBoxLayout*>();
                    const bool rightColumn = std::any_of(rows.cbegin(), rows.cend(), [frame](const auto* pair) {
                        return pair->indexOf(frame) == 1 && pair->itemAt(0)->widget() &&
                               pair->itemAt(0)->widget()->findChild<QLineEdit*>();
                    });
                    return require(rightColumn && frame->findChild<QCheckBox*>(),
                                   QStringLiteral("本机记录与同步快捷词同一行且在右栏"));
                }
                return require(false, QStringLiteral("缺少本机记录栏"));
            };
            const auto alias = QStringLiteral("lan-test-clip");
            const auto at = QStringLiteral("@设备");
            ok = !special(QStringLiteral("clip")) && !special(at) &&
                setEnabled(id, true, error) && setEnabled(clip, true, error) &&
                verifyHistory() && !special(at) &&
                setClipboardSearchAlias(alias, error) && special(alias) && !special(QStringLiteral("clip")) &&
                setEnabled(files, true, error) && special(at) &&
                setEnabled(clip, false, error) && !special(alias) && special(at) &&
                setEnabled(clip, true, error) && setEnabled(id, false, error) &&
                !special(alias) && !special(at) && setEnabled(id, true, error) &&
                special(alias) && special(at) && setEnabled(id, false, error);
        }
    }
    stop();
    if (!ok)
        return fail(error.isEmpty() ? QStringLiteral("前置启停或剪贴板历史回归失败") : error);
    return true;
}
}

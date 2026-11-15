#pragma once

#include <QtPlugin>
#include <QImage>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

class QWidget;
class QMimeData;

// Hotaru 直属插件接口。与 ltool API v4 独立，插件只链接 Qt 和自己的运行库。
inline constexpr int kHotaruNativePluginApi = 1;
#define HOTARU_NATIVE_PLUGIN_IID "app.hotaru.NativePlugin/1.0"
inline constexpr char kHotaruClipboardPrivate[] = "application/x-hotaru-clipboard-private";
inline constexpr char kHotaruClipboardReceived[] = "application/x-hotaru-clipboard-received";
inline constexpr char kHotaruClipboardRestored[] = "application/x-hotaru-clipboard-restored";
inline constexpr int kHotaruFeatureClipboard = 1;
inline constexpr int kHotaruFeatureFiles = 2;

class HotaruNativePlugin {
public:
    virtual ~HotaruNativePlugin() = default;
    virtual bool start(const QString& dataDirectory) = 0;
    virtual void stop() = 0;
    virtual QString state(const QString& id) const = 0;
    virtual QWidget* createPage(const QString& id, QWidget* parent) = 0;
    virtual void setFileOfferNotifier(std::function<void(QString)> callback) = 0;
};

Q_DECLARE_INTERFACE(HotaruNativePlugin, HOTARU_NATIVE_PLUGIN_IID)

// V1 只负责页面生命周期。新增能力放在可选 V2 接口，避免改变已有插件的 vtable。
inline constexpr int kHotaruNativePluginV2Api = 2;
#define HOTARU_NATIVE_PLUGIN_V2_IID "app.hotaru.NativePlugin/2.0"

struct HotaruNativePeer {
    QByteArray id;
    QString name;
    QString address;
    int features = 0;
    QString state;
    qint64 lastSeen = 0;
};

struct HotaruClipboardSnapshot {
    enum Kind { Empty, Text, Image } kind = Empty;
    QString textPreview;
    QImage image;
    qint64 bytes = 0;
    QString connectionState;
};

class HotaruNativePluginV2 {
public:
    virtual ~HotaruNativePluginV2() = default;
    virtual bool enabled(const QString& id) const = 0;
    virtual bool setEnabled(const QString& id, bool enabled, QString& error) = 0;
    virtual QVector<HotaruNativePeer> peers(int feature) const = 0;
    virtual HotaruClipboardSnapshot clipboardSnapshot() const = 0;
    virtual bool sendFiles(const QByteArray& peerId, const QStringList& paths,
                           QString& error) = 0;
    virtual QString clipboardSearchAlias() const = 0;
    virtual bool setClipboardSearchAlias(const QString& alias, QString& error) = 0;
    virtual void setChangedNotifier(std::function<void()> callback) = 0;
};

Q_DECLARE_INTERFACE(HotaruNativePluginV2, HOTARU_NATIVE_PLUGIN_V2_IID)

// 局域网前置与功能包独立加载；旧接口布局保持不变。
inline constexpr int kHotaruLanPackageApi = 1;
inline constexpr char kHotaruLanCoreId[] = "lan-core";
inline constexpr char kHotaruLanClipboardId[] = "lan-clipboard";
inline constexpr char kHotaruLanFilesId[] = "lan-files";
#define HOTARU_LAN_PACKAGE_IID "app.hotaru.LanPackage/1.0"

class HotaruLanPackage {
public:
    virtual ~HotaruLanPackage() = default;
    virtual QString id() const = 0;
    virtual int feature() const = 0;
    virtual void setAvailableFeatures(int) {}
    virtual bool hasPendingFileOffer() const { return false; }
    virtual QWidget* createFeaturePage(const QString&, const QByteArray&, QWidget*) { return nullptr; }
};

Q_DECLARE_INTERFACE(HotaruLanPackage, HOTARU_LAN_PACKAGE_IID)

// 可选接口：把已解码的接收内容交给宿主，不依赖 Windows 的剪贴板通知。
#define HOTARU_LAN_CLIPBOARD_RECEIVE_IID "app.hotaru.LanClipboardReceive/1.0"
class HotaruLanClipboardReceive {
public:
    virtual ~HotaruLanClipboardReceive() = default;
    virtual void setClipboardReceiveNotifier(std::function<void(const QMimeData&)> callback) = 0;
};
Q_DECLARE_INTERFACE(HotaruLanClipboardReceive, HOTARU_LAN_CLIPBOARD_RECEIVE_IID)

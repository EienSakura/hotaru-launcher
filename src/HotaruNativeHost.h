#pragma once
#include "hotaru-native-plugin.h"
#include <QString>
#include <QVector>
#include <functional>

class QWidget;
class QObject;
namespace HotaruNativeHost {
struct PluginInfo {
    QString id;
    QString name;
    QString description;
    QString version;
    QString dependency;
    QString category, source;
    bool preview = false, pending = false, removable = false;
    bool installed = false;
    bool enabled = false;
    bool verified = false;
    bool loaded = false;
    bool dependencyReady = true;
    QString state;
};

void start(std::function<void(QString)> fileOffer = {});
bool tryActivateFirstInstall(QString& error);
void stop();
QString state(const QString& id);
bool enabled(const QString& id);
bool hasPendingFileOffer();
QWidget* createPage(const QString& id, QWidget* parent, const QByteArray& target = {});
QVector<PluginInfo> plugins();
using ConfirmReplace = std::function<bool(const QString&)>;
bool importPackage(const QString& path, QString& error, const ConfirmReplace& confirm = {});
bool uninstall(const QString& id, QStringList& affected, bool confirm, QString& error);
bool previewPackageSelftest(const QStringList& archives, QString& error);
bool setEnabled(const QString& id, bool enabled, QString& error);
QVector<HotaruNativePeer> peers(int feature);
HotaruClipboardSnapshot clipboardSnapshot();
bool sendFiles(const QByteArray& peerId, const QStringList& paths, QString& error);
QString clipboardSearchAlias();
bool setClipboardSearchAlias(const QString& alias, QString& error);
void inventoryChanged();
void subscribeInventory(QObject* context, std::function<void()> callback);
void subscribeChanged(QObject* context, std::function<void()> callback);
}

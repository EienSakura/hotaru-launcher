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
    bool installed = false;
    bool enabled = false;
    bool verified = false;
    QString state;
};

void start(std::function<void(QString)> fileOffer = {});
void stop();
QString state(const QString& id);
QWidget* createPage(const QString& id, QWidget* parent);
QVector<PluginInfo> plugins();
bool importPackage(const QString& path, QString& error);
bool setEnabled(const QString& id, bool enabled, QString& error);
QVector<HotaruNativePeer> peers(int feature);
HotaruClipboardSnapshot clipboardSnapshot();
bool sendFiles(const QByteArray& peerId, const QStringList& paths, QString& error);
QString clipboardSearchAlias();
bool setClipboardSearchAlias(const QString& alias, QString& error);
void subscribeChanged(QObject* context, std::function<void()> callback);
}

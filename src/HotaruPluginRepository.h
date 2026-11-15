#pragma once

#include <QDateTime>
#include <QMap>
#include <QObject>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
namespace HotaruPluginRepository {
struct File {
    QString name;
    qint64 size = 0;
    QByteArray hash;
    bool operator==(const File&) const = default;
};
struct Range {
    QString min, max;
    bool operator==(const Range&) const = default;
};
struct Entry {
    QString id, version, name, description, category, channel, iid, abi, tag;
    int api = 0;
    Range host;
    QMap<QString, Range> requiresPackages;
    File asset;
    QList<File> files;
    QString dll() const;
};
struct Catalog {
    int sequence = 0;
    QDateTime expires;
    QList<Entry> entries;
    QByteArray bytes, signatures;
};
struct Installed {
    Entry entry;
    QString source, folder;
};
using Inventory = QMap<QString, Installed>;
struct InstallPlan {
    QList<Entry> downloads;
    Inventory inventory;
    QString basis;
    qint64 totalBytes = 0;
};

QString abi();
QString root();
bool compatible(const Entry& entry);
bool visible(const Entry& entry, bool development);
QList<Entry> available(const Catalog& catalog, bool development);
bool sameVersion(const QString& left, const QString& right);
QString storagePath(const Installed& item);
QString dllPath(const Installed& item);
bool readLocalPackage(const QString& path, const QString& output, Entry& entry, QString& error);
bool extractPackage(const QString& path, const QString& output, QString& error);
bool validateInventory(const Inventory& items, QString& error);
QStringList loadOrder(const Inventory& items);
bool planInstall(const QString& id, const Catalog& catalog, const Inventory& current, bool development,
                 InstallPlan& plan, QString& error, const QStringList& bundled = {});
Inventory active();
Inventory desired();
bool pending();
bool queue(const Inventory& items, QString& error);
bool queueLocal(const Entry& entry, const QString& staged, bool replace, QString& error);
bool remove(const QString& id, QStringList& affected, bool confirm, QString& error);
void activate(const std::function<bool(const Inventory&, bool trial, QString&)>& load);
QString activationError();
bool selftest(const QStringList& paths, QString& error);

class Client : public QObject {
    Q_OBJECT
public:
    explicit Client(QObject* parent);
    Catalog catalog() const;
    QString status() const { return m_status; }
    bool busy() const { return m_busy; }
    void refresh(bool force = false);
    void install(const InstallPlan& plan, bool activateFirst = false);
    void cancel();
signals:
    void changed();
    void progress(qint64 received, qint64 total);
    void installed();
private:
    using Consume = std::function<bool(const QByteArray&)>;
    using Done = std::function<void(QString)>;
    void fetch(const QUrl& url, qint64 limit, const Consume& consume, const Done& done,
               int hops = 0, const QString& mirrorHost = {});
    void readCache();
    void finish(const QString& status);
    QNetworkAccessManager* m_network;
    QNetworkReply* m_reply = nullptr;
    Catalog m_catalog;
    QDateTime m_checked;
    QString m_status;
    bool m_busy = false;
    bool m_cancelled = false;
};
}

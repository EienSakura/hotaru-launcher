#pragma once
#include "LanNetwork.h"
#include "LanTransfer.h"
#include "hotaru-native-plugin.h"

class QMimeData;
namespace HotaruLan {
enum Feature : int { Clipboard = 1, Files = 2 };
QByteArray encodeClipboard(const QMimeData& mime);
std::unique_ptr<QMimeData> decodeClipboard(Wire type, QByteArrayView data);

class Service final : public QObject {
    Q_OBJECT
public:
    struct Device {
        QByteArray id;
        QString name;
        QString address;
        int features = 0;
        qint64 lastSeen = 0;
    };
    struct Discovery {
        QByteArray id;
        QString address;
        int features = 0;
        qint64 lastSeen = 0;
        bool paired = false;
        Network::Phase phase = Network::Phase::Waiting;
    };
    explicit Service(QString dataPath, QObject* parent = nullptr);
    ~Service() override;
    QString name() const { return m_name; }
    bool hasCode() const { return !m_code.isEmpty(); }
    bool enabled(int feature) const { return m_features & feature; }
    QString status() const;
    QString diagnosticDirectory() const;
    QList<Device> devices() const;
    QList<Discovery> discoveredDevices() const;
    bool probeAddress(const QString& address, QString& error);
    Transfers& transfers() { return m_transfers; }
    bool configure(const QString& name, const QString& newCode, QString& error);
    bool setEnabled(int feature, bool enabled, QString& error);
    void copyCode();
    void share(const QByteArray& id, const QStringList& paths);
    bool sendFiles(const QByteArray& id, const QStringList& paths, QString& error);
    HotaruClipboardSnapshot clipboardSnapshot() const;
    QString clipboardSearchAlias() const { return m_clipboardAlias; }
    bool setClipboardSearchAlias(const QString& alias, QString& error);

signals:
    void changed();
    void incomingOffer();

private:
    struct Session { QString name; int features = 0; qint64 seen = 0; qint64 clipboardAt = -1000; };
    void load();
    void writeDiagnostic(const QString& message) const;
    bool save(const QString& name, QByteArrayView code, int features);
    void restart();
    void onConnected(Peer* peer);
    void onMessage(Peer* peer, QByteArrayView data);
    void capture();
    bool applyClipboard(Wire type, QByteArrayView data);
    void pulse();
    QString m_path, m_name, m_error, m_clipboardAlias = QStringLiteral("jtb");
    QByteArray m_code;
    int m_features = 0;
    Network m_network;
    Transfers m_transfers;
    QHash<Peer*, Session> m_sessions;
    QHash<QByteArray, QHostAddress> m_knownPeers;
    QTimer m_pulse;
    QElapsedTimer m_clock;
    quint32 m_clipSequence = 0;
    bool m_writingClipboard = false;
};
}

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
        QString name;
    };
    explicit Service(QString dataPath, QObject* parent = nullptr);
    ~Service() override;
    QString name() const { return m_name; }
    bool coreEnabled() const { return m_coreEnabled; }
    bool enabled(int feature) const { return (activeFeatures() & feature) != 0; }
    bool requested(int feature) const { return (m_features & feature) != 0; }
    bool setCoreEnabled(bool on, QString& error);
    void setAvailableFeatures(int features);
    QString status() const;
    QString diagnosticDirectory() const;
    QList<Device> devices() const;
    QList<Device> pairedDevices() const;
    QList<Discovery> discoveredDevices() const;
    bool probeAddress(const QString& address, QString& error);
    Transfers& transfers() { return m_transfers; }
    bool configure(const QString& name, QString& error);
    bool setEnabled(int feature, bool enabled, QString& error);
    QByteArray invitationCode() const { return m_network.invitationCode(); }
    bool invitationInProgress() const { return m_network.invitationInProgress(); }
    QStringList invitationAddresses() const { return m_network.localAddresses(); }
    bool beginInvitation(QString& error);
    void cancelInvitation() { m_network.endInvitation(); }
    bool joinInvitation(const QString& address, const QString& code, QString& error);
    QString pairingRequest() const;
    QString pairingStatus() const { return m_pairingStatus; }
    bool decidePairing(bool accept, QString& error);
    bool forgetDevice(const QByteArray& id, QString& error);
    QString downloadDirectory() const;
    bool setDownloadDirectory(const QString& directory, QString& error);
    bool receiveFiles(const QString& directory, QString& error);
    void share(const QByteArray& id, const QStringList& paths);
    bool sendFiles(const QByteArray& id, const QStringList& paths, QString& error);
    HotaruClipboardSnapshot clipboardSnapshot() const;
    QString clipboardSearchAlias() const { return m_clipboardAlias; }
    bool setClipboardSearchAlias(const QString& alias, QString& error);

signals:
    void changed();
    void clipboardReceived(const QMimeData& mime);
    void incomingOffer();

private:
    friend struct NetworkSelftest;
    struct Session { QString name; int features = 0; qint64 seen = 0; qint64 clipboardAt = -1000; };
    struct KnownPeer { QString name; QHostAddress address; QByteArray key; bool pending = true; };
    enum class PairStep { Hello, WaitForUser, WaitForKey, WaitForSaved, WaitForCommit, WaitForReceipt, Done };
    struct Pairing { QString name; PairStep step = PairStep::Hello; QByteArray key; };
    void load();
    void writeDiagnostic(const QString& message) const;
    bool save();
    int activeFeatures() const { return m_coreEnabled ? m_features & m_available : 0; }
    void broadcastFeatures();
    void refreshCachedAddresses();
    bool savePair(Peer* peer, const Pairing& pair);
    void onPairing(Peer* peer, Wire type, QByteArrayView data);
    void restart();
    void onConnected(Peer* peer);
    void onMessage(Peer* peer, QByteArrayView data);
    void capture();
    bool applyClipboard(Wire type, QByteArrayView data);
    void pulse();
    QString m_path, m_name, m_error, m_pairingStatus, m_downloadDirectory;
    QString m_clipboardAlias = QStringLiteral("clip");
    QByteArray m_identity;
    int m_features = 0, m_available = 0;
    bool m_coreEnabled = false, m_settingsReadable = true;
    Network m_network;
    Transfers m_transfers;
    QHash<Peer*, Session> m_sessions;
    QHash<QByteArray, KnownPeer> m_knownPeers;
    QHash<Peer*, Pairing> m_pairing;
    QTimer m_pulse;
    QElapsedTimer m_clock;
    quint32 m_clipSequence = 0;
    bool m_writingClipboard = false;
};
}

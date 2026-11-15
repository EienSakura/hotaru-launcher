#pragma once
#include "LanPeer.h"
#include <QElapsedTimer>
#include <QHostAddress>
#include <QHash>
#include <QNetworkInterface>
#include <QPointer>
#include <QSet>
#ifdef Q_OS_WIN
#include <windows.h>
#include <windns.h>
#endif
#include <QTcpServer>
#include <QUdpSocket>

namespace HotaruLan {
class Network final : public QObject {
    Q_OBJECT
public:
    enum class Phase { Waiting, Connecting, Authenticating, Failed, Rejected };
    struct Discovery {
        QByteArray id;
        QHostAddress address;
        quint16 port = 0;
        int interfaceIndex = 0;
        int features = 0;
        qint64 lastSeen = 0;
        qint64 firstSeen = 0;
        Phase phase = Phase::Waiting;
        QString name;
        bool operator==(const Discovery&) const = default;
    };
    explicit Network(QObject* parent = nullptr);
    ~Network() override;
    void start(const QByteArray& identity);
    void stop();
    void setCredentialLookup(std::function<QByteArray(const QByteArray&)> lookup);
    QByteArray beginInvitation();
    void setInvitationName(const QString& name);
    void endInvitation();
    void consumeInvitation();
    bool invitationActive() const;
    QByteArray invitationCode() const { return invitationActive() ? m_inviteCode : QByteArray{}; }
    bool invitationInProgress() const { return m_inviteTimer.isActive(); }
    QStringList localAddresses() const;
    bool joinInvitation(const QHostAddress& address, const QByteArray& code, QString& error);
    void forget(const QByteArray& id);
    void setCachedAddresses(const QList<QHostAddress>& addresses);
    QString status() const { return m_status; }
    QList<Discovery> discoveries() const;
    bool probe(const QHostAddress& address, QString& error);

signals:
    void connected(Peer* peer);
    void disconnected(Peer* peer);
    void changed();
    void diagnostic(const QString& message);
    void invitationFailed();

private:
    friend struct NetworkSelftest;
    struct Address {
        int index;
        QHostAddress ip;
        int prefix;
        int windowsIndex = 0;
        bool operator==(const Address&) const = default;
    };
    struct Endpoint { Address address; QTcpServer* listener; bool multicastJoined = false; bool multicastAttempted = false; };
    struct Attempt { qint64 next; };
    static QList<Address> privateAddresses(QString& error);
    void openTopology(const QList<Address>& addresses, quint16 port);
    QString listenerStatus() const;
    void tick();
    void readDiscovery();
    void closeTopology();
    bool attempt(int iface, const QHostAddress& source, bool outgoing);
    void tryCandidate(const Discovery& candidate, qint64 now);
    void retryCandidates(qint64 now);
    void setPhase(const QByteArray& id, Phase phase);
    bool registerAttempt(int iface, const QHostAddress& destination);
    bool registerProbe(const Address& endpoint, const QHostAddress& destination, bool manual = false);
    void classifyIncoming(QTcpSocket* socket, const Address& address);
    void adopt(QTcpSocket* socket, const QHostAddress& source, QByteArray peerId,
               QByteArray inviteCode = {});
    bool connectCandidate(const Discovery& candidate, QByteArray inviteCode = {});
    void setStatus(QString value);
    void recordDiscovery(const QByteArray& id, const QHostAddress& address, quint16 port,
                         int interfaceIndex, int features = 0, QString name = {});
    QByteArray advertisement(quint16 port) const;
    void sendAnnouncement(const Endpoint& endpoint, const QHostAddress& destination,
                          quint16 destinationPort);
#ifdef Q_OS_WIN
    struct DnsRegistration;
    struct DnsResolveContext;
    struct DnsBrowseContext;
    void startDnsSd();
    void stopDnsSd();
    void finishDnsRegistration(DnsRegistration* registration, DWORD status);
    void finishDnsBrowse(DnsBrowseContext* context);
    void resolveDnsService(const QString& name);
    void adoptDnsEndpoint(const QHostAddress& source, quint16 port, const QByteArray& peerId, int flags, const QString& name);
    static VOID WINAPI dnsBrowseCallback(DWORD status, PVOID context, PDNS_RECORD record);
    static VOID WINAPI dnsResolveCallback(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance);
    static VOID WINAPI dnsRegisterCallback(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance);
    QVector<DnsRegistration*> m_dnsRegistrations;
    QVector<DnsResolveContext*> m_dnsResolves;
    DnsBrowseContext* m_dnsBrowseContext = nullptr;
    DNS_SERVICE_BROWSE_REQUEST* m_dnsBrowseRequest = nullptr;
    bool m_dnsBrowseStarted = false;
    bool m_dnsReady = false;
    qint64 m_lastDnsAttempt = -15000;
#endif
    QUdpSocket m_udp;
    QTimer m_timer;
    QElapsedTimer m_clock;
    QList<Address> m_addresses;
    QList<Endpoint> m_endpoints;
    QList<QHostAddress> m_cachedAddresses;
    QSet<Peer*> m_peers;
    QHash<QByteArray, Discovery> m_discoveries;
    QHash<QHostAddress, Attempt> m_incomingAttempts, m_outgoingAttempts;
    QHash<QHostAddress, Attempt> m_registerAttempts;
    QSet<QTcpSocket*> m_registerSockets;
    QSet<QTcpSocket*> m_pendingIncoming;
    QHash<int, QList<qint64>> m_interfaceAttempts;
    QHash<int, QList<qint64>> m_registerInterfaceAttempts;
    QList<qint64> m_totalAttempts;
    QList<qint64> m_registerTotalAttempts;
    QByteArray m_ownId, m_inviteCode, m_joinCode;
    QHostAddress m_joinAddress;
    QTimer m_inviteTimer, m_joinTimer;
    int m_inviteAttempts = 0;
    std::function<QByteArray(const QByteArray&)> m_credential;
    QString m_status, m_tcpError, m_udpError, m_invitationName;
    qint64 m_lastReject = -60000;
};
}

#pragma once
#include "LanCrypto.h"
#include <QTcpSocket>
#include <QTimer>
#include <functional>

namespace HotaruLan {
class Peer final : public QObject {
    Q_OBJECT
public:
    Peer(QTcpSocket* socket, QByteArray ownId, QByteArray expectedId, QByteArray passcode,
         QObject* parent, bool invite = false,
         std::function<QByteArray(const QByteArray&, bool)> credential = {});
    void start();
    QByteArray peerId() const { return m_peerId; }
    QHostAddress address() const;
    bool initiatedLocally() const { return m_initiatedLocally; }
    bool invitation() const { return m_invite; }
    bool usedInvitation() const { return m_usedInvitation; }
    bool ready() const { return m_step == Ready; }
    qint64 pendingBytes() const { return m_socket->bytesToWrite(); }
    bool send(QByteArrayView payload);
    void close();

signals:
    void authenticated();
    void message(QByteArray data);
    void drained();
    void ended(bool rejected);

private:
    enum Step { Connecting, ExpectInitiator, ExpectResponder, ExpectProof, Ready, Closed };
    void begin();
    void receive();
    bool writeFrame(QByteArrayView data);
    bool process(QByteArrayView frame);
    void finish(bool rejected);
    QTcpSocket* m_socket;
    const QByteArray m_ownId;
    const bool m_initiatedLocally;
    QByteArray m_peerId;
    QByteArray m_passcode;
    bool m_invite = false, m_usedInvitation = false;
    std::function<QByteArray(const QByteArray&, bool)> m_credential;
    std::unique_ptr<SecureChannel> m_channel;
    QTimer m_timeout;
    Step m_step;
};
}

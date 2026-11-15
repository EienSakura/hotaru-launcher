#include "LanPeer.h"
#include <QHostAddress>
#include <QtEndian>

namespace HotaruLan {
namespace {
constexpr int kHelloBytes = 4 + 2 * kIdentityBytes + kShareBytes;
constexpr int kQueueBudget = 2 * kMaxPlaintext;
const QByteArray kInvitationWire("HL2I");
}

Peer::Peer(QTcpSocket* socket, QByteArray ownId, QByteArray expectedId, QByteArray passcode,
           QObject* parent, bool invite, std::function<QByteArray(const QByteArray&, bool)> credential)
    : QObject(parent), m_socket(socket), m_ownId(std::move(ownId)),
      m_initiatedLocally(!expectedId.isEmpty()), m_peerId(std::move(expectedId)), m_passcode(std::move(passcode)),
      m_invite(invite), m_credential(std::move(credential)), m_step(m_initiatedLocally ? Connecting : ExpectInitiator)
{
    m_socket->setParent(this);
    m_socket->setReadBufferSize(kMaxPlaintext + kTagBytes + 4);
    connect(m_socket, &QTcpSocket::connected, this, &Peer::begin);
    connect(m_socket, &QTcpSocket::readyRead, this, &Peer::receive);
    connect(m_socket, &QTcpSocket::bytesWritten, this, [this] {
        if (ready() && pendingBytes() < 256 * 1024)
            emit drained();
    });
    connect(m_socket, &QTcpSocket::disconnected, this, [this] { finish(false); });
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this] { finish(false); });
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] { finish(false); });
}

QHostAddress Peer::address() const { return m_socket->peerAddress(); }

void Peer::start()
{
    m_timeout.start(8000);
    if (m_step == ExpectInitiator)
        receive();
}

void Peer::begin()
{
    if (m_step != Connecting)
        return;
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    const auto secret = m_invite ? QByteArrayLiteral("invite:") + m_passcode : m_passcode;
    m_channel = std::make_unique<SecureChannel>(true, m_ownId, m_peerId, secret);
    m_passcode.fill(0);
    m_passcode.clear();
    m_step = ExpectResponder;
    const auto share = m_channel->share();
    if (share.size() != kShareBytes ||
        !writeFrame((m_invite ? kInvitationWire : QByteArray(kWireVersion)) + m_ownId + m_peerId + share))
        finish(true);
}

bool Peer::writeFrame(QByteArrayView data)
{
    char prefix[4];
    qToBigEndian(quint32(data.size()), prefix);
    return m_socket->write(prefix, sizeof(prefix)) == sizeof(prefix) &&
           m_socket->write(data.data(), data.size()) == data.size();
}

bool Peer::send(QByteArrayView payload)
{
    if (!ready() || payload.isEmpty() || payload.size() > kMaxPlaintext ||
        pendingBytes() + payload.size() + kTagBytes + 4 > kQueueBudget)
        return false;
    const auto frame = m_channel->seal(payload);
    if (frame.isEmpty() || !writeFrame(frame)) {
        finish(false);
        return false;
    }
    return true;
}

void Peer::receive()
{
    for (int frames = 0; frames < 16; ++frames) {
        if (m_step == Closed || m_socket->bytesAvailable() < 4)
            return;
        const auto prefix = m_socket->peek(4);
        if (prefix.size() != 4)
            return;
        const quint32 size = qFromBigEndian<quint32>(prefix.constData());
        const bool valid = m_step == Ready ? size > kTagBytes && size <= kMaxPlaintext + kTagBytes :
                           m_step == ExpectProof ? size == kProofBytes :
                           m_step == ExpectResponder ? size == kHelloBytes + kProofBytes :
                           m_step == ExpectInitiator && size == kHelloBytes;
        if (!valid) {
            finish(true);
            return;
        }
        if (m_socket->bytesAvailable() < qint64(size) + 4)
            return;
        m_socket->read(4);
        const auto frame = m_socket->read(size);
        if (frame.size() != size || !process(frame)) {
            finish(true);
            return;
        }
    }
    if (m_step != Closed && m_socket->bytesAvailable() >= 4)
        QTimer::singleShot(0, this, &Peer::receive);
}

bool Peer::process(QByteArrayView frame)
{
    if (m_step == Ready) {
        const auto payload = m_channel->open(frame);
        if (payload.isEmpty())
            return false;
        emit message(payload);
        return true;
    }
    if (m_step == ExpectProof) {
        if (!m_channel->confirm(frame))
            return false;
    } else {
        if (m_step == ExpectInitiator)
            m_invite = frame.first(4) == QByteArrayView(kInvitationWire);
        const auto wire = m_invite ? kInvitationWire : QByteArray(kWireVersion);
        if (frame.first(4) != QByteArrayView(wire) ||
            frame.sliced(4 + kIdentityBytes, kIdentityBytes) != QByteArrayView(m_ownId))
            return false;
        const auto remote = frame.sliced(4, kIdentityBytes).toByteArray();
        const auto share = frame.sliced(4 + 2 * kIdentityBytes, kShareBytes);
        if (m_step == ExpectInitiator) {
            if (remote == m_ownId)
                return false;
            m_peerId = remote;
            if (m_credential)
                m_passcode = m_credential(remote, m_invite);
            if (m_passcode.isEmpty())
                return false;
            m_usedInvitation = m_invite;
            const auto secret = m_invite ? QByteArrayLiteral("invite:") + m_passcode : m_passcode;
            m_channel = std::make_unique<SecureChannel>(false, m_ownId, m_peerId, secret);
            m_passcode.fill(0);
            m_passcode.clear();
            if (!m_channel->derive(share))
                return false;
            const auto proof = m_channel->proof();
            m_step = ExpectProof;
            return proof.size() == kProofBytes &&
                   writeFrame(wire + m_ownId + m_peerId +
                              m_channel->share() + proof);
        }
        if (remote != m_peerId || !m_channel->derive(share) ||
            !m_channel->confirm(frame.last(kProofBytes)))
            return false;
        const auto proof = m_channel->proof();
        if (proof.size() != kProofBytes || !writeFrame(proof))
            return false;
    }
    m_step = Ready;
    m_timeout.stop();
    emit authenticated();
    return true;
}

void Peer::close() { finish(false); }

void Peer::finish(bool rejected)
{
    if (m_step == Closed)
        return;
    m_step = Closed;
    m_timeout.stop();
    m_passcode.fill(0);
    m_passcode.clear();
    m_channel.reset();
    m_socket->abort();
    emit ended(rejected);
}
}

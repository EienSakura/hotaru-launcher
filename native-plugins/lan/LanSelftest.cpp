#include "LanService.h"
#include "hotaru-native-plugin.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QHBoxLayout>
#include <QFrame>
#include <QMimeData>
#include <QNetworkProxy>
#include <QPluginLoader>
#include <QThread>
#include <QTemporaryDir>
#include <QWidget>
#include <QtEndian>
#include <cstdio>
#include <functional>

namespace HotaruLan {
struct NetworkSelftest {
    enum class Link { Mutual, ReverseOnly, OneSided, Crossed, WrongCode };

    static bool automaticAuthentication(Link link, const std::function<bool(const std::function<bool()>&)>& wait)
    {
        Network lower, higher;
        const QHostAddress loopback(QHostAddress::LocalHost);
        lower.m_ownId = QByteArray(kIdentityBytes, 'a');
        higher.m_ownId = QByteArray(kIdentityBytes, 'b');
        lower.setCredentialLookup([&](const QByteArray& id) {
            return id == higher.m_ownId ? QByteArrayLiteral("sample-test-code") : QByteArray{};
        });
        higher.setCredentialLookup([&](const QByteArray& id) {
            return id != lower.m_ownId ? QByteArray{} :
                link == Link::WrongCode ? QByteArrayLiteral("another-test-code") : QByteArrayLiteral("sample-test-code");
        });
        lower.openTopology({{0, loopback, 8}}, 0);
        higher.openTopology({{0, loopback, 8}}, 0);
        if (lower.m_endpoints.isEmpty() || higher.m_endpoints.isEmpty())
            return false;
        const auto lowerPort = lower.m_endpoints.front().listener->serverPort();
        const auto higherPort = higher.m_endpoints.front().listener->serverPort();
        // 模拟较大 ID 的入站不可达，但它仍能从本地地址发起连接。
        if (link == Link::ReverseOnly)
            higher.m_endpoints.front().listener->close();
        if (link != Link::OneSided)
            lower.recordDiscovery(higher.m_ownId, loopback, higherPort, 0);
        higher.recordDiscovery(lower.m_ownId, loopback, lowerPort, 0);
        if (higher.m_discoveries.value(lower.m_ownId).phase != Network::Phase::Waiting ||
            !higher.m_peers.isEmpty())
            return false;

        const auto bothReady = [&] {
            return lower.m_peers.size() == 1 && higher.m_peers.size() == 1 &&
                   (*lower.m_peers.cbegin())->ready() && (*higher.m_peers.cbegin())->ready();
        };
        if (link == Link::WrongCode) {
            return wait([&] {
                return lower.m_discoveries.value(higher.m_ownId).phase == Network::Phase::Rejected &&
                       lower.m_peers.isEmpty() && higher.m_peers.isEmpty();
            });
        }
        if (link == Link::ReverseOnly && !wait([&] {
                return lower.m_discoveries.value(higher.m_ownId).phase == Network::Phase::Failed;
            }))
            return false;

        const bool reverse = link == Link::OneSided || link == Link::ReverseOnly;
        if (reverse || link == Link::Crossed) {
            // 推进候选的等待时间，由定时重试入口再次发起，避免等待真实时钟。
            higher.m_discoveries[lower.m_ownId].firstSeen -= 5000;
            higher.retryCandidates(higher.m_clock.elapsed());
            if (higher.m_peers.size() != 1)
                return false;
        }
        if (!wait(bothReady))
            return false;
        auto* lowerPeer = *lower.m_peers.cbegin();
        auto* higherPeer = *higher.m_peers.cbegin();
        if (lowerPeer->initiatedLocally() == reverse || higherPeer->initiatedLocally() != reverse)
            return false;
        // 两端保留的必须是同一条会话，能互相传递加密记录。
        QByteArray atLower, atHigher;
        QObject::connect(lowerPeer, &Peer::message, &lower, [&](QByteArray data) { atLower = data; });
        QObject::connect(higherPeer, &Peer::message, &higher, [&](QByteArray data) { atHigher = data; });
        return lowerPeer->send("lower") && higherPeer->send("higher") &&
               wait([&] { return atLower == "higher" && atHigher == "lower"; });
    }

    static bool hasPrivateAddress(const QHostAddress& expected)
    {
        QString status;
        const auto addresses = Network::privateAddresses(status);
        std::printf("%s\n", qPrintable(status));
        for (const auto& address : addresses) {
            if (address.ip == expected) {
                std::printf("PASS private IPv4 %s/%d (Qt %d, Windows %d)\n",
                            qPrintable(address.ip.toString()), address.prefix,
                            address.index, address.windowsIndex);
                return true;
            }
        }
        std::printf("FAIL expected private IPv4 not selected: %s\n", qPrintable(expected.toString()));
        return false;
    }

    enum class PairCase { Accept, Reject, Expire, WrongCode, SaveFails };

    static bool invitation(PairCase action, const std::function<bool(const std::function<bool()>&)>& wait)
    {
        QTemporaryDir data;
        const auto aPath = data.filePath(QStringLiteral("a")), bPath = data.filePath(QStringLiteral("b"));
        Service a(aPath), b(bPath);
        QString error;
        if (!a.configure(QStringLiteral("设备 A"), error) || !b.configure(QStringLiteral("设备 B"), error))
            return false;
        const auto aId = a.m_identity, bId = b.m_identity;
        auto& lower = a.m_network;
        auto& higher = b.m_network;
        const QHostAddress loopback(QHostAddress::LocalHost);
        lower.m_ownId = aId;
        higher.m_ownId = bId;
        lower.openTopology({{0, loopback, 8}}, 0);
        higher.openTopology({{0, loopback, 8}}, 0);
        if (lower.m_endpoints.isEmpty() || higher.m_endpoints.isEmpty() ||
            !a.beginInvitation(error) || a.invitationCode().size() != 5)
            return false;
        if (!lower.advertisement(45456).endsWith(QStringLiteral("设备 A").toUtf8()))
            return false;
        // 固定测试值验证前导零，跳过真实等待并保留实际 TCP/SPAKE2 交换。
        lower.m_inviteCode = "00042";
        if (action == PairCase::Expire)
            lower.endInvitation();
        auto candidate = Network::Discovery{aId, loopback, lower.m_endpoints.front().listener->serverPort(), 0, 1};
        higher.m_discoveries.insert(aId, candidate);
        const auto join = [&](QString code) { return b.joinInvitation(loopback.toString(), code, error); };
        if (action == PairCase::WrongCode) {
            for (int i = 0; i < 5; ++i) {
                lower.m_incomingAttempts.clear();
                higher.m_outgoingAttempts.clear();
                if (!join(QStringLiteral("00043")) || !wait([&] {
                        return lower.m_peers.isEmpty() && higher.m_peers.isEmpty() && lower.m_inviteAttempts == i + 1;
                    }))
                    return false;
            }
            return !lower.invitationActive() && a.pairedDevices().isEmpty() && b.pairedDevices().isEmpty();
        }
        if (!join(QStringLiteral("00042")))
            return false;
        if (action == PairCase::Expire)
            return wait([&] { return lower.m_peers.isEmpty() && higher.m_peers.isEmpty(); }) &&
                   a.pairedDevices().isEmpty() && b.pairedDevices().isEmpty();
        if (!wait([&] { return a.pairingRequest() == QStringLiteral("设备 B"); }) ||
            !a.devices().isEmpty() || !b.devices().isEmpty())
            return false;
        if (action == PairCase::SaveFails)
            b.m_settingsReadable = false;
        if (!a.decidePairing(action != PairCase::Reject, error))
            return false;
        if (!wait([&] { return lower.m_peers.isEmpty() && higher.m_peers.isEmpty(); }))
            return false;
        if (action != PairCase::Accept)
            return a.pairedDevices().isEmpty() && b.pairedDevices().isEmpty() &&
                   a.devices().isEmpty() && b.devices().isEmpty();
        if (lower.invitationActive() || lower.advertisement(45456).contains(QStringLiteral("设备 A").toUtf8()) ||
            !a.pairedDevices().isEmpty() || !b.pairedDevices().isEmpty() ||
            a.m_knownPeers.value(bId).key.size() != 32)
            return false;
        lower.m_incomingAttempts.clear();
        higher.m_outgoingAttempts.clear();
        if (!join(QStringLiteral("00042")) || !wait([&] {
                return lower.m_peers.isEmpty() && higher.m_peers.isEmpty();
            }) || !a.pairingRequest().isEmpty())
            return false;
        Service restoredA(aPath), restoredB(bPath);
        const auto stored = restoredA.m_knownPeers.value(bId).key;
        if (restoredA.m_identity != aId || restoredB.m_identity != bId ||
            stored.size() != 32 || stored != restoredB.m_knownPeers.value(aId).key)
            return false;
        QFile raw(QDir(aPath).filePath(QStringLiteral("lan-connection/settings.json")));
        if (!raw.open(QIODevice::ReadOnly) || raw.readAll().contains(stored.toBase64()))
            return false;
        raw.close();
        // 用重启后从磁盘恢复的凭据，建立与邀请完全独立的常规会话。
        auto& na = restoredA.m_network;
        auto& nb = restoredB.m_network;
        na.m_ownId = aId;
        nb.m_ownId = bId;
        na.openTopology({{0, loopback, 8}}, 0);
        nb.openTopology({{0, loopback, 8}}, 0);
        if (na.m_endpoints.isEmpty() || nb.m_endpoints.isEmpty())
            return false;
        candidate.port = na.m_endpoints.front().listener->serverPort();
        if (!nb.connectCandidate(candidate) || !wait([&] {
                return restoredA.devices().size() == 1 && restoredB.devices().size() == 1;
            }))
            return false;
        // 双端都没有启用共享时，仍应应答已认证连接上的心跳。
        bool pong = false;
        auto* peer = *restoredB.m_sessions.keyBegin();
        QObject context;
        QObject::connect(peer, &Peer::message, &context, [&](const QByteArray& packet) {
            pong |= packet == QByteArray(1, Pong);
        });
        if (!peer->send(QByteArray(1, Ping)) || !wait([&] { return pong; }) ||
            restoredA.devices().size() != 1 || restoredB.devices().size() != 1)
            return false;
        // 已认证的接收应先把解码内容交给宿主，再写入系统剪贴板。
        restoredA.m_coreEnabled = true;
        restoredB.m_coreEnabled = true;
        restoredA.m_available = restoredA.m_features = Clipboard;
        restoredB.m_available = restoredB.m_features = Clipboard;
        restoredA.broadcastFeatures();
        restoredB.broadcastFeatures();
        if (!wait([&] {
                const auto aDevices = restoredA.devices(), bDevices = restoredB.devices();
                return aDevices.size() == 1 && bDevices.size() == 1 &&
                       aDevices.front().features == Clipboard && bDevices.front().features == Clipboard;
            }))
            return false;
        QString received;
        QObject::connect(&restoredA, &Service::clipboardReceived, &context, [&](const QMimeData& mime) {
            if (mime.hasFormat(QLatin1String(kHotaruClipboardReceived)))
                received = mime.text();
        });
        const auto payload = QStringLiteral("局域网接收测试");
        if (!peer->send(QByteArray(1, Text) + payload.toUtf8()) ||
            !wait([&] { return received == payload && QApplication::clipboard()->text() == payload; }))
            return false;
        const QByteArray third(kIdentityBytes, 'z');
        restoredA.m_knownPeers.insert(third, {QStringLiteral("其他设备"), loopback, QByteArray(32, 'z'), false});
        if (!restoredA.forgetDevice(bId, error) ||
            !wait([&] { return restoredB.devices().isEmpty(); }))
            return false;
        nb.m_outgoingAttempts.clear();
        na.m_incomingAttempts.clear();
        return !restoredA.m_knownPeers.contains(bId) && restoredA.m_knownPeers.contains(third) &&
            nb.connectCandidate(candidate) && wait([&] { return nb.m_peers.isEmpty(); }) &&
            restoredA.devices().isEmpty() && restoredB.devices().isEmpty();
    }

    static bool probeReportsWhyItDidNotStart()
    {
        Network network;
        const QHostAddress local(QStringLiteral("192.0.2.1"));
        const QHostAddress target(QStringLiteral("192.0.2.2"));
        QString error;
        if (network.probe(target, error) || !error.contains(QStringLiteral("没有可用的专用 IPv4")))
            return false;
        network.m_addresses.push_back({0, local, 24});
        return !network.probe(target, error) && error.contains(QStringLiteral("TCP 45456 未监听"));
    }

    static bool listenerWithoutUdp()
    {
        QUdpSocket occupied;
        if (!occupied.bind(QHostAddress::AnyIPv4, 0, QUdpSocket::DontShareAddress))
            return false;

        Network network;
        const auto port = occupied.localPort();
        network.openTopology({{0, QHostAddress::LocalHost, 8}}, port);
        if (network.m_udp.state() == QAbstractSocket::BoundState || network.m_endpoints.size() != 1 ||
            !network.m_endpoints.front().listener->isListening())
            return false;

        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, port);
        if (!client.waitForConnected(1500))
            return false;

        network.stop();
        QTcpServer replacement;
        return replacement.listen(QHostAddress::LocalHost, port);
    }
};
}

using namespace HotaruLan;
namespace {
int passed = 0, failed = 0;
const QByteArray aId(16, 'a'), bId(16, 'b');
constexpr auto kCode = "sample-test-code";

bool check(bool condition, const char* name)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    condition ? ++passed : ++failed;
    return condition;
}

bool waitFor(const std::function<bool()>& condition, int ms = 4000)
{
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (condition())
            return true;
        QThread::msleep(2);
    } while (timer.elapsed() < ms);
    return false;
}

struct Pair : QObject {
    QTcpServer server;
    Peer *a = nullptr, *b = nullptr;
    int ended = 0;

    bool start(QByteArray peerCode)
    {
        if (!server.listen(QHostAddress::LocalHost))
            return false;
        connect(&server, &QTcpServer::newConnection, this, [this, peerCode] {
            b = new Peer(server.nextPendingConnection(), bId, {}, peerCode, this);
            connect(b, &Peer::ended, this, [this] { ++ended; });
            b->start();
        });
        auto* socket = new QTcpSocket;
        a = new Peer(socket, aId, bId, kCode, this);
        connect(a, &Peer::ended, this, [this] { ++ended; });
        a->start();
        socket->connectToHost(QHostAddress::LocalHost, server.serverPort());
        return true;
    }
    bool ready() const { return a && b && a->ready() && b->ready(); }
};

void crypto()
{
    SecureChannel a(true, aId, bId, kCode), b(false, bId, aId, kCode);
    check(a.seal("blocked").isEmpty() && b.open("blocked").isEmpty(), "no data before authentication");
    const bool derived = a.derive(b.share()) && b.derive(a.share());
    check(derived && a.confirm(b.proof()) && b.confirm(a.proof()), "SPAKE2 and both confirmations");
    const auto packet = a.seal("one");
    check(b.open(packet) == "one" && a.open(b.seal("two")) == "two", "bidirectional AEAD");
    check(b.open(packet).isEmpty(), "replay rejected");
    SecureChannel c(true, aId, bId, kCode), d(false, bId, aId, "another-test-code");
    check(c.derive(d.share()) && d.derive(c.share()) && !c.confirm(d.proof()) &&
          !d.confirm(c.proof()), "wrong passcode rejected");
    SecureChannel e(true, aId, bId, kCode), f(false, bId, aId, kCode);
    const bool ready = e.derive(f.share()) && f.derive(e.share()) && e.confirm(f.proof()) && f.confirm(e.proof());
    auto damaged = e.seal("integrity");
    if (!damaged.isEmpty())
        damaged[0] ^= 1;
    check(ready && !damaged.isEmpty() && f.open(damaged).isEmpty() &&
          f.open(e.seal("subsequent")).isEmpty(), "tampering closes the record channel");
    SecureChannel g(true, aId, bId, kCode), h(false, bId, QByteArray(16, 'x'), kCode);
    check(g.derive(h.share()) && h.derive(g.share()) && !g.confirm(h.proof()), "identities bound to handshake");
    Pair mismatch;
    check(mismatch.start("another-test-code") && waitFor([&] { return mismatch.ended == 2; }) &&
          !mismatch.ready(), "wrong passcode closes both TCP peers");
}

bool writeFile(const QString& path, QByteArrayView bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
           file.write(bytes.data(), bytes.size()) == bytes.size() && file.flush();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

bool stageGone(const QString& path)
{
    return QDir(path).entryList({QStringLiteral(".hotaru-lan-*")},
                                QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty();
}

void files()
{
    Catalog invalid;
    check(!decodeOffer(R"([{"p":"../outside","d":false,"n":"1"}])", invalid) &&
          !decodeOffer(R"([{"p":"folder/child","d":false,"n":"1"}])", invalid) &&
          !decodeOffer(R"([{"p":"File","d":false,"n":"1"},{"p":"file","d":false,"n":"1"}])", invalid) &&
          !validRelativePath(QStringLiteral("a/AUX.txt")) &&
          !validRelativePath(QStringLiteral("x:stream")) &&
          !validRelativePath(QStringLiteral("C:/a")) &&
          !validRelativePath(QStringLiteral("trailing. ")), "reject unsafe and conflicting paths");
    Pair pair;
    if (!check(pair.start(kCode) && waitFor([&] { return pair.ready(); }), "two TCP peers authenticate"))
        return;

    Transfers sender, receiver;
    const auto attach = [](Peer* peer, Transfers& transfers) {
        QObject::connect(peer, &Peer::message, &transfers, [peer, &transfers](QByteArray data) {
            if (data.isEmpty() || !transfers.receive(peer, u"测试设备", Wire(data[0]), QByteArrayView(data).sliced(1)))
                peer->close();
        });
        QObject::connect(peer, &Peer::drained, &transfers, [peer, &transfers] { transfers.pump(peer); });
        QObject::connect(peer, &Peer::ended, &transfers, [peer, &transfers] { transfers.disconnected(peer); });
    };
    attach(pair.a, sender);
    attach(pair.b, receiver);
    QTemporaryDir temp;
    const auto source = temp.filePath(QStringLiteral("源目录"));
    const auto target = temp.filePath(QStringLiteral("接收目录"));
    QByteArray sample;
    for (int i = 0; i < 4; ++i)
        sample += QByteArray(kChunkBytes, char('A' + i));
    if (!check(temp.isValid() && QDir().mkpath(source + QStringLiteral("/空目录")) &&
               QDir().mkpath(target) && writeFile(source + QStringLiteral("/中文文件.txt"), sample) &&
               writeFile(source + QStringLiteral("/空文件.txt"), {}), "isolated directory fixtures"))
        return;

    sender.sendOffer(pair.a, {source});
    if (!check(waitFor([&] { return receiver.awaitingDecision(); }) &&
               QDir(target).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
               "no writes before receiver approval"))
        return;
    QString receiveError;
    check(!receiver.accept(source + QStringLiteral("/中文文件.txt"), &receiveError) &&
          !receiveError.isEmpty() && receiver.awaitingDecision() && sender.outgoing(),
          "invalid receive directory preserves the pending offer");
    receiver.decline();
    check(waitFor([&] { return !sender.outgoing(); }) && stageGone(target), "decline and retry available");

    for (int i = 1; i <= 2; ++i) {
        sender.sendOffer(pair.a, sender.retryPaths());
        if (!check(waitFor([&] { return receiver.awaitingDecision(); }), "retry arrives"))
            return;
        receiver.accept(target);
        const auto run = target + (i == 1 ? QStringLiteral("/Hotaru 接收") : QStringLiteral("/Hotaru 接收 (2)"));
        const auto delivered = run + QStringLiteral("/源目录");
        check(waitFor([&] { return !sender.outgoing() && !receiver.incoming(); }) &&
              sender.outgoingStatus() == QStringLiteral("发送完成") &&
              readFile(delivered + QStringLiteral("/中文文件.txt")) == sample &&
              QFileInfo::exists(delivered + QStringLiteral("/空文件.txt")) &&
              QFileInfo(delivered + QStringLiteral("/空文件.txt")).size() == 0 &&
              QDir(delivered + QStringLiteral("/空目录")).exists() && stageGone(target),
              i == 1 ? "multi-chunk transfer and empty entries" : "same names keep both transfers");
    }

    sender.sendOffer(pair.a, {source});
    if (!check(waitFor([&] { return receiver.awaitingDecision(); }), "cancellation offer"))
        return;
    receiver.accept(target);
    const auto cancel = QObject::connect(&receiver, &Transfers::changed, &receiver, [&] {
        if (receiver.incoming() && receiver.incomingStatus().startsWith(QStringLiteral("接收中")))
            receiver.cancelIncoming();
    });
    check(waitFor([&] { return !sender.outgoing() && !receiver.incoming(); }) && stageGone(target) &&
          !QDir(target + QStringLiteral("/Hotaru 接收 (3)")).exists(), "cancel removes partial files");
    QObject::disconnect(cancel);

    const QByteArray id(16, 'z');
    QByteArray offer(1, Offer);
    offer += id;
    offer += R"([{"p":"digest.txt","d":false,"n":"3"}])";
    if (!check(pair.a->send(offer) && waitFor([&] { return receiver.awaitingDecision(); }),
               "malicious digest fixture offer"))
        return;
    receiver.accept(target);
    QByteArray bad(1, Chunk);
    bad += id;
    bad += QByteArray(4 + 8 + 32, '\0');
    bad += "abc";
    check(pair.a->send(bad) && waitFor([&] { return !receiver.incoming(); }) &&
          receiver.incomingStatus().contains(QStringLiteral("校验")) && stageGone(target),
          "bad block hash aborts and cleans up");

    sender.sendOffer(pair.a, {source});
    if (!check(waitFor([&] { return receiver.awaitingDecision(); }), "disconnect offer"))
        return;
    receiver.accept(target);
    pair.a->close();
    check(waitFor([&] { return !sender.outgoing() && !receiver.incoming(); }) && stageGone(target),
          "disconnection cleans up");
}

void clipboardAndSettings()
{
    QMimeData text;
    text.setText(QStringLiteral("样例 / Hotaru"));
    const auto packet = encodeClipboard(text);
    auto decoded = decodeClipboard(Text, QByteArrayView(packet).sliced(packet.isEmpty() ? 0 : 1));
    check(packet.startsWith(Text) && decoded && decoded->text() == text.text() &&
          encodeClipboard(*decoded).isEmpty(), "text sync and loop suppression");
    text.setData(QString::fromLatin1(kHotaruClipboardPrivate), "1");
    check(encodeClipboard(text).isEmpty() && !decodeClipboard(Text, QByteArray("\xff", 1)),
          "private clipboard and invalid text excluded");
    QMimeData history;
    history.setText(QStringLiteral("恢复的历史"));
    history.setData(QLatin1String(kHotaruClipboardRestored), "1");
    check(encodeClipboard(history).isEmpty(), "restored clipboard history is not sent");
    QMimeData image;
    QImage sample(10, 12, QImage::Format_RGB32);
    sample.fill(Qt::darkGreen);
    image.setImageData(sample);
    const auto png = encodeClipboard(image);
    auto unpacked = decodeClipboard(Image, QByteArrayView(png).sliced(png.isEmpty() ? 0 : 1));
    check(png.startsWith(Image) && unpacked &&
          qvariant_cast<QImage>(unpacked->imageData()).pixel(0, 0) == sample.pixel(0, 0) &&
          encodeClipboard(*unpacked).isEmpty() &&
          !decodeClipboard(Image, "not an image"), "PNG sync, bounds and loop suppression");

    QTemporaryDir temp;
    QString error;
    {
        Service service(temp.path());
        check(service.clipboardSearchAlias() == QStringLiteral("clip"), "fresh clipboard alias is clip");
        check(!service.enabled(Clipboard) && !service.enabled(Files) &&
              !service.setEnabled(Clipboard, true, error), "both features off, core required");
        check(!service.configure({}, error) && service.configure(QStringLiteral("测试设备"), error) &&
              service.setClipboardSearchAlias({}, error), "device name and empty alias saved");
    }
    Service restored(temp.path());
    check(restored.clipboardSearchAlias().isEmpty() &&
          restored.name() == QStringLiteral("测试设备"), "empty shortcut stays disabled after reload");
    const auto legacy = temp.filePath(QStringLiteral("legacy"));
    check(QDir().mkpath(legacy + QStringLiteral("/lan-preview")) &&
          writeFile(legacy + QStringLiteral("/lan-preview/settings.json"),
              R"({"version":1,"name":"Old label","clipboardAlias":"clip-old","features":3,"dpapi":"unused"})"),
          "isolated legacy settings fixture");
    Service migrated(legacy);
    check(migrated.name() == QStringLiteral("Old label") &&
          migrated.clipboardSearchAlias() == QStringLiteral("clip-old") &&
          !migrated.coreEnabled() && !migrated.requested(Clipboard) && !migrated.requested(Files) &&
          migrated.pairedDevices().isEmpty(), "migration preserves labels but discards old trust and switches");
    const auto oldDefault = temp.filePath(QStringLiteral("old-default"));
    check(QDir().mkpath(oldDefault + QStringLiteral("/lan-preview")) &&
          writeFile(oldDefault + QStringLiteral("/lan-preview/settings.json"),
          R"({"version":1,"name":"Old label","clipboardAlias":"jtb"})") &&
          Service(oldDefault).clipboardSearchAlias() == QStringLiteral("clip"),
          "previous default clipboard alias migrates to clip");
    const auto diagnostic = readFile(QDir(restored.diagnosticDirectory()).filePath(QStringLiteral("lan-diagnostics.log")));
    check(!diagnostic.isEmpty() && !diagnostic.contains(kCode) &&
          !diagnostic.contains(QStringLiteral("测试设备").toUtf8()), "diagnostic log excludes passcode and device name");
    QString lifecycleError;
    Service lifecycle(temp.filePath(QStringLiteral("lifecycle")));
    lifecycle.setAvailableFeatures(Clipboard);
    check(lifecycle.setCoreEnabled(true, lifecycleError) && lifecycle.setEnabled(Clipboard, true, lifecycleError) &&
          !lifecycle.setEnabled(Files, true, lifecycleError), "only installed features may be enabled");
    auto* privateMime = new QMimeData;
    privateMime->setText(QStringLiteral("私密测试"));
    privateMime->setData(QString::fromLatin1(kHotaruClipboardPrivate), "1");
    QApplication::clipboard()->setMimeData(privateMime);
    check(lifecycle.clipboardSnapshot().kind == HotaruClipboardSnapshot::Empty,
          "private content omitted from shortcut snapshot");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    check(lifecycle.setCoreEnabled(false, lifecycleError) && !lifecycle.enabled(Clipboard) &&
          lifecycle.requested(Clipboard), "stopping core preserves the requested feature state");
}

void module(const QString& path)
{
    QTemporaryDir data;
    QPluginLoader loader(path);
    auto* plugin = qobject_cast<HotaruNativePlugin*>(loader.instance());
    if (!check(plugin && plugin->start(data.path()), "Qt module ABI and startup"))
        return;
    const QString filesId = QStringLiteral("lan-files"), clipId = QStringLiteral("lan-clipboard");
    auto* package = qobject_cast<HotaruLanPackage*>(loader.instance());
    check(package && package->id() == QLatin1String(kHotaruLanCoreId), "core package IID");
    check(qobject_cast<HotaruLanClipboardReceive*>(loader.instance()) != nullptr, "optional clipboard receive bridge ABI");
    const std::unique_ptr<QWidget> corePage(plugin->createPage(QLatin1String(kHotaruLanCoreId), nullptr));
    QPushButton* diagnostics = nullptr;
    if (corePage)
        for (auto* button : corePage->findChildren<QPushButton*>())
            if (button->text().startsWith(QStringLiteral("连接诊断"))) diagnostics = button;
    check(diagnostics && diagnostics->isCheckable() && !diagnostics->isChecked(), "diagnostics uses themed collapsible button");
    if (diagnostics) {
        diagnostics->click();
        check(diagnostics->isChecked() && diagnostics->text().endsWith(QStringLiteral("▾")), "diagnostics keyboard-capable toggle reflects expansion");
    }
    auto* stack = qobject_cast<QStackedWidget*>(corePage.get());
    QPushButton* discover = nullptr;
    QPushButton* back = nullptr;
    if (stack) {
        for (auto* button : stack->findChildren<QPushButton*>()) {
            if (button->text().startsWith(QStringLiteral("发现邀请"))) discover = button;
            if (button->text() == QStringLiteral("返回局域网连接")) back = button;
        }
    }
    if (discover && back) {
        discover->setEnabled(true);
        discover->click();
        const bool opened = stack->currentIndex() == 1;
        back->click();
        check(opened && stack->currentIndex() == 0, "invite discovery is a separate page with back navigation");
    } else {
        check(false, "invite discovery is a separate page with back navigation");
    }
    const std::unique_ptr<QWidget> filesPage(plugin->createPage(filesId, nullptr));
    const std::unique_ptr<QWidget> clipPage(plugin->createPage(clipId, nullptr));
    check(filesPage && clipPage && !filesPage->findChildren<QCheckBox*>().isEmpty() &&
          !clipPage->findChildren<QCheckBox*>().isEmpty() &&
          plugin->state(filesId).contains(QStringLiteral("已关闭")) &&
          plugin->state(clipId).contains(QStringLiteral("已关闭")), "both settings pages and default state");
    bool parallel = false;
    if (filesPage)
        for (auto* row : filesPage->findChildren<QHBoxLayout*>()) {
            const auto* left = row->count() == 2 ? row->itemAt(0)->widget() : nullptr;
            const auto* right = row->count() == 2 ? row->itemAt(1)->widget() : nullptr;
            if (left && right && left->property("lanTransferSide").toString() == QStringLiteral("send") &&
                right->property("lanTransferSide").toString() == QStringLiteral("receive"))
                parallel = true;
        }
    check(parallel, "file settings keep send and receive in adjacent columns");
    check(filesPage && clipPage && filesPage->findChildren<QLineEdit*>().isEmpty() &&
          clipPage->findChildren<QLineEdit*>().size() == 1, "file settings have no clipboard shortcut input");
}
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    if (QGuiApplication::platformName() != QStringLiteral("offscreen"))
        return 2;
    const auto args = app.arguments();
    if (args.size() == 3 && args.at(1) == QStringLiteral("--check-private-ip")) {
        const QHostAddress expected(args.at(2));
        return expected.protocol() == QAbstractSocket::IPv4Protocol &&
               NetworkSelftest::hasPrivateAddress(expected) ? 0 : 1;
    }
    crypto();
    check(NetworkSelftest::probeReportsWhyItDidNotStart(), "manual probe separates missing interface from missing listener");
    check(NetworkSelftest::listenerWithoutUdp(), "TCP listener survives unavailable UDP and closes on stop");
    using Link = NetworkSelftest::Link;
    const auto testLink = [](Link link) {
        return NetworkSelftest::automaticAuthentication(link,
            [](const std::function<bool()>& ready) { return waitFor(ready); });
    };
    const auto previousProxy = QNetworkProxy::applicationProxy();
    // 注入一个不可用的应用代理，LAN 必须继续使用本地 TCP 直连。
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 1));
    check(testLink(Link::Mutual), "mutual discovery automatically authenticates without using the application proxy");
    QNetworkProxy::setApplicationProxy(previousProxy);
    check(testLink(Link::ReverseOnly), "mutual discovery retries from the reachable direction");
    check(testLink(Link::OneSided), "higher ID alone sees the candidate and authenticates on retry");
    check(testLink(Link::Crossed), "crossed attempts keep one bidirectional authenticated session");
    check(testLink(Link::WrongCode), "automatic pairing rejects a different passcode");
    using PairCase = NetworkSelftest::PairCase;
    const auto testPair = [](PairCase action) {
        return NetworkSelftest::invitation(action, [](const std::function<bool()>& ready) { return waitFor(ready); });
    };
    check(testPair(PairCase::Accept), "invite name, DPAPI reconnect, zero-feature heartbeat, clipboard delivery and selective revoke");
    check(testPair(PairCase::Reject), "declining does not grant access");
    check(testPair(PairCase::Expire), "expired code cannot authenticate");
    check(testPair(PairCase::WrongCode), "five wrong guesses consume the invitation");
    check(testPair(PairCase::SaveFails), "incomplete persistence does not expose a paired device");
    clipboardAndSettings();
    files();
    if (app.arguments().size() > 1)
        module(app.arguments().at(1));
    std::printf("LAN: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

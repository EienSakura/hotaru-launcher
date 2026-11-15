#include "LanService.h"
#include "hotaru-native-plugin.h"
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QHostInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QMimeData>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QStandardPaths>
#include <QRegularExpression>
#include <algorithm>
#include <utility>
#include <qt_windows.h>
#include <dpapi.h>

namespace HotaruLan {
namespace {
constexpr int kTextBytes = 256 * 1024;
constexpr int kImageBytes = 4 * 1024 * 1024;
constexpr int kImagePixels = 8 * 1024 * 1024;
constexpr int kFeatures = Clipboard | Files;

QByteArray protect(QByteArrayView input, bool seal)
{
    if (input.isEmpty())
        return {};
    DATA_BLOB source{DWORD(input.size()), reinterpret_cast<BYTE*>(const_cast<char*>(input.data()))};
    DATA_BLOB result{};
    const bool ok = seal ? CryptProtectData(&source, L"Hotaru LAN", nullptr, nullptr, nullptr,
                                           CRYPTPROTECT_UI_FORBIDDEN, &result)
                         : CryptUnprotectData(&source, nullptr, nullptr, nullptr, nullptr,
                                              CRYPTPROTECT_UI_FORBIDDEN, &result);
    if (!ok)
        return {};
    const QByteArray output(reinterpret_cast<const char*>(result.pbData), result.cbData);
    SecureZeroMemory(result.pbData, result.cbData);
    LocalFree(result.pbData);
    return output;
}

QString cleanName(QStringView name)
{
    const auto text = name.toString().simplified().left(48);
    for (const auto ch : text)
        if (ch.category() == QChar::Other_Control || ch.category() == QChar::Other_Format)
            return {};
    return text;
}



bool boundedImage(const QSize& size)
{
    return size.isValid() && qint64(size.width()) * size.height() <= kImagePixels;
}

QString validAlias(QStringView input)
{
    const QString alias = input.toString().trimmed().toLower();
    if (alias.isEmpty())
        return {};
    if (alias.size() > 24)
        return {};
    for (const QChar c : alias) {
        if (!(c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-')))
            return {};
    }
    return alias;
}
}

Service::Service(QString dataPath, QObject* parent)
    : QObject(parent), m_path(QDir(dataPath).filePath(QStringLiteral("lan-connection/settings.json"))),
      m_name(cleanName(QHostInfo::localHostName()))
{
    if (m_name.isEmpty())
        m_name = QStringLiteral("Hotaru");
    m_clock.start();
    load();
    connect(&m_network, &Network::diagnostic, this, &Service::writeDiagnostic);
    writeDiagnostic(QStringLiteral("LAN 模块已加载；启用能力 %1").arg(m_features));
    if (!m_error.isEmpty())
        writeDiagnostic(m_error);
    connect(&m_network, &Network::connected, this, &Service::onConnected);
    m_network.setCredentialLookup([this](const QByteArray& id) {
        const auto it = m_knownPeers.constFind(id);
        return it == m_knownPeers.cend() ? QByteArray{} : it->key;
    });
    connect(&m_network, &Network::invitationFailed, this, [this] {
        m_pairingStatus = QStringLiteral("配对未完成：请检查邀请码、对端确认和连接状态");
        emit changed();
    });
    connect(&m_network, &Network::changed, this, &Service::changed);
    connect(&m_network, &Network::disconnected, this, [this](Peer* peer) {
        const auto pairing = m_pairing.take(peer);
        if (peer->invitation() && pairing.step != PairStep::Done &&
            (m_pairingStatus.isEmpty() || m_pairingStatus == QStringLiteral("已发起配对，等待对端确认")))
            m_pairingStatus = QStringLiteral("配对连接已结束，请重新邀请");
        m_transfers.disconnected(peer);
        m_sessions.remove(peer);
        emit changed();
    });
    connect(&m_transfers, &Transfers::changed, this, &Service::changed);
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this, &Service::capture);
    m_pulse.setInterval(1500);
    connect(&m_pulse, &QTimer::timeout, this, &Service::pulse);
    restart();
}

Service::~Service()
{
    m_network.stop();
    for (auto& peer : m_knownPeers)
        peer.key.fill(0);
}

QString Service::diagnosticDirectory() const
{
    return QFileInfo(m_path).absolutePath();
}

void Service::writeDiagnostic(const QString& message) const
{
    // 仅记录连接事件，调用者不得传入设备名、邀请码、凭据或业务内容。
    constexpr qint64 limit = 128 * 1024;
    const QDir dir(diagnosticDirectory());
    if (!QDir().mkpath(dir.absolutePath()))
        return;
    QFile log(dir.filePath(QStringLiteral("lan-diagnostics.log")));
    const auto line = (QDateTime::currentDateTime().toString(Qt::ISODateWithMs) +
                       QStringLiteral(" ") + message.left(2048) + QLatin1Char('\n')).toUtf8();
    if (log.size() + line.size() > limit) {
        const auto previous = dir.filePath(QStringLiteral("lan-diagnostics.previous.log"));
        if (QFile::exists(previous) && !QFile::remove(previous))
            return;
        if (!QFile::rename(log.fileName(), previous))
            return;
    }
    if (log.open(QIODevice::WriteOnly | QIODevice::Append))
        log.write(line);
}


void Service::load()
{
    QFile file(m_path);
    const bool legacy = !file.exists();
    if (legacy)
        file.setFileName(QFileInfo(m_path).absoluteDir().absoluteFilePath(QStringLiteral("../lan-preview/settings.json")));
    if (!file.exists())
        return;
    const auto badSettings = [this, legacy] {
        m_error = QStringLiteral("无法读取局域网连接设置");
        m_settingsReadable = legacy;
    };
    if (!file.open(QIODevice::ReadOnly) || file.size() > 128 * 1024) {
        badSettings();
        return;
    }
    const auto doc = QJsonDocument::fromJson(file.readAll());
    const auto settings = doc.object();
    if (settings.value("version").toInt() != (legacy ? 1 : 2)) {
        badSettings();
        return;
    }
    const auto name = cleanName(settings.value("name").toString());
    if (!name.isEmpty())
        m_name = name;
    const auto alias = validAlias(settings.value("clipboardAlias").toString());
    if (!alias.isEmpty() || (settings.value("clipboardAlias").isString() &&
                            settings.value("clipboardAlias").toString().isEmpty()))
        m_clipboardAlias = alias == QLatin1String("jtb") ? QStringLiteral("clip") : alias;
    if (legacy)
        return;
    m_identity = QByteArray::fromHex(settings.value("identity").toString().toLatin1());
    if (m_identity.size() != kIdentityBytes) {
        badSettings();
        return;
    }
    m_coreEnabled = settings.value("enabled").toBool();
    m_features = settings.value("features").toInt() & kFeatures;
    m_downloadDirectory = settings.value("downloadDirectory").toString();
    for (const auto& value : settings.value("peers").toArray()) {
        const auto item = value.toObject();
        const auto id = QByteArray::fromHex(item.value("id").toString().toLatin1());
        const auto protectedKey = QByteArray::fromBase64(item.value("key").toString().toLatin1(),
                                                        QByteArray::AbortOnBase64DecodingErrors);
        auto key = protect(protectedKey, false);
        const auto name = cleanName(item.value("name").toString());
        if (id.size() != kIdentityBytes || id == m_identity || key.size() != 32 || name.isEmpty()) {
            m_error = QStringLiteral("部分配对凭据无法读取，未启动局域网连接");
            m_settingsReadable = false;
            m_coreEnabled = false;
            return;
        }
        m_knownPeers.insert(id, {name, QHostAddress(item.value("address").toString()), std::move(key),
                                 item.value("pending").toBool(true)});
    }
}

bool Service::save()
{
    if (!m_settingsReadable)
        return false;
    if (m_identity.isEmpty())
        m_identity = randomBytes(kIdentityBytes);
    if (m_identity.size() != kIdentityBytes || !QDir().mkpath(QFileInfo(m_path).absolutePath()))
        return false;
    QJsonArray peers;
    for (auto it = m_knownPeers.cbegin(); it != m_knownPeers.cend(); ++it) {
        const auto blob = protect(it->key, true);
        if (blob.isEmpty())
            return false;
        peers.append(QJsonObject{{"id", QString::fromLatin1(it.key().toHex())}, {"name", it->name},
                                 {"address", it->address.toString()}, {"key", QString::fromLatin1(blob.toBase64())},
                                 {"pending", it->pending}});
    }
    const auto json = QJsonDocument(QJsonObject{
        {"version", 2}, {"identity", QString::fromLatin1(m_identity.toHex())},
        {"name", m_name}, {"enabled", m_coreEnabled}, {"features", m_features},
        {"clipboardAlias", m_clipboardAlias}, {"downloadDirectory", m_downloadDirectory}, {"peers", peers}})
                          .toJson(QJsonDocument::Compact);
    QSaveFile file(m_path);
    return file.open(QIODevice::WriteOnly) && file.write(json) == json.size() && file.commit();
}

bool Service::configure(const QString& name, QString& error)
{
    const auto validated = cleanName(name);
    if (validated.isEmpty()) {
        error = QStringLiteral("请输入有效的设备名");
        return false;
    }
    const auto old = std::exchange(m_name, validated);
    if (!save()) {
        m_name = old;
        error = QStringLiteral("无法保存连接设置");
        return false;
    }
    m_network.setInvitationName(m_name);
    broadcastFeatures();
    return true;
}

bool Service::setCoreEnabled(bool on, QString& error)
{
    if (on == m_coreEnabled)
        return true;
    const bool old = std::exchange(m_coreEnabled, on);
    if (!save()) {
        m_coreEnabled = old;
        error = QStringLiteral("无法保存局域网连接状态");
        return false;
    }
    restart();
    emit changed();
    return true;
}

bool Service::setEnabled(int feature, bool on, QString& error)
{
    if (feature != Clipboard && feature != Files)
        return false;
    if (on && (!m_coreEnabled || !(m_available & feature))) {
        error = QStringLiteral("请先安装并启用局域网连接");
        return false;
    }
    const int next = on ? m_features | feature : m_features & ~feature;
    if (next == m_features)
        return true;
    const int old = std::exchange(m_features, next);
    if (!save()) {
        m_features = old;
        error = QStringLiteral("无法保存插件启用状态");
        return false;
    }
    broadcastFeatures();
    return true;
}

void Service::setAvailableFeatures(int features)
{
    m_available = features & kFeatures;
    broadcastFeatures();
}

void Service::broadcastFeatures()
{
    m_clipSequence = GetClipboardSequenceNumber();
    if (!enabled(Files)) {
        m_transfers.cancelIncoming();
        m_transfers.cancelOutgoing();
    }
    const auto peers = m_sessions.keys();
    for (auto* peer : peers)
        if (m_sessions.contains(peer) &&
            !peer->send(QByteArray(1, Hello) + QByteArray(1, char(activeFeatures())) + m_name.toUtf8()))
            peer->close();
    emit changed();
}

void Service::restart()
{
    m_clipSequence = GetClipboardSequenceNumber();
    m_pulse.stop();
    m_network.stop();
    if (m_coreEnabled) {
        refreshCachedAddresses();
        m_network.start(m_identity);
        m_pulse.start();
    }
}

void Service::refreshCachedAddresses()
{
    QList<QHostAddress> addresses;
    for (const auto& item : m_knownPeers)
        addresses.push_back(item.address);
    m_network.setCachedAddresses(addresses);
}

QString Service::status() const
{
    return m_error.isEmpty() ? m_network.status() : m_error;
}

QList<Service::Device> Service::devices() const
{
    QList<Device> result;
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it)
        if (!it.value().name.isEmpty())
            result.push_back({it.key()->peerId(), it.value().name,
                              it.key()->address().toString(), it.value().features,
                              it.value().seen});
    std::sort(result.begin(), result.end(), [](const Device& a, const Device& b) {
        const int cmp = QString::localeAwareCompare(a.name, b.name);
        return cmp == 0 ? a.id < b.id : cmp < 0;
    });
    return result;
}

QList<Service::Device> Service::pairedDevices() const
{
    auto result = devices();
    for (auto it = m_knownPeers.cbegin(); it != m_knownPeers.cend(); ++it) {
        if (!it->pending && std::none_of(result.cbegin(), result.cend(), [&](const Device& item) { return item.id == it.key(); }))
            result.push_back({it.key(), it->name, it->address.toString(), 0, -1});
    }
    return result;
}

QList<Service::Discovery> Service::discoveredDevices() const
{
    QList<Discovery> result;
    for (const auto& item : m_network.discoveries()) {
        Discovery discovery;
        discovery.id = item.id;
        discovery.address = item.address.toString();
        discovery.name = item.name;
        discovery.features = item.features;
        discovery.lastSeen = item.lastSeen;
        discovery.phase = item.phase;
        for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
            if (it.key()->peerId() == item.id && !it.value().name.isEmpty()) {
                discovery.paired = true;
                break;
            }
        }
        result.push_back(std::move(discovery));
    }
    std::sort(result.begin(), result.end(), [](const Discovery& a, const Discovery& b) {
        return a.address < b.address;
    });
    return result;
}

bool Service::probeAddress(const QString& address, QString& error)
{
    const QHostAddress parsed(address.trimmed());
    if (parsed.isNull() || parsed.protocol() != QAbstractSocket::IPv4Protocol) {
        error = QStringLiteral("请输入有效的 IPv4 地址，例如 192.168.1.20");
        return false;
    }
    if (!m_coreEnabled) {
        error = QStringLiteral("请先启用局域网连接");
        return false;
    }
    return m_network.probe(parsed, error);
}


bool Service::beginInvitation(QString& error)
{
    m_pairingStatus.clear();
    m_network.setInvitationName(m_name);
    if (m_network.beginInvitation().isEmpty()) {
        error = QStringLiteral("请先启用局域网连接并连接到专用网络");
        return false;
    }
    emit changed();
    return true;
}

bool Service::joinInvitation(const QString& address, const QString& code, QString& error)
{
    if (code.size() != 5 || std::any_of(code.cbegin(), code.cend(), [](QChar ch) {
            return ch < QLatin1Char('0') || ch > QLatin1Char('9');
        })) {
        error = QStringLiteral("请输入五位数字邀请码");
        return false;
    }
    if (!m_network.joinInvitation(QHostAddress(address.trimmed()), code.toLatin1(), error))
        return false;
    m_pairingStatus = QStringLiteral("已发起配对，等待对端确认");
    emit changed();
    return true;
}

QString Service::pairingRequest() const
{
    for (const auto& item : m_pairing)
        if (item.step == PairStep::WaitForUser)
            return item.name;
    return {};
}

bool Service::decidePairing(bool accept, QString& error)
{
    for (auto it = m_pairing.begin(); it != m_pairing.end(); ++it) {
        if (it->step != PairStep::WaitForUser)
            continue;
        auto* peer = it.key();
        if (!accept) {
            m_pairingStatus = QStringLiteral("已拒绝配对");
            it->step = PairStep::Done;
            m_network.consumeInvitation();
            if (!peer->send(QByteArray(1, PairReject))) {
                m_network.endInvitation();
                error = QStringLiteral("对端已断开连接");
                return false;
            }
            emit changed();
            return true;
        }
        it->key = randomBytes(32);
        if (it->key.size() != 32)
            break;
        it->step = PairStep::WaitForSaved;
        m_network.consumeInvitation();
        if (!peer->send(QByteArray(1, PairKey) + it->key)) {
            error = QStringLiteral("对端已断开连接");
            return false;
        }
        emit changed();
        return true;
    }
    error = QStringLiteral("配对请求已失效，请重新邀请");
    return false;
}

bool Service::savePair(Peer* peer, const Pairing& pair)
{
    const auto id = peer->peerId();
    if (m_knownPeers.size() >= 64 && !m_knownPeers.contains(id))
        return false;
    const auto old = m_knownPeers.value(id);
    m_knownPeers.insert(id, {pair.name, peer->address(), pair.key, true});
    if (save()) {
        refreshCachedAddresses();
        return true;
    }
    if (old.key.isEmpty())
        m_knownPeers.remove(id);
    else
        m_knownPeers.insert(id, old);
    return false;
}

void Service::onPairing(Peer* peer, Wire type, QByteArrayView data)
{
    auto it = m_pairing.find(peer);
    if (it == m_pairing.end()) {
        peer->close();
        return;
    }
    if (type == PairHello && it->step == PairStep::Hello) {
        it->name = data.size() <= 192 ? cleanName(QString::fromUtf8(data)) : QString{};
        if (!it->name.isEmpty()) {
            it->step = peer->initiatedLocally() ? PairStep::WaitForKey : PairStep::WaitForUser;
            emit changed();
            return;
        }
    } else if (type == PairReject && data.isEmpty() && it->step == PairStep::WaitForKey) {
        m_pairingStatus = QStringLiteral("对端拒绝了配对");
        it->step = PairStep::Done;
        peer->close();
        return;
    } else if (type == PairKey && it->step == PairStep::WaitForKey && data.size() == 32) {
        it->key = data.toByteArray();
        it->step = PairStep::WaitForCommit;
        peer->send(QByteArray(1, PairSaved));
        return;
    } else if (type == PairSaved && it->step == PairStep::WaitForSaved && data.isEmpty()) {
        if (savePair(peer, *it)) {
            it->step = PairStep::WaitForReceipt;
            peer->send(QByteArray(1, PairCommit));
            return;
        }
        m_pairingStatus = QStringLiteral("无法保存配对凭据");
    } else if (type == PairCommit && it->step == PairStep::WaitForCommit && data.isEmpty()) {
        if (savePair(peer, *it)) {
            it->step = PairStep::Done;
            m_pairingStatus = QStringLiteral("已保存配对，正在建立连接");
            if (peer->send(QByteArray(1, PairReceipt)))
                emit changed();
            return;
        }
        m_pairingStatus = QStringLiteral("无法保存配对凭据");
    } else if (type == PairReceipt && it->step == PairStep::WaitForReceipt && data.isEmpty()) {
        it->step = PairStep::Done;
        m_pairingStatus = QStringLiteral("已保存配对，正在建立连接");
        m_network.endInvitation();
        return;
    }
    peer->close();
}

bool Service::forgetDevice(const QByteArray& id, QString& error)
{
    const auto old = m_knownPeers.take(id);
    if (old.key.isEmpty())
        return true;
    if (!save()) {
        m_knownPeers.insert(id, old);
        error = QStringLiteral("无法保存设备撤销状态");
        return false;
    }
    m_network.forget(id);
    refreshCachedAddresses();
    emit changed();
    return true;
}

QString Service::downloadDirectory() const
{
    return m_downloadDirectory.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
                                         : m_downloadDirectory;
}

bool Service::setDownloadDirectory(const QString& directory, QString& error)
{
    if (directory.isEmpty() || !QFileInfo(directory).isDir() || !QFileInfo(directory).isWritable()) {
        error = QStringLiteral("此目录不可写，请另选目录");
        return false;
    }
    const auto old = std::exchange(m_downloadDirectory, directory);
    if (!save()) {
        m_downloadDirectory = old;
        error = QStringLiteral("无法保存默认接收目录");
        return false;
    }
    emit changed();
    return true;
}

bool Service::receiveFiles(const QString& directory, QString& error)
{
    if (!enabled(Files) || !m_transfers.awaitingDecision()) {
        error = QStringLiteral("没有待接收的文件");
        return false;
    }
    if (directory.isEmpty() || !QFileInfo(directory).isDir() || !QFileInfo(directory).isWritable()) {
        error = QStringLiteral("接收目录不可用，请点「另存到」选择目录");
        return false;
    }
    return m_transfers.accept(directory, &error);
}

void Service::onConnected(Peer* peer)
{
    connect(peer, &Peer::message, this, [this, peer](const QByteArray& data) {
        if (peer->invitation()) {
            if (data.isEmpty())
                peer->close();
            else
                onPairing(peer, Wire(data.front()), QByteArrayView(data).sliced(1));
        } else {
            onMessage(peer, data);
        }
    });
    if (peer->invitation()) {
        m_pairing.insert(peer, {});
        if (!peer->send(QByteArray(1, PairHello) + m_name.toUtf8()))
            peer->close();
        return;
    }
    if (!m_knownPeers.contains(peer->peerId())) {
        peer->close();
        return;
    }
    m_sessions.insert(peer, {{}, 0, m_clock.elapsed()});
    connect(peer, &Peer::drained, this, [this, peer] { m_transfers.pump(peer); });
    if (!peer->send(QByteArray(1, Hello) + QByteArray(1, char(activeFeatures())) + m_name.toUtf8()))
        peer->close();
}

void Service::onMessage(Peer* peer, QByteArrayView packet)
{
    if (!m_sessions.contains(peer))
        return;
    if (packet.isEmpty()) {
        peer->close();
        return;
    }
    const auto type = Wire(packet.front());
    const auto data = packet.sliced(1);
    auto& session = m_sessions[peer];
    const qint64 now = m_clock.elapsed();
    session.seen = now;
    if (type == Hello) {
        const int flags = data.isEmpty() ? 0 : uchar(data.front());
        const auto name = data.size() >= 2 && data.size() <= 193
                              ? cleanName(QString::fromUtf8(data.sliced(1))) : QString{};
        if ((flags & ~kFeatures) || name.isEmpty()) {
            peer->close();
            return;
        }
        const bool revoked = (session.features & Files) && !(flags & Files);
        const bool firstHello = session.name.isEmpty();
        auto& known = m_knownPeers[peer->peerId()];
        if (known.pending || known.name != name || known.address != peer->address()) {
            const auto previous = known;
            known.pending = false;
            known.name = name;
            known.address = peer->address();
            if (!save()) {
                known = previous;
                peer->close();
                return;
            }
            refreshCachedAddresses();
        }
        session.name = name;
        session.features = flags;
        if (firstHello) {
            writeDiagnostic(QStringLiteral("已配对：%1").arg(peer->address().toString()));
            if (m_pairingStatus == QStringLiteral("已保存配对，正在建立连接"))
                m_pairingStatus = QStringLiteral("配对成功");
        }
        if (revoked)
            m_transfers.disconnected(peer);
        emit changed();
        return;
    }
    // 零共享能力也是有效会话；只有尚未完成 Hello 才拒绝心跳。
    if (session.name.isEmpty()) {
        peer->close();
        return;
    }
    if (type == Ping || type == Pong) {
        if (!data.isEmpty() || (type == Ping && !peer->send(QByteArray(1, Pong))))
            peer->close();
        return;
    }
    if (type == Text || type == Image) {
        if (!(activeFeatures() & session.features & Clipboard))
            return;
        if (type == Image && now - session.clipboardAt < 500)
            return;
        session.clipboardAt = now;
        if (!applyClipboard(type, data))
            peer->close();
        return;
    }
    if (type >= Offer && type <= Cancel) {
        if (!(activeFeatures() & session.features & Files))
            return;
        const bool previouslyPending = m_transfers.awaitingDecision();
        const auto name = session.name;
        if (!m_transfers.receive(peer, name, type, data))
            peer->close();
        else if (!previouslyPending && m_transfers.awaitingDecision())
            emit incomingOffer();
        return;
    }
    peer->close();
}

void Service::share(const QByteArray& id, const QStringList& paths)
{
    QString error;
    sendFiles(id, paths, error);
}

bool Service::sendFiles(const QByteArray& id, const QStringList& paths, QString& error)
{
    if (!enabled(Files)) {
        error = QStringLiteral("局域网文件分享未启用");
        return false;
    }
    if (paths.isEmpty()) {
        error = QStringLiteral("没有选择要发送的文件或文件夹");
        return false;
    }
    if (m_transfers.outgoing()) {
        error = QStringLiteral("已有文件正在发送");
        return false;
    }
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
        if ((it.value().features & Files) && it.key()->peerId() == id) {
            m_transfers.sendOffer(it.key(), paths);
            if (m_transfers.outgoing())
                return true;
            error = m_transfers.outgoingStatus();
            return false;
        }
    }
    error = QStringLiteral("目标设备已断开或未启用文件分享");
    return false;
}

HotaruClipboardSnapshot Service::clipboardSnapshot() const
{
    HotaruClipboardSnapshot snapshot;
    if (!enabled(Clipboard))
        return snapshot;
    const auto* mime = QApplication::clipboard()->mimeData();
    if (!mime || mime->hasFormat(QString::fromLatin1(kHotaruClipboardPrivate))) {
        snapshot.connectionState = m_network.status();
        return snapshot;
    }
    if (mime->hasImage()) {
        const auto image = qvariant_cast<QImage>(mime->imageData());
        if (!image.isNull()) {
            snapshot.kind = HotaruClipboardSnapshot::Image;
            snapshot.image = image;
            snapshot.bytes = image.sizeInBytes();
        }
    } else if (mime->hasText()) {
        const auto text = mime->text();
        snapshot.kind = text.isEmpty() ? HotaruClipboardSnapshot::Empty
                                       : HotaruClipboardSnapshot::Text;
        snapshot.textPreview = text.left(240).replace(QRegularExpression("\\s+"), QStringLiteral(" "));
        snapshot.bytes = text.toUtf8().size();
    }
    snapshot.connectionState = m_network.status();
    return snapshot;
}

bool Service::setClipboardSearchAlias(const QString& alias, QString& error)
{
    const auto value = validAlias(alias);
    if (!alias.trimmed().isEmpty() && value.isEmpty()) {
        error = QStringLiteral("快捷词只能包含字母、数字、下划线或短横线，最长 24 个字符");
        return false;
    }
    const auto old = m_clipboardAlias;
    m_clipboardAlias = value;
    if (!save()) {
        m_clipboardAlias = old;
        error = QStringLiteral("无法保存剪贴板快捷词");
        return false;
    }
    emit changed();
    return true;
}

void Service::pulse()
{
    const auto peers = m_sessions.keys();
    const qint64 now = m_clock.elapsed();
    for (auto* peer : peers) {
        if (!m_sessions.contains(peer))
            continue;
        const auto& session = m_sessions[peer];
        if (now - session.seen >= (!session.name.isEmpty() ? 15000 : 5000))
            peer->close();
        else if (!session.name.isEmpty() && now - session.seen >= 4500 && peer->pendingBytes() < 1024 &&
                 !peer->send(QByteArray(1, Ping)))
            peer->close();
    }
    capture();
}



void Service::capture()
{
    if (!enabled(Clipboard) || m_writingClipboard)
        return;
    const auto sequence = GetClipboardSequenceNumber();
    if (!sequence || sequence == m_clipSequence)
        return;
    m_clipSequence = sequence;
    QList<Peer*> targets;
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it)
        if ((it.value().features & Clipboard) && it.key()->pendingBytes() < 512 * 1024)
            targets.push_back(it.key());
    if (targets.isEmpty())
        return;
    const auto* mime = QApplication::clipboard()->mimeData();
    const auto data = mime ? encodeClipboard(*mime) : QByteArray{};
    if (data.isEmpty())
        return;
    for (auto* peer : targets)
        if (m_sessions.contains(peer))
            peer->send(data);
}

QByteArray encodeClipboard(const QMimeData& mime)
{
    if (mime.hasFormat(QString::fromLatin1(kHotaruClipboardPrivate)) ||
        mime.hasFormat(QString::fromLatin1(kHotaruClipboardReceived)) ||
        mime.hasFormat(QString::fromLatin1(kHotaruClipboardRestored)))
        return {};
    QByteArray data;
    if (mime.hasImage()) {
        const auto image = qvariant_cast<QImage>(mime.imageData());
        if (image.isNull() || !boundedImage(image.size()))
            return {};
        data = QByteArray(1, Image);
        QBuffer buffer(&data);
        if (!buffer.open(QIODevice::WriteOnly | QIODevice::Append) ||
            !image.save(&buffer, "PNG") || data.size() - 1 > kImageBytes)
            return {};
    } else if (mime.hasText() && !mime.hasUrls()) {
        const auto text = mime.text().toUtf8();
        if (text.isEmpty() || text.size() > kTextBytes)
            return {};
        data = QByteArray(1, Text) + text;
    }
    return data;
}

std::unique_ptr<QMimeData> decodeClipboard(Wire type, QByteArrayView data)
{
    if ((type != Text && type != Image) || data.isEmpty() ||
        data.size() > (type == Text ? kTextBytes : kImageBytes))
        return {};
    auto mime = std::make_unique<QMimeData>();
    if (type == Text) {
        const auto text = QString::fromUtf8(data);
        if (text.toUtf8() != data)
            return {};
        mime->setText(text);
    } else {
        auto png = data.toByteArray();
        QBuffer buffer(&png);
        if (!buffer.open(QIODevice::ReadOnly))
            return {};
        QImageReader reader(&buffer, "PNG");
        reader.setDecideFormatFromContent(false);
        reader.setAutoDetectImageFormat(false);
        if (!boundedImage(reader.size()))
            return {};
        const auto image = reader.read();
        if (image.isNull() || !boundedImage(image.size()))
            return {};
        mime->setImageData(image);
    }
    mime->setData(QString::fromLatin1(kHotaruClipboardReceived), QByteArrayLiteral("1"));
    return mime;
}

bool Service::applyClipboard(Wire type, QByteArrayView data)
{
    auto mime = decodeClipboard(type, data);
    if (!mime)
        return false;
    QScopedValueRollback guard(m_writingClipboard, true);
    emit clipboardReceived(*mime);
    QApplication::clipboard()->setMimeData(mime.release());
    m_clipSequence = GetClipboardSequenceNumber();
    return true;
}
}

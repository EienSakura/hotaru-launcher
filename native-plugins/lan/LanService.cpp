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
#include <QRegularExpression>
#include <algorithm>
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

QByteArray cleanCode(QStringView input)
{
    const auto code = input.toString().normalized(QString::NormalizationForm_C).trimmed();
    const auto bytes = code.toUtf8();
    if (code.toUcs4().size() < 6 || bytes.size() > 128)
        return {};
    for (const auto ch : code)
        if (ch.category() == QChar::Other_Control || ch.category() == QChar::Other_Format)
            return {};
    return bytes;
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
    : QObject(parent), m_path(QDir(dataPath).filePath(QStringLiteral("lan-preview/settings.json"))),
      m_name(cleanName(QHostInfo::localHostName()))
{
    if (m_name.isEmpty())
        m_name = QStringLiteral("Hotaru");
    m_clock.start();
    load();
    connect(&m_network, &Network::diagnostic, this, &Service::writeDiagnostic);
    writeDiagnostic(QStringLiteral("LAN 模块已加载；启用能力 %1；安全码状态：%2")
                        .arg(m_features).arg(hasCode() ? QStringLiteral("已设置") : QStringLiteral("未设置")));
    if (!m_error.isEmpty())
        writeDiagnostic(m_error);
    connect(&m_network, &Network::connected, this, &Service::onConnected);
    connect(&m_network, &Network::changed, this, &Service::changed);
    connect(&m_network, &Network::disconnected, this, [this](Peer* peer) {
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
    m_code.fill(0);
}

QString Service::diagnosticDirectory() const
{
    return QFileInfo(m_path).absolutePath();
}

void Service::writeDiagnostic(const QString& message) const
{
    // 仅记录连接事件，调用者不得传入设备名称、安全码或业务内容。
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
    if (!file.exists())
        return;
    if (!file.open(QIODevice::ReadOnly) || file.size() > 8192) {
        m_error = QStringLiteral("无法读取连接设置");
        return;
    }
    const auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject() || doc.object().value("version").toInt() != 1) {
        m_error = QStringLiteral("连接设置格式无效");
        return;
    }
    const auto settings = doc.object();
    const auto name = cleanName(settings.value("name").toString());
    if (!name.isEmpty())
        m_name = name;
    const auto blob = QByteArray::fromBase64(settings.value("dpapi").toString().toLatin1(),
                                             QByteArray::AbortOnBase64DecodingErrors);
    if (!blob.isEmpty())
        m_code = protect(blob, false);
    if (m_code.isEmpty() && !settings.value("dpapi").toString().isEmpty())
        m_error = QStringLiteral("当前 Windows 用户无法解开安全码，请重新设置");
    if (!m_code.isEmpty())
        m_features = settings.value("features").toInt() & kFeatures;
    const auto alias = validAlias(settings.value("clipboardAlias").toString());
    if (!alias.isEmpty())
        m_clipboardAlias = alias;
    const auto peers = settings.value("peers").toArray();
    for (const auto& value : peers) {
        const auto item = value.toObject();
        const auto id = QByteArray::fromHex(item.value("id").toString().toLatin1());
        const QHostAddress address(item.value("address").toString());
        if (id.size() == kIdentityBytes && address.protocol() == QAbstractSocket::IPv4Protocol &&
            !address.isLoopback() && !address.isNull())
            m_knownPeers.insert(id, address);
    }
}

bool Service::save(const QString& name, QByteArrayView code, int features)
{
    const auto blob = protect(code, true);
    if ((!code.isEmpty() && blob.isEmpty()) || !QDir().mkpath(QFileInfo(m_path).absolutePath()))
        return false;
    QJsonArray peers;
    for (auto it = m_knownPeers.cbegin(); it != m_knownPeers.cend(); ++it)
        peers.append(QJsonObject{{"id", QString::fromLatin1(it.key().toHex())},
                                 {"address", it.value().toString()}});
    const auto json = QJsonDocument(QJsonObject{
        {"version", 1}, {"name", name}, {"dpapi", QString::fromLatin1(blob.toBase64())},
        {"features", features}, {"clipboardAlias", m_clipboardAlias}, {"peers", peers}})
                          .toJson(QJsonDocument::Compact);
    QSaveFile file(m_path);
    return file.open(QIODevice::WriteOnly) && file.write(json) == json.size() && file.commit();
}

bool Service::configure(const QString& name, const QString& newCode, QString& error)
{
    const auto validated = cleanName(name);
    auto code = newCode.isEmpty() ? m_code : cleanCode(newCode);
    if (validated.isEmpty() || (!newCode.isEmpty() && code.isEmpty())) {
        error = QStringLiteral("请输入设备名；安全码至少 6 个字符，且不能含控制字符。");
        return false;
    }
    if (!save(validated, code, m_features)) {
        error = QStringLiteral("无法保存设置或保护安全码。");
        return false;
    }
    const bool changedCode = code != m_code;
    m_error.clear();
    m_name = validated;
    m_code.fill(0);
    m_code = std::move(code);
    if (changedCode) {
        restart();
    } else {
        const auto peers = m_sessions.keys();
        for (auto* peer : peers) {
            if (m_sessions.contains(peer))
                peer->send(QByteArray(1, Hello) + QByteArray(1, char(m_features)) + m_name.toUtf8());
        }
    }
    emit changed();
    return true;
}

bool Service::setEnabled(int feature, bool enabled, QString& error)
{
    if (feature != Clipboard && feature != Files)
        return false;
    if (enabled && m_code.isEmpty()) {
        error = QStringLiteral("请先保存安全码，再启用插件。");
        return false;
    }
    const int next = enabled ? m_features | feature : m_features & ~feature;
    if (next == m_features)
        return true;
    if (!save(m_name, m_code, next)) {
        error = QStringLiteral("无法保存插件启用状态。");
        return false;
    }
    const bool active = m_features != 0;
    m_features = next;
    writeDiagnostic(QStringLiteral("插件开关：功能 %1，%2")
                        .arg(feature).arg(enabled ? QStringLiteral("启用") : QStringLiteral("关闭")));
    if (!active || !m_features) {
        restart();
    } else {
        if (feature == Clipboard)
            m_clipSequence = GetClipboardSequenceNumber();
        if (feature == Files && !enabled) {
            m_transfers.cancelIncoming();
            m_transfers.cancelOutgoing();
        }
        const auto peers = m_sessions.keys();
        for (auto* peer : peers) {
            if (m_sessions.contains(peer) &&
                !peer->send(QByteArray(1, Hello) + QByteArray(1, char(m_features)) + m_name.toUtf8()))
                peer->close();
        }
    }
    emit changed();
    return true;
}

void Service::restart()
{
    m_clipSequence = GetClipboardSequenceNumber();
    m_pulse.stop();
    m_network.stop();
    if (m_features && !m_code.isEmpty()) {
        m_network.setCachedAddresses(m_knownPeers.values());
        m_network.start(m_code);
        m_pulse.start();
    }
}

QString Service::status() const
{
    return m_error.isEmpty() ? m_network.status() : m_error;
}

QList<Service::Device> Service::devices() const
{
    QList<Device> result;
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it)
        if (it.value().features)
            result.push_back({it.key()->peerId(), it.value().name,
                              it.key()->address().toString(), it.value().features,
                              it.value().seen});
    std::sort(result.begin(), result.end(), [](const Device& a, const Device& b) {
        const int cmp = QString::localeAwareCompare(a.name, b.name);
        return cmp == 0 ? a.id < b.id : cmp < 0;
    });
    return result;
}

QList<Service::Discovery> Service::discoveredDevices() const
{
    QList<Discovery> result;
    for (const auto& item : m_network.discoveries()) {
        Discovery discovery;
        discovery.id = item.id;
        discovery.address = item.address.toString();
        discovery.features = item.features;
        discovery.lastSeen = item.lastSeen;
        discovery.phase = item.phase;
        for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
            if (it.key()->peerId() == item.id && it.value().features != 0) {
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
    if (!enabled(Clipboard) && !enabled(Files)) {
        error = QStringLiteral("请先启用一个局域网插件");
        return false;
    }
    return m_network.probe(parsed, error);
}

void Service::onConnected(Peer* peer)
{
    if (peer->peerId().size() == kIdentityBytes &&
        peer->address().protocol() == QAbstractSocket::IPv4Protocol &&
        !peer->address().isNull()) {
        m_knownPeers.insert(peer->peerId(), peer->address());
        save(m_name, m_code, m_features);
    }
    m_sessions.insert(peer, {{}, 0, m_clock.elapsed()});
    connect(peer, &Peer::message, this, [this, peer](const QByteArray& data) { onMessage(peer, data); });
    connect(peer, &Peer::drained, this, [this, peer] { m_transfers.pump(peer); });
    if (!peer->send(QByteArray(1, Hello) + QByteArray(1, char(m_features)) + m_name.toUtf8()))
        peer->close();
}

void Service::onMessage(Peer* peer, QByteArrayView packet)
{
    if (packet.isEmpty() || !m_sessions.contains(peer))
        return;
    const auto type = Wire(packet.front());
    const auto data = packet.sliced(1);
    auto& session = m_sessions[peer];
    const qint64 now = m_clock.elapsed();
    session.seen = now;
    if (type == Hello) {
        const int flags = data.isEmpty() ? 0 : uchar(data.front());
        const auto name = data.size() >= 2 && data.size() <= 193
                              ? cleanName(QString::fromUtf8(data.sliced(1))) : QString{};
        if (!flags || (flags & ~kFeatures) || name.isEmpty()) {
            peer->close();
            return;
        }
        const bool revoked = (session.features & Files) && !(flags & Files);
        if (!session.features)
            writeDiagnostic(QStringLiteral("已配对：%1").arg(peer->address().toString()));
        session.name = name;
        session.features = flags;
        if (revoked)
            m_transfers.disconnected(peer);
        emit changed();
        return;
    }
    if (!session.features) {
        peer->close();
        return;
    }
    if (type == Ping || type == Pong) {
        if (!data.isEmpty() || (type == Ping && !peer->send(QByteArray(1, Pong))))
            peer->close();
        return;
    }
    if (type == Text || type == Image) {
        if (!(m_features & session.features & Clipboard))
            return;
        if (type == Image && now - session.clipboardAt < 500)
            return;
        session.clipboardAt = now;
        if (!applyClipboard(type, data))
            peer->close();
        return;
    }
    if (type >= Offer && type <= Cancel) {
        if (!(m_features & session.features & Files))
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
    if (!(m_features & Files)) {
        error = QStringLiteral("局域网文件分享未启用");
        return false;
    }
    if (paths.isEmpty()) {
        error = QStringLiteral("没有选择要发送的文件或文件夹");
        return false;
    }
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
        if ((it.value().features & Files) && it.key()->peerId() == id) {
            m_transfers.sendOffer(it.key(), paths);
            return true;
        }
    }
    error = QStringLiteral("目标设备已断开或未启用文件分享");
    return false;
}

HotaruClipboardSnapshot Service::clipboardSnapshot() const
{
    HotaruClipboardSnapshot snapshot;
    const auto* mime = QApplication::clipboard()->mimeData();
    if (!mime) {
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
    if (!save(m_name, m_code, m_features)) {
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
        if (now - session.seen >= (session.features ? 15000 : 5000))
            peer->close();
        else if (session.features && now - session.seen >= 4500 && peer->pendingBytes() < 1024 &&
                 !peer->send(QByteArray(1, Ping)))
            peer->close();
    }
    capture();
}

void Service::copyCode()
{
    if (m_code.isEmpty())
        return;
    auto* mime = new QMimeData;
    mime->setText(QString::fromUtf8(m_code));
    mime->setData(QString::fromLatin1(kHotaruClipboardPrivate), QByteArrayLiteral("1"));
    mime->setData(QStringLiteral("application/x-qt-windows-mime;value=\"ExcludeClipboardContentFromMonitorProcessing\""),
                  QByteArrayLiteral("1"));
    QScopedValueRollback guard(m_writingClipboard, true);
    QApplication::clipboard()->setMimeData(mime);
    m_clipSequence = GetClipboardSequenceNumber();
}

void Service::capture()
{
    if (!(m_features & Clipboard) || m_writingClipboard)
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
        mime.hasFormat(QString::fromLatin1(kHotaruClipboardReceived)))
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
    QApplication::clipboard()->setMimeData(mime.release());
    m_clipSequence = GetClipboardSequenceNumber();
    return true;
}
}

#include "LanNetwork.h"
#include <utility>
#include <QNetworkDatagram>
#include <QNetworkProxy>
#include <QtEndian>
#include <QUuid>
#include <iterator>
#include <algorithm>
#include <string>
#include <vector>
#include <memory>
#include <QMetaObject>
#include <qt_windows.h>
#include <iphlpapi.h>
#include <netlistmgr.h>
#include <wrl/client.h>

namespace HotaruLan {
namespace {
const QHostAddress kGroup(QStringLiteral("239.255.88.45"));
constexpr quint16 kDiscoveryPort = 45456;
constexpr int kAdvertisementBytes = 4 + kIdentityBytes + 2 + 1;
constexpr int kInviting = 1;
constexpr int kInvitationNameBytes = 192;
constexpr int kMaxAdvertisementBytes = kAdvertisementBytes + 1 + kInvitationNameBytes;

QString invitationName(QByteArrayView bytes)
{
    if (bytes.size() > kInvitationNameBytes)
        return {};
    const auto text = QString::fromUtf8(bytes);
    if (text.size() > 48 || text.toUtf8() != bytes)
        return {};
    for (const auto ch : text)
        if (ch.category() == QChar::Other_Control || ch.category() == QChar::Other_Format)
            return {};
    return text.simplified();
}

QString advertisedName(const QByteArray& packet)
{
    if (packet.size() <= kAdvertisementBytes ||
        packet.size() != kAdvertisementBytes + 1 + uchar(packet.at(kAdvertisementBytes)))
        return {};
    return invitationName(QByteArrayView(packet).sliced(kAdvertisementBytes + 1));
}
const QByteArray kProbe(QByteArray(kWireVersion) + QByteArrayLiteral("?"));
const QByteArray kRegister(QByteArray(kWireVersion) + QByteArrayLiteral("R"));
constexpr int kMaxPeers = 8;
constexpr qint64 kReverseDelayMs = 3500;

QUuid interfaceGuid(const QString& name)
{
    // Qt 在 Windows 上通常返回系统接口名，只有回退路径才返回 GUID 字符串。
    const QUuid parsed(name);
    if (!parsed.isNull())
        return parsed;
    const auto systemName = name.toStdWString();
    NET_LUID luid{};
    GUID guid{};
    if (ConvertInterfaceNameToLuidW(systemName.c_str(), &luid) != NO_ERROR ||
        ConvertInterfaceLuidToGuid(&luid, &guid) != NO_ERROR)
        return {};
    wchar_t text[39]{};
    return StringFromGUID2(guid, text, int(std::size(text)))
               ? QUuid(QString::fromWCharArray(text)) : QUuid{};
}

struct Apartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
}

#ifdef Q_OS_WIN
struct Network::DnsRegistration {
    DNS_SERVICE_REGISTER_REQUEST request{};
    DNS_SERVICE_CANCEL cancel{};
    PDNS_SERVICE_INSTANCE instance = nullptr;
    std::wstring serviceName;
    std::wstring hostName;
    std::vector<std::wstring> keys;
    std::vector<std::wstring> values;
    std::vector<PCWSTR> keyPointers;
    std::vector<PCWSTR> valuePointers;
    QPointer<Network> network;
    bool stopping = false;
    bool registered = false;
};

struct Network::DnsBrowseContext {
    DNS_SERVICE_BROWSE_REQUEST request{};
    DNS_SERVICE_CANCEL cancel{};
    QPointer<Network> network;
    std::wstring query;
};

struct Network::DnsResolveContext {
    DNS_SERVICE_RESOLVE_REQUEST request{};
    DNS_SERVICE_CANCEL cancel{};
    QPointer<Network> network;
    std::wstring query;
};
#endif
QList<Network::Address> Network::privateAddresses(QString& error)
{
    using Microsoft::WRL::ComPtr;
    Apartment apartment;
    ComPtr<INetworkListManager> manager;
    const bool comReady = SUCCEEDED(apartment.result) || apartment.result == RPC_E_CHANGED_MODE;
    if (comReady)
        CoCreateInstance(__uuidof(NetworkListManager), nullptr, CLSCTX_ALL,
                         IID_PPV_ARGS(manager.GetAddressOf()));
    ComPtr<IEnumNetworkConnections> connections;
    QHash<QUuid, int> allowed;
    QSet<QUuid> denied;
    const bool profilesAvailable = manager && SUCCEEDED(manager->GetNetworkConnections(connections.GetAddressOf())) && connections;
    if (profilesAvailable) {
        ComPtr<INetworkConnection> connection;
        ULONG count = 0;
        while (connections->Next(1, connection.ReleaseAndGetAddressOf(), &count) == S_OK && count == 1) {
            GUID guid{};
            NET_LUID luid{};
            NET_IFINDEX index = 0;
            ComPtr<INetwork> network;
            VARIANT_BOOL isConnected = VARIANT_FALSE;
            NLM_NETWORK_CATEGORY category{};
            if (FAILED(connection->GetAdapterId(&guid)) ||
                ConvertInterfaceGuidToLuid(&guid, &luid) != NO_ERROR ||
                ConvertInterfaceLuidToIndex(&luid, &index) != NO_ERROR)
                continue;
            if (FAILED(connection->GetNetwork(network.GetAddressOf())) ||
                FAILED(network->get_IsConnected(&isConnected)) || isConnected != VARIANT_TRUE)
                continue;
            wchar_t guidText[39]{};
            if (!StringFromGUID2(guid, guidText, int(std::size(guidText))))
                continue;
            const QUuid adapter(QString::fromWCharArray(guidText));
            if (SUCCEEDED(network->GetCategory(&category)) && category == NLM_NETWORK_CATEGORY_PRIVATE)
                allowed.insert(adapter, int(index));
            else
                denied.insert(adapter);
        }
    }
    for (const auto& adapter : denied)
        allowed.remove(adapter);
    error = QStringLiteral("Windows 网络配置：读取%1，专用接口 %2，排除接口 %3")
                .arg(profilesAvailable ? QStringLiteral("成功") : QStringLiteral("失败"))
                .arg(allowed.size()).arg(denied.size());
    QList<Address> result;
    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        // Qt 优先使用 IPv6 接口索引，Windows 网络配置用适配器 GUID 对齐。
        const int windowsIndex = allowed.value(interfaceGuid(iface.name()), 0);
        if (!windowsIndex || !flags.testFlag(QNetworkInterface::IsUp) ||
            !flags.testFlag(QNetworkInterface::IsRunning) ||
            flags.testFlag(QNetworkInterface::IsLoopBack))
            continue;
        for (const auto& address : iface.addressEntries()) {
            if (address.ip().protocol() == QAbstractSocket::IPv4Protocol &&
                address.prefixLength() > 0 && address.prefixLength() < 32 &&
                !address.ip().isLoopback() && !address.ip().isNull()) {
                result.push_back({iface.index(), address.ip(), address.prefixLength(), windowsIndex});
                break;
            }
        }
        if (result.size() >= 8)
            break;
    }
    std::sort(result.begin(), result.end(), [](const Address& a, const Address& b) {
        return a.index < b.index;
    });
    error += QStringLiteral("，匹配可用 IPv4 %1 个").arg(result.size());
    return result;
}

QList<Network::Discovery> Network::discoveries() const
{
    return m_discoveries.values();
}

void Network::recordDiscovery(const QByteArray& id, const QHostAddress& address, quint16 port,
                              int interfaceIndex, int features, QString name)
{
    if (id.size() != kIdentityBytes || address.protocol() != QAbstractSocket::IPv4Protocol ||
        address.isBroadcast() || address.isMulticast() || address.isNull() || port == 0 || id == m_ownId)
        return;
    if (!m_discoveries.contains(id)) {
        if (m_discoveries.size() >= 64)
            return;
        emit diagnostic(QStringLiteral("发现候选：%1:%2，经接口 %3")
                            .arg(address.toString()).arg(port).arg(interfaceIndex));
    }
    auto& item = m_discoveries[id];
    const auto now = m_clock.elapsed();
    if (item.id.isEmpty())
        item.firstSeen = now;
    const auto previous = item;
    item.id = id;
    item.address = address;
    item.port = port;
    item.interfaceIndex = interfaceIndex;
    item.features = features & kInviting;
    if (!item.features || !previous.features || previous.address != address || !name.isEmpty())
        item.name = item.features ? std::move(name) : QString{};
    item.lastSeen = now;
    const auto candidate = item;
    if (previous.id.isEmpty() || previous.address != item.address ||
        previous.features != item.features || previous.name != item.name)
        emit changed();
    if (!m_joinCode.isEmpty() && address == m_joinAddress && (features & kInviting)) {
        auto code = std::exchange(m_joinCode, {});
        if (!connectCandidate(candidate, std::move(code))) {
            m_joinTimer.stop();
            emit invitationFailed();
        }
        return;
    }
    tryCandidate(candidate, now);
}

QByteArray Network::advertisement(quint16 port) const
{
    QByteArray packet = QByteArray(kWireVersion) + m_ownId;
    char portBytes[2];
    qToBigEndian(port, portBytes);
    packet.append(portBytes, sizeof(portBytes));
    const bool active = invitationActive();
    packet.append(char(active ? kInviting : 0));
    if (active && !m_invitationName.isEmpty()) {
        const auto bytes = m_invitationName.toUtf8();
        packet.append(char(bytes.size()));
        packet.append(bytes);
    }
    return packet;
}

void Network::sendAnnouncement(const Endpoint& endpoint, const QHostAddress& destination,
                               quint16 destinationPort)
{
    if (m_udp.state() != QAbstractSocket::BoundState || !endpoint.listener ||
        destination.isNull() || destinationPort == 0)
        return;
    QNetworkDatagram packet(advertisement(endpoint.listener->serverPort()), destination, destinationPort);
    if (destination.isMulticast())
        packet.setInterfaceIndex(endpoint.address.index);
    else
        packet.setSender(endpoint.address.ip);
    m_udp.writeDatagram(packet);
}

bool Network::probe(const QHostAddress& address, QString& error)
{
    for (const auto& endpoint : m_endpoints) {
        const auto& local = endpoint.address;
        if (address == local.ip || address.isBroadcast() || address.isMulticast() ||
            !address.isInSubnet(local.ip, local.prefix))
            continue;
        emit diagnostic(QStringLiteral("手动探测 %1，经接口 %2").arg(address.toString()).arg(local.index));
        const bool tcp = registerProbe(local, address, true);
        bool udp = false;
        if (m_udp.state() == QAbstractSocket::BoundState) {
            QNetworkDatagram packet(kProbe, address, kDiscoveryPort);
            packet.setSender(local.ip);
            udp = m_udp.writeDatagram(packet) == kProbe.size();
            emit diagnostic(QStringLiteral("手动 UDP 探测：%1").arg(
                udp ? QStringLiteral("已发出") : m_udp.errorString()));
        }
        if (tcp || udp)
            return true;
        error = QStringLiteral("未发出探测：地址无效、端口不可用或刚探测过，请稍后重试。");
        return false;
    }
    if (m_addresses.isEmpty())
        error = QStringLiteral("未发出探测：本机没有可用的专用 IPv4 网卡。");
    else if (m_endpoints.isEmpty())
        error = QStringLiteral("未发出探测：本机 TCP 45456 未监听，请查看端口绑定状态。");
    else
        error = QStringLiteral("未发出探测：请输入同一专用子网内另一台电脑的 IPv4。");
    emit diagnostic(QStringLiteral("手动探测 %1：%2").arg(address.toString(), error));
    return false;
}

#ifdef Q_OS_WIN
void Network::startDnsSd()
{
    m_lastDnsAttempt = m_clock.elapsed();
    stopDnsSd();
    if (m_endpoints.isEmpty()) return;
    const QString id = QString::fromLatin1(m_ownId.toHex());
    const QString host = QStringLiteral("hotaru-%1.local").arg(id.left(12));
    for (const auto& endpoint : m_endpoints) {
        auto* registration = new DnsRegistration;
        registration->network = this;
        registration->serviceName = QStringLiteral("%1._hotaru._tcp.local").arg(id).toStdWString();
        registration->hostName = host.toStdWString();
        registration->keys = {L"proto", L"id", L"features", L"build"};
        registration->values = {QString::fromLatin1(kWireVersion).toStdWString(), id.toStdWString(),
                                invitationActive() ? L"1" : L"0", L"2"};
        if (invitationActive() && !m_invitationName.isEmpty()) {
            registration->keys.push_back(L"name");
            registration->values.push_back(m_invitationName.toStdWString());
        }
        for (const auto& value : registration->keys) registration->keyPointers.push_back(value.c_str());
        for (const auto& value : registration->values) registration->valuePointers.push_back(value.c_str());
        registration->instance = DnsServiceConstructInstance(
            registration->serviceName.c_str(), registration->hostName.c_str(), nullptr, nullptr,
            endpoint.listener->serverPort(), 0, 0,
            DWORD(registration->keyPointers.size()), registration->keyPointers.data(), registration->valuePointers.data());
        if (!registration->instance) { delete registration; continue; }
        registration->request.Version = DNS_QUERY_REQUEST_VERSION1;
        registration->request.InterfaceIndex = DWORD(endpoint.address.windowsIndex);
        registration->request.pServiceInstance = registration->instance;
        registration->request.pRegisterCompletionCallback = &Network::dnsRegisterCallback;
        registration->request.pQueryContext = registration;
        const DWORD status = DnsServiceRegister(&registration->request, &registration->cancel);
        if (status != DNS_REQUEST_PENDING && status != ERROR_SUCCESS) {
            DnsServiceFreeInstance(registration->instance);
            delete registration;
            continue;
        }
        m_dnsRegistrations.push_back(registration);
    }
    auto* browse = new DnsBrowseContext;
    browse->network = this;
    browse->query = L"_hotaru._tcp.local";
    browse->request.Version = DNS_QUERY_REQUEST_VERSION1;
    browse->request.InterfaceIndex = 0;
    browse->request.QueryName = browse->query.c_str();
    browse->request.pBrowseCallback = &Network::dnsBrowseCallback;
    browse->request.pQueryContext = browse;
    const DNS_STATUS status = DnsServiceBrowse(&browse->request, &browse->cancel);
    m_dnsBrowseStarted = status == DNS_REQUEST_PENDING || status == ERROR_SUCCESS;
    if (m_dnsBrowseStarted) {
        m_dnsBrowseContext = browse;
        m_dnsBrowseRequest = &browse->request;
    } else {
        delete browse;
        m_dnsBrowseRequest = nullptr;
    }
    m_dnsReady = false;
    emit diagnostic(QStringLiteral("DNS-SD：提交注册 %1 项，浏览返回 %2")
                        .arg(m_dnsRegistrations.size()).arg(status));
}

void Network::stopDnsSd()
{
    if (m_dnsBrowseStarted && m_dnsBrowseContext) {
        DnsServiceBrowseCancel(&m_dnsBrowseContext->cancel);
        m_dnsBrowseStarted = false;
    }
    m_dnsBrowseRequest = nullptr;
    for (auto* registration : std::as_const(m_dnsRegistrations)) {
        if (registration->stopping) continue;
        registration->stopping = true;
        if (registration->registered)
            DnsServiceDeRegister(&registration->request, nullptr);
        else
            DnsServiceRegisterCancel(&registration->cancel);
    }
    for (auto* context : std::as_const(m_dnsResolves))
        DnsServiceResolveCancel(&context->cancel);
    m_dnsReady = false;
}

void Network::finishDnsRegistration(DnsRegistration* registration, DWORD status)
{
    if (!registration) return;
    if (!registration->stopping)
        emit diagnostic(QStringLiteral("DNS-SD 注册结果：%1").arg(status));
    if (!registration->stopping && status == ERROR_SUCCESS) {
        registration->registered = true;
        m_dnsReady = true;
        emit changed();
        return;
    }
    m_dnsRegistrations.removeOne(registration);
    if (registration->instance) DnsServiceFreeInstance(registration->instance);
    delete registration;
}

void Network::finishDnsBrowse(DnsBrowseContext* context)
{
    if (m_dnsBrowseContext == context) {
        m_dnsBrowseContext = nullptr;
        m_dnsBrowseRequest = nullptr;
    }
    delete context;
}

void WINAPI Network::dnsRegisterCallback(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance)
{
    if (instance) DnsServiceFreeInstance(instance);
    auto* registration = static_cast<DnsRegistration*>(context);
    if (!registration) return;
    if (auto* network = registration->network.data()) {
        QMetaObject::invokeMethod(network, [network, registration, status] {
            network->finishDnsRegistration(registration, status);
        }, Qt::QueuedConnection);
    } else {
        delete registration;
    }
}

void WINAPI Network::dnsBrowseCallback(DWORD status, PVOID context, PDNS_RECORD record)
{
    auto* browse = static_cast<DnsBrowseContext*>(context);
    const auto network = browse ? browse->network : QPointer<Network>{};
    if (network && status == ERROR_SUCCESS && record) {
        for (auto* current = record; current; current = current->pNext) {
            if (current->wType != DNS_TYPE_PTR || !current->Data.PTR.pNameHost) continue;
            const QString name = QString::fromWCharArray(current->Data.PTR.pNameHost);
            QMetaObject::invokeMethod(network.data(), [network, name] {
                if (network) network->resolveDnsService(name);
            }, Qt::QueuedConnection);
        }
    }
    if (record) DnsRecordListFree(record, DnsFreeFlat);
    if (browse && status == ERROR_CANCELLED) {
        if (network) {
            QMetaObject::invokeMethod(network.data(), [network, browse] {
                if (network) network->finishDnsBrowse(browse);
            }, Qt::QueuedConnection);
        } else {
            delete browse;
        }
    }
}

void Network::resolveDnsService(const QString& name)
{
    auto* context = new DnsResolveContext;
    context->network = this;
    context->query = name.toStdWString();
    context->request.Version = DNS_QUERY_REQUEST_VERSION1;
    context->request.InterfaceIndex = 0;
    context->request.QueryName = const_cast<PWSTR>(context->query.c_str());
    context->request.pResolveCompletionCallback = &Network::dnsResolveCallback;
    context->request.pQueryContext = context;
    const DNS_STATUS status = DnsServiceResolve(&context->request, &context->cancel);
    if (status != DNS_REQUEST_PENDING && status != ERROR_SUCCESS) {
        delete context;
        return;
    }
    m_dnsResolves.push_back(context);
}

void WINAPI Network::dnsResolveCallback(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance)
{
    auto* resolve = static_cast<DnsResolveContext*>(context);
    if (!resolve) return;
    const auto copy = instance ? DnsServiceCopyInstance(instance) : nullptr;
    if (auto* network = resolve->network.data()) {
        QMetaObject::invokeMethod(network, [network, resolve, status, copy] {
            if (status == ERROR_SUCCESS && copy && copy->ip4Address && copy->wPort) {
                const QHostAddress address(qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(copy->ip4Address)));
                QByteArray peerId;
                bool validProto = false;
                int flags = 0;
                QString name;
                for (DWORD i = 0; i < copy->dwPropertyCount; ++i) {
                    if (!copy->keys[i] || !copy->values[i]) continue;
                    const auto key = QString::fromWCharArray(copy->keys[i]);
                    const auto value = QString::fromWCharArray(copy->values[i]);
                    if (key == QStringLiteral("proto")) validProto = value == QLatin1String(kWireVersion);
                    if (key == QStringLiteral("id")) peerId = QByteArray::fromHex(value.toLatin1());
                    if (key == QStringLiteral("features")) flags = value.toInt();
                    if (key == QStringLiteral("name")) name = invitationName(value.toUtf8());
                }
                if (validProto && peerId.size() == kIdentityBytes)
                    network->adoptDnsEndpoint(address, copy->wPort, peerId, flags, name);
            }
            if (copy) DnsServiceFreeInstance(copy);
            network->m_dnsResolves.removeOne(resolve);
            delete resolve;
        }, Qt::QueuedConnection);
    } else {
        if (copy) DnsServiceFreeInstance(copy);
        delete resolve;
    }
}

void Network::adoptDnsEndpoint(const QHostAddress& source, quint16 port, const QByteArray& peerId, int flags, const QString& name)
{
    if (source.isNull() || port == 0 || peerId.isEmpty() || peerId == m_ownId) return;
    for (const auto& endpoint : m_endpoints) {
        if (source != endpoint.address.ip && source.isInSubnet(endpoint.address.ip, endpoint.address.prefix)) {
            recordDiscovery(peerId, source, port, endpoint.address.index, flags, name);
            return;
        }
    }
}
#endif
Network::Network(QObject* parent) : QObject(parent)
{
    // 局域网连接只走选中的本地网卡，不能继承系统或应用代理。
    m_udp.setProxy(QNetworkProxy::NoProxy);
    m_clock.start();
    m_timer.setInterval(2500);
    connect(&m_timer, &QTimer::timeout, this, &Network::tick);
    connect(&m_udp, &QUdpSocket::readyRead, this, &Network::readDiscovery);
    m_inviteTimer.setSingleShot(true);
    connect(&m_inviteTimer, &QTimer::timeout, this, &Network::endInvitation);
    m_joinTimer.setSingleShot(true);
    connect(&m_joinTimer, &QTimer::timeout, this, [this] {
        m_joinCode.fill(0);
        m_joinCode.clear();
        const auto peers = m_peers.values();
        for (auto* peer : peers)
            if (peer->invitation() && peer->initiatedLocally())
                peer->close();
        emit invitationFailed();
    });
    setStatus(QStringLiteral("连接未启用"));
}

Network::~Network() { stop(); }

void Network::setCachedAddresses(const QList<QHostAddress>& addresses)
{
    m_cachedAddresses.clear();
    for (const auto& address : addresses) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback() &&
            !address.isNull() && !m_cachedAddresses.contains(address))
            m_cachedAddresses.push_back(address);
    }
}

void Network::start(const QByteArray& identity)
{
    stop();
    m_ownId = identity;
    if (m_ownId.size() != kIdentityBytes) {
        setStatus(QStringLiteral("连接初始化失败"));
        return;
    }
    emit diagnostic(QStringLiteral("LAN 服务启动：TCP/UDP 固定端口 %1").arg(kDiscoveryPort));
    m_timer.start();
    tick();
}

void Network::setCredentialLookup(std::function<QByteArray(const QByteArray&)> lookup)
{
    m_credential = std::move(lookup);
}

bool Network::invitationActive() const
{
    return m_inviteTimer.isActive() && !m_inviteCode.isEmpty();
}

QStringList Network::localAddresses() const
{
    QStringList result;
    for (const auto& endpoint : m_endpoints)
        if (endpoint.listener->isListening())
            result.push_back(endpoint.address.ip.toString());
    result.removeDuplicates();
    return result;
}

void Network::setInvitationName(const QString& name)
{
    const auto cleaned = invitationName(name.toUtf8());
    if (m_invitationName == cleaned)
        return;
    m_invitationName = cleaned;
#ifdef Q_OS_WIN
    if (invitationActive() && m_timer.isActive())
        startDnsSd();
#endif
}

QByteArray Network::beginInvitation()
{
    if (m_endpoints.isEmpty() || m_joinTimer.isActive() || invitationInProgress())
        return {};
    // 拒绝采样，避免把随机数映射到五位数字时产生偏差。
    quint32 value;
    do {
        const auto bytes = randomBytes(sizeof(value));
        if (bytes.size() != sizeof(value))
            return {};
        value = qFromBigEndian<quint32>(bytes.constData());
    } while (value >= 4294900000u);
    m_inviteCode = QByteArray::number(value % 100000).rightJustified(5, '0');
    m_inviteAttempts = 0;
    m_inviteTimer.start(120000);
#ifdef Q_OS_WIN
    if (m_timer.isActive())
        startDnsSd();
#endif
    emit changed();
    return m_inviteCode;
}

void Network::endInvitation()
{
    m_inviteTimer.stop();
    consumeInvitation();
    const auto peers = m_peers.values();
    for (auto* peer : peers)
        if (peer->invitation() && !peer->initiatedLocally())
            peer->close();
    emit changed();
}

void Network::consumeInvitation()
{
    if (m_inviteCode.isEmpty())
        return;
    m_inviteCode.fill(0);
    m_inviteCode.clear();
#ifdef Q_OS_WIN
    if (m_timer.isActive())
        startDnsSd();
#endif
}

bool Network::joinInvitation(const QHostAddress& address, const QByteArray& code, QString& error)
{
    if (m_joinTimer.isActive() || invitationInProgress()) {
        error = QStringLiteral("已有配对请求正在处理");
        return false;
    }
    m_joinTimer.start(120000);
    for (const auto& candidate : m_discoveries) {
        if (candidate.address == address && (candidate.features & kInviting)) {
            if (connectCandidate(candidate, code))
                return true;
            m_joinTimer.stop();
            error = QStringLiteral("连接邀请失败，请稍后重试");
            return false;
        }
    }
    m_joinAddress = address;
    m_joinCode = code;
    if (probe(address, error)) {
        m_joinTimer.start(8000);
        return true;
    }
    m_joinCode.fill(0);
    m_joinCode.clear();
    m_joinTimer.stop();
    return false;
}

void Network::forget(const QByteArray& id)
{
    const auto peers = m_peers.values();
    for (auto* peer : peers)
        if (peer->peerId() == id)
            peer->close();
}

void Network::closeTopology()
{
    m_inviteTimer.stop();
    m_inviteCode.fill(0);
    m_inviteCode.clear();
    m_joinTimer.stop();
    m_joinCode.fill(0);
    m_joinCode.clear();
#ifdef Q_OS_WIN
    stopDnsSd();
#endif
    m_udp.close();
    const auto sockets = m_registerSockets | m_pendingIncoming;
    m_registerSockets.clear();
    m_pendingIncoming.clear();
    for (auto* socket : sockets) {
        socket->abort();
        socket->deleteLater();
    }
    m_tcpError.clear();
    m_udpError.clear();
    m_registerAttempts.clear();
    m_registerInterfaceAttempts.clear();
    m_registerTotalAttempts.clear();
    for (auto& endpoint : m_endpoints) {
        endpoint.listener->close();
        endpoint.listener->deleteLater();
    }
    m_endpoints.clear();
    const auto peers = m_peers;
    for (auto* peer : peers)
        peer->close();
}

void Network::stop()
{
    if (m_timer.isActive())
        emit diagnostic(QStringLiteral("LAN 服务停止"));
    m_timer.stop();
    closeTopology();
    m_addresses.clear();
    m_discoveries.clear();
    m_incomingAttempts.clear();
    m_outgoingAttempts.clear();
    m_interfaceAttempts.clear();
    m_totalAttempts.clear();
    m_lastReject = -60000;
    setStatus(QStringLiteral("连接未启用"));
}

void Network::setStatus(QString value)
{
    if (value != m_status) {
        m_status = std::move(value);
        emit diagnostic(m_status);
        emit changed();
    }
}

void Network::openTopology(const QList<Address>& addresses, quint16 port)
{
    if (addresses.isEmpty())
        return;

    // UDP 绑定、组播加入和 TCP 监听是独立能力；一个失败不能撤销另一个。
    if (m_udp.state() != QAbstractSocket::BoundState) {
        const bool bound = m_udp.bind(QHostAddress::AnyIPv4, port,
                                     QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
        m_udpError = bound ? QString{} : m_udp.errorString();
        if (bound)
            m_udp.setSocketOption(QAbstractSocket::MulticastTtlOption, 1);
    }

    QStringList tcpErrors;
    for (const auto& address : addresses) {
        auto endpoint = std::find_if(m_endpoints.begin(), m_endpoints.end(),
                                    [&](const Endpoint& e) { return e.address == address; });
        if (endpoint == m_endpoints.end()) {
            auto* server = new QTcpServer(this);
            server->setProxy(QNetworkProxy::NoProxy);
            server->setMaxPendingConnections(kMaxPeers);
            if (!server->listen(address.ip, port)) {
                tcpErrors.append(QStringLiteral("%1：%2").arg(address.ip.toString(), server->errorString()));
                delete server;
                continue;
            }
            m_endpoints.push_back({address, server, false});
            endpoint = std::prev(m_endpoints.end());
            emit diagnostic(QStringLiteral("TCP 监听：接口 %1，%2:%3")
                                .arg(address.index).arg(address.ip.toString()).arg(server->serverPort()));
            connect(server, &QTcpServer::newConnection, this, [this, server, address] {
                while (auto* socket = server->nextPendingConnection()) {
                    const auto source = socket->peerAddress();
                    if (m_pendingIncoming.size() >= 24 || socket->localAddress() != address.ip ||
                        !source.isInSubnet(address.ip, address.prefix)) {
                        socket->abort();
                        socket->deleteLater();
                        continue;
                    }
                    classifyIncoming(socket, address);
                }
            });
        }
        if (m_udp.state() == QAbstractSocket::BoundState && !endpoint->multicastAttempted) {
            endpoint->multicastAttempted = true;
            endpoint->multicastJoined = m_udp.joinMulticastGroup(
                kGroup, QNetworkInterface::interfaceFromIndex(address.index));
            if (!endpoint->multicastJoined)
                emit diagnostic(QStringLiteral("组播加入失败：接口 %1，%2")
                                    .arg(address.index).arg(m_udp.errorString()));
        }
    }
    m_tcpError = tcpErrors.join(QStringLiteral("；"));
}

QString Network::listenerStatus() const
{
    const auto multicast = std::count_if(m_endpoints.cbegin(), m_endpoints.cend(),
                                         [](const Endpoint& e) { return e.multicastJoined; });
    const auto tcp = m_tcpError.isEmpty() ? QStringLiteral("TCP 监听 %1 个地址").arg(m_endpoints.size())
                                         : QStringLiteral("TCP 监听 %1 个地址；绑定失败 %2")
                                               .arg(m_endpoints.size()).arg(m_tcpError);
    const auto udp = m_udp.state() == QAbstractSocket::BoundState
                         ? QStringLiteral("UDP 已绑定；组播 %1/%2").arg(multicast).arg(m_endpoints.size())
                         : QStringLiteral("UDP 未绑定：%1").arg(m_udpError);
    return QStringLiteral("端口 %1：%2；%3").arg(kDiscoveryPort).arg(tcp, udp);
}

void Network::tick()
{
    QString profileStatus;
    const auto addresses = privateAddresses(profileStatus);
    const bool changedTopology = addresses != m_addresses;
    if (changedTopology) {
        closeTopology();
        m_addresses = addresses;
        m_discoveries.clear();
        emit diagnostic(profileStatus);
        for (const auto& address : addresses)
            emit diagnostic(QStringLiteral("选中网卡：Qt 接口 %1，Windows IPv4 接口 %2，%3/%4")
                                .arg(address.index).arg(address.windowsIndex)
                                .arg(address.ip.toString()).arg(address.prefix));
    }
    if (addresses.isEmpty()) {
        setStatus(QStringLiteral("没有可用的 Windows 专用 IPv4 网络；%1").arg(profileStatus));
        return;
    }
    const auto before = m_endpoints.size();
    openTopology(addresses, kDiscoveryPort);
#ifdef Q_OS_WIN
    if (before != m_endpoints.size())
        startDnsSd();
#endif
    const auto now = m_clock.elapsed();
    bool discoveryChanged = false;
    for (auto it = m_discoveries.begin(); it != m_discoveries.end();) {
        if (now - it->lastSeen > 15000) {
            it = m_discoveries.erase(it);
            discoveryChanged = true;
        } else
            ++it;
    }
    if (discoveryChanged)
        emit changed();
    retryCandidates(now);
#ifdef Q_OS_WIN
    if (!m_endpoints.isEmpty() && !m_dnsReady && m_dnsRegistrations.isEmpty() && m_clock.elapsed() - m_lastDnsAttempt >= 15000)
        startDnsSd();
#endif
    setStatus(listenerStatus() + (now - m_lastReject < 30000
                  ? QStringLiteral("；认证或加密校验未通过")
                  : m_endpoints.isEmpty() ? QString{} :
#ifdef Q_OS_WIN
                    m_dnsReady ? QStringLiteral("；DNS-SD 可用") :
#endif
                    QStringLiteral("；正在发现设备")));
    for (const auto& cached : std::as_const(m_cachedAddresses)) {
        const auto endpoint = std::find_if(m_endpoints.cbegin(), m_endpoints.cend(),
                                           [&](const Endpoint& item) {
                                               return cached.isInSubnet(item.address.ip, item.address.prefix);
                                           });
        if (endpoint != m_endpoints.cend())
            registerProbe(endpoint->address, cached);
    }
    for (const auto& endpoint : m_endpoints) {
        const auto& address = endpoint.address;
        if (m_udp.state() != QAbstractSocket::BoundState)
            break;
        if (endpoint.multicastJoined)
            sendAnnouncement(endpoint, kGroup, kDiscoveryPort);
        const quint32 ip = address.ip.toIPv4Address();
        const quint32 mask = address.prefix == 0 ? 0u : 0xffffffffu << (32 - address.prefix);
        const QHostAddress directed((ip & mask) | ~mask);
        if (address.prefix <= 30)
            sendAnnouncement(endpoint, directed, kDiscoveryPort);
    }
    if (m_discoveries.isEmpty() && (now % 15000) < 2500) {
        for (const auto& address : m_addresses) {
            if (address.prefix < 24)
                continue;
            const quint32 ip = address.ip.toIPv4Address();
            const quint32 mask = 0xffffffffu << (32 - address.prefix);
            const quint32 network = ip & mask;
            const quint32 broadcastAddress = network | ~mask;
            const quint32 first = network + 1;
            const quint32 last = std::min(broadcastAddress - 1, first + 254u);
            const auto endpoint = std::find_if(m_endpoints.cbegin(), m_endpoints.cend(),
                                               [&](const Endpoint& item) { return item.address.index == address.index; });
            if (endpoint == m_endpoints.cend())
                continue;
            for (quint32 candidate = first; candidate <= last; ++candidate) {
                const QHostAddress destination(candidate);
                if (destination == address.ip)
                    continue;
                registerProbe(endpoint->address, destination);
            }
        }
    }
}

bool Network::registerProbe(const Address& endpoint, const QHostAddress& destination, bool manual)
{
    if (destination.protocol() != QAbstractSocket::IPv4Protocol || destination == endpoint.ip ||
        !destination.isInSubnet(endpoint.ip, endpoint.prefix) ||
        m_registerSockets.size() >= 64 || !registerAttempt(endpoint.index, destination))
        return false;

    auto* socket = new QTcpSocket(this);
    socket->setProxy(QNetworkProxy::NoProxy);
    socket->setReadBufferSize(kMaxAdvertisementBytes + 1);
    m_registerSockets.insert(socket);
    const auto finish = [this, socket] {
        if (m_registerSockets.remove(socket)) {
            socket->abort();
            socket->deleteLater();
        }
    };
    connect(socket, &QTcpSocket::connected, this, [this, socket, manual] {
        if (manual)
            emit diagnostic(QStringLiteral("手动 TCP 探测：连接已建立"));
        socket->write(kRegister);
    });
    const auto readResponse = [this, socket, endpoint, destination, finish, manual] {
        if (!m_registerSockets.contains(socket))
            return;
        const auto data = socket->readAll();
        const auto peerId = data.mid(4, kIdentityBytes);
        if (data.size() >= kAdvertisementBytes && data.size() <= kMaxAdvertisementBytes &&
            data.startsWith(kWireVersion) && peerId != m_ownId &&
            qFromBigEndian<quint16>(data.constData() + 4 + kIdentityBytes) == kDiscoveryPort) {
            emit diagnostic(QStringLiteral("TCP 注册已回应：%1:%2").arg(destination.toString()).arg(kDiscoveryPort));
            recordDiscovery(peerId, destination, kDiscoveryPort, endpoint.index,
                            uchar(data.at(kAdvertisementBytes - 1)), advertisedName(data));
        } else if (manual) {
            emit diagnostic(QStringLiteral("手动 TCP 探测：收到无效的 Hotaru 注册回应"));
        }
        finish();
    };
    // 注册服务写完即关闭；等完整回应，避免 TCP 分片时只读到不含名称的旧头部。
    connect(socket, &QTcpSocket::disconnected, this, readResponse);
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket, manual, finish](auto error) {
        if (error == QAbstractSocket::RemoteHostClosedError)
            return;
        if (manual && m_registerSockets.contains(socket))
            emit diagnostic(QStringLiteral("手动 TCP 探测失败：%1 (%2)").arg(socket->errorString()).arg(int(error)));
        finish();
    });
    QTimer::singleShot(2500, socket, [this, socket, manual, finish] {
        if (manual && m_registerSockets.contains(socket))
            emit diagnostic(QStringLiteral("手动 TCP 探测：等待连接或注册回应超时"));
        finish();
    });
    if (!socket->bind(endpoint.ip)) {
        if (manual && m_registerSockets.contains(socket))
            emit diagnostic(QStringLiteral("手动 TCP 探测：本地绑定失败，%1").arg(socket->errorString()));
        finish();
        return false;
    }
    socket->connectToHost(destination, kDiscoveryPort);
    return true;
}

bool Network::registerAttempt(int iface, const QHostAddress& destination)
{
    if (destination.isNull() || destination.isLoopback() || destination.isBroadcast() ||
        destination.isMulticast())
        return false;
    const auto now = m_clock.elapsed();
    for (auto it = m_registerAttempts.begin(); it != m_registerAttempts.end();) {
        if (it.value().next <= now)
            it = m_registerAttempts.erase(it);
        else
            ++it;
    }
    m_registerTotalAttempts.removeIf([now](qint64 value) { return now - value >= 15000; });
    for (auto it = m_registerInterfaceAttempts.begin(); it != m_registerInterfaceAttempts.end();) {
        it.value().removeIf([now](qint64 value) { return now - value >= 15000; });
        if (it.value().isEmpty())
            it = m_registerInterfaceAttempts.erase(it);
        else
            ++it;
    }
    if (m_registerAttempts.contains(destination) || m_registerTotalAttempts.size() >= 64 ||
        m_registerInterfaceAttempts.value(iface).size() >= 64)
        return false;
    m_registerAttempts.insert(destination, {now + 30000});
    m_registerTotalAttempts.push_back(now);
    m_registerInterfaceAttempts[iface].push_back(now);
    return true;
}

void Network::classifyIncoming(QTcpSocket* socket, const Address& address)
{
    m_pendingIncoming.insert(socket);
    const auto cleanup = connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
        m_pendingIncoming.remove(socket);
        socket->deleteLater();
    });
    auto* timer = new QTimer(socket);
    timer->setSingleShot(true);
    timer->setInterval(1500);
    const auto classify = [this, socket, address, timer, cleanup] {
        if (!socket || socket->state() == QAbstractSocket::UnconnectedState)
            return;
        if (socket->bytesAvailable() < kRegister.size())
            return;
        if (socket->peek(kRegister.size()) == kRegister) {
            QObject::disconnect(socket, &QTcpSocket::readyRead, socket, nullptr);
            socket->read(kRegister.size());
            socket->write(advertisement(kDiscoveryPort));
            socket->disconnectFromHost();
            return;
        }
        QObject::disconnect(socket, &QTcpSocket::readyRead, socket, nullptr);
        timer->stop();
        if (!attempt(address.index, socket->peerAddress(), false)) {
            socket->abort();
            return;
        }
        QObject::disconnect(cleanup);
        m_pendingIncoming.remove(socket);
        emit diagnostic(QStringLiteral("收到对端认证连接：%1").arg(socket->peerAddress().toString()));
        adopt(socket, socket->peerAddress(), {});
    };
    connect(socket, &QTcpSocket::readyRead, socket, classify);
    connect(timer, &QTimer::timeout, socket, [socket] {
        socket->abort();
    });
    timer->start();
    if (socket->bytesAvailable() >= kRegister.size())
        classify();
}

bool Network::attempt(int iface, const QHostAddress& source, bool outgoing)
{
    if (m_peers.size() >= kMaxPeers || source.isNull() ||
        source.isBroadcast() || source.isMulticast())
        return false;
    const auto now = m_clock.elapsed();
    auto& attempts = outgoing ? m_outgoingAttempts : m_incomingAttempts;
    for (auto it = attempts.begin(); it != attempts.end();) {
        if (it.value().next <= now)
            it = attempts.erase(it);
        else
            ++it;
    }
    if (attempts.contains(source))
        return false;
    m_totalAttempts.removeIf([now](qint64 t) { return now - t >= 60000; });
    for (auto it = m_interfaceAttempts.begin(); it != m_interfaceAttempts.end();) {
        it.value().removeIf([now](qint64 t) { return now - t >= 60000; });
        if (it.value().isEmpty())
            it = m_interfaceAttempts.erase(it);
        else
            ++it;
    }
    if (m_totalAttempts.size() >= 24 || m_interfaceAttempts.value(iface).size() >= 12)
        return false;
    attempts.insert(source, {now + 5000});
    m_totalAttempts.push_back(now);
    m_interfaceAttempts[iface].push_back(now);
    return true;
}

void Network::readDiscovery()
{
    for (int i = 0; i < 32 && m_udp.hasPendingDatagrams(); ++i) {
        const auto packet = m_udp.receiveDatagram(kMaxAdvertisementBytes + 1);
        const auto data = packet.data();
        if (data == kProbe) {
            const auto source = packet.senderAddress();
            for (const auto& endpoint : m_endpoints)
                if (source.isInSubnet(endpoint.address.ip, endpoint.address.prefix))
                    sendAnnouncement(endpoint, source, packet.senderPort());
            continue;
        }
        if (data.size() < kAdvertisementBytes || data.size() > kMaxAdvertisementBytes ||
            !data.startsWith(kWireVersion))
            continue;
        const auto peerId = data.mid(4, kIdentityBytes);
        const auto source = packet.senderAddress();
        const auto port = qFromBigEndian<quint16>(data.constData() + 4 + kIdentityBytes);
        const auto endpoint = std::find_if(m_endpoints.cbegin(), m_endpoints.cend(), [&](const Endpoint& e) {
            return e.address.ip != source && source.isInSubnet(e.address.ip, e.address.prefix) &&
                   (packet.interfaceIndex() == 0 || e.address.index == int(packet.interfaceIndex()));
        });
        if (port == 0 || endpoint == m_endpoints.cend())
            continue;
        recordDiscovery(peerId, source, port, endpoint->address.index,
                        uchar(data.at(kAdvertisementBytes - 1)), advertisedName(data));
    }
    if (m_udp.hasPendingDatagrams())
        QTimer::singleShot(0, this, &Network::readDiscovery);
}

void Network::retryCandidates(qint64 now)
{
    const auto candidates = m_discoveries.values();
    for (const auto& candidate : candidates)
        tryCandidate(candidate, now);
}

void Network::setPhase(const QByteArray& id, Phase phase)
{
    const auto candidate = m_discoveries.find(id);
    if (candidate != m_discoveries.end() && candidate->phase != phase) {
        candidate->phase = phase;
        emit changed();
    }
}

void Network::tryCandidate(const Discovery& candidate, qint64 now)
{
    if (!m_credential || m_credential(candidate.id).isEmpty() ||
        (m_ownId > candidate.id && now - candidate.firstSeen < kReverseDelayMs))
        return;
    if (std::any_of(m_peers.cbegin(), m_peers.cend(), [&](Peer* peer) {
            return peer->peerId() == candidate.id || peer->address() == candidate.address;
        }))
        return;
    connectCandidate(candidate);
}

bool Network::connectCandidate(const Discovery& candidate, QByteArray inviteCode)
{
    const auto endpoint = std::find_if(m_endpoints.cbegin(), m_endpoints.cend(), [&](const Endpoint& e) {
        return e.address.index == candidate.interfaceIndex &&
               candidate.address.isInSubnet(e.address.ip, e.address.prefix);
    });
    if (endpoint == m_endpoints.cend() || !attempt(candidate.interfaceIndex, candidate.address, true))
        return false;

    const auto id = candidate.id;
    auto* socket = new QTcpSocket(this);
    socket->setProxy(QNetworkProxy::NoProxy);
    if (!socket->bind(endpoint->address.ip)) {
        emit diagnostic(QStringLiteral("自动认证本地绑定失败：%1，%2")
                            .arg(candidate.address.toString(), socket->errorString()));
        setPhase(id, Phase::Failed);
        socket->deleteLater();
        return false;
    }
    emit diagnostic(QStringLiteral("自动认证：连接 %1:%2").arg(candidate.address.toString()).arg(candidate.port));
    setPhase(id, Phase::Connecting);
    connect(socket, &QTcpSocket::connected, this, [this, id, address = candidate.address] {
        emit diagnostic(QStringLiteral("TCP 已连接，开始认证：%1").arg(address.toString()));
        setPhase(id, Phase::Authenticating);
    });
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket, address = candidate.address] {
        emit diagnostic(QStringLiteral("认证连接关闭或失败：%1，%2")
                            .arg(address.toString(), socket->errorString()));
    });
    adopt(socket, candidate.address, id, std::move(inviteCode));
    socket->connectToHost(candidate.address, candidate.port);
    return true;
}

void Network::adopt(QTcpSocket* socket, const QHostAddress& source, QByteArray peerId, QByteArray inviteCode)
{
    const bool inviting = !inviteCode.isEmpty();
    auto secret = inviting ? std::move(inviteCode) :
                            (m_credential && !peerId.isEmpty() ? m_credential(peerId) : QByteArray{});
    auto* peer = new Peer(socket, m_ownId, std::move(peerId), std::move(secret), this, inviting,
        [this](const QByteArray& id, bool invite) {
            if (!invite)
                return m_credential ? m_credential(id) : QByteArray{};
            if (!invitationActive() || m_inviteAttempts >= 5 ||
                std::any_of(m_peers.cbegin(), m_peers.cend(), [](Peer* candidate) {
                    return !candidate->initiatedLocally() && candidate->usedInvitation();
                }))
                return QByteArray{};
            ++m_inviteAttempts;
            return m_inviteCode;
        });
    m_peers.insert(peer);
    connect(peer, &Peer::authenticated, this, [this, peer] {
        if (peer->invitation()) {
            if (peer->initiatedLocally())
                m_joinTimer.start(120000);
            else if (!invitationActive()) {
                peer->close();
                return;
            }
            emit connected(peer);
            return;
        }
        const auto id = peer->peerId();
        // 两边同时发起时，双方按同一规则保留由较小 ID 发起的连接。
        const bool preferred = peer->initiatedLocally() == (m_ownId < id);
        const auto others = m_peers.values();
        for (auto* other : others) {
            if (other == peer || other->invitation() || !other->ready() || other->peerId() != id)
                continue;
            if (!preferred || other->initiatedLocally() == peer->initiatedLocally()) {
                peer->close();
                return;
            }
            other->close();
        }
        emit diagnostic(QStringLiteral("对端认证成功：%1").arg(peer->address().toString()));
        emit connected(peer);
    });
    connect(peer, &Peer::ended, this, [this, peer, source](bool rejected) {
        m_peers.remove(peer);
        if (peer->invitation()) {
            if (peer->initiatedLocally())
                m_joinTimer.stop();
            const bool exhausted = !peer->initiatedLocally() && peer->usedInvitation() && m_inviteAttempts >= 5;
            emit disconnected(peer);
            if (exhausted)
                endInvitation();
            if (peer->initiatedLocally() && rejected)
                emit invitationFailed();
            peer->deleteLater();
            return;
        }
        const auto id = peer->peerId();
        const bool stillConnected = std::any_of(m_peers.cbegin(), m_peers.cend(), [&](Peer* other) {
            return other->peerId() == id && other->ready();
        });
        if (rejected) {
            m_lastReject = m_clock.elapsed();
            m_incomingAttempts.insert(source, {m_lastReject + 30000});
            m_outgoingAttempts.insert(source, {m_lastReject + 30000});
            emit diagnostic(QStringLiteral("认证或加密校验失败：%1").arg(source.toString()));
        } else if (!stillConnected) {
            emit diagnostic(QStringLiteral("认证会话结束：%1").arg(source.toString()));
        }
        if (!stillConnected)
            setPhase(id, rejected ? Phase::Rejected : Phase::Failed);
        emit disconnected(peer);
        peer->deleteLater();
    });
    peer->start();
}
}

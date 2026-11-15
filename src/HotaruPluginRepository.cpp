#include "HotaruPluginRepository.h"
#include "BuildChannel.h"
#include "DevEnv.h"
#include "HotaruNativeHost.h"
#include "PluginHost.h"
#include "PluginStore.h"
#include "SettingsDialog.h"
#include "Theme.h"
#include <QImage>
#include <QScrollArea>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QTabBar>
#include "Utils.h"
#include "hotaru-native-plugin.h"
#include "ltool-plugin.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPluginLoader>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QScopedValueRollback>
#include <QSet>
#include <QUuid>
#include <QtEndian>
#include <archive.h>
#include <archive_entry.h>
#include <openssl/evp.h>
#include <algorithm>
#include <memory>

namespace HotaruPluginRepository {
namespace {
constexpr qint64 MiB = 1024 * 1024;
constexpr qint64 packageLimit = 128 * MiB, extractedLimit = 256 * MiB;
constexpr int catalogValidityDays = 365;
QString lastActivationError;
QString storageRootOverride;
QMap<QUrl, QByteArray> testResponses;
bool useTestTransport = false;
bool holdTestResponse = false;

bool fail(QString& error, const QString& reason) { error = reason; return false; }
bool matches(const QString& value, const char* pattern)
{
    return QRegularExpression(QRegularExpression::anchoredPattern(QLatin1String(pattern))).match(value).hasMatch();
}
bool identifier(const QString& value) { return matches(value, "[a-z0-9]+(?:-[a-z0-9]+)*") && value.size() <= 64; }
bool version(const QString& value)
{
    return value.size() <= 48 && matches(value, "(?:0|[1-9][0-9]{0,8})(?:\\.(?:0|[1-9][0-9]{0,8})){2}(?:-[a-zA-Z0-9-]+(?:\\.[a-zA-Z0-9-]+)*)?");
}
int compare(const QString& a, const QString& b)
{
    const auto core = [](const QString& s) { return s.section('-', 0, 0).split('.'); };
    const auto ac = core(a), bc = core(b);
    for (int i = 0; i < 3; ++i) {
        const int x = ac.value(i).toInt(), y = bc.value(i).toInt();
        if (x != y) return x < y ? -1 : 1;
    }
    const auto ap = a.section('-', 1), bp = b.section('-', 1);
    if (ap.isEmpty() != bp.isEmpty()) return ap.isEmpty() ? 1 : -1;
    if (ap == bp) return 0;
    const auto x = ap.split('.'), y = bp.split('.');
    for (int i = 0; i < std::min(x.size(), y.size()); ++i) {
        if (x[i] == y[i]) continue;
        const bool xn = matches(x[i], "[0-9]+"), yn = matches(y[i], "[0-9]+");
        if (xn != yn) return xn ? -1 : 1;
        if (xn && x[i].size() != y[i].size()) return x[i].size() < y[i].size() ? -1 : 1;
        return x[i] < y[i] ? -1 : 1;
    }
    return x.size() < y.size() ? -1 : (x.size() > y.size() ? 1 : 0);
}
bool contains(const Range& r, const QString& v)
{
    return (r.min.isEmpty() || compare(v, r.min) >= 0) && (r.max.isEmpty() || compare(v, r.max) < 0);
}
bool validRange(const Range& r)
{
    return (r.min.isEmpty() || version(r.min)) && (r.max.isEmpty() || version(r.max))
        && (r.min.isEmpty() || r.max.isEmpty() || compare(r.min, r.max) < 0);
}
bool safeName(const QString& n)
{
    return matches(n, "[A-Za-z0-9][A-Za-z0-9_.-]{0,119}")
        && !n.endsWith('.') && !matches(n.section('.', 0, 0).toUpper(), "CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9]");
}
bool regular(const QFileInfo& info)
{
    return info.isFile() && !info.isSymLink() && !info.isJunction();
}
QByteArray read(const QString& path, qint64 max)
{
    QFile f(path);
    if (!regular(QFileInfo(path)) || !f.open(QIODevice::ReadOnly) || f.size() > max) return {};
    return f.read(max + 1);
}
bool write(const QString& path, const QByteArray& bytes)
{
    const auto parent = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(parent) || QFileInfo(parent).isJunction() || QFileInfo(parent).isSymLink()) return false;
    QSaveFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit();
}
QByteArray digest(const QString& path, qint64 size)
{
    QFile f(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!regular(QFileInfo(path)) || !f.open(QIODevice::ReadOnly) || f.size() != size || !hash.addData(&f)) return {};
    return hash.result().toHex();
}
QJsonObject json(const File& f) { return {{"name", f.name}, {"size", f.size}, {"sha256", QString::fromLatin1(f.hash)}}; }
File file(const QJsonObject& j)
{
    return {j.value("name").toString(), j.value("size").toInteger(-1), j.value("sha256").toString().toLatin1()};
}
bool validFile(const File& f, qint64 limit)
{
    return safeName(f.name) && f.size > 0 && f.size <= limit && matches(QString::fromLatin1(f.hash), "[0-9a-f]{64}");
}
QJsonObject json(const Range& r) { return {{"min", r.min}, {"max", r.max}}; }
Range range(const QJsonObject& j) { return {j.value("min").toString(), j.value("max").toString()}; }
QJsonObject json(const Entry& e)
{
    QJsonArray files;
    QJsonObject dependencies;
    for (const auto& f : e.files) files << json(f);
    for (auto it = e.requiresPackages.cbegin(); it != e.requiresPackages.cend(); ++it)
        dependencies.insert(it.key(), json(it.value()));
    return {{"id", e.id}, {"version", e.version}, {"name", e.name}, {"description", e.description},
        {"category", e.category}, {"channel", e.channel}, {"iid", e.iid}, {"api", e.api},
        {"abi", e.abi}, {"host", json(e.host)}, {"platform", "windows"}, {"arch", "x64"},
        {"dependencies", dependencies}, {"tag", e.tag}, {"asset", json(e.asset)}, {"files", files}};
}
Entry entry(const QJsonObject& j)
{
    Entry e;
    e.id = j.value("id").toString(); e.version = j.value("version").toString(); e.name = j.value("name").toString();
    e.description = j.value("description").toString(); e.category = j.value("category").toString();
    e.channel = j.value("channel").toString(); e.iid = j.value("iid").toString();
    e.abi = j.value("abi").toString(); e.api = j.value("api").toInt(); e.tag = j.value("tag").toString();
    e.host = range(j.value("host").toObject()); e.asset = file(j.value("asset").toObject());
    const auto dependencies = j.value("dependencies").toObject();
    for (auto it = dependencies.begin(); it != dependencies.end(); ++it)
        e.requiresPackages.insert(it.key(), range(it.value().toObject()));
    for (const auto f : j.value("files").toArray()) e.files << file(f.toObject());
    return e;
}
bool lan(const QString& id)
{
    return id == kHotaruLanCoreId || id == kHotaruLanClipboardId || id == kHotaruLanFilesId;
}
bool validEntry(const Entry& e)
{
    if (!identifier(e.id) || !version(e.version) || e.name.isEmpty() || e.name.size() > 120
        || e.description.size() > 1000 || e.iid.isEmpty() || e.iid.size() > 100 || e.api <= 0
        || (e.category != "service" && e.category != "feature") || !validRange(e.host)
        || e.files.size() > 32 || e.requiresPackages.size() > 24) return false;
    if (lan(e.id) != (e.iid == HOTARU_LAN_PACKAGE_IID)) return false;
    QSet<QString> names;
    int dllCount = 0;
    qint64 total = 0;
    for (const auto& f : e.files) {
        const auto lower = f.name.toLower();
        if (!validFile(f, extractedLimit) || names.contains(lower)) return false;
        names.insert(lower);
        total += f.size;
        if (lower.endsWith(".dll")) ++dllCount;
        else if (!lower.endsWith(".txt")) return false;
    }
    if (dllCount != 1 || total > extractedLimit) return false;
    for (auto it = e.requiresPackages.cbegin(); it != e.requiresPackages.cend(); ++it)
        if (!identifier(it.key()) || it.key() == e.id || !validRange(it.value())) return false;
    return true;
}
bool validPublished(const Entry& e)
{
    return validEntry(e) && (e.channel == "stable" || e.channel == "preview") && !e.abi.isEmpty()
        && !e.host.min.isEmpty() && safeName(e.tag) && !e.tag.startsWith("catalog-v1-")
        && validFile(e.asset, packageLimit)
        && e.asset.name == QStringLiteral("hotaru-plugin-%1-%2-x64.zip").arg(e.id, e.version);
}
bool amd64(QFile& f)
{
    if (!f.seek(0)) return false;
    const auto dos = f.read(64);
    if (dos.size() != 64 || !dos.startsWith("MZ")) return false;
    const auto offset = qFromLittleEndian<quint32>(dos.constData() + 60);
    if (offset < 64 || offset > 1024 * 1024 || !f.seek(offset)) return false;
    const auto header = f.read(24);
    return header.size() == 24 && header.first(4) == QByteArray("PE\0\0", 4)
        && qFromLittleEndian<quint16>(header.constData() + 4) == 0x8664
        && (qFromLittleEndian<quint16>(header.constData() + 22) & 0x2000);
}
bool metadata(const QString& path, Entry& e, bool published, QString& error)
{
    QFile dll(path);
    if (!dll.open(QIODevice::ReadOnly) || !amd64(dll))
        return fail(error, QStringLiteral("插件必须是 Windows x64 DLL"));
    dll.close();
    const auto qt = QPluginLoader(path).metaData();
    const auto m = qt.value("MetaData").toObject();
    const auto iid = qt.value("IID").toString();
#ifdef QT_DEBUG
    constexpr bool debug = true;
#else
    constexpr bool debug = false;
#endif
    const int qtVersion = qt.value("version").toInt();
    if ((qtVersion >> 16) != QT_VERSION_MAJOR || ((qtVersion >> 8) & 255) > QT_VERSION_MINOR
        || qt.value("debug").toBool() != debug)
        return fail(error, QStringLiteral("插件的 Qt 构建版本不兼容"));
    Entry found;
    found.id = m.value("Id").toString(); found.version = m.value("Version").toString("1.0.0");
    found.name = m.value("Name").toString(found.id); found.description = m.value("Desc").toString();
    found.iid = iid; found.api = m.value("Api").toInt(); found.abi = m.value("Abi").toString();
    found.channel = m.value("Channel").toString();
    found.category = m.value("Category").toString("feature");
    const auto dep = m.value("Dependencies").toObject();
    for (auto it = dep.begin(); it != dep.end(); ++it)
        found.requiresPackages.insert(it.key(), range(it.value().toObject()));
    if (iid == HOTARU_LAN_PACKAGE_IID) {
        const auto required = m.value("Requires").toString();
        const bool core = found.id == kHotaruLanCoreId;
        if (!lan(found.id) || !m.value("Requires").isString()
            || (core ? (!required.isEmpty() || m.value("Protocol").toString() != "HL02") : required != kHotaruLanCoreId))
            return fail(error, QStringLiteral("LAN 插件的接口元数据不匹配"));
        found.category = core ? "service" : "feature";
        if (!core && !found.requiresPackages.contains(required)) found.requiresPackages.insert(required, {});
        if (found.channel == "development-preview") found.channel = "preview";
    }
    if (published) {
        if (!m.value("Version").isString() || found.id != e.id || found.version != e.version
            || found.iid != e.iid || found.api != e.api || found.abi != e.abi || found.channel != e.channel
            || found.category != e.category || found.requiresPackages != e.requiresPackages)
            return fail(error, QStringLiteral("DLL 元数据与签名目录不一致：%1").arg(e.id));
    } else {
        found.files = e.files;
        e = found;
    }
    if (!validEntry(e) || !compatible(e))
        return fail(error, QStringLiteral("插件元数据、接口或 ABI 不兼容：%1").arg(e.id));
    return true;
}
bool verifySignature(const QByteArray& bytes, const QByteArray& signature, const QByteArray& publicKey)
{
    if (signature.size() != 64 || publicKey.size() != 32) return false;
    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
            reinterpret_cast<const unsigned char*>(publicKey.constData()), publicKey.size()), EVP_PKEY_free);
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    return key && ctx && EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) == 1
        && EVP_DigestVerify(ctx.get(), reinterpret_cast<const unsigned char*>(signature.constData()), signature.size(),
            reinterpret_cast<const unsigned char*>(bytes.constData()), bytes.size()) == 1;
}
QByteArray testKey;
QList<QByteArray> publicKeys()
{
    if (!testKey.isEmpty()) return {testKey};
    QList<QByteArray> result;
    for (const auto& key : QStringLiteral(HOTARU_PLUGIN_PUBLIC_KEYS).split(',', Qt::SkipEmptyParts))
        if (matches(key, "[0-9a-fA-F]{64}")) result << QByteArray::fromHex(key.toLatin1());
    return result;
}
QByteArray evidence(const Catalog& c)
{
    return QJsonDocument(QJsonObject{{"index", QString::fromLatin1(c.bytes.toBase64())},
        {"signatures", QString::fromLatin1(c.signatures.toBase64())}}).toJson(QJsonDocument::Compact);
}
Catalog decode(const QByteArray& bundle, QString& error)
{
    const auto object = QJsonDocument::fromJson(bundle).object();
    Catalog c;
    c.bytes = QByteArray::fromBase64(object.value("index").toString().toLatin1());
    c.signatures = QByteArray::fromBase64(object.value("signatures").toString().toLatin1());
    if (c.bytes.isEmpty() || c.bytes.size() > MiB || c.signatures.isEmpty() || c.signatures.size() > 256
        || c.signatures.size() % 64) {
        error = QStringLiteral("目录或签名大小不正确");
        return {};
    }
    bool verified = false;
    for (const auto& key : publicKeys())
        for (qsizetype i = 0; i < c.signatures.size(); i += 64)
            verified |= verifySignature(c.bytes, c.signatures.mid(i, 64), key);
    if (!verified) { error = QStringLiteral("目录签名无效或宿主尚未配置官方公钥"); return {}; }
    const auto index = QJsonDocument::fromJson(c.bytes).object();
    c.sequence = index.value("sequence").toInt();
    c.expires = QDateTime::fromString(index.value("expires").toString(), Qt::ISODate);
    const auto issued = QDateTime::fromString(index.value("issued").toString(), Qt::ISODate);
    if (index.value("format").toInt() != 1 || c.sequence < 1 || !c.expires.isValid() || !issued.isValid()
        || issued > QDateTime::currentDateTimeUtc().addSecs(600) || c.expires <= issued || issued.addDays(catalogValidityDays) < c.expires
        || !index.value("plugins").isArray() || index.value("plugins").toArray().size() > 1000) {
        error = QStringLiteral("目录格式、序号或有效期不正确"); return {};
    }
    QSet<QString> versions;
    for (const auto& value : index.value("plugins").toArray()) {
        const auto j = value.toObject();
        const auto e = entry(j);
        const QString key = e.id + '/' + e.version;
        if (!validPublished(e) || j != json(e) || versions.contains(key)) {
            error = QStringLiteral("目录包含无效或重复条目：%1").arg(key); return {};
        }
        versions.insert(key);
        c.entries << e;
    }
    return c;
}
Inventory readInventory(const QString& fileName, bool* ok = nullptr)
{
    Inventory items;
    const auto path = root() + '/' + fileName;
    const auto doc = QJsonDocument::fromJson(read(path, 4 * MiB));
    bool valid = !QFileInfo::exists(path) || (doc.isArray() && doc.array().size() <= 128);
    if (!valid) { if (ok) *ok = false; return {}; }
    for (const auto& v : doc.array()) {
        const auto j = v.toObject();
        Installed item{entry(j.value("entry").toObject()), j.value("source").toString(), j.value("folder").toString()};
        if (!validEntry(item.entry) || (item.source != "official" && item.source != "local")
            || !matches(item.folder, "[0-9a-f]{32}") || items.contains(item.entry.id)) {
            valid = false;
            continue;
        }
        items.insert(item.entry.id, item);
    }
    if (ok) *ok = valid;
    return items;
}
bool writeInventory(const QString& fileName, const Inventory& items)
{
    QJsonArray list;
    for (const auto& item : items) list << QJsonObject{{"entry", json(item.entry)}, {"source", item.source}, {"folder", item.folder}};
    return write(root() + '/' + fileName, QJsonDocument(list).toJson(QJsonDocument::Compact));
}
bool safeDirectory(const QString& p)
{
    const QFileInfo info(p);
    return info.isDir() && !info.isSymLink() && !info.isJunction();
}
bool checkInstalled(const Installed& item, QString& error)
{
    const auto dir = storagePath(item);
    if (!safeDirectory(root()) || !safeDirectory(root() + "/packages")
        || !safeDirectory(root() + "/packages/" + item.entry.id)
        || !safeDirectory(root() + "/packages/" + item.entry.id + '/' + item.entry.version)
        || !safeDirectory(dir))
        return fail(error, QStringLiteral("插件目录不可用：%1").arg(item.entry.id));
    QSet<QString> allowed;
    if (item.source == "official") {
        const auto proof = decode(read(dir + "/catalog-proof.json", 2 * MiB), error);
        const auto match = std::find_if(proof.entries.cbegin(), proof.entries.cend(), [&](const Entry& e) {
            return json(e) == json(item.entry);
        });
        if (match == proof.entries.cend()) return fail(error, QStringLiteral("已安装插件的签名记录无效：%1").arg(item.entry.id));
        allowed.insert("catalog-proof.json");
    }
    for (const auto& f : item.entry.files) {
        if (digest(dir + '/' + f.name, f.size) != f.hash)
            return fail(error, QStringLiteral("已安装插件文件校验失败：%1").arg(item.entry.id));
        allowed.insert(f.name);
    }
    const auto actual = QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    if (QSet<QString>(actual.begin(), actual.end()) != allowed)
        return fail(error, QStringLiteral("插件目录含未声明文件：%1").arg(item.entry.id));
    auto copy = item.entry;
    return metadata(dllPath(item), copy, item.source == "official", error)
        && (item.source == "official" || (copy.id == item.entry.id && copy.version == item.entry.version
            && copy.iid == item.entry.iid && copy.requiresPackages == item.entry.requiresPackages));
}
bool packageTarget(Installed& item, const QString& stage, QString& error)
{
    item.folder = QUuid::createUuid().toString(QUuid::Id128);
    const auto target = storagePath(item);
    const auto parent = QFileInfo(target).absolutePath();
    if (!QDir().mkpath(parent) || !safeDirectory(root()) || !safeDirectory(root() + "/packages")
        || !safeDirectory(root() + "/packages/" + item.entry.id) || !safeDirectory(parent)
        || !QFile::rename(stage, target)) return fail(error, QStringLiteral("无法保存插件版本目录"));
    return true;
}
QString snapshotHash(const Inventory& items)
{
    QJsonArray list;
    for (const auto& item : items) list << QJsonArray{item.entry.id, item.folder, item.source};
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(list).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}
}
QString Entry::dll() const
{
    for (const auto& f : files)
        if (f.name.endsWith(".dll", Qt::CaseInsensitive)) return f.name;
    return {};
}
QString abi()
{
    return QStringLiteral(HOTARU_PLUGIN_ABI);
}
QString root()
{
    return storageRootOverride.isEmpty() ? QDir(Utils::dataDir()).filePath(
        BuildChannel::development ? "hotaru-plugin-repository-dev" : "hotaru-plugin-repository") : storageRootOverride;
}
bool compatible(const Entry& e)
{
    const bool api = (e.iid == HOTARU_LAN_PACKAGE_IID && e.api == kHotaruLanPackageApi)
        || (e.iid == HOTARU_NATIVE_PLUGIN_IID && e.api == kHotaruNativePluginApi)
        || (e.iid == LTOOL_PLUGIN_IID && e.api == LTOOL_PLUGIN_API_VERSION);
    return api && (e.abi.isEmpty() || e.abi == abi()) && contains(e.host, QStringLiteral(LTOOL_VERSION));
}
bool installable(const Entry& e, bool development)
{
    return compatible(e) && e.iid != LTOOL_PLUGIN_IID && (e.channel == "stable" || (development && e.channel == "preview"));
}
QList<Entry> catalogListings(const Catalog& catalog, bool development)
{
    QMap<QString, Entry> latest;
    for (const auto& e : catalog.entries) {
        if (!installable(e, true)) continue;
        const auto current = latest.constFind(e.id);
        // 正式版优先展示可安装的正式包；没有正式包时才展示预览。
        if (current != latest.cend()) {
            if (!development && e.channel != current->channel) {
                if (e.channel != "stable") continue;
            } else if (compare(e.version, current->version) <= 0) continue;
        }
        latest.insert(e.id, e);
    }
    return latest.values();
}
bool sameVersion(const QString& left, const QString& right) { return compare(left, right) == 0; }
QString storagePath(const Installed& item)
{
    return root() + "/packages/" + item.entry.id + '/' + item.entry.version + '/' + item.folder;
}
QString dllPath(const Installed& item) { return storagePath(item) + '/' + item.entry.dll(); }
bool extractPackage(const QString& path, const QString& output, QString& error)
{
    const QFileInfo input(path);
    if (!regular(input) || input.size() <= 0 || input.size() > packageLimit || !safeDirectory(output))
        return fail(error, QStringLiteral("插件包不存在、路径不安全或大小超限"));
    if (input.suffix().compare("dll", Qt::CaseInsensitive) == 0) {
        if (!safeName(input.fileName()) || !QFile::copy(path, output + '/' + input.fileName()))
            return fail(error, QStringLiteral("无法暂存插件 DLL"));
        return true;
    }
    if (input.suffix().compare("zip", Qt::CaseInsensitive) != 0)
        return fail(error, QStringLiteral("请选择 DLL 或 ZIP 插件包"));
    const std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
    if (!reader) return fail(error, QStringLiteral("无法初始化 ZIP 读取器"));
    auto* a = reader.get();
    archive_read_support_format_zip(a);
    if (archive_read_open_filename_w(a, path.toStdWString().c_str(), 64 * 1024) != ARCHIVE_OK)
        return fail(error, QStringLiteral("无法打开插件 ZIP"));
    QSet<QString> names;
    int dlls = 0;
    qint64 total = 0;
    archive_entry* item = nullptr;
    int rc;
    while ((rc = archive_read_next_header(a, &item)) == ARCHIVE_OK) {
        const auto name = QString::fromUtf8(archive_entry_pathname_utf8(item));
        const auto lower = name.toLower();
        const qint64 size = archive_entry_size(item);
        if (!safeName(name) || names.contains(lower) || names.size() >= 32
            || archive_entry_filetype(item) != AE_IFREG || archive_entry_symlink(item) || archive_entry_hardlink(item)
            || archive_entry_is_encrypted(item) != 0 || size <= 0 || size > extractedLimit - total
            || (!lower.endsWith(".dll") && !lower.endsWith(".txt")))
            return fail(error, QStringLiteral("ZIP 含不支持的条目或解压大小超限"));
        names.insert(lower);
        if (lower.endsWith(".dll") && ++dlls != 1)
            return fail(error, QStringLiteral("每个插件包只允许一个 DLL"));
        QFile out(output + '/' + name);
        if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return fail(error, QStringLiteral("无法写入插件暂存文件"));
        char buffer[64 * 1024];
        qint64 got = 0, chunk = 0;
        while ((chunk = archive_read_data(a, buffer, sizeof(buffer))) > 0) {
            if (chunk > size - got || out.write(buffer, chunk) != chunk)
                return fail(error, QStringLiteral("解压大小超限或写入失败"));
            got += chunk;
        }
        if (chunk < 0 || got != size || !out.flush()) return fail(error, QStringLiteral("ZIP 内容不完整"));
        total += got;
    }
    return (rc == ARCHIVE_EOF && dlls == 1) || fail(error, QStringLiteral("ZIP 内容不完整或缺少 DLL"));
}
bool readLocalPackage(const QString& path, const QString& output, Entry& e, QString& error)
{
    e = {};
    if (!extractPackage(path, output, error)) return false;
    for (const auto& info : QDir(output).entryInfoList(QDir::Files, QDir::Name)) {
        e.files << File{info.fileName(), info.size(), digest(info.absoluteFilePath(), info.size())};
    }
    return metadata(output + '/' + e.dll(), e, false, error);
}
QStringList loadOrder(const Inventory& items)
{
    QStringList order;
    QSet<QString> visited, visiting;
    std::function<bool(const QString&)> visit = [&](const QString& id) {
        if (visited.contains(id)) return true;
        if (visiting.contains(id) || !items.contains(id)) return false;
        visiting.insert(id);
        const auto& item = items[id];
        for (auto it = item.entry.requiresPackages.cbegin(); it != item.entry.requiresPackages.cend(); ++it) {
            if (!items.contains(it.key()) || !contains(it.value(), items[it.key()].entry.version)
                || (item.source == "official" && items[it.key()].source != "official")
                || (lan(id) && item.source != items[it.key()].source) || !visit(it.key())) return false;
        }
        visiting.remove(id);
        visited.insert(id);
        order << id;
        return true;
    };
    for (auto it = items.cbegin(); it != items.cend(); ++it)
        if (!visit(it.key())) return {};
    return order;
}
bool validateInventory(const Inventory& items, QString& error)
{
    if (items.size() > 128 || loadOrder(items).size() != items.size())
        return fail(error, QStringLiteral("插件依赖缺失、来源冲突、版本不兼容或形成循环"));
    for (const auto& item : items)
        if (!checkInstalled(item, error)) return false;
    return true;
}
Inventory active() { return readInventory("active.json"); }
Inventory desired() { return pending() ? readInventory("pending.json") : active(); }
bool pending() { return QFileInfo::exists(root() + "/pending.json"); }
bool queue(const Inventory& items, QString& error)
{
    return validateInventory(items, error) && (writeInventory("pending.json", items)
        || fail(error, QStringLiteral("无法保存待重启的插件变更")));
}
bool queueLocal(const Entry& e, const QString& staged, bool replace, QString& error)
{
    auto next = desired();
    if (next.contains(e.id) && !replace) return fail(error, QStringLiteral("同 ID 插件已存在"));
    if (lan(e.id) && next.contains(QLatin1String(kHotaruLanCoreId)) && next[QLatin1String(kHotaruLanCoreId)].source == "official") {
        for (auto it = next.begin(); it != next.end(); )
            if (lan(it.key())) it = next.erase(it); else ++it;
    }
    Installed item{e, "local", {}};
    next.insert(e.id, item);
    if (loadOrder(next).size() != next.size()) return fail(error, QStringLiteral("请先安装兼容的前置插件"));
    if (!packageTarget(item, staged, error)) return false;
    next[e.id] = item;
    if (queue(next, error)) return true;
    if (safeDirectory(storagePath(item))) QDir(storagePath(item)).removeRecursively();
    return false;
}
bool remove(const QString& id, QStringList& affected, bool confirm, QString& error)
{
    auto next = desired();
    if (!next.contains(id)) return fail(error, QStringLiteral("找不到已安装插件"));
    affected = {id};
    for (qsizetype i = 0; i < affected.size(); ++i)
        for (auto it = next.cbegin(); it != next.cend(); ++it)
            if (!affected.contains(it.key()) && it->entry.requiresPackages.contains(affected[i])) affected << it.key();
    if (!confirm) return true;
    for (const auto& key : affected) next.remove(key);
    return queue(next, error);
}
void activate(const std::function<bool(const Inventory&, bool, QString&)>& load)
{
    lastActivationError.clear();
    QString error;
    const auto pendingPath = root() + "/pending.json";
    const auto trialPath = root() + "/trial";
    if (pending()) {
        bool ok = false;
        const auto next = readInventory("pending.json", &ok);
        const auto old = active();
        const auto marker = read(trialPath, 100);
        if (ok && !marker.isEmpty() && marker == snapshotHash(next).toLatin1() && marker == snapshotHash(old).toLatin1()) {
            // 上次已提交，只剩事务标记未清理。
            if (QFile::remove(pendingPath)) QFile::remove(trialPath);
        } else if (ok && marker.isEmpty() && validateInventory(next, error)
            && write(trialPath, snapshotHash(next).toLatin1()) && load(next, true, error)
            && writeInventory("active.json", next)) {
            if (QFile::remove(pendingPath)) QFile::remove(trialPath);
            return;
        } else {
            lastActivationError = QStringLiteral("插件变更已回退：%1").arg(error.isEmpty() ?
                QStringLiteral("上次启动中断或无法保存状态") : error);
            if (QFile::remove(pendingPath)) QFile::remove(trialPath);
        }
    } else if (QFileInfo::exists(trialPath)) {
        QFile::remove(trialPath);
    }
    bool activeOk = false;
    auto current = readInventory("active.json", &activeOk);
    if (!activeOk) lastActivationError = QStringLiteral("已安装插件清单有损坏");
    for (auto it = current.begin(); it != current.end(); ) {
        if (!checkInstalled(it.value(), error)) {
            lastActivationError = error;
            it = current.erase(it);
        } else ++it;
    }
    // 单个失效包只停用自己的依赖链。
    auto safe = Inventory{};
    bool changed;
    do {
        changed = false;
        for (auto it = current.cbegin(); it != current.cend(); ++it) {
            if (safe.contains(it.key())) continue;
            auto candidate = safe;
            candidate.insert(it.key(), it.value());
            if (loadOrder(candidate).size() == candidate.size()) {
                safe = candidate;
                changed = true;
            }
        }
    } while (changed);
    if (!load(safe, false, error)) lastActivationError = error;
}
QString activationError() { return lastActivationError; }


namespace {
const QString remote = QStringLiteral("https://github.com/EienSakura/Hotaru-Plugins/releases/download/");
const QString latest = QStringLiteral("https://api.github.com/repos/EienSakura/Hotaru-Plugins/releases/latest");

bool safeHttps(const QUrl& url)
{
    return url.isValid() && url.scheme() == "https" && !url.host().isEmpty()
        && url.port(443) == 443 && !url.authority().contains(QLatin1Char('@')) && !url.hasFragment();
}

bool allowedUrl(const QUrl& url)
{
    const auto h = url.host();
    if (!safeHttps(url)) return false;
    if (h == "api.github.com") return url == QUrl(latest);
    if (h == "github.com") {
        const auto path = url.path(QUrl::FullyEncoded);
        return !url.hasQuery() && matches(path, "/EienSakura/Hotaru-Plugins/releases/download/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+");
    }
    return h == "release-assets.githubusercontent.com" || h == "objects.githubusercontent.com"
        || h == "github-releases.githubusercontent.com";
}

QList<QUrl> packageSources(const QUrl& origin, const QString& prefixes)
{
    QList<QUrl> result;
    for (auto prefix : prefixes.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        prefix = prefix.trimmed();
        const QUrl base(prefix, QUrl::StrictMode);
        if (!safeHttps(base) || base.hasQuery() || base.host() == "github.com"
            || base.host() == "api.github.com") continue;
        if (!prefix.endsWith(QLatin1Char('/'))) prefix += QLatin1Char('/');
        const QUrl mirror(prefix + origin.toString(QUrl::FullyEncoded), QUrl::StrictMode);
        if (safeHttps(mirror) && mirror.host() == base.host() && !result.contains(mirror))
            result << mirror;
        if (result.size() == 4) break;
    }
    result << origin;
    return result;
}
}
bool planInstall(const QString& id, const Catalog& catalog, const Inventory& current, bool development,
                 InstallPlan& plan, QString& error, const QStringList& bundled)
{
    plan = {};
    plan.basis = snapshotHash(current);
    if (catalog.sequence <= 0 || catalog.expires <= QDateTime::currentDateTimeUtc())
        return fail(error, QStringLiteral("请先刷新官方目录"));
    QMap<QString, QList<Entry>> candidates;
    for (const auto& e : catalog.entries)
        if (installable(e, development)) candidates[e.id] << e;
    for (auto& versions : candidates)
        std::sort(versions.begin(), versions.end(), [](const Entry& a, const Entry& b) { return compare(a.version, b.version) > 0; });
    if (!candidates.contains(id)) return fail(error, QStringLiteral("当前宿主没有可安装的版本"));
    QSet<QString> required{id};
    auto initial = current;
    if (lan(id)) {
        for (const auto& key : bundled)
            if (lan(key)) required.insert(key);
        for (auto it = initial.begin(); it != initial.end(); ) {
            if (lan(it.key()) && it->source != "official") {
                required.insert(it.key());
                it = initial.erase(it);
            } else ++it;
        }
    }
    for (auto it = initial.cbegin(); it != initial.cend(); ++it) required.insert(it.key());
    int attempts = 4096;
    std::function<bool(Inventory, QSet<QString>)> solve = [&](Inventory chosen, QSet<QString> keys) {
        if (--attempts < 0 || keys.size() > 128) return false;
        QStringList missing;
        for (const auto& key : keys)
            if (!chosen.contains(key)) missing << key;
        if (missing.isEmpty()) {
            if (loadOrder(chosen).size() != chosen.size()) return false;
            plan.inventory = chosen;
            return true;
        }
        missing.sort();
        const auto key = missing.contains(id) ? id : missing.first();
        QList<Installed> options;
        const auto old = initial.value(key);
        if (key != id && !old.source.isEmpty()) options << old;
        for (const auto& e : candidates.value(key)) {
            if (current.contains(key)) {
                const auto& present = current[key];
                const int cmp = compare(e.version, present.entry.version);
                if (cmp < 0 || (cmp == 0 && present.source == "official")) continue;
                // 用户没有选择替换时，不把无关的本地插件改成官方版。
                if (present.source == "local" && key != id && !lan(id) && !std::any_of(chosen.cbegin(), chosen.cend(),
                        [&](const Installed& i) { return i.source == "official" && i.entry.requiresPackages.contains(key); }))
                    continue;
            }
            options << Installed{e, "official", {}};
        }
        for (const auto& option : options) {
            auto next = chosen;
            next.insert(key, option);
            auto deps = keys;
            bool valid = true;
            for (auto it = next.cbegin(); it != next.cend() && valid; ++it) {
                for (auto dep = it->entry.requiresPackages.cbegin(); dep != it->entry.requiresPackages.cend(); ++dep) {
                    deps.insert(dep.key());
                    if (!next.contains(dep.key())) continue;
                    const auto& other = next[dep.key()];
                    if (!contains(dep.value(), other.entry.version) ||
                        (it->source == "official" && other.source != "official") ||
                        (lan(it.key()) && it->source != other.source)) {
                        valid = false;
                        break;
                    }
                }
            }
            if (valid && solve(next, deps)) return true;
        }
        return false;
    };
    if (!solve({}, required)) return fail(error, QStringLiteral("没有可用的版本组合；请检查依赖、渠道和已安装版本"));
    for (const auto& key : loadOrder(plan.inventory)) {
        const auto& item = plan.inventory[key];
        if (!item.folder.isEmpty()) continue;
        plan.downloads << item.entry;
        plan.totalBytes += item.entry.asset.size;
    }
    return !plan.downloads.isEmpty() || fail(error, QStringLiteral("已安装当前版本"));
}

Client::Client(QObject* parent) : QObject(parent), m_network(new QNetworkAccessManager(this))
{
    DevEnv::applyProxy(m_network);
    readCache();
}
Catalog Client::catalog() const { return m_catalog; }
void Client::readCache()
{
    QString error;
    m_catalog = decode(read(root() + "/catalog.json", 2 * MiB), error);
    m_checked = QFileInfo(root() + "/catalog.json").lastModified().toUTC();
    if (m_catalog.sequence > 0 && m_checked.isValid())
        m_status = QStringLiteral("目录更新于 %1%2").arg(m_checked.toLocalTime().toString("yyyy-MM-dd HH:mm"),
            m_catalog.expires <= QDateTime::currentDateTimeUtc() ? QStringLiteral(" · 已过期") : QString{});
    else m_status = QStringLiteral("尚无已验证的官方目录");
}
void Client::finish(const QString& status)
{
    m_busy = false;
    if (!status.isEmpty()) m_status = status;
    emit changed();
}
void Client::cancel()
{
    m_cancelled = true;
    if (m_reply) m_reply->abort();
}
void Client::fetch(const QUrl& url, qint64 limit, const Consume& consume, const Done& done,
                   int hops, const QString& mirrorHost)
{
    const bool allowed = allowedUrl(url) || (!mirrorHost.isEmpty() && safeHttps(url) && url.host() == mirrorHost);
    if (m_cancelled || hops > 5 || !allowed) {
        done(m_cancelled ? QStringLiteral("已取消") : QStringLiteral("下载地址或跳转不受支持"));
        return;
    }
    if (useTestTransport) {
        const auto payload = testResponses.value(url);
        QTimer::singleShot(0, this, [=, this] {
            if (m_cancelled) { done(QStringLiteral("已取消")); return; }
            if (holdTestResponse || !testResponses.contains(url)) { done(QStringLiteral("测试网络不可达")); return; }
            if (payload.size() > limit || !consume(payload)) { done(QStringLiteral("下载大小超限或无法写入")); return; }
            done({});
        });
        return;
    }
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    req.setRawHeader("User-Agent", "Hotaru-Plugin-Repository");
    req.setRawHeader("Accept-Encoding", "identity");
    req.setTransferTimeout(20000);
    auto* reply = m_network->get(req);
    m_reply = reply;
    reply->setReadBufferSize(128 * 1024);
    QTimer::singleShot(limit > MiB ? 180000 : 40000, reply, [reply] {
        if (reply->isRunning()) reply->abort();
    });
    auto count = std::make_shared<qint64>(0);
    auto rejected = std::make_shared<bool>(false);
    const auto drain = [=, this] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200) {
            if (status) {
                *count += reply->readAll().size();
                if (*count > MiB) { *rejected = true; reply->abort(); }
            }
            return;
        }
        while (reply->bytesAvailable() > 0) {
            const auto chunk = reply->read(64 * 1024);
            if (chunk.isEmpty()) break;
            *count += chunk.size();
            if (*count > limit || !consume(chunk)) {
                *rejected = true;
                reply->abort();
                return;
            }
        }
    };
    connect(reply, &QNetworkReply::readyRead, this, drain);
    connect(reply, &QNetworkReply::finished, this, [=, this] {
        drain();
        m_reply = nullptr;
        const auto redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        if (m_cancelled) done(QStringLiteral("已取消"));
        else if (*rejected) done(QStringLiteral("下载大小超限或无法写入"));
        else if (ok && code >= 300 && code < 400 && !redirect.isEmpty())
            fetch(url.resolved(redirect), limit, consume, done, hops + 1, mirrorHost);
        else if (ok && code == 200) done({});
        else done(QStringLiteral("无法读取官方插件仓库（HTTP %1）").arg(code));
    });
}
void Client::refresh(bool force)
{
    const auto now = QDateTime::currentDateTimeUtc();
    if (m_busy || (!force && m_catalog.expires > now && m_checked.isValid() && m_checked.secsTo(now) < 86400)) return;
    m_busy = true; m_cancelled = false; m_status = QStringLiteral("正在刷新官方目录…"); emit changed();
    auto response = std::make_shared<QByteArray>();
    const auto append = [](const std::shared_ptr<QByteArray>& bytes) {
        return [bytes](const QByteArray& chunk) { bytes->append(chunk); return true; };
    };
    fetch(QUrl(latest), MiB, append(response), [this, response, append](const QString& why) {
        if (!why.isEmpty()) { finish(why); return; }
        const auto release = QJsonDocument::fromJson(*response).object();
        const auto tag = release.value("tag_name").toString();
        if (!matches(tag, "catalog-v1-[1-9][0-9]{0,9}") || release.value("draft").toBool()
            || release.value("prerelease").toBool()) {
            finish(QStringLiteral("官方仓库尚未发布可用目录")); return;
        }
        auto bytes = std::make_shared<QByteArray>();
        fetch(QUrl(remote + tag + "/index-v1.json"), MiB, append(bytes), [this, tag, bytes, append](const QString& why) {
            if (!why.isEmpty()) { finish(why); return; }
            auto sig = std::make_shared<QByteArray>();
            fetch(QUrl(remote + tag + "/index-v1.sig"), 256, append(sig), [this, tag, bytes, sig](const QString& why) {
                if (!why.isEmpty()) { finish(why); return; }
                QString error;
                Catalog candidate;
                candidate.bytes = *bytes; candidate.signatures = *sig;
                const auto packed = evidence(candidate);
                auto incoming = decode(packed, error);
                if (!incoming.sequence) { finish(error); return; }
                if (tag != QStringLiteral("catalog-v1-%1").arg(incoming.sequence) || incoming.sequence < m_catalog.sequence
                    || (incoming.sequence == m_catalog.sequence && incoming.bytes != m_catalog.bytes)
                    || incoming.expires <= QDateTime::currentDateTimeUtc()) {
                    finish(QStringLiteral("官方目录已过期或版本校验失败")); return;
                }
                if (!write(root() + "/catalog.json", packed)) { finish(QStringLiteral("无法保存官方目录缓存")); return; }
                readCache();
                finish({});
            });
        });
    });
}
void Client::install(const InstallPlan& plan, bool activateFirst)
{
    if (m_busy || plan.downloads.isEmpty()) return;
    const auto before = desired();
    if (snapshotHash(before) != plan.basis) {
        finish(QStringLiteral("已安装插件列表发生变化，请重新选择安装")); return;
    }
    // 重新验证确认时的版本组合，避免模态期间另一个入口改变库存。
    for (const auto& e : plan.downloads) {
        const auto it = std::find_if(m_catalog.entries.cbegin(), m_catalog.entries.cend(),
            [&](const Entry& signedEntry) { return json(signedEntry) == json(e); });
        if (it == m_catalog.entries.cend() || !installable(e, BuildChannel::development)
            || m_catalog.expires <= QDateTime::currentDateTimeUtc()) {
            finish(QStringLiteral("目录已变化，请重新选择安装")); return;
        }
    }
    struct Job {
        explicit Job(const QString& path) : stage(path) {}
        QTemporaryDir stage;
        InstallPlan plan;
        Inventory before;
        QByteArray proof;
        QStringList promoted;
        QString mirrorPrefixes;
        bool committed = false;
        ~Job() {
            if (!committed)
                for (const auto& path : promoted)
                    if (safeDirectory(path)) QDir(path).removeRecursively();
        }
        qsizetype position = 0;
        qsizetype source = 0;
        qint64 complete = 0;
    };
    const auto stageRoot = root() + "/staging";
    if (!QDir().mkpath(stageRoot) || !safeDirectory(root()) || !safeDirectory(stageRoot)) {
        finish(QStringLiteral("无法创建插件暂存目录")); return;
    }
    auto job = std::make_shared<Job>(stageRoot + "/txn-XXXXXX");
    if (!job->stage.isValid()) { finish(QStringLiteral("无法创建插件暂存目录")); return; }
    job->plan = plan; job->proof = evidence(m_catalog); job->before = before;
    job->mirrorPrefixes = PluginStore::mirrorList();
    m_busy = true; m_cancelled = false; m_status = QStringLiteral("正在下载插件…"); emit changed();
    auto step = std::make_shared<std::function<void()>>();
    const std::weak_ptr<std::function<void()>> weak = step;
    *step = [this, job, weak, activateFirst] {
        if (m_cancelled) { finish(QStringLiteral("已取消")); return; }
        if (job->position == job->plan.downloads.size()) {
            QString error;
            if (snapshotHash(desired()) != snapshotHash(job->before)) {
                finish(QStringLiteral("已安装插件列表发生变化，请重试")); return;
            }
            if (m_catalog.expires <= QDateTime::currentDateTimeUtc()) {
                finish(QStringLiteral("目录已过期，请刷新后重试")); return;
            }
            if (!queue(job->plan.inventory, error)) { finish(error); return; }
            const bool applied = activateFirst && job->before.isEmpty()
                && HotaruNativeHost::tryActivateFirstInstall(error);
            if (!error.isEmpty()) { finish(error); emit installed(); return; }
            job->committed = true;
            finish(applied ? QStringLiteral("安装完成，可直接启用")
                           : QStringLiteral("已暂存，重启 Hotaru 后生效"));
            emit installed();
            return;
        }
        const auto e = job->plan.downloads[job->position];
        const auto sources = packageSources(QUrl(remote + e.tag + '/' + e.asset.name), job->mirrorPrefixes);
        const auto url = sources.at(job->source);
        const auto mirrorHost = job->source + 1 < sources.size() ? url.host() : QString{};
        auto file = std::make_shared<QFile>(job->stage.filePath(e.asset.name));
        if (!file->open(QIODevice::WriteOnly | QIODevice::NewOnly)) { finish(QStringLiteral("无法写入暂存文件")); return; }
        auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
        fetch(url, e.asset.size, [this, job, file, hash](const QByteArray& chunk) {
            if (file->write(chunk) != chunk.size()) return false;
            hash->addData(chunk);
            emit progress(job->complete + file->pos(), job->plan.totalBytes);
            return true;
        }, [this, job, e, file, hash, count = sources.size(), next = weak.lock()](const QString& why) {
            const bool ok = file->flush() && file->size() == e.asset.size;
            file->close();
            if (!why.isEmpty() || !ok || hash->result().toHex() != e.asset.hash) {
                if (!m_cancelled && job->source + 1 < count && next) {
                    if (!file->remove()) { finish(QStringLiteral("无法清理下载暂存文件")); return; }
                    ++job->source;
                    emit progress(job->complete, job->plan.totalBytes);
                    (*next)();
                    return;
                }
                finish(why.isEmpty() ? QStringLiteral("插件包大小或 SHA-256 不匹配") : why);
                return;
            }
            QString error;
            const auto unpacked = job->stage.filePath(e.id);
            Installed item{e, "official", {}};
            if (!QDir().mkdir(unpacked) || !extractPackage(file->fileName(), unpacked, error)
                || !write(unpacked + "/catalog-proof.json", job->proof) || !packageTarget(item, unpacked, error)) {
                finish(error.isEmpty() ? QStringLiteral("无法保存或校验插件包") : error); return;
            }
            job->promoted << storagePath(item);
            if (!checkInstalled(item, error)) { finish(error); return; }
            job->plan.inventory[e.id] = item;
            job->complete += e.asset.size;
            job->source = 0;
            ++job->position;
            if (next) (*next)();
        }, 0, mirrorHost);
    };
    (*step)();
}
}


namespace HotaruPluginRepository {
namespace {
bool checkCompatibility(const QString& path, const QTemporaryDir& temp, QString& error)
{
    error.clear();
    const auto compatDirectory = temp.filePath("compat");
    PluginHost::setPluginDirectoryForSelftest(compatDirectory);
    const auto compatibility = HotaruNativeHost::importPackage(path, error);
    const auto compatibilityId = QPluginLoader(path).metaData().value("MetaData").toObject().value("Id").toString();
    const bool defaultOff = compatibility && !compatibilityId.isEmpty() && PluginHost::isDisabled(compatibilityId)
        && !PluginHost::plugin(compatibilityId);
    PluginHost::setPluginDirectoryForSelftest({});
    if (!defaultOff) return fail(error, QStringLiteral("本地兼容插件没有默认关闭：%1").arg(error));
    return true;
}
}
bool selftest(const QStringList& paths, QString& error)
{
    if (paths.size() != 5 && paths.size() != 7 && paths.size() != 8)
        return fail(error, QStringLiteral("参数为 LAN 三个 ZIP、兼容 DLL、测试直属 ZIP；可追加 [公钥 hex]、索引和签名文件"));
    if (paths.size() > 5) {
        const bool overrideKey = paths.size() == 8;
        if (overrideKey && !matches(paths[5], "[0-9a-fA-F]{64}"))
            return fail(error, QStringLiteral("发布夹具公钥无效"));
        const QScopedValueRollback fixtureKey(testKey, overrideKey ? QByteArray::fromHex(paths[5].toLatin1()) : QByteArray{});
        Catalog fixture;
        fixture.bytes = read(paths[paths.size() - 2], MiB);
        fixture.signatures = read(paths.last(), 256);
        if (decode(evidence(fixture), error).entries.size() != 3)
            return fail(error, QStringLiteral("发布工具生成的 LAN 目录无法读取：%1").arg(error));
    }
    QTemporaryDir temp;
    if (!temp.isValid()) return fail(error, QStringLiteral("无法建立测试目录"));
    qputenv(BuildChannel::dataOverride, temp.filePath("data").toUtf8());
    const QScopedValueRollback storage(storageRootOverride, temp.filePath("repository"));
    EVP_PKEY* generated = nullptr;
    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> keygen(
        EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr), EVP_PKEY_CTX_free);
    if (!keygen || EVP_PKEY_keygen_init(keygen.get()) != 1 || EVP_PKEY_keygen(keygen.get(), &generated) != 1)
        return fail(error, QStringLiteral("生成临时测试密钥失败"));
    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(generated, EVP_PKEY_free);
    QByteArray raw(32, '\0');
    size_t keySize = 32;
    if (EVP_PKEY_get_raw_public_key(key.get(), reinterpret_cast<unsigned char*>(raw.data()), &keySize) != 1)
        return fail(error, QStringLiteral("读取临时测试公钥失败"));
    const QScopedValueRollback useKey(testKey, raw);
    Catalog catalog;
    QJsonArray published;
    QList<Entry> entries;
    for (int i = 0; i < 3; ++i) {
        QTemporaryDir stage(temp.filePath("local-XXXXXX"));
        Entry e;
        if (!readLocalPackage(paths[i], stage.path(), e, error)) return false;
        if (i == 1 && HotaruNativeHost::importPackage(paths[i], error))
            return fail(error, QStringLiteral("未验证包绕过了依赖检查"));
        e.channel = i == 2 ? "preview" : "stable";
        e.host.min = "1.0.0";
        e.tag = "test-v1.0.0";
        const QFileInfo zip(paths[i]);
        e.asset = {QStringLiteral("hotaru-plugin-%1-%2-x64.zip").arg(e.id, e.version),
            zip.size(), digest(zip.absoluteFilePath(), zip.size())};
        published << json(e);
        entries << e;
    }
    const auto now = QDateTime::currentDateTimeUtc();
    const auto sign = [&](int sequence, const QDateTime& issued, const QDateTime& expires) {
        Catalog c;
        c.bytes = QJsonDocument(QJsonObject{{"format", 1}, {"sequence", sequence},
            {"issued", issued.toString(Qt::ISODate)}, {"expires", expires.toString(Qt::ISODate)},
            {"plugins", published}}).toJson(QJsonDocument::Compact);
        const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        c.signatures.resize(64);
        size_t size = 64;
        if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1
            || EVP_DigestSign(ctx.get(), reinterpret_cast<unsigned char*>(c.signatures.data()), &size,
                reinterpret_cast<const unsigned char*>(c.bytes.constData()), c.bytes.size()) != 1)
            c.signatures.clear();
        return c;
    };
    const auto signedIndex = sign(2, now, now.addDays(catalogValidityDays));
    catalog = decode(evidence(signedIndex), error);
    if (catalog.entries.size() != 3 || catalogListings(catalog, false).size() != 3 || catalogListings(catalog, true).size() != 3)
        return fail(error, QStringLiteral("两种宿主均应列出符合条件的预览包：%1").arg(error));
    auto mixed = catalog;
    auto newerPreview = entries[0];
    newerPreview.channel = "preview";
    newerPreview.version = "999.0.0";
    mixed.entries.prepend(newerPreview);
    const auto listsVersion = [&](bool development, const QString& version) {
        const auto listings = catalogListings(mixed, development);
        return std::any_of(listings.cbegin(), listings.cend(), [&](const Entry& e) {
            return e.id == newerPreview.id && e.version == version;
        });
    };
    if (!listsVersion(false, entries[0].version) || !listsVersion(true, newerPreview.version))
        return fail(error, QStringLiteral("同一插件的预览包不应遮挡正式版可安装的版本"));
    if (decode(evidence(sign(2, now, now.addDays(catalogValidityDays + 1))), error).sequence)
        return fail(error, QStringLiteral("超过一年有效期的目录应被拒绝"));
    auto changed = signedIndex;
    changed.bytes += ' ';
    if (decode(evidence(changed), error).sequence)
        return fail(error, QStringLiteral("修改后的目录不应通过验签"));
    InstallPlan plan;
    if (!planInstall(kHotaruLanClipboardId, catalog, {}, false, plan, error) || plan.downloads.size() != 2
        || plan.downloads.first().id != kHotaruLanCoreId || planInstall(kHotaruLanFilesId, catalog, {}, false, plan, error))
        return fail(error, QStringLiteral("依赖顺序或正式版预览规则未生效"));
    const auto expired = decode(evidence(sign(1, now.addDays(-2), now.addDays(-1))), error);
    if (!expired.sequence || planInstall(kHotaruLanCoreId, expired, {}, true, plan, error))
        return fail(error, QStringLiteral("过期目录可浏览但不可安装"));

    error.clear();
    for (int i = 0; i < 3; ++i)
        if (!HotaruNativeHost::importPackage(paths[i], error))
            return fail(error, QStringLiteral("导入本地包 %1 失败：%2").arg(i).arg(error));
    if (!pending() || !active().isEmpty() || desired().size() != 3)
        return fail(error, QStringLiteral("本地导入不应立即切换活动清单"));
    HotaruNativeHost::start();
    if (pending() || active().size() != 3 || HotaruNativeHost::enabled(kHotaruLanCoreId))
        return fail(error, QStringLiteral("本地 LAN 包没有按默认关闭状态激活：%1").arg(activationError()));
    {
        SettingsDialog settings;
        settings.openAt(1);
        const auto cards = settings.findChildren<QFrame*>(QStringLiteral("pluginCard"));
        const auto badges = settings.findChildren<QLabel*>(QStringLiteral("hotaruNativeBadge"));
        const auto versions = settings.findChildren<QLabel*>(QStringLiteral("pluginVersionBadge"));
        if (cards.size() != 3 || !badges.isEmpty() || versions.size() != 3)
            return fail(error, QStringLiteral("本地导入列表应显示三张卡片、零个官方徽标；实际 %1 张、%2 个")
                .arg(cards.size()).arg(badges.size()));
    }
    if (!HotaruNativeHost::setEnabled(kHotaruLanCoreId, true, error)) return false;
    if (!HotaruNativeHost::setEnabled(kHotaruLanClipboardId, true, error)) return false;
    QStringList affected;
    if (!HotaruNativeHost::uninstall(kHotaruLanCoreId, affected, true, error) || affected.size() != 3
        || HotaruNativeHost::enabled(kHotaruLanClipboardId))
        return fail(error, QStringLiteral("卸载前置未立即停用依赖"));
    HotaruNativeHost::stop();
    activate([](const Inventory&, bool trial, QString& error) {
        return !trial || fail(error, QStringLiteral("模拟加载失败"));
    });
    if (pending() || active().size() != 3 || activationError().isEmpty())
        return fail(error, QStringLiteral("试启动失败未保留旧版"));
    if (!queue({}, error) || !write(root() + "/trial", snapshotHash({}).toLatin1()))
        return fail(error, QStringLiteral("无法准备中断夹具：%1").arg(error));
    bool trialWasRepeated = false;
    activate([&](const Inventory&, bool trial, QString&) { trialWasRepeated |= trial; return true; });
    if (trialWasRepeated || active().size() != 3 || pending())
        return fail(error, QStringLiteral("中断的试启动没有回退"));

    // 模拟官方源，实际走 Client 的缓存、下载、摘要与提交路径。
    published = {};
    for (auto e : entries) { e.channel = "preview"; published << json(e); }
    const auto previewOnly = sign(3, now, now.addDays(30));
    QScopedValueRollback fakeNetwork(useTestTransport, true);
    QScopedValueRollback restoreResponses(testResponses, QMap<QUrl, QByteArray>{});
    const auto serve = [&](int sequence, const Catalog& c) {
        const auto tag = QStringLiteral("catalog-v1-%1").arg(sequence);
        testResponses[QUrl(latest)] = QJsonDocument(QJsonObject{{"tag_name", tag}}).toJson();
        testResponses[QUrl(remote + tag + "/index-v1.json")] = c.bytes;
        testResponses[QUrl(remote + tag + "/index-v1.sig")] = c.signatures;
    };
    serve(3, previewOnly);
    Client client(nullptr);
    const auto wait = [&](const std::function<void()>& action) {
        QEventLoop loop;
        const auto done = QObject::connect(&client, &Client::changed, &loop, [&] { if (!client.busy()) loop.quit(); });
        action();
        if (client.busy()) { QTimer::singleShot(5000, &loop, &QEventLoop::quit); loop.exec(); }
        QObject::disconnect(done);
        return !client.busy();
    };
    if (!wait([&] { client.refresh(true); }) || client.catalog().sequence != 3)
        return fail(error, QStringLiteral("刷新签名目录失败：%1").arg(client.status()));
    const auto trusted = client.catalog();
    {
        InstallPlan denied;
        QString reason;
        if (planInstall(kHotaruLanCoreId, trusted, {}, false, denied, reason) || !denied.downloads.isEmpty())
            return fail(error, QStringLiteral("正式版不能为仅预览的目录条目生成安装计划"));
    }
    {
        const QTemporaryDir noCache(temp.filePath("no-catalog-XXXXXX"));
        if (!noCache.isValid()) return fail(error, QStringLiteral("无法准备无目录夹具"));
        const QScopedValueRollback rootOverride(storageRootOverride, noCache.path());
        const QScopedValueRollback offline(holdTestResponse, true);
        SettingsDialog settings;
        settings.openAt(1);
        auto* button = settings.findChild<QPushButton*>(QStringLiteral("openPluginRepository"));
        if (!button) return fail(error, QStringLiteral("插件首页缺少仓库入口"));
        button->click();
        QCoreApplication::processEvents();
        auto* manual = settings.findChild<QPushButton*>(QStringLiteral("pluginManualDownload"));
        if (!manual || !manual->isVisible())
            return fail(error, QStringLiteral("没有有效目录时应提示手动下载"));
    }
    {
        SettingsDialog settings;
        settings.openAt(1);
        auto* button = settings.findChild<QPushButton*>(QStringLiteral("openPluginRepository"));
        auto* homeTabs = settings.findChild<QTabBar*>(QStringLiteral("pluginCategoryTabs"));
        if (!button || !homeTabs) return fail(error, QStringLiteral("插件首页缺少仓库入口或分类"));
        homeTabs->setCurrentIndex(2);
        const auto initialTheme = Theme::current();
        for (int i = 0; i < 2; ++i) {
            button->click();
            auto* popup = settings.findChild<QDialog*>(QStringLiteral("pluginRepositoryDialog"));
            if (!popup || !popup->isVisible() || !popup->isModal())
                return fail(error, QStringLiteral("未打开官方仓库弹窗"));
            const auto cards = popup->findChildren<QFrame*>(QStringLiteral("pluginCard"));
            if (cards.size() != 3)
                return fail(error, QStringLiteral("仓库弹窗未显示三个签名预览插件"));
            for (auto* card : cards) {
                auto* status = card->findChild<QLabel*>(QStringLiteral("pluginStatus"));
                auto* action = card->findChild<QPushButton*>(QStringLiteral("pluginCatalogInstall"));
                if (!status || !status->text().contains(QStringLiteral("开发预览")) || !action ||
                    (!BuildChannel::development && (action->isEnabled() ||
                        action->text() != QStringLiteral("仅开发版可从仓库安装"))))
                    return fail(error, QStringLiteral("预览条目缺少渠道标记或正式版安装限制"));
            }
            auto* scroll = popup->findChild<QScrollArea*>(QStringLiteral("pluginRepositoryScroll"));
            auto* manual = popup->findChild<QPushButton*>(QStringLiteral("pluginManualDownload"));
            if (!scroll || !manual || manual->isVisible())
                return fail(error, QStringLiteral("有效目录应显示列表并隐藏手动下载提示"));
            const auto matchesTheme = [popup, scroll] {
                QImage image(popup->size(), QImage::Format_RGB32);
                popup->render(&image);
                const auto expected = Theme::color("window");
                const auto listBottom = scroll->viewport()->mapTo(popup, scroll->viewport()->rect().bottomLeft() + QPoint(4, -4));
                return image.pixelColor(4, 4) == expected
                    && image.pixelColor(4, popup->height() - 5) == expected
                    && image.rect().contains(listBottom) && image.pixelColor(listBottom) == expected;
            };
            const auto otherTheme = initialTheme == Theme::Mode::Dark ? Theme::Mode::Light : Theme::Mode::Dark;
            const bool before = matchesTheme();
            if (i == 0) Theme::setMode(otherTheme);
            const bool after = matchesTheme();
            Theme::setMode(initialTheme);
            if (!before || !after)
                return fail(error, QStringLiteral("仓库弹窗外框和列表底色应随深浅主题统一切换"));
            popup->reject();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            if (settings.findChild<QDialog*>(QStringLiteral("pluginRepositoryDialog")) || homeTabs->currentIndex() != 2)
                return fail(error, QStringLiteral("关闭仓库弹窗后应保留主页面分类"));
        }
    }
    serve(2, sign(2, now, now.addDays(30)));
    if (!wait([&] { client.refresh(true); }) || client.catalog().sequence != 3)
        return fail(error, QStringLiteral("目录回退覆盖了缓存"));
    serve(3, signedIndex);
    if (!wait([&] { client.refresh(true); }) || client.catalog().bytes != trusted.bytes)
        return fail(error, QStringLiteral("同序号内容覆盖了缓存"));
    {
        const QScopedValueRollback offline(holdTestResponse, true);
        if (!wait([&] { client.refresh(true); }) || Client(nullptr).catalog().sequence != 3)
            return fail(error, QStringLiteral("离线不能读取最近签名目录"));
    }
    if (!BuildChannel::development) {
        QTemporaryDir stage(temp.filePath("stable-XXXXXX"));
        Entry stable;
        if (!readLocalPackage(paths[4], stage.path(), stable, error)) return false;
        stable.host.min = "1.0.0";
        stable.tag = "test-v1.0.0";
        const QFileInfo zip(paths[4]);
        stable.asset = {zip.fileName(), zip.size(), digest(zip.absoluteFilePath(), zip.size())};
        published << json(stable);
        serve(4, sign(4, now, now.addDays(30)));
        if (!wait([&] { client.refresh(true); }) || client.catalog().sequence != 4
            || !planInstall(stable.id, client.catalog(), active(), false, plan, error))
            return fail(error, QStringLiteral("正式插件目录规划失败：%1").arg(error));
        testResponses[QUrl(remote + stable.tag + '/' + stable.asset.name)] = read(paths[4], packageLimit);
        if (!wait([&] { client.install(plan); }) || !pending())
            return fail(error, QStringLiteral("正式签名包下载安装失败：%1").arg(client.status()));
        HotaruNativeHost::start();
        if (!activationError().isEmpty() || active().value(stable.id).source != "official"
            || HotaruNativeHost::enabled(stable.id) || !HotaruNativeHost::setEnabled(stable.id, true, error))
            return fail(error, QStringLiteral("正式包激活或默认关闭检查失败：%1").arg(error));
        HotaruNativeHost::stop();
        return checkCompatibility(paths[3], temp, error);
    }
    if (!planInstall(kHotaruLanFilesId, trusted, active(), true, plan, error))
        return fail(error, QStringLiteral("签名预览依赖无法规划：%1").arg(error));
    for (int i = 0; i < 3; ++i)
        testResponses[QUrl(remote + trusted.entries[i].tag + '/' + trusted.entries[i].asset.name)] = read(paths[i], packageLimit);
    if (!wait([&] { client.install(plan); client.cancel(); }) || pending())
        return fail(error, QStringLiteral("取消下载不能提交安装"));
    const auto url = QUrl(remote + plan.downloads[0].tag + '/' + plan.downloads[0].asset.name);
    auto original = testResponses[url];
    testResponses[url] += ' ';
    if (!wait([&] { client.install(plan); }) || pending())
        return fail(error, QStringLiteral("超限下载不能提交安装"));
    testResponses[url] = original;
    testResponses[url][0] = 'X';
    if (!wait([&] { client.install(plan); }) || pending())
        return fail(error, QStringLiteral("哈希错误不能提交安装"));
    testResponses[url] = original;
    if (!wait([&] { client.install(plan); }) || !pending())
        return fail(error, QStringLiteral("签名预览下载安装失败：%1").arg(client.status()));
    const auto official = desired();
    HotaruNativeHost::start();
    if (active().value(kHotaruLanCoreId).source != "official" || !activationError().isEmpty())
        return fail(error, QStringLiteral("签名包重启安装失败：%1").arg(activationError()));
    HotaruNativeHost::stop();
    const auto badFile = storagePath(official.value(kHotaruLanFilesId)) + "/extra.dll";
    if (!write(badFile, QByteArray("invalid"))) return fail(error, QStringLiteral("无法准备损坏的 DLL"));
    Inventory after;
    activate([&](const Inventory& safe, bool, QString&) { after = safe; return true; });
    if (after.size() != 2 || after.contains(kHotaruLanFilesId))
        return fail(error, QStringLiteral("文件损坏没有按依赖链隔离"));
    if (!queue({}, error)) return false;
    HotaruNativeHost::start();
    if (pending() || !active().isEmpty() || !planInstall(kHotaruLanFilesId, trusted, active(), true, plan, error))
        return fail(error, QStringLiteral("无法清理现有插件并准备首次安装"));
    const auto sourceFor = [](const Entry& e) { return QUrl(remote + e.tag + '/' + e.asset.name); };
    const auto mirrorFor = [&](const Entry& e) { return packageSources(sourceFor(e), PluginStore::mirrorList()).first(); };
    const auto coreUrl = sourceFor(plan.downloads.first());
    const auto filesUrl = sourceFor(plan.downloads.last());
    const auto coreMirror = mirrorFor(plan.downloads.first());
    const auto filesMirror = mirrorFor(plan.downloads.last());
    if (coreMirror == coreUrl || filesMirror == filesUrl)
        return fail(error, QStringLiteral("未生成 HTTPS 镜像下载地址"));
    testResponses[coreMirror] = testResponses[coreUrl];
    testResponses[coreMirror][0] = 'X';
    testResponses[filesMirror] = testResponses.take(filesUrl);
    if (!wait([&] { client.install(plan, true); }) || pending()
        || active().value(kHotaruLanFilesId).source != "official"
        || HotaruNativeHost::enabled(kHotaruLanCoreId))
        return fail(error, QStringLiteral("镜像校验回退或首次即时安装失败：%1").arg(client.status()));
    HotaruNativeHost::stop();
    return checkCompatibility(paths[3], temp, error);
}
}

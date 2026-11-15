#include "LanFiles.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <qt_windows.h>

namespace HotaruLan {
namespace {
constexpr int kMaxEntries = 1024;
constexpr qint64 kMaxTransfer = 32LL * 1024 * 1024 * 1024;
bool add(Catalog& catalog, QSet<QString>& seen, QSet<QString>& dirs, Entry entry)
{
    if (!validRelativePath(entry.path) || catalog.entries.size() >= kMaxEntries ||
        entry.size < 0 || entry.size > kMaxTransfer - catalog.bytes ||
        (entry.directory && entry.size != 0))
        return false;
    const QString key = entry.path.toCaseFolded();
    const auto slash = key.lastIndexOf(u'/');
    if (seen.contains(key) || (slash >= 0 && !dirs.contains(key.left(slash))))
        return false;
    seen.insert(key);
    if (entry.directory)
        dirs.insert(key);
    catalog.bytes += entry.size;
    catalog.entries.push_back(std::move(entry));
    return true;
}

bool readTree(Catalog& catalog, QSet<QString>& seen, QSet<QString>& dirs,
              const QString& full, const QString& relative)
{
    if (!safeLocalPath(full))
        return false;
    const QFileInfo item(full);
    if ((!item.isDir() && !item.isFile()) ||
        !add(catalog, seen, dirs, {relative, full, item.isDir() ? 0 : item.size(), item.isDir()}))
        return false;
    if (!item.isDir())
        return true;
    QDirIterator it(full, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        const QString child = it.next();
        if (!readTree(catalog, seen, dirs, child, relative + u'/' + QFileInfo(child).fileName()))
            return false;
    }
    return true;
}

QString stagingTemplate(const QString& destination)
{
    if (safeLocalPath(destination) && QFileInfo(destination).isDir())
        return QDir(destination).filePath(QStringLiteral(".hotaru-lan-XXXXXX"));
    // 指向不存在的设备目录，确保无效目标不回退到系统临时目录。
    return QStringLiteral("NUL:/hotaru-lan-XXXXXX");
}
}

bool safeLocalPath(const QString& path)
{
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
    if (clean.size() < 3 || !clean[0].isLetter() || clean[1] != u':' || clean[2] != u'/' ||
        clean.mid(2).contains(u':'))
        return false;
    const QString root = QDir::toNativeSeparators(clean.left(3));
    const UINT drive = GetDriveTypeW(reinterpret_cast<LPCWSTR>(root.utf16()));
    if (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE && drive != DRIVE_RAMDISK)
        return false;
    for (QString current = clean; current.size() > 3;) {
        const QString native = QDir::toNativeSeparators(current);
        const DWORD attrs = GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
        current = current.left(qMax(3, current.lastIndexOf(u'/')));
    }
    return true;
}

bool validRelativePath(const QString& path)
{
    static const QRegularExpression reserved(QStringLiteral(
        "^(CON|PRN|AUX|NUL|CONIN\\$|CONOUT\\$|COM[1-9¹²³]|LPT[1-9¹²³])$"),
        QRegularExpression::CaseInsensitiveOption);
    if (path.isEmpty() || path.size() > 200)
        return false;
    const auto parts = path.split(u'/');
    if (parts.size() > 16)
        return false;
    for (const auto& part : parts) {
        if (part.isEmpty() || part == u"." || part == u".." ||
            part.endsWith(u'.') || part.endsWith(u' ') ||
            reserved.match(part.section(u'.', 0, 0).trimmed()).hasMatch())
            return false;
        for (const auto ch : part)
            if (ch.unicode() < 32 || QStringView(u"\\:*?\"<>|").contains(ch))
                return false;
    }
    return true;
}

bool collectFiles(const QStringList& paths, Catalog& result, QString& error)
{
    Catalog catalog;
    QSet<QString> seen, dirs;
    if (paths.isEmpty())
        return false;
    for (const auto& path : paths) {
        const auto full = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        if (!readTree(catalog, seen, dirs, full, QFileInfo(full).fileName())) {
            error = QStringLiteral("无法发送：路径、链接、同名文件或传输大小不符合要求。");
            return false;
        }
    }
    result = std::move(catalog);
    return true;
}

QByteArray encodeOffer(const Catalog& catalog)
{
    QJsonArray items;
    for (const auto& entry : catalog.entries)
        items.append(QJsonObject{{"p", entry.path}, {"n", QString::number(entry.size)},
                                 {"d", entry.directory}});
    return QJsonDocument(items).toJson(QJsonDocument::Compact);
}

bool decodeOffer(QByteArrayView payload, Catalog& result)
{
    if (payload.isEmpty() || payload.size() > kOfferBytes)
        return false;
    const auto doc = QJsonDocument::fromJson(payload.toByteArray());
    if (!doc.isArray() || doc.array().isEmpty() || doc.array().size() > kMaxEntries)
        return false;
    Catalog catalog;
    QSet<QString> seen, dirs;
    for (const auto value : doc.array()) {
        const auto item = value.toObject();
        const auto number = item.value("n").toString();
        bool ok = false;
        const auto size = number.toLongLong(&ok);
        if (!ok || QString::number(size) != number || !item.value("d").isBool() ||
            !add(catalog, seen, dirs, {item.value("p").toString(), {}, size,
                                       item.value("d").toBool()}))
            return false;
    }
    result = std::move(catalog);
    return true;
}

Staging::Staging(const QString& destination)
    : m_destination(QDir(destination).absolutePath()), m_stage(stagingTemplate(destination))
{
}

bool Staging::valid() const { return m_stage.isValid() && safeLocalPath(m_stage.path()); }

QString Staging::path(const Entry& entry) const
{
    return validRelativePath(entry.path) && valid() ? QDir(m_stage.path()).filePath(entry.path)
                                                   : QString{};
}

bool Staging::prepare(const Catalog& catalog)
{
    if (!valid())
        return false;
    for (const auto& entry : catalog.entries) {
        if (!entry.directory)
            continue;
        const auto output = path(entry);
        if (output.isEmpty() || !safeLocalPath(QFileInfo(output).absolutePath()) ||
            !QDir().mkdir(output) || !safeLocalPath(output))
            return false;
    }
    return true;
}

QString Staging::publish()
{
    if (!valid() || !safeLocalPath(m_destination))
        return {};
    const auto from = QDir::toNativeSeparators(m_stage.path());
    for (int i = 1; i <= 1000; ++i) {
        const auto name = i == 1 ? QStringLiteral("Hotaru 接收") :
                                   QStringLiteral("Hotaru 接收 (%1)").arg(i);
        const auto target = QDir(m_destination).filePath(name);
        const auto to = QDir::toNativeSeparators(target);
        if (MoveFileW(reinterpret_cast<LPCWSTR>(from.utf16()),
                      reinterpret_cast<LPCWSTR>(to.utf16()))) {
            m_stage.setAutoRemove(false);
            return target;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
            return {};
    }
    return {};
}
}

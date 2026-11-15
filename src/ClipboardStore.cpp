#include "ClipboardStore.h"
#include "hotaru-native-plugin.h"
#include "Utils.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QSaveFile>
#include <QImageReader>
#include <QScopedValueRollback>

#include <algorithm>
#include <memory>

#include <windows.h>   // GetClipboardSequenceNumber：POSIX 没有对等计数器（#90 第五件）

namespace {

QString configPath() { return Utils::dataDir() + "/config.ini"; }
QString storePath() { return Utils::dataDir() + "/clipboard.json"; }
QString imageDir() { return Utils::dataDir() + "/clipimg"; }

QString fingerprint(const QImage& canonical)
{
    constexpr qint64 cap = 64LL * 1024 * 1024;
    const auto bytes = qMin(qint64(canonical.bytesPerLine()) * canonical.height(), cap);
    return QString::fromLatin1(QCryptographicHash::hash(
        QByteArrayView(reinterpret_cast<const char*>(canonical.constBits()), qsizetype(bytes)),
        QCryptographicHash::Sha1).toHex());
}

bool privateContent(const QMimeData* mime)
{
    return !mime || mime->hasFormat(QLatin1String(kHotaruClipboardPrivate));
}

QString human(qint64 bytes)
{
    const double kb = bytes / 1024.0;
    if (kb < 1024)
        return QString("%1 KB").arg(kb, 0, 'f', kb < 10 ? 1 : 0);
    return QString("%1 MB").arg(kb / 1024.0, 0, 'f', 1);
}

} // namespace

ClipboardStore& ClipboardStore::instance()
{
    static ClipboardStore s;
    return s;
}

void ClipboardStore::start()
{
    if (m_started)
        return;
    m_started = true;
    load();
    QDir().mkpath(imageDir());
    m_seq = GetClipboardSequenceNumber();
    // dataChanged 在部分程序写剪贴板时不发（尤其带延迟渲染的），再加一个 2 秒兜底轮询。
    // 轮询只问一次 GetClipboardSequenceNumber()：Windows 维护的计数器，剪贴板没变就几微秒返回。
    // 不加这道闸门时，一张 2560x1440 截图躺在剪贴板上会让每 2 秒重读+重编码一次，
    // 实测稳定烧掉 13.8% 单核（4125ms/30s），而且全在 GUI 线程上。
    //
    // 先取得实时预览，历史写盘放到下一轮事件循环，避免恢复时重入修改历史列表。
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this, [this] {
        // 在下次写入前保住各来源的最新一条；历史编码和写盘仍延后处理。
        observeLive(true);
        QTimer::singleShot(0, this, [this] {
            const UINT s = GetClipboardSequenceNumber();
            if (!m_available)
                return;
            observeLive();
            // Windows 有时先通知再更新序号。轮询可用序号去重，真实通知仍需读取。
            m_seq = s;
            capture(QApplication::clipboard()->mimeData());
            emit changed();
        });
    });
    auto* poll = new QTimer(qApp);
    poll->setInterval(2000);
    QObject::connect(poll, &QTimer::timeout, poll, [this] {
        // 按天裁剪一天只做一次就够了：常驻实例可能连着开好几天，或者收集本来就是关着的
        // （那样 capture() 第一行就返回，永远走不到 prune()）—— 两种情况下超龄的也得走。
        if (!m_available) return;
        if (m_sweptOn != QDate::currentDate()) {
            m_sweptOn = QDate::currentDate();
            const int before = m_items.size();
            prune();
            if (m_items.size() != before) {
                save();
                emit changed();
            }
        }
        const UINT seq = GetClipboardSequenceNumber();
        if (seq == m_seq)
            return;
        m_seq = seq;
        observeLive();
        capture(QApplication::clipboard()->mimeData());
        emit changed();
    });
    poll->start();
    Utils::log(QString("clipboard: started enabled=%1 items=%2").arg(m_enabled).arg(m_items.size()));
}

void ClipboardStore::setAvailable(bool available)
{
    if (available)
        start();
    if (m_available == available)
        return;
    m_available = available;
    m_seq = GetClipboardSequenceNumber();
    m_liveLocal.reset();
    m_liveLan.reset();
    m_liveSequence.reset();
    if (available)
        observeLive();
}

void ClipboardStore::observeLive(bool signaled)
{
    if (!m_available || m_readingLive)
        return;
    const QScopedValueRollback guard(m_readingLive, true);
    const quint32 sequence = GetClipboardSequenceNumber();
    if (!signaled && sequence && m_liveSequence == sequence)
        return;
    const auto* mime = QApplication::clipboard()->mimeData();
    if (!mime)
        return;
    if (privateContent(mime) || mime->hasFormat(QLatin1String(kHotaruClipboardRestored))) {
        m_liveSequence = sequence;
        return;
    }
    auto result = makeLive(*mime);
    if (sequence != GetClipboardSequenceNumber() || result.item.hash.isEmpty())
        return;
    m_liveSequence = sequence;
    auto& slot = result.item.fromLan ? m_liveLan : m_liveLocal;
    if (!slot || slot->item.hash != result.item.hash)
        slot = std::move(result);
}

void ClipboardStore::recordReceived(const QMimeData& mime)
{
    if (!m_available || privateContent(&mime) || !mime.hasFormat(QLatin1String(kHotaruClipboardReceived)) ||
        mime.hasFormat(QLatin1String(kHotaruClipboardRestored)))
        return;
    observeLive();
    auto result = makeLive(mime);
    if (result.item.hash.isEmpty())
        return;
    m_liveLan = std::move(result);
    capture(&mime);
    QTimer::singleShot(0, this, [this] { if (m_available) emit changed(); });
}

ClipboardStore::Live ClipboardStore::makeLive(const QMimeData& mime)
{
    Live result;
    auto& item = result.item;
    item.fromLan = mime.hasFormat(QLatin1String(kHotaruClipboardReceived));
    if (mime.hasImage()) {
        const auto image = qvariant_cast<QImage>(mime.imageData());
        // 每个来源只在内存保留一张图，按像素限制缓存体积。
        constexpr qint64 maxLivePixels = 16LL * 1024 * 1024;
        if (!image.isNull() && qint64(image.width()) * image.height() <= maxLivePixels) {
            result.image = image.convertToFormat(QImage::Format_ARGB32);
            item.isImage = true;
            item.w = image.width();
            item.h = image.height();
            item.hash = fingerprint(result.image);
            result.thumbnail = image.scaled(40, 40, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
    } else if (mime.hasText()) {
        const auto text = mime.text();
        const auto bytes = text.toUtf8();
        if (!text.trimmed().isEmpty() && bytes.size() <= kMaxTextBytes) {
            item.text = text;
            item.hash = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex());
        }
    }
    item.when = QDateTime::currentDateTime();
    return result;
}

QVector<ClipboardStore::Live> ClipboardStore::liveItems()
{
    observeLive();
    QVector<Live> items;
    if (m_liveLocal) items.append(*m_liveLocal);
    if (m_liveLan) items.append(*m_liveLan);
    return items;
}

QString ClipboardStore::liveId(const Item& item)
{
    return (item.fromLan ? QStringLiteral("lan:") : QStringLiteral("local:")) + stableId(item);
}

bool ClipboardStore::restoreLive(const QString& id)
{
    if (!m_available)
        return false;
    for (const auto& value : liveItems()) {
        if (liveId(value.item) != id)
            continue;
        auto mime = std::make_unique<QMimeData>();
        if (value.item.isImage)
            mime->setImageData(value.image);
        else
            mime->setText(value.item.text);
        if (value.item.fromLan)
            mime->setData(QLatin1String(kHotaruClipboardReceived), QByteArrayLiteral("1"));
        mime->setData(QLatin1String(kHotaruClipboardRestored), QByteArrayLiteral("1"));
        QApplication::clipboard()->setMimeData(mime.release());
        return true;
    }
    return false;
}

QString ClipboardStore::stableId(const Item& item)
{
    return item.isImage ? QStringLiteral("image:%1:%2:%3").arg(item.hash).arg(item.w).arg(item.h)
                        : QStringLiteral("text:") + item.hash;
}

void ClipboardStore::load()
{
    QSettings cfg(configPath(), QSettings::IniFormat);
    m_enabled = cfg.value("clipboard/enable", true).toBool();
    m_keepDays = cfg.value("clipboard/keepDays", kDefaultKeepDays).toInt();
    if (m_keepDays < 0)
        m_keepDays = 0;
    m_sweptOn = QDate::currentDate();     // 这次 load() 就是一次按天裁剪，别紧接着再扫一遍

    m_items.clear();
    QFile f(storePath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        Item it;
        it.when = QDateTime::fromString(o.value("when").toString(), Qt::ISODate);
        it.hash = o.value("hash").toString();
        it.isImage = o.value("image").toBool();
        it.fromLan = o.value("lan").toBool();
        it.text = o.value("text").toString();
        it.imagePath = resolveImagePath(o.value("path").toString());
        it.w = o.value("w").toInt();
        it.h = o.value("h").toInt();
        it.bytes = qint64(o.value("bytes").toDouble());
        if (!it.isImage && it.text.isEmpty())
            continue;
        if (it.isImage && !QFileInfo::exists(it.imagePath))
            continue;                       // 图被删了就不留空条目
        m_items.append(it);
        if (m_items.size() >= kMaxItems)
            break;
    }
    // 超龄的先裁掉（老版本只按条数封顶，所以第一次带上天数规则启动时，
    // 这一步真的会删东西 —— 删完就把 json 重写，别让同一批条目每次启动都重算一遍）。
    const int before = m_items.size();
    prune();
    if (m_items.size() != before) {
        save();
        Utils::log(QString("clipboard: pruned on load %1 -> %2 (keepDays=%3)")
                       .arg(before).arg(m_items.size()).arg(m_keepDays));
    }
}

void ClipboardStore::save() const
{
    QJsonArray arr;
    for (const Item& it : m_items) {
        QJsonObject o;
        o.insert("when", it.when.toString(Qt::ISODate));
        o.insert("hash", it.hash);
        o.insert("image", it.isImage);
        o.insert("lan", it.fromLan);
        o.insert("bytes", double(it.bytes));
        if (it.isImage) {
            o.insert("path", it.imagePath);
            o.insert("w", it.w);
            o.insert("h", it.h);
        } else {
            o.insert("text", it.text);
        }
        arr.append(o);
    }
    QSaveFile f(storePath());
    const auto bytes = QJsonDocument(arr).toJson(QJsonDocument::Compact);
    if (f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size())
        f.commit();
}

void ClipboardStore::setEnabled(bool on)
{
    if (m_enabled == on)
        return;
    m_enabled = on;
    QSettings cfg(configPath(), QSettings::IniFormat);
    cfg.setValue("clipboard/enable", on);
    Utils::log(QString("clipboard: enabled=%1").arg(on));
    m_seq = GetClipboardSequenceNumber();
    emit changed();
}

bool ClipboardStore::expired(const Item& it, int keepDays, const QDateTime& now)
{
    if (keepDays <= 0)
        return false;
    if (!it.when.isValid())
        return true;
    return it.when.secsTo(now) >= qint64(keepDays) * 86400;
}

void ClipboardStore::setKeepDays(int days)
{
    const int clamped = days < 0 ? 0 : days;
    QSettings cfg(configPath(), QSettings::IniFormat);
    cfg.setValue("clipboard/keepDays", clamped);
    if (clamped == m_keepDays)
        return;
    m_keepDays = clamped;
    const int before = m_items.size();
    prune();
    save();
    emit changed();
    Utils::log(QString("clipboard: keepDays=%1 items %2 -> %3")
                   .arg(m_keepDays).arg(before).arg(m_items.size()));
}

void ClipboardStore::capture(const QMimeData* mime)
{
    if (!m_enabled || !m_available)
        return;
    if (privateContent(mime) || mime->hasFormat(QLatin1String(kHotaruClipboardRestored)))
        return;

    Item it;
    it.when = QDateTime::currentDateTime();
    it.fromLan = mime->hasFormat(QLatin1String(kHotaruClipboardReceived));

    QImage src = qvariant_cast<QImage>(mime->imageData());
    if (!src.isNull()) {
        // 去重的指纹用规范化后的原始位（一次格式转换 + 哈希，十几 ms 量级）。
        // 之前是"先 PNG 编码再哈希"：一张 2560x1440 截图编码要 ~400ms，白花在重复内容上。
        const QImage canon = src.convertToFormat(QImage::Format_ARGB32);
        it.isImage = true;
        it.w = canon.width();
        it.h = canon.height();
        it.hash = fingerprint(canon);
        if (!m_items.isEmpty() && stableId(m_items.first()) == stableId(it) && m_items.first().fromLan == it.fromLan)
            return;
        QByteArray raw;
        {
            QBuffer b(&raw);
            if (!b.open(QIODevice::WriteOnly) || !canon.save(&b, "PNG"))
                return;
        }
        it.bytes = raw.size();
        it.imagePath = imageDir() + "/" + it.hash + ".png";
        if (!QFileInfo::exists(it.imagePath)) {
            QSaveFile f(it.imagePath);
            if (!f.open(QIODevice::WriteOnly) || f.write(raw) != raw.size() || !f.commit())
                return;
            // 收进来的这一刻顺手落一张 40px 的：那张页以后每一行都只读这张小的，
            // 不该为了"你刚截的那张 2560×1440"在 GUI 线程上等一次整图解码。
            canon.scaled(40, 40, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                .save(thumbPath(it.imagePath), "PNG");
        }
    } else if (mime->hasText()) {
        const QString text = mime->text();
        if (text.trimmed().isEmpty())
            return;
        const QByteArray utf8 = text.toUtf8();
        if (utf8.size() > kMaxTextBytes)
            return;                       // 大段日志不值得占内存和磁盘
        it.hash = QString::fromLatin1(
            QCryptographicHash::hash(utf8, QCryptographicHash::Sha1).toHex());
        it.text = text;
        it.bytes = utf8.size();
    } else {
        return;                           // 文件列表之类先不收
    }

    if (!m_items.isEmpty() && stableId(m_items.first()) == stableId(it) && m_items.first().fromLan == it.fromLan)
        return;

    const int dup = std::find_if(m_items.begin(), m_items.end(),
                                 [&](const Item& x) { return stableId(x) == stableId(it); })
                      - m_items.begin();
    if (dup < m_items.size())
        m_items.removeAt(dup);            // 去重：同一内容重新置顶
    m_items.prepend(it);
    prune();
    save();
}

bool ClipboardStore::sweepAllowed(int itemCount)
{
    // 一条历史都没 load 到 ≠ 盘上那些图都不需要了。真要全删走的是 clear() 自己那一段，
    // 不该由"读不到条目"这件事顺带触发。
    return itemCount > 0;
}

QString ClipboardStore::resolveImagePath(const QString& recorded)
{
    if (recorded.isEmpty())
        return recorded;
    // 文件名就是那张图内容的 sha1（capture() 里这么起的），所以"同名"就等于"同内容"，
    // 这一格可以放心地优先落在**现在这个**数据目录里 —— 顺带让下面那句 save() 把老路径带回来。
    const QString here = imageDir() + '/' + QFileInfo(recorded).fileName();
    if (QFileInfo::exists(here))
        return here;
    return recorded;              // 新目录里还没有（搬家之后新复制的那几张本来就在新的）：原样返回
}

QString ClipboardStore::keepKey(const QString& path)
{
    // 比较两侧都要归一化 + 折叠大小写：json 里那条路径的写法不保证是正斜杠（APPDATA 本身
    // 就是反斜杠，拼出来是混着的），而 entryList 给的名字要自己拼回去才算路径。
    // 2026-10-16 那次只归一化了一边，两边永远不相等，启动那一趟把 49 张还在用的图
    // 全判成孤儿删了 —— 这就是"清垃圾"那段自己变成凶手的形状。
    QString s = QDir::fromNativeSeparators(path);
    s.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (s.contains(QStringLiteral("//")))
        s.replace(QStringLiteral("//"), QStringLiteral("/"));
    return s.toLower();
}

void ClipboardStore::prune()
{
    // 两条上限取交集：先按天裁，再按条数封顶。列表不保证严格按时间排（重复内容会被
    // 重新置顶），所以逐条问，不敢假设超龄的都堆在尾巴上。
    if (m_keepDays > 0) {
        const QDateTime now = QDateTime::currentDateTime();
        for (int i = m_items.size() - 1; i >= 0; --i)
            if (expired(m_items.at(i), m_keepDays, now))
                m_items.removeAt(i);
    }
    while (m_items.size() > kMaxItems)
        m_items.removeLast();

    if (!sweepAllowed(static_cast<int>(m_items.size())))
        return;
    QSet<QString> keep;
    for (const Item& it : m_items)
        if (it.isImage) {
            keep.insert(keepKey(it.imagePath));
            keep.insert(keepKey(thumbPath(it.imagePath)));   // 缩略图跟着原图活：原图在，它就不算孤儿
        }
    int wiped = 0;
    const auto files = QDir(imageDir()).entryList(QStringList() << QStringLiteral("*.png"), QDir::Files);
    for (const QString& name : files) {
        const QString full = imageDir() + "/" + name;
        if (!keep.contains(keepKey(full))) {
            QFile::remove(full);
            ++wiped;
        }
    }
    if (wiped > 0)     // 批量删文件必须留一行日志：不写的话这件事永远不会有人知道
        Utils::log(QString("clipboard: swept %1 orphan image file(s), items=%2 keepDays=%3")
                       .arg(wiped).arg(m_items.size()).arg(m_keepDays));
}

bool ClipboardStore::restore(const QString& id)
{
    if (!m_available)
        return false;
    const auto found = std::find_if(m_items.cbegin(), m_items.cend(), [&id, this](const Item& item) {
        return stableId(item) == id && !expired(item, m_keepDays, QDateTime::currentDateTime());
    });
    if (found == m_items.cend())
        return false;
    const Item item = *found;
    auto mime = std::make_unique<QMimeData>();
    if (item.isImage) {
        const QImage image(item.imagePath);
        if (image.isNull())
            return false;
        mime->setImageData(image);
    } else {
        mime->setText(item.text);
    }
    if (item.fromLan)
        mime->setData(QLatin1String(kHotaruClipboardReceived), QByteArrayLiteral("1"));
    mime->setData(QLatin1String(kHotaruClipboardRestored), QByteArrayLiteral("1"));
    QApplication::clipboard()->setMimeData(mime.release());
    return true;
}

void ClipboardStore::remove(int index)
{
    if (index < 0 || index >= m_items.size())
        return;
    m_items.removeAt(index);
    prune();
    save();
    emit changed();
}

void ClipboardStore::clear()
{
    m_items.clear();
    const auto files = QDir(imageDir()).entryList(QStringList() << QStringLiteral("*.png"), QDir::Files);
    for (const QString& name : files)
        QFile::remove(imageDir() + "/" + name);
    save();
    emit changed();
    Utils::log("clipboard: history cleared");
}

QString ClipboardStore::thumbPath(const QString& imagePath)
{
    // clipimg/<sha1>.png -> clipimg/<sha1>.t120.png：和原图同一个目录、同一次清理，
    // 不另开一棵目录树（开一棵就得再写一遍"孤儿怎么撤"，两处迟早对不上）。
    const QFileInfo fi(imagePath);
    return fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + QStringLiteral(".t120.png");
}

// 卡片上那块图最多 120×84（2026-10-19 他把这张列表改成卡片之后，40px 那颗就不够看了）。
constexpr int kThumbW = 120;
constexpr int kThumbH = 84;

QIcon ClipboardStore::iconFor(const Item& it) const
{
    if (!it.isImage || it.imagePath.isEmpty())
        return QIcon();
    const QString thumb = thumbPath(it.imagePath);
    // 这张页每敲一个过滤字、每次被看见都要把列表重铺一遍 —— 原来那是 51 张整图重解码（实测 632ms）。
    // 键用路径而不是 hash：路径本来就由 hash 派生，多一个键源只会多一处不一致。
    static QHash<QString, QIcon> cache;
    if (auto hit = cache.constFind(thumb); hit != cache.constEnd())
        return hit.value();
    QPixmap pm(thumb);
    if (pm.isNull()) {   // 没有缩略图（老历史里的第一批）：拿原图做一次，落盘，之后只读这张小的
        QImageReader reader(it.imagePath);
        const QSize size = reader.size();
        if (!size.isValid())
            return QIcon();
        reader.setScaledSize(size.scaled(kThumbW, kThumbH, Qt::KeepAspectRatio));
        pm = QPixmap::fromImage(reader.read());
        if (pm.isNull())
            return QIcon();
        pm.save(thumb, "PNG");
    }
    const QIcon icon(pm);
    if (cache.size() >= kMaxItems) cache.clear();
    cache.insert(thumb, icon);
    return icon;
}

QString ClipboardStore::summary(const Item& it)
{
    if (it.isImage)
        return QString("图片 %1×%2 · %3").arg(it.w).arg(it.h).arg(human(it.bytes));
    const QString first = it.text.section(QLatin1Char('\n'), 0, 0).trimmed();
    const int lines = it.text.count(QLatin1Char('\n')) + 1;
    QString shown = first;
    if (shown.size() > 70)
        shown = shown.left(70) + QStringLiteral("…");
    if (shown.isEmpty())
        shown = QStringLiteral("(空白)");
    return lines > 1 ? QString("%1  · %2 行 · %3").arg(shown).arg(lines).arg(human(it.bytes))
                     : QString("%1  · %2").arg(shown).arg(human(it.bytes));
}

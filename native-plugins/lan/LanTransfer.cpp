#include "LanTransfer.h"
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QStorageInfo>
#include <QtEndian>

namespace HotaruLan {
namespace {
constexpr int kHashBytes = 32;
constexpr int kChunkHeader = 4 + 8 + kHashBytes;
constexpr int kPendingMs = 90000;
constexpr int kIdleMs = 30000;

QByteArray entryIndex(int index)
{
    QByteArray bytes(4, Qt::Uninitialized);
    qToBigEndian(quint32(index), bytes.data());
    return bytes;
}
}

struct Transfers::Job {
    QPointer<Peer> peer;
    QByteArray id;
    QString device;
    Catalog catalog;
    State state = Pending;
    int index = 0;
    qint64 offset = 0, total = 0;
    QDeadlineTimer deadline{kPendingMs};
    std::unique_ptr<Staging> stage;
    QFile file;
    QCryptographicHash hash{QCryptographicHash::Sha256};

    void skipDirs()
    {
        while (index < catalog.entries.size() && catalog.entries[index].directory)
            ++index;
    }
    void nextFile()
    {
        file.close();
        hash.reset();
        offset = 0;
        ++index;
        skipDirs();
    }
};

Transfers::Transfers(QObject* parent) : QObject(parent)
{
    m_expiry.setInterval(1000);
    connect(&m_expiry, &QTimer::timeout, this, [this] {
        if (m_in && m_in->deadline.hasExpired())
            finish(true, true, QStringLiteral("接收超时"));
        if (m_out && m_out->deadline.hasExpired())
            finish(false, true, QStringLiteral("发送超时，可重试"));
    });
    m_pump.setSingleShot(true);
    connect(&m_pump, &QTimer::timeout, this, &Transfers::pumpNow);
}

Transfers::~Transfers() = default;
bool Transfers::awaitingDecision() const { return m_in && m_in->state == Pending; }

QString Transfers::progress(qint64 processed, qint64 total)
{
    return QStringLiteral("%1 / %2").arg(QLocale().formattedDataSize(processed),
                                          QLocale().formattedDataSize(total));
}

QString Transfers::pendingSummary() const
{
    if (!awaitingDecision())
        return {};
    return QStringLiteral("%1 想发送 %2 项（%3）\n首项：%4")
        .arg(m_in->device).arg(m_in->catalog.entries.size())
        .arg(QLocale().formattedDataSize(m_in->catalog.bytes), m_in->catalog.entries.first().path);
}

bool Transfers::send(const std::shared_ptr<Job>& job, Wire type, QByteArrayView data)
{
    QByteArray payload(1, char(type));
    payload.append(job->id);
    payload.append(data.data(), data.size());
    return job->peer && job->peer->send(payload);
}

void Transfers::sendOffer(Peer* peer, const QStringList& paths)
{
    if (m_out || !peer || !peer->ready() || paths.isEmpty())
        return;
    auto job = std::make_shared<Job>();
    job->peer = peer;
    job->id = randomBytes(kIdentityBytes);
    m_retryPaths = paths;
    QString error;
    if (!collectFiles(paths, job->catalog, error)) {
        m_outStatus = error;
        emit changed();
        return;
    }
    const auto offer = encodeOffer(job->catalog);
    if (job->id.isEmpty() || offer.size() > kOfferBytes || !send(job, Offer, offer)) {
        m_outStatus = QStringLiteral("无法发送请求，可重试");
        emit changed();
        return;
    }
    m_out = job;
    m_outStatus = QStringLiteral("等待对方确认");
    m_expiry.start();
    emit changed();
}

bool Transfers::accept(const QString& destination, QString* error)
{
    if (!awaitingDecision() || destination.isEmpty()) {
        if (error)
            *error = QStringLiteral("没有待接收的文件或未选择目录");
        return false;
    }
    const auto job = m_in;
    const QStorageInfo disk(destination);
    job->stage = std::make_unique<Staging>(destination);
    if (!disk.isReady() || disk.bytesAvailable() < job->catalog.bytes ||
        !job->stage->prepare(job->catalog)) {
        job->stage.reset();
        m_inStatus = QStringLiteral("无法接收：目录不可写、空间不足或包含链接，请另选目录");
        if (error)
            *error = m_inStatus;
        emit changed();
        return false;
    }
    job->skipDirs();
    job->state = Active;
    job->deadline = QDeadlineTimer(kIdleMs);
    if (!send(job, Accept)) {
        finish(true, false, QStringLiteral("无法确认，连接已断开"));
        if (error)
            *error = QStringLiteral("无法确认，连接已断开");
        return false;
    }
    m_inStatus = QStringLiteral("接收中：%1").arg(progress(0, job->catalog.bytes));
    emit changed();
    return true;
}

void Transfers::decline()
{
    if (!awaitingDecision())
        return;
    const auto job = m_in;
    finish(true, false, QStringLiteral("已拒绝接收"));
    send(job, Reject);
}

void Transfers::cancelOutgoing() { finish(false, true, QStringLiteral("已取消发送")); }
void Transfers::cancelIncoming() { finish(true, true, QStringLiteral("已取消接收")); }

void Transfers::finish(bool incoming, bool notify, QString note)
{
    auto& slot = incoming ? m_in : m_out;
    if (!slot)
        return;
    auto job = std::exchange(slot, {});
    job->file.close();
    job->stage.reset();
    if (notify)
        send(job, Cancel);
    (incoming ? m_inStatus : m_outStatus) = std::move(note);
    if (!m_in && !m_out)
        m_expiry.stop();
    emit changed();
}

void Transfers::disconnected(Peer* peer)
{
    if (m_in && m_in->peer == peer)
        finish(true, false, QStringLiteral("连接已断开，已清理未完成的接收"));
    if (m_out && m_out->peer == peer)
        finish(false, false, QStringLiteral("连接已断开，可重试"));
}

void Transfers::pump(Peer* peer)
{
    if (m_out && m_out->peer == peer && m_out->state == Active && !m_pump.isActive())
        m_pump.start(0);
}

void Transfers::pumpNow()
{
    const auto job = m_out;
    if (!job || job->state != Active || !job->peer)
        return;
    for (int i = 0; i < 4 && job == m_out && job->peer && job->peer->pendingBytes() < 256 * 1024; ++i) {
        if (job->index == job->catalog.entries.size()) {
            job->state = WaitingReceipt;
            if (!send(job, AllDone))
                finish(false, false, QStringLiteral("发送失败，可重试"));
            return;
        }
        const auto& entry = job->catalog.entries[job->index];
        if (!job->file.isOpen()) {
            job->file.setFileName(entry.source);
            if (!safeLocalPath(entry.source) || !job->file.open(QIODevice::ReadOnly) ||
                job->file.size() != entry.size) {
                finish(false, true, QStringLiteral("源文件已移动或大小已改变"));
                return;
            }
        }
        QByteArray payload = entryIndex(job->index);
        if (job->offset == entry.size) {
            payload += job->hash.result();
            if (!send(job, FileDone, payload)) {
                finish(false, true, QStringLiteral("发送失败，可重试"));
                return;
            }
            job->nextFile();
        } else {
            const qint64 count = qMin(qint64(kChunkBytes), entry.size - job->offset);
            const auto chunk = job->file.read(count);
            char offset[8];
            qToBigEndian(quint64(job->offset), offset);
            payload.append(offset, sizeof(offset));
            payload += QCryptographicHash::hash(chunk, QCryptographicHash::Sha256);
            payload += chunk;
            if (chunk.size() != count || !send(job, Chunk, payload)) {
                finish(false, true, QStringLiteral("无法读取或发送数据，可重试"));
                return;
            }
            job->hash.addData(chunk);
            job->offset += count;
            job->total += count;
        }
        job->deadline = QDeadlineTimer(kIdleMs);
    }
    if (job != m_out)
        return;
    m_outStatus = QStringLiteral("发送中：%1").arg(progress(job->total, job->catalog.bytes));
    emit changed();
    if (job->peer && job->peer->pendingBytes() < 256 * 1024)
        pump(job->peer);
}

bool Transfers::receiveChunk(const std::shared_ptr<Job>& job, QByteArrayView data, bool last)
{
    if (job->index >= job->catalog.entries.size() ||
        (last ? data.size() != 4 + kHashBytes :
                data.size() <= kChunkHeader || data.size() > kChunkHeader + kChunkBytes) ||
        qFromBigEndian<quint32>(data.data()) != quint32(job->index))
        return false;
    const auto& entry = job->catalog.entries[job->index];
    if (last) {
        if (job->offset != entry.size || job->hash.result() != data.sliced(4))
            return false;
    } else {
        const auto chunk = data.sliced(kChunkHeader);
        if (qFromBigEndian<quint64>(data.data() + 4) != quint64(job->offset) ||
            chunk.size() > entry.size - job->offset ||
            QCryptographicHash::hash(chunk, QCryptographicHash::Sha256) != data.sliced(12, kHashBytes))
            return false;
    }
    if (!job->file.isOpen()) {
        const auto output = job->stage->path(entry);
        if (output.isEmpty() || !safeLocalPath(QFileInfo(output).absolutePath()))
            return false;
        job->file.setFileName(output);
        if (!job->file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return false;
    }
    if (last) {
        if (!job->file.flush())
            return false;
        job->nextFile();
    } else {
        const auto chunk = data.sliced(kChunkHeader);
        if (job->file.write(chunk.data(), chunk.size()) != chunk.size())
            return false;
        job->hash.addData(chunk);
        job->offset += chunk.size();
        job->total += chunk.size();
    }
    job->deadline = QDeadlineTimer(kIdleMs);
    return true;
}

bool Transfers::receive(Peer* peer, QStringView device, Wire type, QByteArrayView data)
{
    if (data.size() < kIdentityBytes)
        return false;
    const auto id = data.first(kIdentityBytes);
    data = data.sliced(kIdentityBytes);
    if (type == Offer) {
        auto job = std::make_shared<Job>();
        job->peer = peer;
        job->id = id.toByteArray();
        job->device = device.toString();
        if (!decodeOffer(data, job->catalog))
            return false;
        if (m_in) {
            send(job, Reject);
            return true;
        }
        m_in = job;
        m_inStatus = QStringLiteral("有新文件等待确认");
        m_expiry.start();
        emit changed();
        return true;
    }
    const auto matches = [&](const std::shared_ptr<Job>& job) {
        return job && job->peer == peer && id == QByteArrayView(job->id);
    };
    if (type == Cancel && data.isEmpty()) {
        if (matches(m_in))
            finish(true, false, QStringLiteral("对方取消了传输"));
        if (matches(m_out))
            finish(false, false, QStringLiteral("对方取消了传输，可重试"));
        return true;
    }
    if (type == Accept || type == Reject || type == Received) {
        if (!data.isEmpty())
            return false;
        if (!matches(m_out))
            return true; // 本地取消后，允许忽略已在途的确认。
        if (type == Accept && m_out->state == Pending) {
            m_out->state = Active;
            m_out->skipDirs();
            m_out->deadline = QDeadlineTimer(kIdleMs);
            pump(peer);
        } else if (type == Reject && m_out->state == Pending) {
            finish(false, false, QStringLiteral("对方未接收，可重试"));
        } else if (type == Received && m_out->state == WaitingReceipt) {
            finish(false, false, QStringLiteral("发送完成"));
        } else {
            return false;
        }
        return true;
    }
    const auto job = m_in;
    if (!matches(job))
        return true;
    if (job->state != Active)
        return false;
    if (type == Chunk || type == FileDone) {
        if (!receiveChunk(job, data, type == FileDone)) {
            finish(true, true, QStringLiteral("文件校验或写入失败，已清理临时文件"));
        } else {
            m_inStatus = QStringLiteral("接收中：%1").arg(progress(job->total, job->catalog.bytes));
            emit changed();
        }
        return true;
    }
    if (type != AllDone || !data.isEmpty() || job->index != job->catalog.entries.size() ||
        job->total != job->catalog.bytes)
        return false;
    const auto result = job->stage->publish();
    if (result.isEmpty()) {
        finish(true, true, QStringLiteral("无法保存接收目录，已清理临时文件"));
        return true;
    }
    finish(true, false, QStringLiteral("接收完成：%1").arg(result));
    send(job, Received);
    return true;
}
}

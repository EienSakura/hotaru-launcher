#pragma once
#include <QByteArray>
#include <QStringList>
#include <QTemporaryDir>
#include <QVector>

namespace HotaruLan {
inline constexpr int kChunkBytes = 128 * 1024;
inline constexpr int kOfferBytes = 256 * 1024;
struct Entry {
    QString path, source;
    qint64 size = 0;
    bool directory = false;
};
struct Catalog {
    QVector<Entry> entries;
    qint64 bytes = 0;
};

bool safeLocalPath(const QString& path);
bool validRelativePath(const QString& path);
bool collectFiles(const QStringList& paths, Catalog& result, QString& error);
QByteArray encodeOffer(const Catalog& catalog);
bool decodeOffer(QByteArrayView payload, Catalog& result);

class Staging {
public:
    explicit Staging(const QString& destination);
    bool valid() const;
    bool prepare(const Catalog& catalog);
    QString path(const Entry& entry) const;
    QString publish();
private:
    QString m_destination;
    QTemporaryDir m_stage;
};
}

#pragma once
#include "LanFiles.h"
#include "LanPeer.h"
#include <QPointer>

namespace HotaruLan {
enum Wire : char {
    Hello = 1, Text = 2, Image = 3, Ping = 4, Pong = 5,
    Offer = 10, Accept, Reject, Chunk, FileDone, AllDone, Received, Cancel,
    PairHello = 20, PairKey, PairSaved, PairCommit, PairReceipt, PairReject
};

class Transfers final : public QObject {
    Q_OBJECT
public:
    explicit Transfers(QObject* parent = nullptr);
    ~Transfers() override;
    QString outgoingStatus() const { return m_outStatus; }
    QString incomingStatus() const { return m_inStatus; }
    bool outgoing() const { return bool(m_out); }
    bool incoming() const { return bool(m_in); }
    bool awaitingDecision() const;
    QString pendingSummary() const;
    QStringList retryPaths() const { return m_retryPaths; }
    void sendOffer(Peer* peer, const QStringList& paths);
    bool accept(const QString& destination, QString* error = nullptr);
    void decline();
    void cancelOutgoing();
    void cancelIncoming();
    void disconnected(Peer* peer);
    void pump(Peer* peer);
    bool receive(Peer* peer, QStringView device, Wire type, QByteArrayView data);

signals:
    void changed();

private:
    struct Job;
    enum State { Pending, Active, WaitingReceipt };
    void finish(bool incoming, bool notify, QString note);
    void pumpNow();
    bool receiveChunk(const std::shared_ptr<Job>& job, QByteArrayView data, bool last);
    static bool send(const std::shared_ptr<Job>& job, Wire type, QByteArrayView data = {});
    static QString progress(qint64 processed, qint64 total);
    std::shared_ptr<Job> m_out, m_in;
    QTimer m_expiry, m_pump;
    QString m_outStatus, m_inStatus;
    QStringList m_retryPaths;
};
}

#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <memory>

namespace HotaruLan {

inline constexpr int kIdentityBytes = 16;
inline constexpr int kShareBytes = 32;
inline constexpr int kProofBytes = 32;
inline constexpr int kTagBytes = 16;
inline constexpr int kMaxPlaintext = 4 * 1024 * 1024 + 64;
inline constexpr char kWireVersion[] = "HL02";

// 一次握手只创建一个对象；仅对端确认通过后开放加密记录接口。
class SecureChannel {
public:
    SecureChannel(bool initiator, QByteArrayView ownId, QByteArrayView peerId,
                  QByteArrayView passcode);
    ~SecureChannel();
    SecureChannel(const SecureChannel&) = delete;
    SecureChannel& operator=(const SecureChannel&) = delete;

    QByteArray share() const;
    bool derive(QByteArrayView peerShare);
    QByteArray proof() const;
    bool confirm(QByteArrayView peerProof);
    QByteArray seal(QByteArrayView plaintext);
    QByteArray open(QByteArrayView ciphertext);

private:
    struct State;
    std::unique_ptr<State> state;
};

QByteArray randomBytes(int size);

}

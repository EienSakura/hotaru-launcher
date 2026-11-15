#include "LanCrypto.h"

#include <QtEndian>
#include <openssl/aead.h>
#include <openssl/crypto.h>
#include <openssl/curve25519.h>
#include <openssl/digest.h>
#include <openssl/hkdf.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <array>
#include <cstring>
#include <limits>

namespace HotaruLan {
namespace {
using Key = std::array<uint8_t, 32>;
class AeadContext {
public:
    EVP_AEAD_CTX value{};
    ~AeadContext() { EVP_AEAD_CTX_cleanup(&value); }
};
const uint8_t* bytes(QByteArrayView input)
{
    return reinterpret_cast<const uint8_t*>(input.data());
}

std::array<uint8_t, 12> nonce(quint64 sequence)
{
    std::array<uint8_t, 12> result{};
    qToBigEndian(sequence, result.data() + 4);
    return result;
}

QByteArray aad(quint64 sequence, quint32 length)
{
    QByteArray data(kWireVersion);
    char fields[12];
    qToBigEndian(sequence, fields);
    qToBigEndian(length, fields + 8);
    return data.append(fields, sizeof(fields));
}

QByteArray makeProof(const Key& key)
{
    QByteArray mac(kProofBytes, Qt::Uninitialized);
    unsigned int size = 0;
    static constexpr auto label = "Hotaru LAN HL02 key confirmation";
    if (!HMAC(EVP_sha256(), key.data(), key.size(), reinterpret_cast<const uint8_t*>(label),
              std::strlen(label),
              reinterpret_cast<uint8_t*>(mac.data()), &size) || size != kProofBytes)
        return {};
    return mac;
}
}

struct SecureChannel::State {
    explicit State(bool initiator) : initiator(initiator) {}
    ~State()
    {
        for (auto* key : {&txKey, &rxKey, &txConfirmation, &rxConfirmation})
            OPENSSL_cleanse(key->data(), key->size());
    }
    const bool initiator;
    std::unique_ptr<SPAKE2_CTX, decltype(&SPAKE2_CTX_free)> exchange{nullptr, &SPAKE2_CTX_free};
    QByteArray localShare;
    Key txKey{}, rxKey{}, txConfirmation{}, rxConfirmation{};
    quint64 txSequence = 0, rxSequence = 0;
    bool derived = false, authenticated = false, failed = false;
};

SecureChannel::SecureChannel(bool initiator, QByteArrayView ownId, QByteArrayView peerId,
                             QByteArrayView passcode)
    : state(std::make_unique<State>(initiator))
{
    if (ownId.size() != kIdentityBytes || peerId.size() != kIdentityBytes ||
        ownId == peerId || passcode.isEmpty() || passcode.size() > 128)
        return;
    const QByteArray local = QByteArray(kWireVersion) + ownId.toByteArray();
    const QByteArray remote = QByteArray(kWireVersion) + peerId.toByteArray();
    state->exchange.reset(SPAKE2_CTX_new(initiator ? spake2_role_alice : spake2_role_bob,
                                       bytes(local), local.size(), bytes(remote), remote.size()));
    if (!state->exchange)
        return;
    QByteArray share(kShareBytes, Qt::Uninitialized);
    size_t size = 0;
    if (SPAKE2_generate_msg(state->exchange.get(), reinterpret_cast<uint8_t*>(share.data()), &size,
                           share.size(), bytes(passcode), passcode.size()) && size == kShareBytes)
        state->localShare = std::move(share);
}

SecureChannel::~SecureChannel() = default;
QByteArray SecureChannel::share() const { return state->localShare; }

bool SecureChannel::derive(QByteArrayView peerShare)
{
    if (state->localShare.isEmpty() || !state->exchange || peerShare.size() != kShareBytes)
        return false;
    std::array<uint8_t, SPAKE2_MAX_KEY_SIZE> material{};
    size_t size = 0;
    const bool ok = SPAKE2_process_msg(state->exchange.get(), material.data(), &size,
                                      material.size(), bytes(peerShare), peerShare.size());
    state->exchange.reset();
    const auto deriveKey = [&](Key& key, QByteArrayView label) {
        static constexpr auto salt = "Hotaru LAN HL02 SPAKE2";
        return HKDF(key.data(), key.size(), EVP_sha256(), material.data(), size, bytes(salt),
                    std::strlen(salt), bytes(label), label.size()) == 1;
    };
    const bool a = state->initiator;
    state->derived = ok && size == material.size() &&
                     deriveKey(state->txKey, a ? "record/alice" : "record/bob") &&
                     deriveKey(state->rxKey, a ? "record/bob" : "record/alice") &&
                     deriveKey(state->txConfirmation, a ? "confirm/alice" : "confirm/bob") &&
                     deriveKey(state->rxConfirmation, a ? "confirm/bob" : "confirm/alice");
    OPENSSL_cleanse(material.data(), material.size());
    return state->derived;
}

QByteArray SecureChannel::proof() const
{
    return state->derived && !state->failed ? makeProof(state->txConfirmation) : QByteArray{};
}

bool SecureChannel::confirm(QByteArrayView peerProof)
{
    if (state->authenticated || state->failed)
        return false;
    const auto expected = state->derived ? makeProof(state->rxConfirmation) : QByteArray{};
    state->authenticated = expected.size() == kProofBytes && peerProof.size() == kProofBytes &&
                           CRYPTO_memcmp(expected.constData(), peerProof.data(), kProofBytes) == 0;
    state->failed = !state->authenticated;
    return state->authenticated;
}

QByteArray SecureChannel::seal(QByteArrayView plaintext)
{
    if (!state->authenticated || state->failed || plaintext.isEmpty() ||
        plaintext.size() > kMaxPlaintext || state->txSequence == std::numeric_limits<quint64>::max())
        return {};
    const auto iv = nonce(state->txSequence);
    const auto ad = aad(state->txSequence, quint32(plaintext.size()));
    QByteArray result(plaintext.size() + kTagBytes, Qt::Uninitialized);
    size_t size = 0;
    AeadContext context;
    if (!EVP_AEAD_CTX_init(&context.value, EVP_aead_aes_256_gcm(), state->txKey.data(),
                           state->txKey.size(), kTagBytes, nullptr) ||
        !EVP_AEAD_CTX_seal(&context.value, reinterpret_cast<uint8_t*>(result.data()), &size,
                           result.size(), iv.data(), iv.size(), bytes(plaintext), plaintext.size(),
                           bytes(ad), ad.size()) || size != size_t(result.size())) {
        state->failed = true;
        return {};
    }
    ++state->txSequence;
    return result;
}

QByteArray SecureChannel::open(QByteArrayView ciphertext)
{
    if (!state->authenticated || state->failed || ciphertext.size() <= kTagBytes ||
        ciphertext.size() > kMaxPlaintext + kTagBytes ||
        state->rxSequence == std::numeric_limits<quint64>::max())
        return {};
    const auto iv = nonce(state->rxSequence);
    QByteArray result(ciphertext.size() - kTagBytes, Qt::Uninitialized);
    const auto ad = aad(state->rxSequence, quint32(result.size()));
    size_t size = 0;
    AeadContext context;
    if (!EVP_AEAD_CTX_init(&context.value, EVP_aead_aes_256_gcm(), state->rxKey.data(),
                           state->rxKey.size(), kTagBytes, nullptr) ||
        !EVP_AEAD_CTX_open(&context.value, reinterpret_cast<uint8_t*>(result.data()), &size,
                           result.size(), iv.data(), iv.size(), bytes(ciphertext), ciphertext.size(),
                           bytes(ad), ad.size()) || size != size_t(result.size())) {
        state->failed = true;
        return {};
    }
    ++state->rxSequence;
    return result;
}

QByteArray randomBytes(int size)
{
    if (size <= 0 || size > 1024)
        return {};
    QByteArray result(size, Qt::Uninitialized);
    if (!RAND_bytes(reinterpret_cast<uint8_t*>(result.data()), result.size()))
        return {};
    return result;
}
}

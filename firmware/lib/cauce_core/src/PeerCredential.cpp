#include "cauce/core/PeerCredential.h"

#include <cstring>

#include "cauce/core/Ed25519Points.h"

namespace cauce {

namespace {

constexpr char kDomain[] = "cauce-peer-v1";
constexpr size_t kDomainLength = sizeof(kDomain) - 1;
constexpr size_t kBodyBytes = 56;

}  // namespace

PeerCredentialStatus verifyPeerCredential(const uint8_t raw[kPeerCredentialBytes],
                                          const uint8_t caPublicKey[32], uint64_t nowUtcMs,
                                          PeerCredential& out) {
  out = PeerCredential{};
  if (raw == nullptr || caPublicKey == nullptr) return PeerCredentialStatus::BadFormat;

  // The id must be terminated inside its 16 bytes and not empty; anything else is a key that
  // would read past itself everywhere it is used.
  if (raw[0] == 0 || std::memchr(raw, 0, 16) == nullptr) return PeerCredentialStatus::BadFormat;

  uint8_t message[kDomainLength + kBodyBytes];
  std::memcpy(message, kDomain, kDomainLength);
  std::memcpy(message + kDomainLength, raw, kBodyBytes);
  if (!ed25519Verify(caPublicKey, message, sizeof(message), raw + kBodyBytes)) {
    return PeerCredentialStatus::BadSignature;
  }

  std::memcpy(out.nodeId, raw, 16);
  std::memcpy(out.publicKey, raw + 16, 32);
  uint64_t notAfter = 0;
  for (int i = 7; i >= 0; --i) notAfter = (notAfter << 8) | raw[48 + i];
  out.notAfterUtcMs = notAfter;
  if (nowUtcMs != 0 && nowUtcMs > notAfter) return PeerCredentialStatus::Expired;
  return PeerCredentialStatus::Ok;
}

}  // namespace cauce

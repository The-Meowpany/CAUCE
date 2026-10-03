#pragma once

// SHA-512 (FIPS 180-4).
//
// Present because Ed25519 needs it: the hash stretches a 32-byte seed into a
// signing scalar and derives the per-message nonce, and SHA-256 cannot do that
// because Ed25519 is defined over a 512-bit internal state. There is no way to
// substitute a narrower hash without changing the signatures.
//
// Streaming, so a 115-byte LoRa frame or an OTA manifest can be hashed without
// being buffered whole.

#include <cstddef>
#include <cstdint>

namespace cauce {

constexpr size_t kSha512DigestBytes = 64;
constexpr size_t kSha512BlockBytes = 128;

struct Sha512Ctx {
  uint64_t state[8];
  uint64_t bitLengthLow;
  uint64_t bitLengthHigh;
  uint8_t buffer[kSha512BlockBytes];
  size_t buffered;
};

void sha512Begin(Sha512Ctx* ctx);
void sha512Append(Sha512Ctx* ctx, const uint8_t* data, size_t length);
void sha512Finish(Sha512Ctx* ctx, uint8_t outDigest[kSha512DigestBytes]);

// One-shot convenience wrapper.
void sha512(const uint8_t* data, size_t length,
            uint8_t outDigest[kSha512DigestBytes]);

}  // namespace cauce
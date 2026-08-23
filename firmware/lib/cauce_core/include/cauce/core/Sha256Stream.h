#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce {

struct Sha256Ctx {
  uint32_t state[8];
  uint64_t totalBits;
  uint8_t buffer[64];
  size_t used;
};

void sha256Begin(Sha256Ctx* ctx);
void sha256Append(Sha256Ctx* ctx, const uint8_t* data, size_t length);
void sha256Finish(Sha256Ctx* ctx, uint8_t outDigest[32]);

}  // namespace cauce

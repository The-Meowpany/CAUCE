#include "cauce/core/SecurityUtils.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "cauce/core/Sha256Stream.h"

namespace cauce {
namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32 - n));
}

void processBlock(const uint8_t block[64], uint32_t state[8]) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
           (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = h + s1 + ch + kK[i] + w[i];
    const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state[0] += a; state[1] += b; state[2] += c; state[3] += d;
  state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

}  // namespace

void sha256Begin(Sha256Ctx* ctx) {
  ctx->state[0] = 0x6a09e667;
  ctx->state[1] = 0xbb67ae85;
  ctx->state[2] = 0x3c6ef372;
  ctx->state[3] = 0xa54ff53a;
  ctx->state[4] = 0x510e527f;
  ctx->state[5] = 0x9b05688c;
  ctx->state[6] = 0x1f83d9ab;
  ctx->state[7] = 0x5be0cd19;
  ctx->totalBits = 0;
  ctx->used = 0;
}

void sha256Append(Sha256Ctx* ctx, const uint8_t* data, size_t length) {
  ctx->totalBits += static_cast<uint64_t>(length) * 8u;
  while (length > 0) {
    const size_t space = 64 - ctx->used;
    const size_t take = length < space ? length : space;
    std::memcpy(ctx->buffer + ctx->used, data, take);
    ctx->used += take;
    data += take;
    length -= take;
    if (ctx->used == 64) {
      processBlock(ctx->buffer, ctx->state);
      ctx->used = 0;
    }
  }
}

void sha256Finish(Sha256Ctx* ctx, uint8_t outDigest[32]) {
  const uint64_t bitLen = ctx->totalBits;
  size_t tail = ctx->used;
  ctx->buffer[tail++] = 0x80;
  if (tail > 56) {
    std::memset(ctx->buffer + tail, 0, 64 - tail);
    processBlock(ctx->buffer, ctx->state);
    tail = 0;
    std::memset(ctx->buffer, 0, 56);
  } else {
    std::memset(ctx->buffer + tail, 0, 56 - tail);
  }
  for (int i = 0; i < 8; ++i) {
    ctx->buffer[56 + i] = static_cast<uint8_t>((bitLen >> (56 - 8 * i)) & 0xFF);
  }
  processBlock(ctx->buffer, ctx->state);

  for (int i = 0; i < 8; ++i) {
    outDigest[i * 4] = static_cast<uint8_t>((ctx->state[i] >> 24) & 0xFF);
    outDigest[i * 4 + 1] = static_cast<uint8_t>((ctx->state[i] >> 16) & 0xFF);
    outDigest[i * 4 + 2] = static_cast<uint8_t>((ctx->state[i] >> 8) & 0xFF);
    outDigest[i * 4 + 3] = static_cast<uint8_t>(ctx->state[i] & 0xFF);
  }
}

void sha256(const uint8_t* data, size_t length, uint8_t outDigest[32]) {
  Sha256Ctx ctx;
  sha256Begin(&ctx);
  sha256Append(&ctx, data, length);
  sha256Finish(&ctx, outDigest);
}

void sha256Hex(const char* input, char outHex[65]) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(input);
  const size_t len = input ? std::strlen(input) : 0;
  uint8_t digest[32];
  sha256(bytes, len, digest);
  static const char kHexDigits[] = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) {
    outHex[i * 2] = kHexDigits[(digest[i] >> 4) & 0xF];
    outHex[i * 2 + 1] = kHexDigits[digest[i] & 0xF];
  }
  outHex[64] = '\0';
}

bool secureEquals(const char* a, const char* b) {
  volatile uint8_t diff = 0;
  size_t i = 0;
  for (; ; ++i) {
    const uint8_t ca = static_cast<uint8_t>(a ? a[i] : 0);
    const uint8_t cb = static_cast<uint8_t>(b ? b[i] : 0);
    diff |= ca ^ cb;
    if (ca == '\0' || cb == '\0') break;
  }
  return diff == 0;
}

}  // namespace cauce

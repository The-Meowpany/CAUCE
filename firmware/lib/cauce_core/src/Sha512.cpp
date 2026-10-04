#include "cauce/core/Sha512.h"

#include <cstring>

namespace cauce {
namespace {

// FIPS 180-4 section 4.2.3 constants. Round constants are the first 64 bits of
// the fractional parts of the cube roots of the first 80 primes.
constexpr uint64_t kK[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

// Section 4.2.2: first 64 bits of the fractional parts of the square roots of
// the first eight primes.
constexpr uint64_t kInitialState[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
    0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};

inline uint64_t rotr(uint64_t x, unsigned n) {
  return (x >> n) | (x << (64 - n));
}

inline uint64_t loadBe64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v << 8) | p[i];
  }
  return v;
}

inline void storeBe64(uint8_t* p, uint64_t v) {
  for (int i = 7; i >= 0; --i) {
    p[i] = static_cast<uint8_t>(v & 0xFF);
    v >>= 8;
  }
}

void compress(uint64_t state[8], const uint8_t block[kSha512BlockBytes]) {
  uint64_t w[80];
  for (int i = 0; i < 16; ++i) {
    w[i] = loadBe64(block + i * 8);
  }
  for (int i = 16; i < 80; ++i) {
    const uint64_t s0 = rotr(w[i - 15], 1) ^ rotr(w[i - 15], 8) ^
                        (w[i - 15] >> 7);
    const uint64_t s1 = rotr(w[i - 2], 19) ^ rotr(w[i - 2], 61) ^
                        (w[i - 2] >> 6);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint64_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint64_t e = state[4], f = state[5], g = state[6], h = state[7];

  for (int i = 0; i < 80; ++i) {
    const uint64_t S1 = rotr(e, 14) ^ rotr(e, 18) ^ rotr(e, 41);
    const uint64_t ch = (e & f) ^ ((~e) & g);
    const uint64_t temp1 = h + S1 + ch + kK[i] + w[i];
    const uint64_t S0 = rotr(a, 28) ^ rotr(a, 34) ^ rotr(a, 39);
    const uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint64_t temp2 = S0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

}  // namespace

void sha512Begin(Sha512Ctx* ctx) {
  std::memcpy(ctx->state, kInitialState, sizeof(kInitialState));
  ctx->bitLengthLow = 0;
  ctx->bitLengthHigh = 0;
  ctx->buffered = 0;
}

void sha512Append(Sha512Ctx* ctx, const uint8_t* data, size_t length) {
  if (!data || length == 0) return;

  // Bits, as a 128-bit big-endian counter. The high half exists because the
  // length in bits cannot overflow 64 bits for any input this device can hold,
  // but the field is part of the standard and a signature covers the padding.
  const uint64_t addedBits = static_cast<uint64_t>(length) * 8;
  const uint64_t previousLow = ctx->bitLengthLow;
  ctx->bitLengthLow += addedBits;
  if (ctx->bitLengthLow < previousLow) ++ctx->bitLengthHigh;

  size_t offset = 0;
  if (ctx->buffered > 0) {
    const size_t want = kSha512BlockBytes - ctx->buffered;
    const size_t take = length < want ? length : want;
    std::memcpy(ctx->buffer + ctx->buffered, data, take);
    ctx->buffered += take;
    offset = take;
    if (ctx->buffered == kSha512BlockBytes) {
      compress(ctx->state, ctx->buffer);
      ctx->buffered = 0;
    }
  }

  while (offset + kSha512BlockBytes <= length) {
    compress(ctx->state, data + offset);
    offset += kSha512BlockBytes;
  }

  if (offset < length) {
    std::memcpy(ctx->buffer, data + offset, length - offset);
    ctx->buffered = length - offset;
  }
}

void sha512Finish(Sha512Ctx* ctx, uint8_t outDigest[kSha512DigestBytes]) {
  const uint64_t low = ctx->bitLengthLow;
  const uint64_t high = ctx->bitLengthHigh;

  uint8_t pad = 0x80;
  sha512Append(ctx, &pad, 1);
  // sha512Append just advanced the length, which is already captured above.
  pad = 0x00;
  while (ctx->buffered != kSha512BlockBytes - 16) {
    sha512Append(ctx, &pad, 1);
  }

  uint8_t lengthField[16];
  storeBe64(lengthField, high);
  storeBe64(lengthField + 8, low);
  std::memcpy(ctx->buffer + ctx->buffered, lengthField, 16);
  compress(ctx->state, ctx->buffer);
  ctx->buffered = 0;

  for (int i = 0; i < 8; ++i) {
    storeBe64(outDigest + i * 8, ctx->state[i]);
  }
}

void sha512(const uint8_t* data, size_t length,
            uint8_t outDigest[kSha512DigestBytes]) {
  Sha512Ctx ctx;
  sha512Begin(&ctx);
  sha512Append(&ctx, data, length);
  sha512Finish(&ctx, outDigest);
}

}  // namespace cauce

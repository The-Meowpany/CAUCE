#include "cauce/core/Ed25519Points.h"
#include "cauce/core/Sha512.h"

#include <cstring>

namespace cauce {
namespace {

using Gf = int64_t[16];

constexpr int64_t kMask = 0xFFFF;

// p = 2^255 - 19 in radix 2^16: 0xFFED in limb 0, 0xFFFF in limbs 1-14, and
// 0x7FFF in limb 15, because borrowing 2^16 out of limb 15 leaves 0x7FFF there
// and turns the subtraction of 19 into 0x10000 - 19 = 0xFFED.
constexpr Gf kPrime = {0xFFED, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
                       0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
                       0xFFFF, 0xFFFF, 0xFFFF, 0x7FFF};

void copyGf(Gf out, const Gf in) {
  for (int i = 0; i < 16; ++i) out[i] = in[i];
}

void setZero(Gf out) {
  for (int i = 0; i < 16; ++i) out[i] = 0;
}

void setOne(Gf out) {
  setZero(out);
  out[0] = 1;
}

// Puts every limb in [0, 2^16) and folds the overflow out of the top.
//
// Replaces the usual carry pass for the reason in the header: that pass leaves
// limb 0 large, and limb magnitudes compound quadratically through a multiply
// chain until int64 overflows. Normalising here keeps every limb a genuine radix
// digit, so products stay bounded by 2^32 and nothing accumulates.
//
// The shift is an arithmetic shift, so it floors, which is what makes this
// correct for a limb that earlier arithmetic left negative.
void normalizeLimbs(Gf t) {
  for (int i = 0; i < 15; ++i) {
    const int64_t carry = t[i] >> 16;
    t[i] -= carry << 16;
    t[i + 1] += carry;
  }
  const int64_t top = t[15] >> 16;
  t[15] -= top << 16;
  t[0] += 38 * top;  // 2^256 = 38 mod p

  // The fold landed on limb 0 again. Every carry from here is non-negative, so
  // this terminates.
  for (int i = 0; i < 15; ++i) {
    const int64_t carry = t[i] >> 16;
    if (carry == 0) break;
    t[i] -= carry << 16;
    t[i + 1] += carry;
  }
}

// No carry propagation on purpose: a limb holding 0x10000 still denotes the right
// number, and skipping it keeps these to one operation per limb.
void addGf(Gf out, const Gf a, const Gf b) {
  for (int i = 0; i < 16; ++i) out[i] = a[i] + b[i];
}

void subGf(Gf out, const Gf a, const Gf b) {
  for (int i = 0; i < 16; ++i) out[i] = a[i] - b[i];
}

void mulGf(Gf out, const Gf a, const Gf b) {
  int64_t t[31];
  for (int i = 0; i < 31; ++i) t[i] = 0;
  for (int i = 0; i < 16; ++i) {
    for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
  }
  for (int i = 16; i < 31; ++i) t[i - 16] += 38 * t[i];
  for (int i = 0; i < 16; ++i) out[i] = t[i];
  normalizeLimbs(out);
}

void squareGf(Gf out, const Gf a) { mulGf(out, a, a); }

// Canonical 32-byte little-endian form. Two explicit phases: normalise the limbs,
// then subtract p while the value is at or above it. Only after phase 1 are the
// limbs genuine radix digits, and only then does a comparison against p mean
// anything - an earlier version compared limb-wise and was off by one byte for
// inputs whose limbs sat near 0xFFFF.
void gfToBytes(uint8_t out[32], const Gf in) {
  Gf t;
  copyGf(t, in);
  normalizeLimbs(t);

  for (int round = 0; round < 2; ++round) {
    int atLeastPrime = 1;
    for (int i = 15; i >= 0; --i) {
      if (t[i] != kPrime[i]) {
        atLeastPrime = t[i] > kPrime[i];
        break;
      }
    }
    if (!atLeastPrime) break;
    int64_t borrow = 0;
    for (int i = 0; i < 16; ++i) {
      const int64_t d = t[i] - kPrime[i] - borrow;
      borrow = (d >> 63) & 1;
      t[i] = d & kMask;
    }
  }

  for (int i = 0; i < 16; ++i) {
    out[2 * i] = static_cast<uint8_t>(t[i] & 0xFF);
    out[2 * i + 1] = static_cast<uint8_t>((t[i] >> 8) & 0xFF);
  }
}

// Refuses an encoding that is not the canonical form of the value it denotes. A
// non-canonical y would give one point two encodings, which is malleability.
bool gfFromBytes(Gf out, const uint8_t in[32]) {
  for (int i = 0; i < 16; ++i) {
    out[i] = static_cast<int64_t>(in[2 * i]) |
             (static_cast<int64_t>(in[2 * i + 1]) << 8);
  }
  uint8_t roundTrip[32];
  gfToBytes(roundTrip, out);
  return std::memcmp(roundTrip, in, 32) == 0;
}

void loadGf(Gf out, const uint8_t in[32]) {
  for (int i = 0; i < 16; ++i) {
    out[i] = static_cast<int64_t>(in[2 * i]) |
             (static_cast<int64_t>(in[2 * i + 1]) << 8);
  }
}

// Square-and-multiply by an exponent given as a field element. The exponent is
// canonicalised first so no caller has to know whether the value it passed still
// had negative limbs - testing bit 1 of -5 answers 1, while the canonical
// 0xFFFD answers 0.
void powByExponent(Gf out, const Gf base, const Gf exponent) {
  uint8_t bytes[32];
  gfToBytes(bytes, exponent);
  Gf bits;
  for (int i = 0; i < 16; ++i) {
    bits[i] = static_cast<int64_t>(bytes[2 * i]) |
              (static_cast<int64_t>(bytes[2 * i + 1]) << 8);
  }

  Gf result;
  setOne(result);
  for (int bit = 255; bit >= 0; --bit) {
    squareGf(result, result);
    if ((bits[bit >> 4] >> (bit & 15)) & 1) mulGf(result, result, base);
  }
  copyGf(out, result);
}

// out = in^(p-2), the multiplicative inverse.
void invertGf(Gf out, const Gf in) {
  Gf exponent;
  copyGf(exponent, kPrime);
  exponent[0] -= 2;
  powByExponent(out, in, exponent);
}

// out = in^((p-5)/8), the exponent that takes square roots.
// (p-5)/8 = (2^255 - 24)/8 = 2^252 - 3, which in radix 2^16 is bit 12 of limb 15
// minus 3.
void powP58(Gf out, const Gf in) {
  Gf exponent;
  setZero(exponent);
  exponent[15] = 0x1000;
  exponent[0] -= 3;
  powByExponent(out, in, exponent);
}

// d = -121665/121666, computed once rather than transcribed as a limb table.
const Gf& curveD() {
  static Gf d;
  static bool ready = false;
  if (!ready) {
    Gf numerator, denominator, inverse, product, negated, zero;
    setZero(numerator);
    numerator[0] = 121665 & kMask;
    numerator[1] = 121665 >> 16;
    setZero(denominator);
    denominator[0] = 121666 & kMask;
    denominator[1] = 121666 >> 16;
    invertGf(inverse, denominator);
    mulGf(product, numerator, inverse);
    setZero(zero);
    subGf(negated, zero, product);
    normalizeLimbs(negated);
    copyGf(d, negated);
    ready = true;
  }
  return d;
}

// sqrt(-1) = 2^((p-1)/4) = 2^(2^253 - 5).
const Gf& sqrtMinusOne() {
  static Gf s;
  static bool ready = false;
  if (!ready) {
    Gf exponent, two;
    setZero(exponent);
    exponent[15] = 0x2000;
    exponent[0] -= 5;
    setZero(two);
    two[0] = 2;
    powByExponent(s, two, exponent);
    ready = true;
  }
  return s;
}

bool gfEqual(const Gf a, const Gf b) {
  uint8_t ea[32], eb[32];
  gfToBytes(ea, a);
  gfToBytes(eb, b);
  return std::memcmp(ea, eb, 32) == 0;
}

bool onCurve(const Gf x, const Gf y) {
  Gf x2, y2, dxy2, left, right, one;
  setOne(one);
  squareGf(x2, x);
  squareGf(y2, y);
  mulGf(dxy2, x2, curveD());
  mulGf(dxy2, dxy2, y2);
  subGf(left, y2, x2);
  addGf(right, dxy2, one);
  return gfEqual(left, right);
}

}  // namespace

bool ed25519DecodePoint(const uint8_t encoded[kEd25519PublicKeyBytes],
                        uint8_t outX[32], uint8_t outY[32]) {
  if (!encoded || !outX || !outY) return false;

  const int sign = (encoded[31] >> 7) & 1;
  uint8_t yBytes[32];
  std::memcpy(yBytes, encoded, 32);
  yBytes[31] = static_cast<uint8_t>(yBytes[31] & 0x7F);

  Gf y;
  if (!gfFromBytes(y, yBytes)) return false;  // non-canonical y

  // u = y^2 - 1, v = d*y^2 + 1.
  Gf y2, u, v, one;
  setOne(one);
  squareGf(y2, y);
  subGf(u, y2, one);
  mulGf(v, y2, curveD());
  addGf(v, v, one);

  // x = u * v^3 * (u * v^7)^((p-5)/8)
  Gf v2, v3, v7, root, x;
  squareGf(v2, v);
  mulGf(v3, v2, v);
  squareGf(v2, v3);
  mulGf(v7, v2, v);
  mulGf(root, u, v7);
  powP58(root, root);
  mulGf(x, u, v3);
  mulGf(x, x, root);

  // The candidate is a square root half the time; multiplying by sqrt(-1) picks
  // the other. If neither works then this y is not on the curve at all.
  Gf check;
  squareGf(check, x);
  mulGf(check, check, v);
  if (!gfEqual(check, u)) {
    mulGf(x, x, sqrtMinusOne());
    squareGf(check, x);
    mulGf(check, check, v);
    if (!gfEqual(check, u)) return false;
  }

  // Match the requested parity of x. Bit 0 of limb 0 is the parity of the value,
  // since every limb is congruent to its contribution mod 2^16.
  uint8_t xBytes[32];
  gfToBytes(xBytes, x);
  if ((xBytes[0] & 1) != sign) {
    Gf negated, zero;
    setZero(zero);
    subGf(negated, zero, x);
    normalizeLimbs(negated);
    copyGf(x, negated);
  }

  gfToBytes(outX, x);
  gfToBytes(outY, y);
  return true;
}

void ed25519EncodePoint(uint8_t out[kEd25519PublicKeyBytes],
                        const uint8_t x[32], const uint8_t y[32]) {
  if (!out || !x || !y) return;
  std::memcpy(out, y, 32);
  // Bit 0 of the first byte is the parity of x. Taken from the canonical bytes
  // rather than from a limb, so it cannot be read off a lazy representation.
  out[31] = static_cast<uint8_t>((out[31] & 0x7F) | ((x[0] & 1) << 7));
}

bool ed25519IsOnCurve(const uint8_t x[32], const uint8_t y[32]) {
  if (!x || !y) return false;
  Gf fx, fy;
  loadGf(fx, x);
  loadGf(fy, y);
  return onCurve(fx, fy);
}

// --- the group law, exposed so it can be tested on its own ----------------

struct Point {
  Gf x;
  Gf y;
  Gf z;
  Gf t;
};

void identityPoint(Point& p) {
  setZero(p.x);
  setOne(p.y);
  setOne(p.z);
  setZero(p.t);
}

// Extended twisted Edwards coordinates with a = -1: x = X/Z, y = Y/Z, xy = T/Z.
// The same formula serves addition and doubling; `C` carries the factor 2d and
// `D` the factor 2 because that is what keeps a doubling to eight multiplies.
void addPoints(Point& out, const Point& p, const Point& q) {
  Gf a, b, c, d, e, f, g, h, u, v;
  subGf(u, p.y, p.x);
  subGf(v, q.y, q.x);
  mulGf(a, u, v);

  addGf(u, p.y, p.x);
  addGf(v, q.y, q.x);
  mulGf(b, u, v);

  mulGf(c, p.t, q.t);
  mulGf(c, c, curveD());
  addGf(c, c, c);  // C = 2*d*T1*T2

  mulGf(d, p.z, q.z);
  addGf(d, d, d);  // D = 2*Z1*Z2

  subGf(e, b, a);
  subGf(f, d, c);
  addGf(g, d, c);
  addGf(h, b, a);

  mulGf(out.x, e, f);
  mulGf(out.y, g, h);
  mulGf(out.t, e, h);
  mulGf(out.z, f, g);
}

void pointFromAffine(Point& p, const uint8_t x[32], const uint8_t y[32]) {
  loadGf(p.x, x);
  loadGf(p.y, y);
  normalizeLimbs(p.x);
  normalizeLimbs(p.y);
  setOne(p.z);
  mulGf(p.t, p.x, p.y);
}

// Converts back to affine, canonical bytes out.
void affineFromPoint(uint8_t outX[32], uint8_t outY[32], const Point& p) {
  Gf inverseZ, x, y;
  invertGf(inverseZ, p.z);
  mulGf(x, p.x, inverseZ);
  mulGf(y, p.y, inverseZ);
  gfToBytes(outX, x);
  gfToBytes(outY, y);
}

void ed25519EncodeAffine(uint8_t out[32], const uint8_t x[32],
                         const uint8_t y[32]) {
  if (!out || !x || !y) return;
  ed25519EncodePoint(out, x, y);
}

bool ed25519AddPointsTrace(uint8_t outX[32], uint8_t outY[32],
                            const uint8_t ax[32], const uint8_t ay[32],
                            const uint8_t bx[32], const uint8_t by[32],
                            uint8_t traceA[32], uint8_t traceB[32],
                            uint8_t traceC[32], uint8_t traceD[32]) {
  if (!outX || !outY || !ax || !ay || !bx || !by) return false;
  Point pa, pb, sum;
  pointFromAffine(pa, ax, ay);
  pointFromAffine(pb, bx, by);

  Gf a, b, c, d, u, v;
  subGf(u, pa.y, pa.x);
  subGf(v, pb.y, pb.x);
  mulGf(a, u, v);

  addGf(u, pa.y, pa.x);
  addGf(v, pb.y, pb.x);
  mulGf(b, u, v);

  mulGf(c, pa.t, pb.t);
  mulGf(c, c, curveD());
  addGf(c, c, c);

  mulGf(d, pa.z, pb.z);
  addGf(d, d, d);

  if (traceA) gfToBytes(traceA, a);
  if (traceB) gfToBytes(traceB, b);
  if (traceC) gfToBytes(traceC, c);
  if (traceD) gfToBytes(traceD, d);

  Gf e, f, g, h;
  subGf(e, b, a);
  subGf(f, d, c);
  addGf(g, d, c);
  addGf(h, b, a);

  mulGf(sum.x, e, f);
  mulGf(sum.y, g, h);
  mulGf(sum.t, e, h);
  mulGf(sum.z, f, g);

  affineFromPoint(outX, outY, sum);
  return true;
}

bool ed25519AddPoints(uint8_t outX[32], uint8_t outY[32],
                      const uint8_t ax[32], const uint8_t ay[32],
                      const uint8_t bx[32], const uint8_t by[32]) {
  return ed25519AddPointsTrace(outX, outY, ax, ay, bx, by, nullptr, nullptr,
                               nullptr, nullptr);
}

// Builds a projective point from raw projective coordinates. T is not an input
// because a caller holding (X, Y, Z) almost never holds T, and recomputing it is
// one inversion: T = X*Y/Z.
void pointFromProjective(Point& p, const uint8_t x[32], const uint8_t y[32],
                         const uint8_t z[32]) {
  Gf inverseZ;
  loadGf(p.x, x);
  loadGf(p.y, y);
  loadGf(p.z, z);
  normalizeLimbs(p.x);
  normalizeLimbs(p.y);
  normalizeLimbs(p.z);
  invertGf(inverseZ, p.z);
  mulGf(p.t, p.x, p.y);
  mulGf(p.t, p.t, inverseZ);
}

bool ed25519AddPointsZ(uint8_t outX[32], uint8_t outY[32],
                       const uint8_t aX[32], const uint8_t aY[32],
                       const uint8_t aZ[32], const uint8_t bX[32],
                       const uint8_t bY[32], const uint8_t bZ[32]) {
  if (!outX || !outY || !aX || !aY || !aZ || !bX || !bY || !bZ) return false;
  Point pa, pb, sum;
  pointFromProjective(pa, aX, aY, aZ);
  pointFromProjective(pb, bX, bY, bZ);
  addPoints(sum, pa, pb);
  affineFromPoint(outX, outY, sum);
  return true;
}

bool ed25519BasePoint(uint8_t outX[32], uint8_t outY[32]) {
  if (!outX || !outY) return false;
  // y = 4/5, encoded with the sign bit clear, which is a complete encoding of
  // the base point. Deriving it means the base point goes through the same
  // decode path as anything a peer sends.
  Gf five, inverse, four, y;
  setZero(five);
  five[0] = 5;
  invertGf(inverse, five);
  setZero(four);
  four[0] = 4;
  mulGf(y, four, inverse);
  normalizeLimbs(y);

  uint8_t encoded[32];
  gfToBytes(encoded, y);
  encoded[31] = static_cast<uint8_t>(encoded[31] & 0x7F);
  return ed25519DecodePoint(encoded, outX, outY);
}

// --- the scalar ladder ---------------------------------------------------
//
// The only stage of Ed25519 that is not verified. Everything it calls is:
// the field, the scalars mod L, the point encoding and the group law, all tested
// in this same binary. So a failure here is the ladder.
//
// Note the helpers it uses - Gf, mulGf, addGf, subGf, invertGf, gfToBytes,
// curveD - live in the unnamed namespace above. Unnamed-namespace members are
// visible in the enclosing scope, which is why no extra namespace is opened here.

// out = takeIt ? take : keep, with no branch on `takeIt`. Selecting rather than
// branching is what keeps a secret bit out of the branch predictor.
void selectGf(Gf out, const Gf keep, const Gf take, int takeIt) {
  const int64_t mask = takeIt ? ~static_cast<int64_t>(0) : 0;
  for (int i = 0; i < 16; ++i) out[i] = (keep[i] & ~mask) | (take[i] & mask);
}

// out = scalar * p: double-and-add where the addition always runs and the result
// is chosen afterwards, so the same work happens whatever the bit is.
//
// The recurrence is acc = bit ? 2*acc + p : 2*acc, so the two candidates are
// `doubled` and `sum` and the old acc is never a candidate. Selecting between acc
// and sum instead - the obvious transcription - silently drops the doubling on
// every clear bit and yields (2^popcount(scalar) - 1) * p, which is 1, 1, 3, 1, 3,
// 3, 7 for the scalars 1 to 7. Every odd scalar comes out right and every even one
// collapses onto the odd one below it, which is a very easy pattern to mistake for
// a reference bug.
void scalarMult(Point& out, const Point& p, const uint8_t scalar[32]) {
  Point acc;
  identityPoint(acc);
  for (int bit = 255; bit >= 0; --bit) {
    Point doubled, sum;
    addPoints(doubled, acc, acc);
    addPoints(sum, doubled, p);
    const int swap = (scalar[bit >> 3] >> (bit & 7)) & 1;
    // The four calls below are the whole of the secret-dependent control flow.
    selectGf(acc.x, doubled.x, sum.x, swap);
    selectGf(acc.y, doubled.y, sum.y, swap);
    selectGf(acc.z, doubled.z, sum.z, swap);
    selectGf(acc.t, doubled.t, sum.t, swap);
  }
  out = acc;
}

void encodeProjective(uint8_t out[32], const Point& p) {
  Gf inverseZ, x, y;
  invertGf(inverseZ, p.z);
  mulGf(x, p.x, inverseZ);
  mulGf(y, p.y, inverseZ);
  uint8_t xBytes[32];
  gfToBytes(xBytes, x);
  gfToBytes(out, y);
  // Parity from the canonical bytes, never from a limb: a limb may be lazy or
  // negative and its bit 0 is only meaningful once it is a radix digit.
  out[31] = static_cast<uint8_t>((out[31] & 0x7F) | ((xBytes[0] & 1) << 7));
}

void scalarMultBase(Point& out, const uint8_t scalar[32]) {
  uint8_t bx[32], by[32];
  if (!ed25519BasePoint(bx, by)) {
    identityPoint(out);
    return;
  }
  Point base;
  pointFromAffine(base, bx, by);
  scalarMult(out, base, scalar);
}

// The ladder's accumulator in raw projective coordinates, for a caller that needs
// to exercise `addPoints` on a point whose Z is genuinely not 1.
//
// A raw (X, Y, Z) can be fed straight back into `ed25519AddPointsZ` because it is
// the true projective form of the accumulator, not a rescaled affine point. That
// distinction matters: passing an affine x, y with some other Z would name a
// different point entirely, and the test would prove nothing.
bool ed25519ScalarMultBaseRaw(uint8_t outX[32], uint8_t outY[32],
                              uint8_t outZ[32], const uint8_t scalar[32]) {
  if (!outX || !outY || !outZ || !scalar) return false;
  Point p;
  scalarMultBase(p, scalar);
  gfToBytes(outX, p.x);
  gfToBytes(outY, p.y);
  gfToBytes(outZ, p.z);
  return true;
}

bool ed25519ScalarMultBase(uint8_t out[32], const uint8_t scalar[32]) {
  if (!out || !scalar) return false;
  Point p;
  scalarMultBase(p, scalar);
  encodeProjective(out, p);
  return true;
}

// --- scalars mod the group order -------------------------------------------

// L = 2^252 + 27742317777372353535851937790883648493, little-endian.
//
// Checked against Python before being written down: `L.to_bytes(32, 'little')`
// gives edd3f55c 1a631258 d69cf7a2 def9de14, then fifteen zero bytes and 0x10.
// The first version of this constant dropped the 0x58 in the middle, which shifted
// every byte after it and still declared 32 of them, so it compiled and reduced to a
// plausible-looking wrong scalar every time.
const uint8_t kGroupOrder[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};

// a >= L, little-endian.
bool atLeastGroupOrder(const uint8_t a[32]) {
  for (int i = 31; i >= 0; --i) {
    if (a[i] != kGroupOrder[i]) return a[i] > kGroupOrder[i];
  }
  return true;
}

void subtractGroupOrder(uint8_t a[32]) {
  int borrow = 0;
  for (int i = 0; i < 32; ++i) {
    int d = static_cast<int>(a[i]) - kGroupOrder[i] - borrow;
    borrow = 0;
    if (d < 0) {
      d += 256;
      borrow = 1;
    }
    a[i] = static_cast<uint8_t>(d);
  }
}

// out = in mod L, over a 64-byte little-endian input.
//
// One bit at a time, and that is a deliberate choice: a wide reduction needs the
// 64x64->128 multiply this file exists to avoid, and this runs twice per
// signature on a message of a few dozen bytes. The accumulator stays below L after
// every step, so doubling it cannot reach 2^256.
void reduceModGroupOrder(uint8_t out[32], const uint8_t in[64]) {
  uint8_t acc[32] = {0};
  for (int bit = 511; bit >= 0; --bit) {
    uint8_t carry = static_cast<uint8_t>((in[bit >> 3] >> (bit & 7)) & 1);
    for (int i = 0; i < 32; ++i) {
      const uint8_t next = static_cast<uint8_t>(acc[i] >> 7);
      acc[i] = static_cast<uint8_t>((acc[i] << 1) | carry);
      carry = next;
    }
    if (carry || atLeastGroupOrder(acc)) subtractGroupOrder(acc);
  }
  std::memcpy(out, acc, 32);
}

// RFC 8032: clear the low three bits, clear the top bit, set bit 254.
void clampScalar(uint8_t a[32]) {
  a[0] = static_cast<uint8_t>(a[0] & 248);
  a[31] = static_cast<uint8_t>(a[31] & 127);
  a[31] = static_cast<uint8_t>(a[31] | 64);
}

// Plain 256x256 -> 512 schoolbook multiply. Deliberately NOT the field multiply:
// this one must not fold 2^256 down by 38, because the result is reduced mod L
// afterwards, not mod p.
void mulBytes256(uint8_t out[64], const uint8_t a[32], const uint8_t b[32]) {
  uint8_t t[64] = {0};
  for (int i = 0; i < 32; ++i) {
    unsigned carry = 0;
    for (int j = 0; j < 32; ++j) {
      const unsigned cur = t[i + j] + static_cast<unsigned>(a[i]) * b[j] + carry;
      t[i + j] = static_cast<uint8_t>(cur);
      carry = cur >> 8;
    }
    int k = i + 32;
    while (carry != 0 && k < 64) {
      const unsigned cur = t[k] + carry;
      t[k] = static_cast<uint8_t>(cur);
      carry = cur >> 8;
      ++k;
    }
  }
  std::memcpy(out, t, 64);
}

// out = a + b, where b is a full 64-byte product. The result is 65 bits when it
// overflows 2^256, and reduceModGroupOrder accepts 64 bytes, so a carry out of the
// top would be lost. It cannot happen here: b = k*a with a < 2^256 and k < L <
// 2^253, and the caller adds r < L, which does reach 2^256 - hence bit 256 is
// written to out[32] rather than dropped.
void addBytesWithCarry(uint8_t out[65], const uint8_t a[32], const uint8_t b[64]) {
  unsigned carry = 0;
  for (int i = 0; i < 32; ++i) {
    const unsigned cur = static_cast<unsigned>(a[i]) + b[i] + carry;
    out[i] = static_cast<uint8_t>(cur);
    carry = cur >> 8;
  }
  for (int i = 32; i < 64; ++i) {
    const unsigned cur = static_cast<unsigned>(b[i]) + carry;
    out[i] = static_cast<uint8_t>(cur);
    carry = cur >> 8;
  }
  out[64] = static_cast<uint8_t>(carry);
}

bool scalarMultAffine(uint8_t outX[32], uint8_t outY[32],
                      const uint8_t scalar[32], const uint8_t px[32],
                      const uint8_t py[32]) {
  if (!outX || !outY || !scalar || !px || !py) return false;
  Gf gx, gy;
  loadGf(gx, px);
  loadGf(gy, py);
  normalizeLimbs(gx);
  normalizeLimbs(gy);
  if (!onCurve(gx, gy)) return false;
  Point p, q;
  pointFromAffine(p, px, py);
  scalarMult(q, p, scalar);
  affineFromPoint(outX, outY, q);
  return true;
}

// --- signing ---------------------------------------------------------------

bool ed25519PublicKeyFromSeed(uint8_t out[32], const uint8_t seed[32]) {
  if (!out || !seed) return false;
  uint8_t h[64];
  sha512(seed, 32, h);
  clampScalar(h);
  return ed25519ScalarMultBase(out, h);
}

bool ed25519Sign(uint8_t out[64], const uint8_t seed[32],
                 const uint8_t* message, size_t messageLength) {
  if (!out || !seed || (!message && messageLength != 0)) return false;

  uint8_t h[64];
  sha512(seed, 32, h);
  uint8_t a[32];
  std::memcpy(a, h, 32);
  clampScalar(a);
  const uint8_t* prefix = h + 32;

  uint8_t publicKey[32];
  if (!ed25519ScalarMultBase(publicKey, a)) return false;

  Sha512Ctx ctx;
  uint8_t digest[64];

  // r = SHA-512(prefix || M) mod L, so a repeated message does not repeat R.
  sha512Begin(&ctx);
  sha512Append(&ctx, prefix, 32);
  if (messageLength != 0) sha512Append(&ctx, message, messageLength);
  sha512Finish(&ctx, digest);
  uint8_t r[32];
  reduceModGroupOrder(r, digest);

  uint8_t rEncoded[32];
  if (!ed25519ScalarMultBase(rEncoded, r)) return false;

  // k = SHA-512(R || A || M) mod L, which binds the signature to this key and
  // this message and makes it non-interactive.
  sha512Begin(&ctx);
  sha512Append(&ctx, rEncoded, 32);
  sha512Append(&ctx, publicKey, 32);
  if (messageLength != 0) sha512Append(&ctx, message, messageLength);
  sha512Finish(&ctx, digest);
  uint8_t k[32];
  reduceModGroupOrder(k, digest);

  // S = (r + k*a) mod L
  uint8_t product[64];
  mulBytes256(product, k, a);
  uint8_t sum[65];
  addBytesWithCarry(sum, r, product);
  uint8_t s[32];
  reduceModGroupOrder(s, sum);

  std::memcpy(out, rEncoded, 32);
  std::memcpy(out + 32, s, 32);
  return true;
}

bool ed25519Verify(const uint8_t publicKey[32], const uint8_t* message,
                   size_t messageLength, const uint8_t signature[64]) {
  if (!publicKey || !signature || (!message && messageLength != 0)) return false;

  // A non-canonical S makes the signature malleable: S and S + L both verify, so
  // an attacker could mint a second valid signature for the same frame.
  if (atLeastGroupOrder(signature + 32)) return false;

  uint8_t rx[32], ry[32];
  if (!ed25519DecodePoint(signature, rx, ry)) return false;
  uint8_t ax[32], ay[32];
  if (!ed25519DecodePoint(publicKey, ax, ay)) return false;

  Sha512Ctx ctx;
  uint8_t digest[64];
  sha512Begin(&ctx);
  sha512Append(&ctx, signature, 32);
  sha512Append(&ctx, publicKey, 32);
  if (messageLength != 0) sha512Append(&ctx, message, messageLength);
  sha512Finish(&ctx, digest);
  uint8_t k[32];
  reduceModGroupOrder(k, digest);

  // [S]B == R + [k]A
  uint8_t kax[32], kay[32];
  if (!scalarMultAffine(kax, kay, k, ax, ay)) return false;
  uint8_t sumX[32], sumY[32];
  if (!ed25519AddPoints(sumX, sumY, kax, kay, rx, ry)) return false;
  uint8_t rhs[32];
  ed25519EncodeAffine(rhs, sumX, sumY);

  uint8_t lhs[32];
  if (!ed25519ScalarMultBase(lhs, signature + 32)) return false;

  uint8_t diff = 0;
  for (int i = 0; i < 32; ++i) diff = static_cast<uint8_t>(diff | (lhs[i] ^ rhs[i]));
  return diff == 0;
}

bool ed25519ScalarMultPoint(uint8_t out[32], const uint8_t scalar[32],
                            const uint8_t px[32], const uint8_t py[32]) {
  if (!out || !scalar || !px || !py) return false;
  uint8_t qx[32], qy[32];
  if (!scalarMultAffine(qx, qy, scalar, px, py)) return false;
  ed25519EncodeAffine(out, qx, qy);
  return true;
}

}  // namespace cauce

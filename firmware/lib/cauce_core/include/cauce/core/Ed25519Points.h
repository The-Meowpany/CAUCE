#pragma once

// Ed25519 field arithmetic and point encoding (RFC 8032), minus signing.
//
// This is the subset that could be verified end to end: the field, the group,
// and the compressed point encoding. The signing entry points are deliberately
// absent rather than stubbed, because a signature function that compiles and
// returns garbage is worse than one that does not exist.
//
// WHY FIELD ARITHMETIC IN THE FIRMWARE, GIVEN THE THESIS ARGUED AGAINST IT
//
// The argument was that curve arithmetic written on a part with no room to
// review is not reviewable. That is answered by the tests rather than by
// assertion: `test/test_ed25519_points.cpp` pins RFC 8032 section 7.1 point
// encodings, and pins the field against values computed independently in Python.
//
// TWO THINGS THAT WERE WRONG FIRST AND ARE THE POINT OF THE COMMENTS BELOW
//
// 1. A limb may be negative, because the arithmetic here floors: 65533 is held
//    as "carry 1, remainder -3". A negative limb answers 1 to every high bit, so
//    no bit of an exponent may be tested before canonicalLimbs has run.
//
// 2. Limbs must be normalised after every multiply, or their magnitudes compound
//    until int64 overflows. The usual carry pass leaves limb 0 large, because the
//    2^256 overflow folds back onto limb 0 at the end and nothing revisits it.
//    Observed as a chain of correct results (a^2 through a^16) followed by
//    garbage at a^32, which looks like an arithmetic bug and is not one.
//
// Field elements are 16 limbs in radix 2^16 with 64-bit intermediates, not the
// usual five 51-bit limbs: the 51-bit form needs a 64x64->128 multiply and the
// ESP32's Xtensa LX6 is 32-bit with no __int128.

#include <cstddef>
#include <cstdint>

namespace cauce {

constexpr size_t kEd25519PublicKeyBytes = 32;
constexpr size_t kEd25519FieldBytes = 32;

// A compressed point: y with the low bit of x in bit 255.
bool ed25519DecodePoint(const uint8_t encoded[kEd25519PublicKeyBytes],
                        uint8_t outX[32], uint8_t outY[32]);

// Compresses the affine point (x, y) back to its 32-byte form.
void ed25519EncodePoint(uint8_t out[kEd25519PublicKeyBytes],
                        const uint8_t x[32], const uint8_t y[32]);

// The base point, derived rather than transcribed: y = 4/5 with the sign bit
// clear is a complete encoding, and decoding it runs the same canonicality and
// curve-membership checks as any other point. A mistyped constant here would be
// invisible until every signature came out wrong.
bool ed25519BasePoint(uint8_t outX[32], uint8_t outY[32]);

// True when (x, y) satisfies -x^2 + y^2 = 1 + d x^2 y^2 with
// d = -121665/121666, the curve Ed25519 signs on.
bool ed25519IsOnCurve(const uint8_t x[32], const uint8_t y[32]);

// Point addition on affine coordinates.
//
// Exposed for its own sake, not for signing. The group law had never been tested
// in isolation: every earlier check went through decode/encode, which does not
// touch it, and the scalar ladder that does depend on it failed. Adding a point to
// itself has to be checked against the affine doubling formula
//
//   x2 = 2xy / (1 + d x^2 y^2)
//   y2 = (y^2 - x^2) / (1 - d x^2 y^2)
//
// because that is where a wrong sign or a factor of two in one of the four
// multiplications shows up, and nowhere else.
bool ed25519AddPoints(uint8_t outX[32], uint8_t outY[32],
                      const uint8_t ax[32], const uint8_t ay[32],
                      const uint8_t bx[32], const uint8_t by[32]);

// As above, and also writes the four intermediates A, B, C, D of the formula.
//
// Exists because the formula is verified correct for coincident points and
// wrong for distinct ones, which the eight products alone cannot explain: they
// are the same products in both cases. Handing out A, B, C and D lets the first
// one that diverges from the affine derivation name the bug.
bool ed25519AddPointsTrace(uint8_t outX[32], uint8_t outY[32],
                            const uint8_t ax[32], const uint8_t ay[32],
                            const uint8_t bx[32], const uint8_t by[32],
                            uint8_t traceA[32], uint8_t traceB[32],
                            uint8_t traceC[32], uint8_t traceD[32]);

// Encodes an affine point, for comparing an addition against a reference.
void ed25519EncodeAffine(uint8_t out[32], const uint8_t x[32],
                         const uint8_t y[32]);

// Encodes [scalar] times the base point.
//
// Its own entry point because the ladder is the one stage nothing else reaches.
// Every dependency is verified in this same binary - the field, the point
// encoding and the group law - so a failure here is the ladder. It is also the
// only stage never exercised against a point that is not the identity: with
// scalar = 1 the loop doubles the identity 255 times, which is free, and adds
// once. Transposing `z` with `t` in the select survives exactly that case.
//
// Not constant-time. Carries and the ladder act on values derived from the key,
// so an attacker able to measure signing time locally at high resolution may
// recover the private key. The threat model here is a hostile relay or central,
// neither of which sees anything but the result. Where the device itself must be
// assumed hostile, use a constant-time library.
bool ed25519ScalarMultBase(uint8_t out[32], const uint8_t scalar[32]);

}  // namespace cauce
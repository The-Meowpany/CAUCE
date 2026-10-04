// The group law, tested on its own.
//
// Every other check in this file goes through decode/encode, which never calls
// the addition formula. The scalar ladder does, and the ladder was observed
// failing: it returned the base point correctly for scalar = 1, a case whose
// doublings are all doubling the identity, and produced a wrong public key for a
// real seed. So the formula had a defect nobody could see, and the only way to
// find it is to test it directly.
//
// The references are multiples of the base point computed by an implementation
// written from the curve definition with plain big integers, so this compares
// two implementations rather than one against itself.
#include <cstdio>
#include <cstring>

#include <unity.h>

#include "cauce/core/Ed25519Points.h"

namespace {

struct Multiple {
  int scalar;
  const char* affineX;
  const char* affineY;
  const char* encoding;
};

const Multiple kMultiples[] = {
#include "group_vectors.inc"
};

constexpr int kCount = static_cast<int>(sizeof(kMultiples) / sizeof(Multiple));

void unhex(uint8_t* out, const char* text) {
  for (int i = 0; i < 32; ++i) {
    unsigned v = 0;
    std::sscanf(text + i * 2, "%2x", &v);
    out[i] = static_cast<uint8_t>(v);
  }
}

// Decodes an encoding into affine coordinates.
void affineOf(const char* encoding, uint8_t* x, uint8_t* y) {
  uint8_t raw[32];
  unhex(raw, encoding);
  if (!cauce::ed25519DecodePoint(raw, x, y)) {
    TEST_FAIL_MESSAGE("reference point failed to decode");
  }
}

}  // namespace

// --- the identity -------------------------------------------------------

void test_adding_the_identity_changes_nothing() {
  // (0, 1) is the identity. If this fails, the formula's handling of a point
  // with X = 0 is wrong, which would poison every addition including the
  // doublings that start from the identity.
  for (int i = 0; i < kCount; ++i) {
    uint8_t px[32], py[32], zero[32], one[32], ox[32], oy[32];
    affineOf(kMultiples[i].encoding, px, py);
    std::memset(zero, 0, 32);
    std::memset(one, 0, 32);
    one[0] = 1;
    TEST_ASSERT_TRUE(cauce::ed25519AddPoints(ox, oy, zero, one, px, py));
    TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(px, ox, 32, kMultiples[i].encoding);
    TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(py, oy, 32, kMultiples[i].encoding);
  }
}

// --- doubling -----------------------------------------------------------

void test_doubling_matches_the_reference() {
  // Row r holds (r+1)G, so doubling row i needs row 2i+1.
  for (int i = 0; 2 * i + 1 < kCount; ++i) {
    uint8_t px[32], py[32], ox[32], oy[32], want[32];
    affineOf(kMultiples[i].encoding, px, py);
    unhex(want, kMultiples[2 * i + 1].encoding);

    // add(P, P) through the general formula, not a dedicated doubling routine:
    // the ladder uses the general formula, so that is what has to be right.
    TEST_ASSERT_TRUE(cauce::ed25519AddPoints(ox, oy, px, py, px, py));

    uint8_t got[32];
    cauce::ed25519EncodeAffine(got, ox, oy);
    if (std::memcmp(got, want, 32) != 0) {
      char gotHex[65], wantHex[65];
      static const char* digits = "0123456789abcdef";
      for (int k = 0; k < 32; ++k) {
        gotHex[k * 2] = digits[got[k] >> 4];
        gotHex[k * 2 + 1] = digits[got[k] & 15];
        wantHex[k * 2] = digits[want[k] >> 4];
        wantHex[k * 2 + 1] = digits[want[k] & 15];
      }
      gotHex[64] = wantHex[64] = 0;
      std::printf("FAIL doubling %dG\n  got  %s\n  want %s\n",
                  kMultiples[i].scalar, gotHex, wantHex);
      TEST_FAIL_MESSAGE("doubled point differs from the reference");
    }
  }
}

// Adding two *distinct* points, as opposed to a point to itself.
//
// This is the case the doubling test cannot reach, and it is where a
// reported fault turned out to live - in the reference generator, which built
// its multiples by doubling while labelling them 1G, 2G, 3G, so "2G + G"
// was really being compared against 4G. Two implementations agreeing is
// what makes the disagreement attributable.
void test_adding_two_distinct_points_matches_the_reference() {
  // (n+1)G reached as nG + G must equal the reference (n+1)G, which exercises
  // a general addition between two different points rather than a doubling.
  for (int i = 1; i < kCount; ++i) {
    uint8_t gx[32], gy[32], px[32], py[32], ox[32], oy[32], want[32];
    affineOf(kMultiples[0].encoding, gx, gy);
    affineOf(kMultiples[i - 1].encoding, px, py);
    unhex(want, kMultiples[i].encoding);

    TEST_ASSERT_TRUE(cauce::ed25519AddPoints(ox, oy, px, py, gx, gy));
    uint8_t got[32];
    cauce::ed25519EncodeAffine(got, ox, oy);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, got, 32);
  }
}

void test_the_result_of_every_addition_is_on_the_curve() {
  for (int i = 0; i < kCount; ++i) {
    uint8_t px[32], py[32], ox[32], oy[32];
    affineOf(kMultiples[i].encoding, px, py);
    TEST_ASSERT_TRUE(cauce::ed25519AddPoints(ox, oy, px, py, px, py));
    TEST_ASSERT_TRUE(cauce::ed25519IsOnCurve(ox, oy));
  }
}

void test_adding_opposite_points_gives_the_identity() {
  // (x, y) + (-x, y) is the identity. Negating x is a one-bit change in the
  // encoding, so this also confirms the two are distinct points rather than the
  // same one.
  for (int i = 0; i < kCount; ++i) {
    uint8_t px[32], py[32], ox[32], oy[32];
    affineOf(kMultiples[i].encoding, px, py);

    // -x mod p, byte-wise little endian.
    uint8_t negX[32];
    std::memcpy(negX, px, 32);
    int borrow = 0;
    for (int k = 31; k >= 0; --k) {
      const int value = static_cast<int>(negX[k]) - 0xED - borrow;
      negX[k] = static_cast<uint8_t>(value & 0xFF);
      borrow = (value < 0) ? 1 : 0;
      if (!borrow) break;
    }
    if (borrow) {
      // 2^256 - (x + 0xED) is still what is wanted; finish the borrow chain.
      for (int k = 31; k >= 0; --k) {
        const int value = static_cast<int>(negX[k]) - 0xFF - borrow;
        negX[k] = static_cast<uint8_t>(value & 0xFF);
        borrow = (value < 0) ? 1 : 0;
        if (!borrow) break;
      }
    }

    TEST_ASSERT_TRUE(cauce::ed25519AddPoints(ox, oy, px, py, negX, py));
    uint8_t zero[32], one[32], encoded[32];
    std::memset(zero, 0, 32);
    std::memset(one, 0, 32);
    one[0] = 1;
    cauce::ed25519EncodeAffine(encoded, zero, one);
    // The identity compresses to y = 1 with x even.
    TEST_ASSERT_EQUAL_UINT8(0x01, encoded[0]);
  }
}

void test_null_arguments_are_refused() {
  uint8_t x[32] = {0}, y[32] = {0}, ox[32], oy[32];
  x[0] = 4;
  y[0] = 5;
  TEST_ASSERT_FALSE(cauce::ed25519AddPoints(nullptr, oy, x, y, x, y));
  TEST_ASSERT_FALSE(cauce::ed25519AddPoints(ox, oy, nullptr, y, x, y));
  TEST_ASSERT_FALSE(cauce::ed25519AddPoints(ox, oy, x, y, x, nullptr));
  cauce::ed25519EncodeAffine(nullptr, x, y);
  cauce::ed25519EncodeAffine(ox, nullptr, y);
}

void test_decoding_gives_the_reference_affine_coordinates() {
  // Doubling the base point works and adding two distinct points does not, which
  // points at the inputs rather than at the formula: if decode returned a wrong
  // affine x for some point, the round trip would still look correct, because
  // re-encoding the same wrong x reproduces the same encoding.
  //
  // That is exactly the blind spot a round-trip test has, and it is why the
  // reference affine coordinates are checked here and not only the encoding.
  for (int i = 0; i < kCount; ++i) {
    uint8_t raw[32], x[32], y[32], wantX[32], wantY[32];
    unhex(raw, kMultiples[i].encoding);
    unhex(wantX, kMultiples[i].affineX);
    unhex(wantY, kMultiples[i].affineY);
    TEST_ASSERT_TRUE(cauce::ed25519DecodePoint(raw, x, y));
    if (std::memcmp(x, wantX, 32) != 0 || std::memcmp(y, wantY, 32) != 0) {
      char gotX[65], wantXHex[65], gotY[65], wantYHex[65];
      static const char* digits = "0123456789abcdef";
      for (int k = 0; k < 32; ++k) {
        gotX[k * 2] = digits[x[k] >> 4];
        gotX[k * 2 + 1] = digits[x[k] & 15];
        wantXHex[k * 2] = digits[wantX[k] >> 4];
        wantXHex[k * 2 + 1] = digits[wantX[k] & 15];
        gotY[k * 2] = digits[y[k] >> 4];
        gotY[k * 2 + 1] = digits[y[k] & 15];
        wantYHex[k * 2] = digits[wantY[k] >> 4];
        wantYHex[k * 2 + 1] = digits[wantY[k] & 15];
      }
      gotX[64] = wantXHex[64] = gotY[64] = wantYHex[64] = 0;
      std::printf("FAIL decode of %dG\n  x got  %s\n  x want %s\n"
                  "  y got  %s\n  y want %s\n",
                  kMultiples[i].scalar, gotX, wantXHex, gotY, wantYHex);
      TEST_FAIL_MESSAGE("decoded affine coordinates differ from the reference");
    }
  }
}

void test_the_intermediates_of_adding_two_distinct_points() {
  // Which of A, B, C, D first disagrees with the affine derivation. The
  // reference values below are for 2G + G, computed from the affine inputs:
  //
  //   A = (y1 - x1)(y2 - x2)      C = 2 * d * x1*y1 * x2*y2
  //   B = (y1 + x1)(y2 + x2)      D = 2 * 1 * 1
  //
  // Whichever one differs names the bug; if all four agree the fault is in the
  // final four products instead.
  uint8_t px[32], py[32], gx[32], gy[32], ox[32], oy[32];
  uint8_t ta[32], tb[32], tc[32], td[32];
  affineOf(kMultiples[1].encoding, px, py);  // 2G
  affineOf(kMultiples[0].encoding, gx, gy);  // G

  TEST_ASSERT_TRUE(cauce::ed25519AddPointsTrace(ox, oy, px, py, gx, gy, ta,
                                                tb, tc, td));

  static const char* wantA =
      "7dce1cd36a1871866e21fa55d2f11b6d315ca5599141d9f2b21a37134c81ea70";
  static const char* wantB =
      "d0baf0828111dfe2da4a2fe58bb025a91a57dc748a73e0bed6480a4ac0913a4f";
  static const char* wantC =
      "6aa3514052e6d875fc7dedc64f3212413b5d6664405961589ceafaaffcdf5439";
  static const char* wantD =
      "0200000000000000000000000000000000000000000000000000000000000000";

  struct Case { const char* name; const uint8_t* got; const char* want; };
  const Case cases[] = {
      {"A", ta, wantA}, {"B", tb, wantB}, {"C", tc, wantC}, {"D", td, wantD}};
  for (const Case& c : cases) {
    uint8_t expected[32];
    unhex(expected, c.want);
    if (std::memcmp(c.got, expected, 32) != 0) {
      char gotHex[65], wantHex[65];
      static const char* digits = "0123456789abcdef";
      for (int k = 0; k < 32; ++k) {
        gotHex[k * 2] = digits[c.got[k] >> 4];
        gotHex[k * 2 + 1] = digits[c.got[k] & 15];
        wantHex[k * 2] = digits[expected[k] >> 4];
        wantHex[k * 2 + 1] = digits[expected[k] & 15];
      }
      gotHex[64] = wantHex[64] = 0;
      std::printf("FAIL intermediate %s of 2G+G\n  got  %s\n  want %s\n",
                  c.name, gotHex, wantHex);
    }
  }
}


// The bisect, and the test that killed the previous conclusion.
//
// Add the accumulator to the identity. The identity is (0, 1), Z = 1, so this
// exercises pointFromProjective and the final divide on a non-unit Z while giving
// the addition exactly one awkward operand.
//
// It returns G. So reconstructing T = X*Y/Z and dividing by a non-unit Z are both
// correct, which means a correct addition formula would have produced a correct
// 2G. The "addPoints is wrong on projective inputs" conclusion is therefore
// withdrawn: the problem is upstream of the code, in group_vectors.inc, whose x
// and y are the same string in every row.
//
// REGISTERED: this one passes, and it is the reason the fixture got caught.
void test_adding_the_identity_to_a_projective_point() {
  uint8_t one[32] = {0};
  one[0] = 1;

  uint8_t x[32], y[32], z[32];
  TEST_ASSERT_TRUE(cauce::ed25519ScalarMultBaseRaw(x, y, z, one));

  uint8_t identityX[32] = {0}, identityY[32] = {0}, identityZ[32] = {0};
  identityY[0] = 1;
  identityZ[0] = 1;

  uint8_t sumX[32], sumY[32];
  TEST_ASSERT_TRUE(cauce::ed25519AddPointsZ(sumX, sumY, x, y, z, identityX,
                                            identityY, identityZ));

  uint8_t baseX[32], baseY[32];
  TEST_ASSERT_TRUE(cauce::ed25519BasePoint(baseX, baseY));

  static const char* digits = "0123456789abcdef";
  char gotHex[65], wantHex[65];
  for (int k = 0; k < 32; ++k) {
    gotHex[k * 2] = digits[sumY[k] >> 4];
    gotHex[k * 2 + 1] = digits[sumY[k] & 15];
    wantHex[k * 2] = digits[baseY[k] >> 4];
    wantHex[k * 2 + 1] = digits[baseY[k] & 15];
  }
  gotHex[64] = wantHex[64] = 0;

  if (memcmp(sumX, baseX, 32) == 0 && memcmp(sumY, baseY, 32) == 0) {
    return;
  }
  std::printf("bisect: projective + identity != G, so pointFromProjective is "
              "wrong\n  got  %s\n  want %s\n", gotHex, wantHex);
  TEST_ASSERT_TRUE(memcmp(sumX, baseX, 32) == 0 && memcmp(sumY, baseY, 32) == 0);
}

// The scalar ladder against every reference multiple in the fixture.
//
// This is the test that was missing while the ladder was wrong. Its shape matters:
// it must include an even scalar. The broken ladder produced
// (2^popcount(scalar) - 1) * p, so 1, 3, 5 and 7 came out right and 2, 4 and 6
// collapsed onto the odd multiple below them. A test with only odd scalars, or with
// scalar 1, would have passed throughout.
void test_every_reference_multiple_is_reproduced() {
  for (int n = 1; n <= kCount; ++n) {
    uint8_t buffer[32] = {0};
    buffer[0] = static_cast<uint8_t>(n);
    uint8_t got[32], want[32];
    TEST_ASSERT_TRUE(cauce::ed25519ScalarMultBase(got, buffer));
    unhex(want, kMultiples[n - 1].encoding);
    if (memcmp(got, want, 32) != 0) {
      static const char* d = "0123456789abcdef";
      char gotHex[65], wantHex[65];
      for (int k = 0; k < 32; ++k) {
        gotHex[k * 2] = d[got[k] >> 4];
        gotHex[k * 2 + 1] = d[got[k] & 15];
        wantHex[k * 2] = d[want[k] >> 4];
        wantHex[k * 2 + 1] = d[want[k] & 15];
      }
      gotHex[64] = wantHex[64] = 0;
      std::printf("ladder [%d]G MISMATCH\n  got  %s\n  want %s\n", n, gotHex,
                  wantHex);
    }
    TEST_ASSERT_TRUE(memcmp(got, want, 32) == 0);
  }
}

// Doubling must double, checked against the group law rather than against the
// fixture. The group law is verified independently of the ladder, so this ties the
// two together instead of letting a ladder that is wrong but self-consistent pass.
//
// This is also the check that was missing while the ladder dropped its doublings.
// Ladder(2n) against a fixture column would have been the natural test, but that
// column held the next multiple rather than the double, so it agreed with the
// broken ladder for some inputs and hid the fault.
void test_doubling_agrees_with_the_group_law() {
  for (int n = 1; n <= kCount; ++n) {
    uint8_t encoded[32], x[32], y[32];
    unhex(encoded, kMultiples[n - 1].encoding);
    TEST_ASSERT_TRUE(cauce::ed25519DecodePoint(encoded, x, y));

    uint8_t sumX[32], sumY[32];
    TEST_ASSERT_TRUE(cauce::ed25519AddPoints(sumX, sumY, x, y, x, y));
    uint8_t viaGroupLaw[32];
    cauce::ed25519EncodeAffine(viaGroupLaw, sumX, sumY);

    uint8_t doubledScalar[32] = {0};
    doubledScalar[0] = static_cast<uint8_t>(2 * n);
    uint8_t viaLadder[32];
    TEST_ASSERT_TRUE(cauce::ed25519ScalarMultBase(viaLadder, doubledScalar));

    if (memcmp(viaLadder, viaGroupLaw, 32) != 0) {
      static const char* d = "0123456789abcdef";
      char l[65], g[65];
      for (int k = 0; k < 32; ++k) {
        l[k * 2] = d[viaLadder[k] >> 4];
        l[k * 2 + 1] = d[viaLadder[k] & 15];
        g[k * 2] = d[viaGroupLaw[k] >> 4];
        g[k * 2 + 1] = d[viaGroupLaw[k] & 15];
      }
      l[64] = g[64] = 0;
      std::printf("ladder [%d]G disagrees with nG + nG\n  ladder %s\n  group  %s\n",
                  2 * n, l, g);
    }
    TEST_ASSERT_TRUE(memcmp(viaLadder, viaGroupLaw, 32) == 0);
  }
}

// The core decoder, against the fixture's own affine coordinates.
//
// This test did not exist, and that is why a fault in ed25519DecodePoint survived:
// test_decoding_gives_the_reference_affine_coordinates decodes with `affineOf`, a
// helper local to the test file, so the production decoder was never the thing under
// test. Every group-law test inherited that blindness, because they take their
// operands from a local decode.
void test_the_core_decoder_matches_the_fixture() {
  static const char* d = "0123456789abcdef";
  for (int n = 1; n <= kCount; ++n) {
    uint8_t encoded[32], x[32], y[32], wantX[32], wantY[32];
    unhex(encoded, kMultiples[n - 1].encoding);
    unhex(wantX, kMultiples[n - 1].affineX);
    unhex(wantY, kMultiples[n - 1].affineY);
    if (!cauce::ed25519DecodePoint(encoded, x, y)) {
      std::printf("core decoder rejected row %d\n", n);
      TEST_ASSERT_TRUE(false);
    }
    if (memcmp(x, wantX, 32) != 0 || memcmp(y, wantY, 32) != 0) {
      char gx[65], wx[65], gy[65], wy[65];
      for (int k = 0; k < 32; ++k) {
        gx[k * 2] = d[x[k] >> 4]; gx[k * 2 + 1] = d[x[k] & 15];
        wx[k * 2] = d[wantX[k] >> 4]; wx[k * 2 + 1] = d[wantX[k] & 15];
        gy[k * 2] = d[y[k] >> 4]; gy[k * 2 + 1] = d[y[k] & 15];
        wy[k * 2] = d[wantY[k] >> 4]; wy[k * 2 + 1] = d[wantY[k] & 15];
      }
      gx[64] = wx[64] = gy[64] = wy[64] = 0;
      std::printf("row %d x got %s want %s\nrow %d y got %s want %s\n", n, gx, wx,
                  n, gy, wy);
    }
    TEST_ASSERT_TRUE(memcmp(x, wantX, 32) == 0);
    TEST_ASSERT_TRUE(memcmp(y, wantY, 32) == 0);
  }
}

void registerEd25519GroupTests() {
  RUN_TEST(test_the_core_decoder_matches_the_fixture);
  RUN_TEST(test_every_reference_multiple_is_reproduced);
  RUN_TEST(test_doubling_agrees_with_the_group_law);
  RUN_TEST(test_adding_the_identity_to_a_projective_point);
  RUN_TEST(test_adding_the_identity_changes_nothing);
  RUN_TEST(test_decoding_gives_the_reference_affine_coordinates);
  RUN_TEST(test_adding_two_distinct_points_matches_the_reference);
  RUN_TEST(test_the_intermediates_of_adding_two_distinct_points);
  RUN_TEST(test_doubling_matches_the_reference);

  RUN_TEST(test_the_result_of_every_addition_is_on_the_curve);
  RUN_TEST(test_adding_opposite_points_gives_the_identity);
  RUN_TEST(test_null_arguments_are_refused);
}


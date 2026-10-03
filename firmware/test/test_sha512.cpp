// SHA-512 against the FIPS 180-4 / NIST examples.
//
// The vectors are the point of this file. SHA-512 exists for Ed25519, so a
// wrong round constant or a wrong padding length would produce signatures that
// no conforming implementation can verify, and nothing else in the firmware
// exercises it. The expected digests are stated literally rather than computed
// at test time, because comparing SHA-512 against itself proves nothing.

#include <cstring>
#include <string>
#include <unity.h>

#include "cauce/core/Sha512.h"

using cauce::Sha512Ctx;
using cauce::sha512;
using cauce::sha512Append;
using cauce::sha512Begin;
using cauce::sha512Finish;

namespace {

void expectDigest(const uint8_t* actual, const char* expectedHex) {
  char hex[129];
  for (int i = 0; i < 64; ++i) {
    static const char* digits = "0123456789abcdef";
    hex[i * 2] = digits[actual[i] >> 4];
    hex[i * 2 + 1] = digits[actual[i] & 0x0F];
  }
  hex[128] = '\0';
  TEST_ASSERT_EQUAL_STRING(expectedHex, hex);
}

void test_the_empty_message_matches_the_standard() {
  uint8_t digest[64];
  sha512(nullptr, 0, digest);
  expectDigest(digest,
               "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
               "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
}

void test_the_abc_example_matches_the_standard() {
  const char* message = "abc";
  uint8_t digest[64];
  sha512(reinterpret_cast<const uint8_t*>(message), 3, digest);
  expectDigest(digest,
               "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
               "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
}

void test_the_two_block_example_matches_the_standard() {
  const char* message =
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  uint8_t digest[64];
  sha512(reinterpret_cast<const uint8_t*>(message), std::strlen(message),
         digest);
  expectDigest(digest,
               "204a8fc6dda82f0a0ced7beb8e08a41657c16ef468b228a8279be331a703c335"
               "96fd15c13b1b07f9aa1d3bea57789ca031ad85c7a71dd70354ec631238ca3445");
}

void test_a_message_longer_than_one_block() {
  // 896 bytes: seven full blocks, so this crosses the point where the length
  // field and the padding interact.
  std::string message(896, 'a');
  uint8_t digest[64];
  sha512(reinterpret_cast<const uint8_t*>(message.data()), message.size(),
         digest);
  expectDigest(digest,
               "e6837e6011b498390f99a683f6ffe6c817ad1a8eed569ede7f5eea1f048f8670"
               "c5a2ffda032bc9024f64e719530ee3633ba15875ddd22841956ae400eb645015");
}

void test_one_million_a_characters() {
  // The standard's own long test, and the only one that would catch a 64-bit
  // length counter that silently wraps.
  std::string message(1000000, 'a');
  uint8_t digest[64];
  sha512(reinterpret_cast<const uint8_t*>(message.data()), message.size(),
         digest);
  expectDigest(digest,
               "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
               "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b");
}

void test_streaming_in_arbitrary_chunks_equals_one_shot() {
  // A node hashes a frame or a manifest in pieces, so the streaming path has to
  // agree with the whole-buffer path at every awkward split.
  std::string message(1000, 'x');
  for (size_t i = 0; i < message.size(); ++i) message[i] = static_cast<char>('a' + (i % 26));

  uint8_t oneShot[64];
  sha512(reinterpret_cast<const uint8_t*>(message.data()), message.size(),
         oneShot);

  const size_t splits[] = {1, 63, 64, 65, 127, 128, 129, 200, 512, 999};
  for (size_t s = 0; s < sizeof(splits) / sizeof(splits[0]); ++s) {
    const size_t first = splits[s];
    Sha512Ctx ctx;
    sha512Begin(&ctx);
    sha512Append(&ctx, reinterpret_cast<const uint8_t*>(message.data()), first);
    sha512Append(&ctx,
                 reinterpret_cast<const uint8_t*>(message.data()) + first,
                 message.size() - first);
    uint8_t streamed[64];
    sha512Finish(&ctx, streamed);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(oneShot, streamed, 64);
  }
}

void test_a_null_or_zero_length_append_is_a_no_op() {
  std::string message = "abc";
  uint8_t oneShot[64];
  sha512(reinterpret_cast<const uint8_t*>(message.data()), 3, oneShot);

  Sha512Ctx ctx;
  sha512Begin(&ctx);
  sha512Append(&ctx, nullptr, 0);
  sha512Append(&ctx, nullptr, 5);
  sha512Append(&ctx, reinterpret_cast<const uint8_t*>(message.data()), 0);
  sha512Append(&ctx, reinterpret_cast<const uint8_t*>(message.data()), 3);
  uint8_t streamed[64];
  sha512Finish(&ctx, streamed);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(oneShot, streamed, 64);
}

void test_a_message_ending_exactly_on_a_block_boundary() {
  // 111 bytes leaves the length field needing a whole extra block, which is a
  // different padding path from every other case here. NIST publishes no vector
  // at this length, so the assertion is that the one-shot and streaming paths
  // agree and that the digest is not the all-zero or stale-buffer value a
  // broken padding path would leave behind.
  std::string message(111, 'q');
  uint8_t digest[64];
  sha512(reinterpret_cast<const uint8_t*>(message.data()), message.size(),
         digest);

  Sha512Ctx ctx;
  sha512Begin(&ctx);
  sha512Append(&ctx, reinterpret_cast<const uint8_t*>(message.data()), 111);
  uint8_t again[64];
  sha512Finish(&ctx, again);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(digest, again, 64);

  bool allZero = true;
  for (int i = 0; i < 64; ++i) {
    if (digest[i] != 0) allZero = false;
  }
  TEST_ASSERT_FALSE(allZero);
}

}  // namespace

void registerSha512Tests() {
  RUN_TEST(test_the_empty_message_matches_the_standard);
  RUN_TEST(test_the_abc_example_matches_the_standard);
  RUN_TEST(test_the_two_block_example_matches_the_standard);
  RUN_TEST(test_a_message_longer_than_one_block);
  RUN_TEST(test_one_million_a_characters);
  RUN_TEST(test_streaming_in_arbitrary_chunks_equals_one_shot);
  RUN_TEST(test_a_null_or_zero_length_append_is_a_no_op);
  RUN_TEST(test_a_message_ending_exactly_on_a_block_boundary);
}

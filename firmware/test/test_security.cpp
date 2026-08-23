#include <cstring>

#include <string>

#include <unity.h>

#include "cauce/core/SecurityUtils.h"

using namespace cauce;

using namespace cauce;

void test_sha256_empty_string_vector() {
  char hex[65];
  sha256Hex("", hex);
  TEST_ASSERT_EQUAL_STRING(
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", hex);
}

void test_sha256_abc_vector() {
  char hex[65];
  sha256Hex("abc", hex);
  TEST_ASSERT_EQUAL_STRING(
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", hex);
}

void test_sha256_multiblock_vector() {
  char hex[65];
  sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", hex);
  TEST_ASSERT_EQUAL_STRING(
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", hex);
}

void test_secure_equals_basic() {
  TEST_ASSERT_TRUE(secureEquals("abc", "abc"));
  TEST_ASSERT_FALSE(secureEquals("abc", "abd"));
  TEST_ASSERT_FALSE(secureEquals("abc", "ab"));
  TEST_ASSERT_FALSE(secureEquals("", "a"));
  TEST_ASSERT_TRUE(secureEquals(nullptr, nullptr));
}


void test_hmac_rfc4231_case2() {
  // RFC 4231 test case 2: key="Jefe", data="what do ya want for nothing?"
  const char* key = "Jefe";
  const char* data = "what do ya want for nothing?";
  uint8_t mac[32];
  hmacSha256(reinterpret_cast<const uint8_t*>(key), 4,
             reinterpret_cast<const uint8_t*>(data), 28, mac);
  static const char* hex = "0123456789abcdef";
  char out[65];
  for (int i = 0; i < 32; ++i) {
    out[i * 2] = hex[(mac[i] >> 4) & 0xF];
    out[i * 2 + 1] = hex[mac[i] & 0xF];
  }
  out[64] = 0;
  TEST_ASSERT_EQUAL_STRING(
      "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", out);
}

void test_hmac_long_key_matches_reference() {
  // RFC 4231 case 6: key of 131 bytes (0xAA), data "Test Using Larger Than Block-Size Key - Hash Key First"
  std::string key(131, static_cast<char>(0xAA));
  const char* data = "Test Using Larger Than Block-Size Key - Hash Key First";
  uint8_t mac[32];
  hmacSha256(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
             reinterpret_cast<const uint8_t*>(data), 54, mac);
  char out[65];
  static const char* hex = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) {
    out[i * 2] = hex[(mac[i] >> 4) & 0xF];
    out[i * 2 + 1] = hex[mac[i] & 0xF];
  }
  out[64] = 0;
  TEST_ASSERT_EQUAL_STRING(
      "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", out);
}

void registerSecurityTests() {
  UNITY_BEGIN();
  RUN_TEST(test_sha256_empty_string_vector);
  RUN_TEST(test_sha256_abc_vector);
  RUN_TEST(test_sha256_multiblock_vector);
  RUN_TEST(test_secure_equals_basic);
  RUN_TEST(test_hmac_rfc4231_case2);
  RUN_TEST(test_hmac_long_key_matches_reference);
  UNITY_END();
}

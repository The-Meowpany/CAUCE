#include <cstring>

#include <unity.h>

#include "cauce/core/SecurityUtils.h"

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

void registerSecurityTests() {
  UNITY_BEGIN();
  RUN_TEST(test_sha256_empty_string_vector);
  RUN_TEST(test_sha256_abc_vector);
  RUN_TEST(test_sha256_multiblock_vector);
  RUN_TEST(test_secure_equals_basic);
  UNITY_END();
}

// The Ed25519 credential in `NodeConfig`, and the hex decoding that gets it to a signer.
//
// Two properties are tested here that a config round-trip alone would miss. The first is
// that a *wrong* seed is refused at parse time rather than at authentication time: a 63-character
// seed is a provisioning typo, and accepting it produces a node that boots, looks correctly
// configured, and fails every authentication as an invalid signature - which points at the
// certificate instead of at the config line that is wrong.
//
// The second is that a malformed seed writes *nothing*. A caller that ignored the false would
// otherwise be authenticating with whatever was in the buffer, which on a first call is
// uninitialised stack.

#include <cstdio>
#include <cstring>
#include <unity.h>

#include "cauce/core/NodeConfig.h"

namespace cauce {
namespace {

constexpr const char* kValidSeedHex =
    "0707070707070707070707070707070707070707070707070707070707070707";
constexpr const char* kCertificateJson =
    R"({"node_id":"CAUCE-001","public_key":"3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c","serial":"A1"})";

void fill(NodeConfig& c) {
  std::snprintf(c.nodeId, sizeof(c.nodeId), "CAUCE-007");
  std::snprintf(c.syncAuthSeedHex, sizeof(c.syncAuthSeedHex), "%s", kValidSeedHex);
  std::snprintf(c.syncCertificate, sizeof(c.syncCertificate), "%s", kCertificateJson);
}

void test_the_credential_survives_a_config_round_trip() {
  NodeConfig original;
  fill(original);
  char text[4096];
  TEST_ASSERT_TRUE(serializeConfig(original, text, sizeof(text)));

  NodeConfig parsed;
  TEST_ASSERT_TRUE(parseConfig(text, parsed));
  TEST_ASSERT_EQUAL_STRING(kValidSeedHex, parsed.syncAuthSeedHex);
  TEST_ASSERT_EQUAL_STRING(kCertificateJson, parsed.syncCertificate);
}

void test_an_absent_credential_parses_as_absent_rather_than_garbage() {
  // A node provisioned before this existed has neither field. The parse must succeed and leave
  // them empty, because refusing the whole config would strand every deployed node.
  const char* legacy =
      "# CAUCE node configuration v1\n"
      "schema_version=1\nnode_id=CAUCE-001\nsync_server_url=https://central.example\n"
      "sync_device_key=secret\n";
  NodeConfig parsed;
  TEST_ASSERT_TRUE(parseConfig(legacy, parsed));
  TEST_ASSERT_EQUAL_STRING("", parsed.syncAuthSeedHex);
  TEST_ASSERT_EQUAL_STRING("", parsed.syncCertificate);
  // The old secret still parses, which is the whole point of a backwards-compatible schema.
  TEST_ASSERT_EQUAL_STRING("secret", parsed.syncDeviceKey);
}

void test_a_short_seed_is_refused_rather_than_padded() {
  NodeConfig parsed;
  char text[512];
  std::snprintf(text, sizeof(text),
                "# CAUCE node configuration v1\nschema_version=1\nnode_id=CAUCE-001\n"
                "sync_auth_seed=%s\n",
                "07070707070707070707070707070707070707070707070707070707070707");  // 62
  // Refused: the config is reported malformed rather than half-parsed with a 62-character
  // credential in it.
  TEST_ASSERT_FALSE(parseConfig(text, parsed));
  TEST_ASSERT_EQUAL_STRING("", parsed.syncAuthSeedHex);
}

void test_a_non_hex_seed_is_refused() {
  NodeConfig parsed;
  char text[512];
  // One character replaced by 'z'. Every other character is a valid hex digit, so this is
  // exactly what a mangled copy-paste looks like.
  char mangled[65];
  std::memcpy(mangled, kValidSeedHex, 64);
  mangled[10] = 'z';
  mangled[64] = '\0';
  std::snprintf(text, sizeof(text),
                "# CAUCE node configuration v1\nschema_version=1\nnode_id=CAUCE-001\n"
                "sync_auth_seed=%s\n",
                mangled);
  TEST_ASSERT_FALSE(parseConfig(text, parsed));
  TEST_ASSERT_EQUAL_STRING("", parsed.syncAuthSeedHex);
}

void test_uppercase_hex_is_accepted() {
  // An operator typing a seed by hand will not remember which case the generator printed.
  NodeConfig parsed;
  char text[512];
  char upper[65];
  std::memcpy(upper, kValidSeedHex, 64);
  for (size_t i = 0; i < 64; ++i) {
    if (upper[i] >= 'a' && upper[i] <= 'f') upper[i] = static_cast<char>(upper[i] - 32);
  }
  upper[64] = '\0';
  std::snprintf(text, sizeof(text),
                "# CAUCE node configuration v1\nschema_version=1\nnode_id=CAUCE-001\n"
                "sync_auth_seed=%s\n",
                upper);
  TEST_ASSERT_TRUE(parseConfig(text, parsed));
  TEST_ASSERT_EQUAL_STRING(upper, parsed.syncAuthSeedHex);
}

void test_an_empty_seed_is_left_alone_rather_than_reported_malformed() {
  // No credential configured is a legitimate state. Only a *wrong* credential is an error.
  NodeConfig parsed;
  char text[512];
  std::snprintf(text, sizeof(text),
                "# CAUCE node configuration v1\nschema_version=1\nnode_id=CAUCE-001\n"
                "sync_auth_seed=\n");
  TEST_ASSERT_TRUE(parseConfig(text, parsed));
  TEST_ASSERT_EQUAL_STRING("", parsed.syncAuthSeedHex);
}

void test_a_valid_seed_decodes_to_thirty_two_bytes_of_seven() {
  uint8_t seed[32];
  TEST_ASSERT_TRUE(decodeSeedHex(kValidSeedHex, seed, sizeof(seed)));
  for (uint8_t b : seed) {
    TEST_ASSERT_EQUAL_UINT8(7, b);
  }
}

void test_mixed_case_hex_decodes_to_the_same_seed() {
  uint8_t lower[32];
  uint8_t upper[32];
  char upperHex[65];
  std::memcpy(upperHex, kValidSeedHex, 64);
  for (size_t i = 0; i < 64; ++i) {
    if (upperHex[i] >= 'a' && upperHex[i] <= 'f') upperHex[i] = static_cast<char>(upperHex[i] - 32);
  }
  upperHex[64] = '\0';
  TEST_ASSERT_TRUE(decodeSeedHex(kValidSeedHex, lower, sizeof(lower)));
  TEST_ASSERT_TRUE(decodeSeedHex(upperHex, upper, sizeof(upper)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(lower, upper, sizeof(lower));
}

// The one that matters most. A refused seed must leave the caller's buffer untouched, because
// the caller is about to hand it to an authenticator.
void test_a_refused_seed_writes_nothing_to_the_callers_buffer() {
  uint8_t seed[32];
  std::memset(seed, 0xAA, sizeof(seed));
  TEST_ASSERT_FALSE(decodeSeedHex("0707", seed, sizeof(seed)));
  for (uint8_t b : seed) {
    TEST_ASSERT_EQUAL_UINT8(0xAA, b);
  }
}

void test_a_refused_seed_in_the_middle_writes_nothing() {
  // The failure is discovered at the last byte, so the decoder has already written thirty-one
  // of them somewhere. If it writes into the caller's buffer as it goes, a caller that ignores
  // the false authenticates with a truncated seed.
  uint8_t seed[32];
  std::memset(seed, 0xAA, sizeof(seed));
  char almost[65];
  std::memcpy(almost, kValidSeedHex, 64);
  almost[62] = 'q';  // the second-to-last nibble pair
  almost[64] = '\0';
  TEST_ASSERT_FALSE(decodeSeedHex(almost, seed, sizeof(seed)));
  for (uint8_t b : seed) {
    TEST_ASSERT_EQUAL_UINT8(0xAA, b);
  }
}

void test_a_too_small_output_buffer_is_refused() {
  uint8_t tiny[16];
  TEST_ASSERT_FALSE(decodeSeedHex(kValidSeedHex, tiny, sizeof(tiny)));
}

void test_a_null_or_empty_seed_is_refused() {
  uint8_t seed[32];
  TEST_ASSERT_FALSE(decodeSeedHex(nullptr, seed, sizeof(seed)));
  TEST_ASSERT_FALSE(decodeSeedHex("", seed, sizeof(seed)));
  TEST_ASSERT_FALSE(decodeSeedHex(kValidSeedHex, nullptr, 32));
}

void test_a_seed_with_a_trailing_space_is_refused_rather_than_trimmed() {
  // The config parser trims, so by the time this is called there is no whitespace. A caller
  // that passes some anyway is passing something it did not get from the parser, and quietly
  // trimming here would make this function disagree with the validator that gates it.
  uint8_t seed[32];
  TEST_ASSERT_FALSE(decodeSeedHex("0707 ", seed, sizeof(seed)));
}

void test_the_certificate_survives_a_round_trip_with_its_json_intact() {
  // The JSON is stored verbatim. `parseConfig` must not try to understand it - a second
  // parser for a format this module does not own is how the encoding drifts.
  NodeConfig original;
  fill(original);
  char text[4096];
  TEST_ASSERT_TRUE(serializeConfig(original, text, sizeof(text)));
  NodeConfig parsed;
  TEST_ASSERT_TRUE(parseConfig(text, parsed));
  TEST_ASSERT_EQUAL_STRING(kCertificateJson, parsed.syncCertificate);
  TEST_ASSERT_NOT_NULL(std::strstr(parsed.syncCertificate, "\"public_key\""));
}

void test_a_credential_without_a_certificate_is_half_configured() {
  // Both halves are needed: a seed with no certificate cannot be used, because presenting
  // nothing commits the central to certificate auth and falls back to nothing. So the pair has
  // to be visible together rather than each being individually optional.
  NodeConfig parsed;
  char text[512];
  std::snprintf(text, sizeof(text),
                "# CAUCE node configuration v1\nschema_version=1\nnode_id=CAUCE-001\n"
                "sync_auth_seed=%s\n",
                kValidSeedHex);
  TEST_ASSERT_TRUE(parseConfig(text, parsed));
  TEST_ASSERT_TRUE(parsed.syncAuthSeedHex[0] != '\0');
  TEST_ASSERT_EQUAL_STRING("", parsed.syncCertificate);
}

}  // namespace

void registerNodeCredentialTests() {
  UNITY_BEGIN();
  RUN_TEST(test_the_credential_survives_a_config_round_trip);
  RUN_TEST(test_an_absent_credential_parses_as_absent_rather_than_garbage);
  RUN_TEST(test_a_short_seed_is_refused_rather_than_padded);
  RUN_TEST(test_a_non_hex_seed_is_refused);
  RUN_TEST(test_uppercase_hex_is_accepted);
  RUN_TEST(test_an_empty_seed_is_left_alone_rather_than_reported_malformed);
  RUN_TEST(test_a_valid_seed_decodes_to_thirty_two_bytes_of_seven);
  RUN_TEST(test_mixed_case_hex_decodes_to_the_same_seed);
  RUN_TEST(test_a_refused_seed_writes_nothing_to_the_callers_buffer);
  RUN_TEST(test_a_refused_seed_in_the_middle_writes_nothing);
  RUN_TEST(test_a_too_small_output_buffer_is_refused);
  RUN_TEST(test_a_null_or_empty_seed_is_refused);
  RUN_TEST(test_a_seed_with_a_trailing_space_is_refused_rather_than_trimmed);
  RUN_TEST(test_the_certificate_survives_a_round_trip_with_its_json_intact);
  RUN_TEST(test_a_credential_without_a_certificate_is_half_configured);
}

}  // namespace cauce
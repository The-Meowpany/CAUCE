// The firmware-update key, and its separation from the data key.
//
// The coupling being removed: the manifest signature was derived from `syncDeviceKey`, so the
// credential that could authorise a firmware image was the credential every measurement arrived
// under - and an HMAC key is symmetric, so it authenticates *as* that node as well as for it.
//
// The property that matters most here is that the fallback survives. A node provisioned before
// this field existed has no separate update key, and a config parser that refused it would strand
// every deployed node on its next update - trading a real exposure for a guaranteed outage.

#include <cstdio>
#include <cstring>
#include <unity.h>

#include "cauce/core/NodeConfig.h"

namespace cauce {
namespace {

constexpr const char* kValidSeedHex =
    "0707070707070707070707070707070707070707070707070707070707070707";

void test_a_separate_manifest_key_survives_a_config_round_trip() {
  NodeConfig original;
  std::snprintf(original.nodeId, sizeof(original.nodeId), "CAUCE-007");
  std::snprintf(original.syncDeviceKey, sizeof(original.syncDeviceKey), "%s",
                "the-data-secret");
  std::snprintf(original.syncAuthSeedHex, sizeof(original.syncAuthSeedHex), "%s",
                kValidSeedHex);
  std::snprintf(original.otaManifestKey, sizeof(original.otaManifestKey), "%s",
                "the-update-secret");

  char text[4096];
  TEST_ASSERT_TRUE(serializeConfig(original, text, sizeof(text)));
  NodeConfig parsed;
  TEST_ASSERT_TRUE(parseConfig(text, parsed));

  TEST_ASSERT_EQUAL_STRING("the-update-secret", parsed.otaManifestKey);
  TEST_ASSERT_EQUAL_STRING("the-data-secret", parsed.syncDeviceKey);
  // Independent fields, not one derived from the other. Deriving it here would be the coupling
  // this field exists to remove.
  TEST_ASSERT_TRUE(std::strcmp(parsed.syncDeviceKey, parsed.otaManifestKey) != 0);
}

void test_an_absent_manifest_key_parses_as_absent() {
  const char* legacy =
      "# CAUCE node configuration v1\nschema_version=1\nnode_id=CAUCE-001\n"
      "sync_device_key=shared-secret-value\nota_manifest_url=https://x/y\n";
  NodeConfig parsed;
  TEST_ASSERT_TRUE(parseConfig(legacy, parsed));
  TEST_ASSERT_EQUAL_STRING("", parsed.otaManifestKey);
  // The old behaviour is still reachable: the node keeps updating, and `main.cpp` reports the
  // coupling once at boot rather than failing closed.
  TEST_ASSERT_EQUAL_STRING("shared-secret-value", parsed.syncDeviceKey);
}

void test_an_empty_manifest_key_is_indistinguishable_from_an_absent_line() {
  // Both leave the field empty, and both mean "fall back". Deliberate: there is no third state,
  // because "present but empty" would only invite a key that is the empty string, which is a
  // constant anyone can compute.
  NodeConfig parsed;
  char text[512];
  std::snprintf(text, sizeof(text),
                "# CAUCE node configuration v1\nschema_version=1\nnode_id=CAUCE-001\n"
                "ota_manifest_key=\n");
  TEST_ASSERT_TRUE(parseConfig(text, parsed));
  TEST_ASSERT_EQUAL_STRING("", parsed.otaManifestKey);
}

// A 64-character secret fits a 65-byte field. A 70-character one does not, and what happens
// next is the question: silently dropping the tail produces a key that differs from the one the
// central holds, which fails as an invalid signature and points at the OTA code rather than at
// the config line.
void test_an_over_long_manifest_key_is_a_prefix_not_a_scrambled_secret() {
  NodeConfig original;
  std::snprintf(original.otaManifestKey, sizeof(original.otaManifestKey), "%s",
                "0123456789012345678901234567890123456789012345678901234567890123456789");
  char text[4096];
  TEST_ASSERT_TRUE(serializeConfig(original, text, sizeof(text)));
  NodeConfig parsed;
  TEST_ASSERT_TRUE(parseConfig(text, parsed));
  TEST_ASSERT_EQUAL_UINT32(64, std::strlen(parsed.otaManifestKey));
  // A prefix, never a secret with its middle removed: truncation in the middle would produce
  // something that looks like a key and is not one.
  TEST_ASSERT_EQUAL_STRING("0123456789012345678901234567890123456789012345678901234567890123",
                           parsed.otaManifestKey);
}

void test_the_manifest_key_and_the_credential_are_both_optional_together() {
  // A config with neither parses; so does one with both. What must not parse is a *wrong*
  // credential, which the other suite pins.
  NodeConfig parsed;
  char neither[512];
  std::snprintf(neither, sizeof(neither),
                "# CAUCE node configuration v1\nschema_version=1\nnode_id=CAUCE-001\n");
  TEST_ASSERT_TRUE(parseConfig(neither, parsed));
  TEST_ASSERT_EQUAL_STRING("", parsed.otaManifestKey);
  TEST_ASSERT_EQUAL_STRING("", parsed.syncAuthSeedHex);
}

}  // namespace

void registerManifestKeyTests() {
  UNITY_BEGIN();
  RUN_TEST(test_a_separate_manifest_key_survives_a_config_round_trip);
  RUN_TEST(test_an_absent_manifest_key_parses_as_absent);
  RUN_TEST(test_an_empty_manifest_key_is_indistinguishable_from_an_absent_line);
  RUN_TEST(test_an_over_long_manifest_key_is_a_prefix_not_a_scrambled_secret);
  RUN_TEST(test_the_manifest_key_and_the_credential_are_both_optional_together);
}

}  // namespace cauce
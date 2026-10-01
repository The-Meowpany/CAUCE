#include <cstring>
#include <string>

#include <unity.h>

#include "cauce/app/FieldDiagnostics.h"

using namespace cauce::app;

namespace {

DiagnosticsInput baseInput() {
  DiagnosticsInput in;
  in.nodeId = "CAUCE-001";
  in.siteId = "canelones-centro";
  in.firmwareVersion = "1.4.0";
  in.nodeState = "Sampling";
  in.netState = "Connected";
  in.uptimeMs = 123456;
  in.clockValid = true;
  in.rssiDbm = -63;
  in.batteryVoltageV = 3.92f;
  in.measurementCount = 1200;
  in.storedCount = 1150;
  in.invalidCount = 7;
  in.suspectCount = 3;
  in.readFailures = 1;
  in.storageFailures = 0;
  in.corruptedFrames = 2;
  in.storageRecords = 1150;
  in.storageBytes = 40960;
  in.lastSuccessUtcMs = 1787356800000ULL;
  in.sampleIntervalS = 60;
  in.syncAttempts = 20;
  in.syncFailures = 2;
  return in;
}

bool contains(const char* haystack, const char* needle) {
  return std::strstr(haystack, needle) != nullptr;
}

}  // namespace

void test_diagnostics_json_carries_health_and_identity() {
  char body[1200];
  const size_t n = buildDiagnosticsJson(body, sizeof(body), baseInput());
  TEST_ASSERT_GREATER_THAN(0, n);
  TEST_ASSERT_EQUAL('{', body[0]);
  TEST_ASSERT_EQUAL('}', body[n - 1]);
  TEST_ASSERT_TRUE_MESSAGE(contains(body, "\"schema\":\"cauce.diag/1\""), "schema");
  TEST_ASSERT_TRUE_MESSAGE(contains(body, "\"node_id\":\"CAUCE-001\""), "node_id");
  TEST_ASSERT_TRUE_MESSAGE(contains(body, "\"site_id\":\"canelones-centro\""), "site_id");
  TEST_ASSERT_TRUE_MESSAGE(contains(body, "\"firmware\":\"1.4.0\""), "firmware");
  TEST_ASSERT_TRUE_MESSAGE(contains(body, "\"uptime_ms\":123456"), "uptime_ms");
  TEST_ASSERT_TRUE_MESSAGE(contains(body, "\"utc_time_valid\":true"), "clock");
  TEST_ASSERT_TRUE_MESSAGE(contains(body, "\"rssi_dbm\":-63"), "rssi");
  TEST_ASSERT_TRUE_MESSAGE(contains(body, "\"corrupted_frames\":2"), "corrupted");
  TEST_ASSERT_TRUE_MESSAGE(contains(body, "\"sample_interval_s\":60"), "interval");
  TEST_ASSERT_TRUE_MESSAGE(contains(body, "\"failures\":2"), "sync_failures");
}

void test_diagnostics_json_flags_missing_clock() {
  DiagnosticsInput in = baseInput();
  in.clockValid = false;
  in.loraEnabled = true;
  in.loraRssiDbm = -101;
  in.loraSnrDb = -7;
  in.loraSf = 9;
  char body[1200];
  const size_t n = buildDiagnosticsJson(body, sizeof(body), in);
  TEST_ASSERT_GREATER_THAN(0, n);
  TEST_ASSERT_TRUE(contains(body, "\"utc_time_valid\":false"));
  TEST_ASSERT_TRUE(contains(body, "\"lora_enabled\":true"));
  TEST_ASSERT_TRUE(contains(body, "\"sf\":9"));
}

void test_diagnostics_json_escapes_hostile_strings() {
  DiagnosticsInput in = baseInput();
  in.nodeId = "bad\"id\\with\ncontrol";
  in.lastError = "line1\nline2";
  char body[1200];
  const size_t n = buildDiagnosticsJson(body, sizeof(body), in);
  TEST_ASSERT_GREATER_THAN(0, n);
  TEST_ASSERT_TRUE(contains(body, "\\\""));
  TEST_ASSERT_TRUE(contains(body, "\\\\"));
  TEST_ASSERT_TRUE(contains(body, "\\n"));
  TEST_ASSERT_NULL(std::strchr(body + 1, '\n'));
}

void test_diagnostics_json_refuses_truncated_buffer() {
  char tiny[16];
  const size_t n = buildDiagnosticsJson(tiny, sizeof(tiny), baseInput());
  TEST_ASSERT_EQUAL(0, n);
  TEST_ASSERT_EQUAL('\0', tiny[0]);
  TEST_ASSERT_EQUAL(0, buildDiagnosticsJson(nullptr, 0, baseInput()));
}

void test_diagnostics_signature_matches_known_hmac() {
  uint8_t key[20];
  std::memset(key, 0x0b, sizeof(key));
  const char data[] = "Hi There";
  char hex[65];
  const size_t n = diagnosticsSignatureHex(
      reinterpret_cast<const char*>(key), data, std::strlen(data), hex);
  TEST_ASSERT_EQUAL(64, n);
  TEST_ASSERT_EQUAL_STRING(
      "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
      hex);
}

void test_diagnostics_signature_is_stable_and_key_dependent() {
  char a[65], b[65], c[65];
  diagnosticsSignatureHex("device-key-1", "{\"a\":1}", 7, a);
  diagnosticsSignatureHex("device-key-1", "{\"a\":1}", 7, b);
  diagnosticsSignatureHex("device-key-2", "{\"a\":1}", 7, c);
  TEST_ASSERT_EQUAL_STRING(a, b);
  TEST_ASSERT_TRUE(std::strcmp(a, c) != 0);
  TEST_ASSERT_EQUAL(64, std::strspn(a, "0123456789abcdef"));
}

void test_diagnostics_signature_rejects_missing_inputs() {
  char hex[65];
  TEST_ASSERT_EQUAL(0, diagnosticsSignatureHex(nullptr, "body", 4, hex));
  TEST_ASSERT_EQUAL('\0', hex[0]);
  TEST_ASSERT_EQUAL(0, diagnosticsSignatureHex("key", nullptr, 4, hex));
  TEST_ASSERT_EQUAL(0, diagnosticsSignatureHex("key", "body", 4, nullptr));
}

void registerDiagnosticsTests() {
  RUN_TEST(test_diagnostics_json_carries_health_and_identity);
  RUN_TEST(test_diagnostics_json_flags_missing_clock);
  RUN_TEST(test_diagnostics_json_escapes_hostile_strings);
  RUN_TEST(test_diagnostics_json_refuses_truncated_buffer);
  RUN_TEST(test_diagnostics_signature_matches_known_hmac);
  RUN_TEST(test_diagnostics_signature_is_stable_and_key_dependent);
  RUN_TEST(test_diagnostics_signature_rejects_missing_inputs);
}

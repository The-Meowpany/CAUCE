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

namespace {

struct HmacVector {
  size_t keyLen;
  size_t bodyLen;
  const char* hex;
};

const HmacVector kHmacVectors[] = {
    {1, 1, "d0c9918673d81e054f55f2491b1442611919bb206b9d788108496c27e8214338"},
    {1, 8, "410a7377f6613f26aa1b18c57d601b135baaef181af2cdc4861e44227ced73cf"},
    {1, 25, "6162cae41ffdc40b9c65018cc09c0545ce23433c47ef544679e214ccddedb632"},
    {1, 64, "00bfe4dc4c41d78ca37c7d417a05311306e37872af463c8b206dd6be701303f2"},
    {1, 200, "12a5b71d26d0c40b1dd579b454bba86a2dabd621c7ba6346d88bc918c443c854"},
    {20, 1, "6673247a21a19464659bc14cd57fdeeb36ab2d06572d2249065bc59acdf752a8"},
    {20, 8, "508f77fd694479aedac2fc727ea4bb768a25e85f0416297f36964c9d6091331d"},
    {20, 25, "bfd5b4524a6aecac83abbfdf0ed62a3c75e1f77d5d0d8ead67cad054150cf866"},
    {20, 64, "deb5697aa9981b12d7c32565c79055f3cd130e9ba29302dcce02fc35be6c18b5"},
    {20, 200, "c4194b7b7337f7d8312360375fb51cc7c0fc2f898faa8c5816eb28e1686bb93e"},
    {33, 1, "6e3ae8414e45e77763f11902cede264dca63533bd1a08734d5ecbdf082c54fa6"},
    {33, 8, "e41962b34e55df051db699005111134a6f5e2bd3819da0f884d21885b4033284"},
    {33, 25, "a0116cbbf47554069e3dec1fafa0c9008097609b3fbbfdd155c624406a09bf86"},
    {33, 64, "db7e992184c89baef8e213cf0a6955727e4e4dea6160a5192791544a27c04019"},
    {33, 200, "d5b8cb39fadca49870e992dfe3be0e1af32140941cf730b210d29027054510ec"},
    {64, 1, "263779e44a30252a2e81074c6857016a00ff984206c5f724190f177ac893080c"},
    {64, 8, "549048d6fc8e225635768b64cdee1576c2bbe1857fc609bcdf9982fac0141f35"},
    {64, 25, "e3b01182ca482e8ffadc365617694ea56f5754f193a9b1851907c34dd7764877"},
    {64, 64, "aaf9699c72f050eef7e08622d61e732126128430099fc4a0519eef4c1118f0a2"},
    {64, 200, "ecd5a3d8c24591bcb47a83083fb884717d740cb4047a6b7ab3e4faef3097a8f1"},
    {100, 1, "8b1121fbb92ef1dc69c80c0c5e466a2340945872cd89746ecfb77c1647fdd54f"},
    {100, 8, "8bdd3a658f884b7abb2803b6dc5340e12a4b29033a49f7a6271c1742baf15a9d"},
    {100, 25, "968f5691dba72fa624c6634dc1d327387f1b24037527e47b274890d46b2d7adb"},
    {100, 64, "0a7f8b3659158058422da649a9e62c4ffca1c145665d286f2e28d36eff3fc8ae"},
    {100, 200, "46bbfe1d598bfad013a00642975b6b2fa5f4a80df4e5cdef8d7a9f5aeba6d043"},
};

void fillKey(uint8_t* out, size_t n) {
  for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>((i * 7 + 3) % 251);
}

void fillBody(uint8_t* out, size_t n) {
  for (size_t i = 0; i < n; ++i) out[i] = static_cast<uint8_t>((i * 11 + 5) % 253);
}

}  // namespace

void test_diagnostics_signature_matches_known_hmac() {
  uint8_t key[20];
  std::memset(key, 0x0b, sizeof(key));
  const char data[] = "Hi There";
  char hex[65];
  const size_t n = diagnosticsSignatureHex(
      reinterpret_cast<const char*>(key), sizeof(key), data,
      std::strlen(data), hex);
  TEST_ASSERT_EQUAL(64, n);
  TEST_ASSERT_EQUAL_STRING(
      "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
      hex);
}

void test_diagnostics_signature_uses_the_given_key_length() {
  char buffer[24];
  std::memset(buffer, 0x0b, sizeof(buffer));
  buffer[20] = 'X';
  buffer[21] = 'Y';
  buffer[22] = 'Z';
  const char data[] = "Hi There";
  char withLen[65];
  char withoutLen[65];
  const size_t n1 =
      diagnosticsSignatureHex(buffer, 20, data, std::strlen(data), withLen);
  const size_t n2 = diagnosticsSignatureHex(buffer, 23, data,
                                            std::strlen(data), withoutLen);
  TEST_ASSERT_EQUAL(64, n1);
  TEST_ASSERT_EQUAL(64, n2);
  TEST_ASSERT_EQUAL_STRING(
      "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
      withLen);
  TEST_ASSERT_TRUE(std::strcmp(withLen, withoutLen) != 0);
}

void test_diagnostics_signature_is_stable_and_key_dependent() {
  char a[65], b[65], c[65];
  diagnosticsSignatureHex("device-key-1", 12, "{\"a\":1}", 7, a);
  diagnosticsSignatureHex("device-key-1", 12, "{\"a\":1}", 7, b);
  diagnosticsSignatureHex("device-key-2", 12, "{\"a\":1}", 7, c);
  TEST_ASSERT_EQUAL_STRING(a, b);
  TEST_ASSERT_TRUE(std::strcmp(a, c) != 0);
  TEST_ASSERT_EQUAL(64, std::strspn(a, "0123456789abcdef"));
}

void test_diagnostics_signature_rejects_missing_inputs() {
  char hex[65];
  TEST_ASSERT_EQUAL(0, diagnosticsSignatureHex(nullptr, 4, "body", 4, hex));
  TEST_ASSERT_EQUAL('\0', hex[0]);
  TEST_ASSERT_EQUAL(0, diagnosticsSignatureHex("key", 3, nullptr, 4, hex));
  TEST_ASSERT_EQUAL(0, diagnosticsSignatureHex("key", 0, "body", 4, hex));
  TEST_ASSERT_EQUAL(0, diagnosticsSignatureHex("key", 3, "body", 0, hex));
  TEST_ASSERT_EQUAL(0, diagnosticsSignatureHex("key", 3, "body", 4, nullptr));
}

void test_diagnostics_signature_matches_reference_vectors() {
  uint8_t key[128];
  uint8_t body[256];
  for (const HmacVector& v : kHmacVectors) {
    fillKey(key, v.keyLen);
    fillBody(body, v.bodyLen);
    char hex[65];
    const size_t n = diagnosticsSignatureHex(reinterpret_cast<const char*>(key),
                                             v.keyLen,
                                             reinterpret_cast<const char*>(body),
                                             v.bodyLen, hex);
    TEST_ASSERT_EQUAL_MESSAGE(64, n, v.hex);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(v.hex, hex, v.hex);
  }
}

void registerDiagnosticsTests() {
  RUN_TEST(test_diagnostics_json_carries_health_and_identity);
  RUN_TEST(test_diagnostics_json_flags_missing_clock);
  RUN_TEST(test_diagnostics_json_escapes_hostile_strings);
  RUN_TEST(test_diagnostics_json_refuses_truncated_buffer);
  RUN_TEST(test_diagnostics_signature_matches_known_hmac);
  RUN_TEST(test_diagnostics_signature_matches_reference_vectors);
  RUN_TEST(test_diagnostics_signature_uses_the_given_key_length);
  RUN_TEST(test_diagnostics_signature_is_stable_and_key_dependent);
  RUN_TEST(test_diagnostics_signature_rejects_missing_inputs);
}

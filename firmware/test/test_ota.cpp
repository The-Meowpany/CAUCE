#include <cstring>
#include <string>
#include <vector>

#include <unity.h>

#include "cauce/app/OtaJson.h"
#include "cauce/app/OtaManager.h"
#include "cauce/core/SecurityUtils.h"
#include "cauce/core/Sha256Stream.h"
#include "cauce/core/Types.h"
#include "cauce/hal/ManualClock.h"

using namespace cauce;
using namespace cauce::app;

namespace {

class FakeCatalog final : public IManifestSource {
 public:
  bool hasRelease{false};
  OtaRelease release{};
  int fetchCalls{0};

  bool fetchLatest(const char*, OtaRelease& out) override {
    ++fetchCalls;
    if (!hasRelease) return false;
    out = release;
    return true;
  }
};

class FakeReader final : public IFirmwareReader {
 public:
  std::string payload;
  bool isOpen{false};
  size_t pos{0};
  int openCalls{0};

bool open(const char*) override {
       isOpen = true;
       pos = 0;
       ++openCalls;
       return true;
     }
     ReadStatus read(uint8_t* buffer, size_t capacity,
                     size_t* bytesRead) override {
       if (bytesRead != nullptr) *bytesRead = 0;
       ++readCalls;
       if (stallEvery > 0 && readCalls % stallEvery == 0) {
         return ReadStatus::NoDataYet;
       }
       if (alwaysStall) return ReadStatus::NoDataYet;
       if (pos >= payload.size()) return ReadStatus::Eof;
       const size_t n = std::min(capacity, payload.size() - pos);
       std::memcpy(buffer, payload.data() + pos, n);
       pos += n;
       if (bytesRead != nullptr) *bytesRead = n;
       return ReadStatus::Data;
     }
     void close() override { isOpen = false; }
     int readCalls{0};
     int stallEvery{0};
     bool alwaysStall{false};
   };

class FakeInstaller final : public IFirmwareInstaller {
 public:
  std::vector<uint8_t> received;
  bool begun{false};
  bool aborted{false};
  InstallDecision finishDecision{InstallDecision::Proceed};

  bool beginInstall(uint32_t) override {
    begun = true;
    received.clear();
    return true;
  }
  bool writeChunk(const uint8_t* data, size_t length) override {
    received.insert(received.end(), data, data + length);
    return true;
  }
  InstallDecision finishInstall() override { return finishDecision; }
  void abortInstall() override {
    aborted = true;
    received.clear();
  }
};

hal::ManualClock otaClock(1787356800000ULL);

class SilentSink2 final : public ILogSink {
 public:
  void writeLine(const char* line) override {
    lines.push_back(line ? line : "");
  }
  bool contains(const char* needle) const {
    for (const std::string& l : lines) {
      if (l.find(needle) != std::string::npos) return true;
    }
    return false;
  }
  std::vector<std::string> lines;
} sink2;

struct OtaRig {
  FakeCatalog catalog;
  FakeReader reader;
  FakeInstaller installer;
  Logger logger{sink2};
  OtaManager manager;

  // A signing key is set by default because the manager now refuses to update without one,
  // and most of these tests are about the download path rather than about the gate. The
  // tests that are about the gate set or clear it explicitly.
  static const uint8_t* defaultManifestKey() {
    static const uint8_t key[32] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
                                    17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32};
    return key;
  }

  OtaRig()
      : manager(catalog, reader, installer, otaClock, logger) {
    manager.setFirmwareVersion("1.0.0");
    manager.setManifestKey(defaultManifestKey());
    sink2.lines.clear();
  }

  bool logContains(const char* needle) const { return sink2.contains(needle); }
};

std::string makeFirmware(int seed, size_t size) {
  std::string fw;
  fw.reserve(size);
  uint32_t x = static_cast<uint32_t>(seed) * 2654435761u + 1u;
  for (size_t i = 0; i < size; ++i) {
    x = x * 1664525u + 1013904223u;
    fw.push_back(static_cast<char>((x >> 16) & 0xFF));
  }
  return fw;
}

// Signs a release the way the central does: HMAC-SHA256 keyed with the 32-byte manifest
// key, over "version|sha256|url|totalSize". One implementation, so a fixture and the
// cross-language vector cannot drift apart by being written twice.
void signRelease(OtaRelease& r) {
   static const uint8_t* key = OtaRig::defaultManifestKey();
   char canonical[256];
   std::snprintf(canonical, sizeof(canonical), "%s|%s|%s|%u", r.version,
                 r.sha256Hex, r.url, static_cast<unsigned>(r.totalSize));
   uint8_t mac[32];
   cauce::hmacSha256(key, 32, reinterpret_cast<const uint8_t*>(canonical),
                     std::strlen(canonical), mac);
   static const char* hex = "0123456789abcdef";
   for (int i = 0; i < 32; ++i) {
      r.manifestHmacHex[i * 2] = hex[(mac[i] >> 4) & 0xF];
      r.manifestHmacHex[i * 2 + 1] = hex[mac[i] & 0xF];
   }
   r.manifestHmacHex[64] = '\0';
}
// Produces a complete, correctly signed release. It used to clear the signature instead,
// which was harmless while the manager only verified when a key happened to be configured
// and became a gate failure once it did not. A fixture whose name is "fill" should hand
// back something the manager accepts; the tests that want a bad signature overwrite it.
void fillRelease(OtaRelease& r, const std::string& fw) {
  copyString(r.version, sizeof(r.version), "1.1.0");
  r.manifestHmacHex[0] = '\0';
  uint8_t digest[32];
  sha256(reinterpret_cast<const uint8_t*>(fw.data()), fw.size(), digest);
  static const char* hex = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) {
    r.sha256Hex[i * 2] = hex[(digest[i] >> 4) & 0xF];
    r.sha256Hex[i * 2 + 1] = hex[digest[i] & 0xF];
  }
  r.sha256Hex[64] = '\0';
   copyString(r.url, sizeof(r.url), "mem://fw.bin");
   r.totalSize = static_cast<uint32_t>(fw.size());
   signRelease(r);
   }


void drainOta(OtaManager& mgr, hal::ManualClock& clk) {
  int guard = 0;
  while (mgr.state() == OtaState::Downloading && guard++ < 1000) {
    clk.advanceMs(10);
    mgr.tick();
  }
}

}  // namespace

void test_compare_semver_pairs() {
  TEST_ASSERT_EQUAL_INT(0, compareSemver("1.2.3", "1.2.3"));
  TEST_ASSERT_EQUAL_INT(1, compareSemver("1.2.4", "1.2.3"));
  TEST_ASSERT_EQUAL_INT(-1, compareSemver("1.2.3", "1.10.0"));
  TEST_ASSERT_EQUAL_INT(1, compareSemver("2.0.0", "1.9.9"));
  TEST_ASSERT_EQUAL_INT(1, compareSemver("1.2.10", "1.2.9"));
  TEST_ASSERT_EQUAL_INT(-1, compareSemver("basura", "1.0.0"));
  TEST_ASSERT_EQUAL_INT(0, compareSemver("x", "y"));
}

void test_sha256_streaming_matches_one_shot() {
  const std::string fw = makeFirmware(7, 1000);
  uint8_t oneShot[32];
  sha256(reinterpret_cast<const uint8_t*>(fw.data()), fw.size(), oneShot);

  Sha256Ctx ctx;
  sha256Begin(&ctx);
  size_t pos = 0;
  while (pos < fw.size()) {
    const size_t n = std::min<size_t>(137, fw.size() - pos);
    sha256Append(&ctx, reinterpret_cast<const uint8_t*>(fw.data() + pos), n);
    pos += n;
  }
  uint8_t streamed[32];
  sha256Finish(&ctx, streamed);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(oneShot, streamed, 32);
}

void test_happy_path_applies_and_reports_reboot_pending() {
  otaClock = hal::ManualClock(1787356800000ULL);
  OtaRig rig;
  const std::string fw = makeFirmware(42, 3000);
  fillRelease(rig.catalog.release, fw);
  rig.catalog.hasRelease = true;
  rig.reader.payload = fw;

  rig.manager.tick();
  drainOta(rig.manager, otaClock);

  TEST_ASSERT_EQUAL(OtaState::RebootPending, rig.manager.state());
  TEST_ASSERT_EQUAL_STRING("1.1.0", rig.manager.pendingVersion());
  TEST_ASSERT_TRUE(rig.installer.begun);
  TEST_ASSERT_FALSE(rig.installer.aborted);
  TEST_ASSERT_EQUAL_UINT32(3000, rig.installer.received.size());
  TEST_ASSERT_EQUAL_INT(1, rig.catalog.fetchCalls);
}
void test_hash_mismatch_aborts_and_marks_verify_failed() {
  otaClock = hal::ManualClock(1787356800000ULL);
  OtaRig rig;
  const std::string fw = makeFirmware(1, 2000);
  fillRelease(rig.catalog.release, fw);
  copyString(rig.catalog.release.sha256Hex,
             sizeof(rig.catalog.release.sha256Hex),
             "0000000000000000000000000000000000000000000000000000000000000000");
  // Re-signed after the mutation. The point of this test is a hash mismatch discovered
  // after the download, so the manifest gate has to let it through - which means the
  // signature must cover the wrong hash, not the original one. Without the re-sign this
  // fails at the gate and never reaches the check it exists to exercise.
  signRelease(rig.catalog.release);
  rig.catalog.hasRelease = true;
  rig.reader.payload = fw;

  rig.manager.tick();
  drainOta(rig.manager, otaClock);
  TEST_ASSERT_EQUAL(OtaState::VerifyFailed, rig.manager.state());
  TEST_ASSERT_TRUE(rig.installer.aborted);
}

void test_size_exceeded_is_verify_failed() {
  otaClock = hal::ManualClock(1787356800000ULL);
  OtaRig rig;
  const std::string fw = makeFirmware(3, 1000);
  fillRelease(rig.catalog.release, makeFirmware(3, 900));
  rig.catalog.hasRelease = true;
  rig.reader.payload = fw;

  rig.manager.tick();
  drainOta(rig.manager, otaClock);
  TEST_ASSERT_EQUAL(OtaState::VerifyFailed, rig.manager.state());
  TEST_ASSERT_TRUE(rig.installer.aborted);
}

void test_same_version_stays_up_to_date() {
  otaClock = hal::ManualClock(1787356800000ULL);
  OtaRig rig;
  const std::string fw = makeFirmware(5, 500);
  fillRelease(rig.catalog.release, fw);
  copyString(rig.catalog.release.version, sizeof(rig.catalog.release.version),
             "1.0.0");
  // Re-signed after changing the version, or the gate rejects it and this stops being a
  // test about "already on this version" and becomes a test about a bad signature.
  signRelease(rig.catalog.release);
  rig.catalog.hasRelease = true;

  rig.manager.tick();
  TEST_ASSERT_EQUAL(OtaState::UpToDate, rig.manager.state());
  TEST_ASSERT_EQUAL(0, rig.reader.openCalls);
}

void test_no_release_is_up_to_date() {
  otaClock = hal::ManualClock(1787356800000ULL);
  OtaRig rig;
  rig.manager.tick();
  TEST_ASSERT_EQUAL(OtaState::UpToDate, rig.manager.state());
  TEST_ASSERT_EQUAL(OtaState::UpToDate, rig.manager.state());
}

void test_installer_rejection_is_verify_failed_with_abort() {
  otaClock = hal::ManualClock(1787356800000ULL);
  OtaRig rig;
  const std::string fw = makeFirmware(9, 800);
  fillRelease(rig.catalog.release, fw);
  rig.catalog.hasRelease = true;
  rig.reader.payload = fw;
  rig.installer.finishDecision = InstallDecision::Abort;

  rig.manager.tick();
  drainOta(rig.manager, otaClock);
  TEST_ASSERT_EQUAL(OtaState::VerifyFailed, rig.manager.state());
  TEST_ASSERT_TRUE(rig.installer.aborted);
}

void test_safety_gate_blocks_before_download() {
  otaClock = hal::ManualClock(1787356800000ULL);
  OtaRig rig;
  const std::string fw = makeFirmware(11, 700);
  fillRelease(rig.catalog.release, fw);
  rig.catalog.hasRelease = true;
  rig.manager.setSafetyHooks([]() { return 1024u; },
                             []() { return 4.0f; });

  rig.manager.tick();
  TEST_ASSERT_EQUAL(OtaState::CheckFailed, rig.manager.state());
  TEST_ASSERT_EQUAL(0, rig.reader.openCalls);
}

void test_low_battery_blocks_update() {
  otaClock = hal::ManualClock(1787356800000ULL);
  OtaRig rig;
  const std::string fw = makeFirmware(13, 600);
  fillRelease(rig.catalog.release, fw);
  rig.catalog.hasRelease = true;
  rig.manager.setSafetyHooks(nullptr, []() { return 3.1f; });
  rig.manager.setMinBatteryV(3.3f);
  rig.manager.setInterval(3600);

  rig.manager.tick();
  TEST_ASSERT_EQUAL(OtaState::CheckFailed, rig.manager.state());
  TEST_ASSERT_EQUAL(0, rig.reader.openCalls);
}



void test_manifest_signature_gate() {
  otaClock = hal::ManualClock(1787356800000ULL);
  OtaRig rig;
  const std::string fw = makeFirmware(21, 900);
  rig.manager.setFirmwareVersion("1.0.0");
  const uint8_t key[32] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
                           17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32};
  rig.manager.setManifestKey(key);

  // sin firma -> rechazado
  fillRelease(rig.catalog.release, fw);
  copyString(rig.catalog.release.manifestHmacHex,
             sizeof(rig.catalog.release.manifestHmacHex), "");
  rig.catalog.hasRelease = true;
  rig.reader.payload = fw;
  rig.manager.tick();
  TEST_ASSERT_EQUAL(OtaState::CheckFailed, rig.manager.state());
  TEST_ASSERT_EQUAL(0, rig.reader.openCalls);

  // firma invalida -> rechazado
  fillRelease(rig.catalog.release, fw);
  copyString(rig.catalog.release.manifestHmacHex,
             sizeof(rig.catalog.release.manifestHmacHex),
             "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef");
  otaClock.advanceMs(21600001);
  rig.manager.tick();
  TEST_ASSERT_EQUAL(OtaState::CheckFailed, rig.manager.state());

  // firma valida sobre "version|sha256|url|totalSize" -> descarga y aplica
  fillRelease(rig.catalog.release, fw);
  char canonical[256];
  std::snprintf(canonical, sizeof(canonical), "%s|%s|%s|%u",
                rig.catalog.release.version, rig.catalog.release.sha256Hex,
                rig.catalog.release.url,
                static_cast<unsigned>(rig.catalog.release.totalSize));
  uint8_t mac[32];
  cauce::hmacSha256(key, sizeof(key),
                    reinterpret_cast<const uint8_t*>(canonical),
                    std::strlen(canonical), mac);
  char sig[65];
  static const char* hd = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) {
    sig[i * 2] = hd[(mac[i] >> 4) & 0xF];
    sig[i * 2 + 1] = hd[mac[i] & 0xF];
  }
  sig[64] = 0;
  copyString(rig.catalog.release.manifestHmacHex,
             sizeof(rig.catalog.release.manifestHmacHex), sig);
  otaClock.advanceMs(21600001);
  rig.manager.tick();
  drainOta(rig.manager, otaClock);
  TEST_ASSERT_EQUAL(OtaState::RebootPending, rig.manager.state());
}

// The shared cross-language vector. backend/tests/test_api.py::
// test_the_manifest_signature_matches_the_firmware_known_answer asserts the same hex from
// the same inputs on the Python side.
//
// It exists because the two implementations disagreed twice over - the firmware keyed the
// HMAC with SHA-256(device_key) while the central used the ASCII key, and the firmware
// signed version|sha256|url|totalSize while the central signed version|url|totalSize - so
// no node could ever accept a manifest. Nothing in either suite could see it, because the
// existing test built its own signature with its own key and was self-consistent, and the
// real failure mode is refusing an update, which looks like a node that is simply not
// being offered one.
void test_manifest_signature_matches_the_central() {
   OtaRig rig;
   const std::string fw = makeFirmware(21, 900);
   rig.manager.setFirmwareVersion("1.0.0");
   const uint8_t key[32] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
                            17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32};
   rig.manager.setManifestKey(key);

   // device_key "0123456789abcdef" -> SHA-256 as 32 raw bytes is the manifest key.
   // The firmware derives the manifest key as SHA-256(device_key) over 32 raw bytes; the
   // central must do the same or the HMACs are computed under different keys.
   uint8_t derived[32];
   sha256(reinterpret_cast<const uint8_t*>("0123456789abcdef"), 16, derived);

   // Release fields the central used for the shared vector.
   OtaRelease release{};
   copyString(release.version, sizeof(release.version), "1.2.3");
   // Built rather than typed: a hand-counted run of "ab" is 31 pairs often enough to
   // matter, and the failure mode is a silent cross-language disagreement.
   for (int i = 0; i < 32; ++i) {
      release.sha256Hex[i * 2] = 'a';
      release.sha256Hex[i * 2 + 1] = 'b';
   }
   release.sha256Hex[64] = '\0';
   copyString(release.url, sizeof(release.url), "http://central/v1/fw.bin");
   release.totalSize = 12345;

   char canonical[256];
   std::snprintf(canonical, sizeof(canonical), "%s|%s|%s|%u", release.version,
                 release.sha256Hex, release.url,
                 static_cast<unsigned>(release.totalSize));
   uint8_t mac[32];
   cauce::hmacSha256(derived, sizeof(derived),
                     reinterpret_cast<const uint8_t*>(canonical),
                     std::strlen(canonical), mac);
   static const char* hexDigits = "0123456789abcdef";
   char sig[65];
   for (int i = 0; i < 32; ++i) {
      sig[i * 2] = hexDigits[(mac[i] >> 4) & 0xF];
      sig[i * 2 + 1] = hexDigits[mac[i] & 0xF];
   }
   sig[64] = 0;
   TEST_ASSERT_EQUAL_STRING(
      "424f819f01f1b31837e7318f7d9c944e971c8c42178dbaa615d97a0ef87e3721", sig);
}

// A node that cannot verify a signature must not update. This used to be the other way
// round: the signature was only checked when a key happened to be configured, so OTA with
// no device key accepted every manifest it was offered.
void test_a_node_without_a_manifest_key_refuses_to_update() {
   OtaRig rig;
   const std::string fw = makeFirmware(21, 900);
   // Explicitly unsigned: that is the state under test.
   rig.manager.clearManifestKey();
   fillRelease(rig.catalog.release, fw);
   copyString(rig.catalog.release.manifestHmacHex,
              sizeof(rig.catalog.release.manifestHmacHex), "");
   rig.catalog.hasRelease = true;
   rig.reader.payload = fw;
   otaClock.advanceMs(21600001);
   rig.manager.tick();

   TEST_ASSERT_EQUAL(OtaState::CheckFailed, rig.manager.state());
   TEST_ASSERT_EQUAL(0, rig.reader.openCalls);
   TEST_ASSERT_TRUE_MESSAGE(rig.logContains("OTA_NO_MANIFEST_KEY"),
                            "a node with no manifest key must say why it refused");
}
void test_manifest_json_full_parse();
void test_manifest_json_missing_sha_rejected();
void test_manifest_json_truncated_rejected();
void test_manifest_json_rejects_short_sha_and_non_http_url();

void test_reading_slowly_still_completes() {
  otaClock = hal::ManualClock(1787356800000ULL);
  OtaRig rig;
  const std::string fw = makeFirmware(42, 3000);
  fillRelease(rig.catalog.release, fw);
  rig.catalog.hasRelease = true;
  rig.reader.payload = fw;
  rig.reader.stallEvery = 3;
  rig.manager.setMaxStallTicks(50);

  rig.manager.tick();
  TEST_ASSERT_EQUAL(OtaState::Downloading, rig.manager.state());
  for (int i = 0; i < 4000 && rig.manager.state() == OtaState::Downloading; ++i) {
    rig.manager.tick();
    otaClock.advanceMs(10);
  }
  TEST_ASSERT_EQUAL(OtaState::RebootPending, rig.manager.state());
  TEST_ASSERT_EQUAL(0, rig.manager.stallTicks());
}

void test_reader_that_never_delivers_aborts_as_stalled() {
  otaClock = hal::ManualClock(1787356800000ULL);
  OtaRig rig;
  const std::string fw = makeFirmware(42, 3000);
  fillRelease(rig.catalog.release, fw);
  rig.catalog.hasRelease = true;
  rig.reader.payload = fw;
  rig.reader.alwaysStall = true;
  rig.manager.setMaxStallTicks(5);

  rig.manager.tick();
  TEST_ASSERT_EQUAL(OtaState::Downloading, rig.manager.state());
  for (int i = 0; i < 20 && rig.manager.state() == OtaState::Downloading; ++i) {
    rig.manager.tick();
    otaClock.advanceMs(10);
  }
  TEST_ASSERT_EQUAL(OtaState::VerifyFailed, rig.manager.state());
  TEST_ASSERT_TRUE(rig.logContains("OTA_STALLED"));
  TEST_ASSERT_EQUAL(0, rig.installer.received.size());
}

void test_zero_stall_budget_is_clamped() {
  otaClock = hal::ManualClock(1787356800000ULL);
  OtaRig rig;
  rig.manager.setMaxStallTicks(0);
  const std::string fw = makeFirmware(42, 1000);
  fillRelease(rig.catalog.release, fw);
  rig.catalog.hasRelease = true;
  rig.reader.payload = fw;
  rig.reader.alwaysStall = true;

  rig.manager.tick();
  for (int i = 0; i < 5 && rig.manager.state() == OtaState::Downloading; ++i) {
    rig.manager.tick();
    otaClock.advanceMs(10);
  }
  TEST_ASSERT_EQUAL(OtaState::VerifyFailed, rig.manager.state());
}

void registerOtaTests() {
  UNITY_BEGIN();
  RUN_TEST(test_compare_semver_pairs);
  RUN_TEST(test_sha256_streaming_matches_one_shot);
  RUN_TEST(test_happy_path_applies_and_reports_reboot_pending);
  RUN_TEST(test_hash_mismatch_aborts_and_marks_verify_failed);
  RUN_TEST(test_size_exceeded_is_verify_failed);
  RUN_TEST(test_same_version_stays_up_to_date);
  RUN_TEST(test_no_release_is_up_to_date);
  RUN_TEST(test_installer_rejection_is_verify_failed_with_abort);
  RUN_TEST(test_safety_gate_blocks_before_download);
  RUN_TEST(test_low_battery_blocks_update);
  RUN_TEST(test_manifest_signature_gate);
  RUN_TEST(test_manifest_signature_matches_the_central);
  RUN_TEST(test_a_node_without_a_manifest_key_refuses_to_update);
  RUN_TEST(test_manifest_json_full_parse);
  RUN_TEST(test_manifest_json_missing_sha_rejected);
  RUN_TEST(test_manifest_json_truncated_rejected);
  RUN_TEST(test_manifest_json_rejects_short_sha_and_non_http_url);
  RUN_TEST(test_reading_slowly_still_completes);
  RUN_TEST(test_reader_that_never_delivers_aborts_as_stalled);
  RUN_TEST(test_zero_stall_budget_is_clamped);
  UNITY_END();
}

void test_manifest_json_full_parse() {
  OtaRelease out{};
  const char* json =
      "{\"version\":\"1.2.3\","
      "\"sha256\":\"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\","
      "\"url\":\"http://x/fw.bin\",\"total_size\":12345,"
      "\"hmac\":\"0011223344556677\"}";
  TEST_ASSERT_TRUE(parseOtaManifestJson(json, out));
  TEST_ASSERT_EQUAL_STRING("1.2.3", out.version);
  TEST_ASSERT_EQUAL_STRING(
      "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
      out.sha256Hex);
  TEST_ASSERT_EQUAL_STRING("http://x/fw.bin", out.url);
  TEST_ASSERT_EQUAL_UINT32(12345, out.totalSize);
  TEST_ASSERT_EQUAL_STRING("0011223344556677", out.manifestHmacHex);
}

void test_manifest_json_missing_sha_rejected() {
  OtaRelease out{};
  const char* json =
      "{\"version\":\"1.2.3\",\"url\":\"http://x/fw.bin\",\"total_size\":10}";
  TEST_ASSERT_FALSE(parseOtaManifestJson(json, out));
}

void test_manifest_json_truncated_rejected() {
  OtaRelease out{};
  TEST_ASSERT_FALSE(parseOtaManifestJson("{\"version\":\"1.2.3\"", out));
  TEST_ASSERT_FALSE(parseOtaManifestJson(nullptr, out));
}

void test_manifest_json_rejects_short_sha_and_non_http_url() {
  OtaRelease out{};
  const char* shortSha =
      "{\"version\":\"1.2.3\",\"sha256\":\"abcdef\","
      "\"url\":\"http://x/fw.bin\",\"total_size\":10}";
  TEST_ASSERT_FALSE(parseOtaManifestJson(shortSha, out));
  const char* fileUrl =
      "{\"version\":\"1.2.3\","
      "\"sha256\":\"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\","
      "\"url\":\"file:///etc/fw.bin\",\"total_size\":10}";
  TEST_ASSERT_FALSE(parseOtaManifestJson(fileUrl, out));
}

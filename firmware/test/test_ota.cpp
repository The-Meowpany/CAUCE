#include <cstring>
#include <string>
#include <vector>

#include <unity.h>

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
  size_t read(uint8_t* buffer, size_t capacity) override {
    if (pos >= payload.size()) return 0;
    const size_t n = std::min(capacity, payload.size() - pos);
    std::memcpy(buffer, payload.data() + pos, n);
    pos += n;
    return n;
  }
  void close() override { isOpen = false; }
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
  void writeLine(const char*) override {}
} sink2;

struct OtaRig {
  FakeCatalog catalog;
  FakeReader reader;
  FakeInstaller installer;
  Logger logger{sink2};
  OtaManager manager;

  OtaRig()
      : manager(catalog, reader, installer, otaClock, logger) {
    manager.setFirmwareVersion("1.0.0");
  }
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

void fillRelease(OtaRelease& r, const std::string& fw) {
  copyString(r.version, sizeof(r.version), "1.1.0");
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

  TEST_ASSERT_EQUAL(OtaState::RebootPending, rig.manager.state());
  TEST_ASSERT_EQUAL_STRING("1.1.0", rig.manager.pendingVersion());
  TEST_ASSERT_TRUE(rig.installer.begun);
  TEST_ASSERT_FALSE(rig.installer.aborted);
  TEST_ASSERT_EQUAL_UINT32(3000, rig.installer.received.size());
}
void test_hash_mismatch_aborts_and_marks_verify_failed() {
  otaClock = hal::ManualClock(1787356800000ULL);
  OtaRig rig;
  const std::string fw = makeFirmware(1, 2000);
  fillRelease(rig.catalog.release, fw);
  copyString(rig.catalog.release.sha256Hex,
             sizeof(rig.catalog.release.sha256Hex),
             "0000000000000000000000000000000000000000000000000000000000000000");
  rig.catalog.hasRelease = true;
  rig.reader.payload = fw;

  rig.manager.tick();

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
  UNITY_END();
}

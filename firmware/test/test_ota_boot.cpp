#include <cstring>
#include <unity.h>

#include "cauce/app/OtaBootConfirm.h"
#include "cauce/hal/MemoryFileSystem.h"

using cauce::hal::MemoryFileSystem;
using namespace cauce::app;

namespace {

class FakeOtaControl final : public IOtaControl {
 public:
  bool pending{true};
  bool markValidOk{true};
  bool rollbackOk{true};
  int markValidCalls{0};
  int rollbackCalls{0};

  bool isPendingVerify() const override { return pending; }
  bool markAppValid() override {
    ++markValidCalls;
    if (markValidOk) pending = false;
    return markValidOk;
  }
  bool requestRollback() override {
    ++rollbackCalls;
    return rollbackOk;
  }
  bool selfTestPassed() const override { return false; }
};

class CapturingSink final : public cauce::ILogSink {
 public:
  int lines{0};
  char last[256]{};
  void writeLine(const char* line) override {
    ++lines;
    const char* src = line;
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < sizeof(last); ++i) last[i] = src[i];
    last[i] = '\0';
  }
  bool saw(const char* needle) const {
    return std::strstr(last, needle) != nullptr;
  }
};

BootSelfTest healthy() {
  BootSelfTest st;
  st.storageWritable = true;
  st.measurementsStored = 3;
  st.uptimeMs = 60000;
  return st;
}

BootSelfTest broken() {
  BootSelfTest st;
  st.storageWritable = false;
  st.uptimeMs = 60000;
  return st;
}

}  // namespace

void test_boot_confirm_normal_boot_touches_nothing() {
  FakeOtaControl control;
  control.pending = false;
  MemoryFileSystem fs;
  CapturingSink sink;
  cauce::Logger logger(sink);
  OtaBootConfirm confirm(control, fs, logger);
  confirm.loadAttempts();

  TEST_ASSERT_TRUE(confirm.tick(healthy()));
  TEST_ASSERT_EQUAL(static_cast<int>(BootVerdict::NotPending),
                    static_cast<int>(confirm.evaluate(healthy())));
  TEST_ASSERT_EQUAL(0, control.markValidCalls);
  TEST_ASSERT_EQUAL(0, control.rollbackCalls);
  TEST_ASSERT_FALSE(fs.exists("/state/ota_boot"));
}

void test_boot_confirm_marks_valid_after_a_stored_measurement() {
  FakeOtaControl control;
  MemoryFileSystem fs;
  CapturingSink sink;
  cauce::Logger logger(sink);
  OtaBootConfirm confirm(control, fs, logger);
  confirm.loadAttempts();

  TEST_ASSERT_TRUE(confirm.tick(healthy()));
  TEST_ASSERT_EQUAL(1, control.markValidCalls);
  TEST_ASSERT_FALSE(control.pending);
  TEST_ASSERT_TRUE(confirm.settled());
  TEST_ASSERT_TRUE(sink.saw("OTA_BOOT_CONFIRMED"));
}

void test_boot_confirm_stays_pending_while_the_image_looks_broken() {
  FakeOtaControl control;
  MemoryFileSystem fs;
  CapturingSink sink;
  cauce::Logger logger(sink);
  OtaBootConfirm confirm(control, fs, logger, "/state/ota_boot", 3);
  confirm.loadAttempts();

  TEST_ASSERT_TRUE(confirm.tick(broken()));
  TEST_ASSERT_EQUAL(0, control.markValidCalls);
  TEST_ASSERT_EQUAL(0, control.rollbackCalls);
  TEST_ASSERT_TRUE(control.pending);
  TEST_ASSERT_EQUAL(1u, confirm.attempts());
  TEST_ASSERT_TRUE(sink.saw("OTA_BOOT_ATTEMPT"));
}

void test_boot_confirm_counter_survives_a_reboot() {
  MemoryFileSystem fs;
  {
    FakeOtaControl control;
    CapturingSink sink;
    cauce::Logger logger(sink);
    OtaBootConfirm confirm(control, fs, logger, "/state/ota_boot", 3);
    confirm.loadAttempts();
    confirm.tick(broken());
    TEST_ASSERT_EQUAL(1u, confirm.attempts());
  }
  // Fresh object on the same flash: this is the reboot the counter exists for.
  {
    FakeOtaControl control;
    CapturingSink sink;
    cauce::Logger logger(sink);
    OtaBootConfirm confirm(control, fs, logger, "/state/ota_boot", 3);
    confirm.loadAttempts();
    TEST_ASSERT_EQUAL(1u, confirm.attempts());
    confirm.tick(broken());
    TEST_ASSERT_EQUAL(2u, confirm.attempts());
    TEST_ASSERT_EQUAL(0, control.rollbackCalls);
  }
  {
    FakeOtaControl control;
    CapturingSink sink;
    cauce::Logger logger(sink);
    OtaBootConfirm confirm(control, fs, logger, "/state/ota_boot", 3);
    confirm.loadAttempts();
    TEST_ASSERT_EQUAL(2u, confirm.attempts());
    confirm.tick(broken());
    TEST_ASSERT_EQUAL(1, control.rollbackCalls);
    TEST_ASSERT_TRUE(sink.saw("OTA_BOOT_ROLLBACK"));
  }
}

void test_boot_confirm_clears_the_counter_once_confirmed() {
  FakeOtaControl control;
  MemoryFileSystem fs;
  CapturingSink sink;
  cauce::Logger logger(sink);

  OtaBootConfirm first(control, fs, logger, "/state/ota_boot", 3);
  first.loadAttempts();
  first.tick(broken());
  first.tick(healthy());
  TEST_ASSERT_EQUAL(1, control.markValidCalls);
  TEST_ASSERT_EQUAL(0u, first.attempts());

  // A settled image must not leave a stale attempt count behind, or a later
  // unrelated crash would inherit it.
  FakeOtaControl rebooted;
  rebooted.pending = false;
  OtaBootConfirm second(rebooted, fs, logger, "/state/ota_boot", 3);
  second.loadAttempts();
  TEST_ASSERT_TRUE(second.tick(healthy()));
  TEST_ASSERT_EQUAL(0u, second.attempts());
}

void test_boot_confirm_self_test_prefers_a_measurement_then_falls_back_to_grace() {
  FakeOtaControl control;
  MemoryFileSystem fs;
  CapturingSink sink;
  cauce::Logger logger(sink);
  OtaBootConfirm confirm(control, fs, logger);

  BootSelfTest noSensor;
  noSensor.storageWritable = true;
  noSensor.uptimeMs = 1000;
  TEST_ASSERT_FALSE(confirm.selfTestPassed(noSensor));

  noSensor.uptimeMs = 400000;
  TEST_ASSERT_TRUE(confirm.selfTestPassed(noSensor));

  BootSelfTest readFailures = healthy();
  readFailures.measurementsStored = 0;
  readFailures.uptimeMs = 400000;
  readFailures.storageFailures = 1;
  TEST_ASSERT_FALSE(confirm.selfTestPassed(readFailures));

  BootSelfTest notWritable = healthy();
  notWritable.storageWritable = false;
  TEST_ASSERT_FALSE(confirm.selfTestPassed(notWritable));
}

void test_boot_confirm_reports_a_failed_flash_write() {
  FakeOtaControl control;
  MemoryFileSystem fs;
  CapturingSink sink;
  cauce::Logger logger(sink);
  control.markValidOk = false;
  OtaBootConfirm confirm(control, fs, logger);
  confirm.loadAttempts();

  TEST_ASSERT_FALSE(confirm.tick(healthy()));
  TEST_ASSERT_EQUAL(1, control.markValidCalls);
  TEST_ASSERT_TRUE(sink.saw("OTA_BOOT_APPLY_FAILED"));
}

void test_boot_confign_accepts_an_empty_state_path() {
  FakeOtaControl control;
  control.pending = false;
  MemoryFileSystem fs;
  CapturingSink sink;
  cauce::Logger logger(sink);
  OtaBootConfirm confirm(control, fs, logger, "");
  confirm.loadAttempts();
  TEST_ASSERT_EQUAL(0u, confirm.attempts());
  TEST_ASSERT_TRUE(confirm.tick(healthy()));
}

void registerOtaBootConfirmTests() {
  RUN_TEST(test_boot_confirm_normal_boot_touches_nothing);
  RUN_TEST(test_boot_confirm_marks_valid_after_a_stored_measurement);
  RUN_TEST(test_boot_confirm_stays_pending_while_the_image_looks_broken);
  RUN_TEST(test_boot_confirm_counter_survives_a_reboot);
  RUN_TEST(test_boot_confirm_clears_the_counter_once_confirmed);
  RUN_TEST(test_boot_confirm_self_test_prefers_a_measurement_then_falls_back_to_grace);
  RUN_TEST(test_boot_confirm_reports_a_failed_flash_write);
  RUN_TEST(test_boot_confign_accepts_an_empty_state_path);
}

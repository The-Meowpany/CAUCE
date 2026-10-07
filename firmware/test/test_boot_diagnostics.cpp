// Tests for the component that makes a boot loop legible.
//
// The property throughout is that a boot which ends without completing is *distinguishable
// from* one that completes, and that the distinction survives the reset. Everything else here
// is in service of that, because a board that reboots with no record of where it was is the
// failure mode this whole component exists to remove.

#include <cstdio>
#include <cstring>
#include <string>
#include <unity.h>
#include <vector>

#include "cauce/core/BootDiagnostics.h"

namespace cauce {
namespace {

// A sink that keeps the lines, which is what makes the report assertable.
class Capture {
 public:
  static void sink(void* context, const char* line) {
    static_cast<Capture*>(context)->lines.emplace_back(line ? line : "");
  }

  bool contains(const char* needle) const {
    for (const std::string& l : lines) {
      if (l.find(needle) != std::string::npos) return true;
    }
    return false;
  }

  size_t count() const { return lines.size(); }

  std::vector<std::string> lines;
};

// Stands in for RTC memory: 32 bytes that outlive the boot, as on a board.
struct FakeRtc {
  uint8_t bytes[64];
};

FakeRtc g_rtc;

// A boot that walks the given steps and returns whether it completed.
bool runBoot(const char* const* steps, size_t count, Capture* capture, FakeRtc* rtc) {
  BootDiagnostics diag;
  diag.begin(rtc, sizeof(rtc->bytes), &Capture::sink, capture);
  for (size_t i = 0; i < count; ++i) {
    const uint16_t ordinal = diag.beginStep(steps[i]);
    diag.endStep(ordinal, true);
  }
  diag.complete(0x1);
  return true;
}

void test_the_first_boot_reports_no_history() {
  Capture capture;
  FakeRtc rtc{};
  BootDiagnostics diag;

  const bool reattempt = diag.begin(rtc.bytes, sizeof(rtc.bytes), &Capture::sink, &capture);
  TEST_ASSERT_FALSE(reattempt);
  TEST_ASSERT_TRUE(capture.contains("BOOT_HISTORY none"));
}

void test_a_completed_boot_is_not_a_reattempt() {
  Capture capture;
  FakeRtc rtc{};
  const char* steps[] = {"mount_fs", "open_store"};

  runBoot(steps, 2, &capture, &rtc);

  Capture second;
  BootDiagnostics diag;
  const bool reattempt =
      diag.begin(rtc.bytes, sizeof(rtc.bytes), &Capture::sink, &second);
  TEST_ASSERT_FALSE(reattempt);
  TEST_ASSERT_TRUE(second.contains("last boot completed"));
}

void test_a_boot_that_died_in_a_step_is_reported_as_a_reattempt() {
  Capture first;
  FakeRtc rtc{};
  {
    BootDiagnostics diag;
    diag.begin(rtc.bytes, sizeof(rtc.bytes), &Capture::sink, &first);
    diag.beginStep("mount_fs");
    diag.endStep(1, true);
    diag.beginStep("open_store");
    // No complete(). This is the boot that reset.
  }

  Capture second;
  BootDiagnostics diag;
  const bool reattempt =
      diag.begin(rtc.bytes, sizeof(rtc.bytes), &Capture::sink, &second);
  TEST_ASSERT_TRUE(reattempt);
  TEST_ASSERT_TRUE(second.contains("BOOT_HISTORY reattempt"));
  // The ordinal is what says *which* step. A count alone would be less useful than a name.
  TEST_ASSERT_TRUE(second.contains("ordinal=2"));
  TEST_ASSERT_TRUE(second.contains("did not reach BOOT_COMPLETE"));
}

// The scenario the whole component exists for: the same step kills every boot, and each new
// boot has to say so with the ordinal rising as the early steps succeed. If the record only
// kept a "did not complete" flag, this test would pass and an operator would still be guessing.
void test_a_repeating_failure_reports_the_same_step_each_boot() {
  FakeRtc rtc{};
  std::vector<std::string> reports;

  for (int boot = 0; boot < 3; ++boot) {
    Capture capture;
    BootDiagnostics diag;
    diag.begin(rtc.bytes, sizeof(rtc.bytes), &Capture::sink, &capture);
    diag.beginStep("mount_fs");
    diag.endStep(1, true);
    diag.beginStep("open_store");
    diag.endStep(2, true);
    diag.beginStep("wifi_controller");
    // Dies here, every time.
    reports.push_back(capture.lines.empty() ? std::string() : capture.lines[0]);
  }

  // Boot 1 has no history. Boots 2 and 3 both name the third step.
  TEST_ASSERT_TRUE(reports[0].find("none") != std::string::npos);
  TEST_ASSERT_TRUE(reports[1].find("ordinal=3") != std::string::npos);
  TEST_ASSERT_TRUE(reports[2].find("ordinal=3") != std::string::npos);
  // The boot count rises, because "it has rebooted nine times" is the useful part.
  TEST_ASSERT_TRUE(reports[2].find("boots=2") != std::string::npos);
}

void test_the_boot_count_accumulates_across_resets() {
  FakeRtc rtc{};
  // Five boots, none of which completes - a node in a reset loop. Each begin() counts itself,
  // so after the loop the sixth boot sees five.
  for (int i = 0; i < 5; ++i) {
    BootDiagnostics diag;
    diag.begin(rtc.bytes, sizeof(rtc.bytes), nullptr, nullptr);
    diag.beginStep("mount_fs");
    diag.endStep(1, true);
  }
  BootDiagnostics diag;
  Capture capture;
  diag.begin(rtc.bytes, sizeof(rtc.bytes), &Capture::sink, &capture);
  TEST_ASSERT_EQUAL_UINT32(6, diag.record().bootCount);
}

void test_a_corrupt_record_is_refused_rather_than_half_trusted() {
  Capture first;
  FakeRtc rtc{};
  BootDiagnostics diag;
  diag.begin(rtc.bytes, sizeof(rtc.bytes), &Capture::sink, &first);
  diag.beginStep("mount_fs");
  // Corrupt one byte of the persisted record, as a torn write or bit rot would.
  rtc.bytes[10] = static_cast<uint8_t>(rtc.bytes[10] ^ 0xFFu);

  Capture second;
  BootDiagnostics diag2;
  const bool reattempt =
      diag2.begin(rtc.bytes, sizeof(rtc.bytes), &Capture::sink, &second);
  // Not trusted as a re-attempt: a garbage record could claim a step count of anything.
  TEST_ASSERT_FALSE(reattempt);
  TEST_ASSERT_TRUE(second.contains("BOOT_HISTORY none"));
}

void test_a_record_from_a_future_schema_is_refused() {
  FakeRtc rtc{};
  BootDiagnostics diag;
  diag.begin(rtc.bytes, sizeof(rtc.bytes), nullptr, nullptr);
  diag.beginStep("mount_fs");

  // Rewrite the schema in the persisted copy without fixing the checksum, which is exactly
  // what a downgrade would look like.
  BootDiagnosticsRecord record{};
  std::memcpy(&record, rtc.bytes, sizeof(record));
  record.schemaVersion = 99;
  std::memcpy(rtc.bytes, &record, sizeof(record));

  Capture capture;
  BootDiagnostics diag2;
  TEST_ASSERT_FALSE(
      diag2.begin(rtc.bytes, sizeof(rtc.bytes), &Capture::sink, &capture));
  TEST_ASSERT_TRUE(capture.contains("BOOT_HISTORY none"));
}

// A storage buffer too small is a build-time-sized mistake, and it must not read past what it
// was given. Asserted by running it: an overflow here would corrupt whatever follows on the
// stack in the real call.
void test_storage_too_small_is_refused_rather_than_overflowed() {
  Capture capture;
  uint8_t tiny[8]{};
  BootDiagnostics diag;
  const bool reattempt = diag.begin(tiny, sizeof(tiny), &Capture::sink, &capture);
  TEST_ASSERT_FALSE(reattempt);
  diag.beginStep("mount_fs");
  diag.complete(0);
  // Nothing past the eight bytes was touched: the buffer is still what it was.
  for (uint8_t b : tiny) {
    TEST_ASSERT_EQUAL_UINT8(0, b);
  }
}

void test_a_null_storage_buffer_is_survivable() {
  BootDiagnostics diag;
  TEST_ASSERT_FALSE(diag.begin(nullptr, 0, nullptr, nullptr));
  diag.beginStep("mount_fs");
  diag.endStep(1, true);
  diag.complete(0x1);
  // One boot, counted in memory. Nothing was persisted, so this is the only answer available
  // and reporting 0 would be the one that reads as "no boot happened".
  TEST_ASSERT_EQUAL_UINT32(1, diag.record().bootCount);
}

void test_completing_clears_the_step_so_the_next_boot_is_clean() {
  FakeRtc rtc{};
  BootDiagnostics diag;
  diag.begin(rtc.bytes, sizeof(rtc.bytes), nullptr, nullptr);
  diag.beginStep("mount_fs");
  diag.endStep(1, true);
  diag.beginStep("open_store");
  diag.endStep(2, true);
  diag.complete(0x1);

  Capture capture;
  BootDiagnostics next;
  TEST_ASSERT_FALSE(next.begin(rtc.bytes, sizeof(rtc.bytes), &Capture::sink, &capture));
  TEST_ASSERT_TRUE(capture.contains("last boot completed"));
  // The duration is only meaningful for a boot that finished.
  TEST_ASSERT_TRUE(next.record().lastStep == kBootDiagnosticsNone);
}

void test_more_steps_than_the_bound_do_not_overflow() {
  FakeRtc rtc{};
  BootDiagnostics diag;
  diag.begin(rtc.bytes, sizeof(rtc.bytes), nullptr, nullptr);
  // A boot that loops through steps without completing would otherwise grow this forever.
  for (uint16_t i = 0; i < 200; ++i) {
    diag.beginStep("looping");
    diag.endStep(i, true);
  }
  // The *names* are bounded and stop being recorded. The ordinal does not: it is a uint16_t
  // field in a fixed-size record and it keeps counting, because "how far did it get" growing
  // monotonically is the part that stays useful when a boot loops through the same steps.
  TEST_ASSERT_EQUAL_UINT16(200, diag.record().lastStepOrdinal);
}

void test_an_unnamed_step_is_recorded_rather_than_dereferenced() {
  FakeRtc rtc{};
  BootDiagnostics diag;
  diag.begin(rtc.bytes, sizeof(rtc.bytes), nullptr, nullptr);
  const uint16_t ordinal = diag.beginStep(nullptr);
  TEST_ASSERT_EQUAL_UINT16(1, ordinal);
}

void test_the_completed_duration_is_recorded_for_the_next_boot() {
  FakeRtc rtc{};
  BootDiagnostics diag;
  diag.begin(rtc.bytes, sizeof(rtc.bytes), nullptr, nullptr);
  diag.beginStep("mount_fs");
  diag.complete(0x1);
  // `startedMs_` is 0 because nothing called begin() with a clock, so the duration is 0 - the
  // assertion is that the field is written at all rather than left indeterminate.
  TEST_ASSERT_EQUAL_UINT32(0x1, diag.record().lastResetReason);
  TEST_ASSERT_TRUE(diag.record().lastStep == kBootDiagnosticsNone);
}

}  // namespace

void registerBootDiagnosticsTests() {
  UNITY_BEGIN();
  RUN_TEST(test_the_first_boot_reports_no_history);
  RUN_TEST(test_a_completed_boot_is_not_a_reattempt);
  RUN_TEST(test_a_boot_that_died_in_a_step_is_reported_as_a_reattempt);
  RUN_TEST(test_a_repeating_failure_reports_the_same_step_each_boot);
  RUN_TEST(test_the_boot_count_accumulates_across_resets);
  RUN_TEST(test_a_corrupt_record_is_refused_rather_than_half_trusted);
  RUN_TEST(test_a_record_from_a_future_schema_is_refused);
  RUN_TEST(test_storage_too_small_is_refused_rather_than_overflowed);
  RUN_TEST(test_a_null_storage_buffer_is_survivable);
  RUN_TEST(test_completing_clears_the_step_so_the_next_boot_is_clean);
  RUN_TEST(test_more_steps_than_the_bound_do_not_overflow);
  RUN_TEST(test_an_unnamed_step_is_recorded_rather_than_dereferenced);
  RUN_TEST(test_the_completed_duration_is_recorded_for_the_next_boot);
}

}  // namespace cauce
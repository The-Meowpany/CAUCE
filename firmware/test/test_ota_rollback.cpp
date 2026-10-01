#include <unity.h>

#include "cauce/app/OtaRollback.h"

using namespace cauce::app;

namespace {

class FakeOtaControl final : public IOtaControl {
 public:
  bool pending{true};
  bool healthy{false};
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
  bool selfTestPassed() const override { return healthy; }
};

}  // namespace

void test_rollback_guard_ignores_normal_boot() {
  FakeOtaControl control;
  control.pending = false;
  OtaRollbackGuard guard(control);
  TEST_ASSERT_EQUAL(static_cast<int>(BootVerdict::NotPending),
                    static_cast<int>(guard.evaluate(0)));
  TEST_ASSERT_TRUE(guard.apply(BootVerdict::NotPending));
  TEST_ASSERT_EQUAL(0, control.markValidCalls);
  TEST_ASSERT_EQUAL(0, control.rollbackCalls);
}

void test_rollback_guard_marks_valid_when_selftest_passes() {
  FakeOtaControl control;
  control.pending = true;
  control.healthy = true;
  OtaRollbackGuard guard(control);
  const BootVerdict verdict = guard.evaluate(0);
  TEST_ASSERT_EQUAL(static_cast<int>(BootVerdict::MarkValid),
                    static_cast<int>(verdict));
  TEST_ASSERT_TRUE(guard.apply(verdict));
  TEST_ASSERT_EQUAL(1, control.markValidCalls);
  TEST_ASSERT_EQUAL(0, control.rollbackCalls);
  TEST_ASSERT_FALSE(control.pending);
}

void test_rollback_guard_waits_before_rolling_back() {
  FakeOtaControl control;
  control.pending = true;
  control.healthy = false;
  OtaRollbackGuard guard(control, 3);
  TEST_ASSERT_EQUAL(static_cast<int>(BootVerdict::StayPending),
                    static_cast<int>(guard.evaluate(0)));
  TEST_ASSERT_EQUAL(static_cast<int>(BootVerdict::StayPending),
                    static_cast<int>(guard.evaluate(1)));
  TEST_ASSERT_TRUE(guard.apply(BootVerdict::StayPending));
  TEST_ASSERT_EQUAL(0, control.rollbackCalls);
  TEST_ASSERT_TRUE(control.pending);
}

void test_rollback_guard_rolls_back_after_max_attempts() {
  FakeOtaControl control;
  control.pending = true;
  control.healthy = false;
  OtaRollbackGuard guard(control, 3);
  const BootVerdict verdict = guard.evaluate(2);
  TEST_ASSERT_EQUAL(static_cast<int>(BootVerdict::Rollback),
                    static_cast<int>(verdict));
  TEST_ASSERT_TRUE(guard.apply(verdict));
  TEST_ASSERT_EQUAL(1, control.rollbackCalls);
}

void test_rollback_guard_single_attempt_rolls_back_immediately() {
  FakeOtaControl control;
  control.pending = true;
  control.healthy = false;
  OtaRollbackGuard guard(control, 1);
  TEST_ASSERT_EQUAL(1, guard.maxAttempts());
  TEST_ASSERT_EQUAL(static_cast<int>(BootVerdict::Rollback),
                    static_cast<int>(guard.evaluate(0)));
}

void test_rollback_guard_zero_attempts_is_clamped() {
  FakeOtaControl control;
  OtaRollbackGuard guard(control, 0);
  TEST_ASSERT_EQUAL(1, guard.maxAttempts());
}

void test_rollback_guard_reports_hardware_failures() {
  FakeOtaControl control;
  control.pending = true;
  control.healthy = true;
  control.markValidOk = false;
  OtaRollbackGuard guard(control);
  TEST_ASSERT_FALSE(guard.apply(BootVerdict::MarkValid));

  FakeOtaControl other;
  other.pending = true;
  other.healthy = false;
  other.rollbackOk = false;
  OtaRollbackGuard guard2(other, 1);
  TEST_ASSERT_FALSE(guard2.apply(BootVerdict::Rollback));
}

void registerOtaRollbackTests() {
  RUN_TEST(test_rollback_guard_ignores_normal_boot);
  RUN_TEST(test_rollback_guard_marks_valid_when_selftest_passes);
  RUN_TEST(test_rollback_guard_waits_before_rolling_back);
  RUN_TEST(test_rollback_guard_rolls_back_after_max_attempts);
  RUN_TEST(test_rollback_guard_single_attempt_rolls_back_immediately);
  RUN_TEST(test_rollback_guard_zero_attempts_is_clamped);
  RUN_TEST(test_rollback_guard_reports_hardware_failures);
}

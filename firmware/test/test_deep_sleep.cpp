#include <unity.h>

#include "cauce/app/DeepSleepController.h"
#include "cauce/hal/ManualClock.h"

using namespace cauce;
using namespace cauce::app;

namespace {

DeepSleepInputs baseInputs() {
  DeepSleepInputs in;
  in.enabled = true;
  in.samplingIntervalS = 60;
  in.syncIntervalS = 900;
  in.batteryV = 3.9f;
  in.msSinceBoot = 60000;
  in.msSinceLastMeasurement = 60000;
  return in;
}

}  // namespace

void test_deep_sleep_is_off_unless_enabled() {
  DeepSleepController controller;
  DeepSleepInputs in = baseInputs();
  in.enabled = false;
  const SleepPlan plan = controller.evaluate(in);
  TEST_ASSERT_EQUAL(static_cast<int>(SleepDecision::StayAwake),
                    static_cast<int>(plan.decision));
  TEST_ASSERT_EQUAL_STRING("disabled", plan.reason);
}

void test_deep_sleep_sleeps_when_everything_is_quiet() {
  DeepSleepController controller;
  const SleepPlan plan = controller.evaluate(baseInputs());
  TEST_ASSERT_EQUAL(static_cast<int>(SleepDecision::Sleep),
                    static_cast<int>(plan.decision));
  TEST_ASSERT_EQUAL(60u - 17u, plan.sleepS);
  TEST_ASSERT_EQUAL_STRING("ok", plan.reason);
}

void test_deep_sleep_never_while_link_is_up() {
  DeepSleepController controller;
  DeepSleepInputs in = baseInputs();
  in.networkConnected = true;
  const SleepPlan plan = controller.evaluate(in);
  TEST_ASSERT_EQUAL(static_cast<int>(SleepDecision::StayAwake),
                    static_cast<int>(plan.decision));
  TEST_ASSERT_EQUAL_STRING("link_up_stay_awake", plan.reason);
}

void test_deep_sleep_never_while_portal_is_active() {
  DeepSleepController controller;
  DeepSleepInputs in = baseInputs();
  in.portalActive = true;
  TEST_ASSERT_EQUAL_STRING("portal_active", controller.evaluate(in).reason);
}

void test_deep_sleep_never_during_ota() {
  DeepSleepController controller;
  DeepSleepInputs in = baseInputs();
  in.otaBusy = true;
  TEST_ASSERT_EQUAL_STRING("ota_in_progress", controller.evaluate(in).reason);
}

void test_deep_sleep_waits_for_boot_settle() {
  DeepSleepController controller;
  DeepSleepInputs in = baseInputs();
  in.msSinceBoot = 5000;
  in.bootSettleS = 20;
  TEST_ASSERT_EQUAL_STRING("boot_settle", controller.evaluate(in).reason);
  in.msSinceBoot = 25000;
  TEST_ASSERT_EQUAL(static_cast<int>(SleepDecision::Sleep),
                    static_cast<int>(controller.evaluate(in).decision));
}

void test_deep_sleep_waits_until_the_sample_is_due() {
  DeepSleepController controller;
  DeepSleepInputs in = baseInputs();
  in.msSinceLastMeasurement = 59000;
  TEST_ASSERT_EQUAL_STRING("not_due_yet", controller.evaluate(in).reason);
}

void test_deep_sleep_defers_to_the_energy_policy() {
  DeepSleepController controller;
  DeepSleepInputs external = baseInputs();
  external.externalPower = true;
  TEST_ASSERT_EQUAL_STRING("external_power_no_sleep_needed",
                          controller.evaluate(external).reason);
  DeepSleepInputs tight = baseInputs();
  tight.samplingIntervalS = 10;
  TEST_ASSERT_EQUAL_STRING("sampling_interval_too_short_for_sleep_overhead",
                          controller.evaluate(tight).reason);
  DeepSleepInputs backlog = baseInputs();
  backlog.storageHasPendingSync = true;
  backlog.syncIntervalS = 60;
  TEST_ASSERT_EQUAL_STRING("sync_cadence_too_tight_for_sleep",
                          controller.evaluate(backlog).reason);
}

void test_deep_sleep_on_empty_plan_never_sleeps_zero() {
  DeepSleepController controller;
  DeepSleepInputs in = baseInputs();
  in.samplingIntervalS = 16;
  const SleepPlan plan = controller.evaluate(in);
  TEST_ASSERT_EQUAL(static_cast<int>(SleepDecision::StayAwake),
                    static_cast<int>(plan.decision));
  TEST_ASSERT_EQUAL(0u, plan.sleepS);
}

void registerDeepSleepTests() {
  RUN_TEST(test_deep_sleep_is_off_unless_enabled);
  RUN_TEST(test_deep_sleep_sleeps_when_everything_is_quiet);
  RUN_TEST(test_deep_sleep_never_while_link_is_up);
  RUN_TEST(test_deep_sleep_never_while_portal_is_active);
  RUN_TEST(test_deep_sleep_never_during_ota);
  RUN_TEST(test_deep_sleep_waits_for_boot_settle);
  RUN_TEST(test_deep_sleep_waits_until_the_sample_is_due);
  RUN_TEST(test_deep_sleep_defers_to_the_energy_policy);
  RUN_TEST(test_deep_sleep_on_empty_plan_never_sleeps_zero);
}
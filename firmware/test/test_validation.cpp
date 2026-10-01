#include <cmath>
#include <cstring>

#include <unity.h>

#include "cauce/core/ValidationEngine.h"

using namespace cauce;

namespace {

Measurement makeMeasurement(Variable var, float value, uint32_t seq,
                            uint64_t tsMs) {
  Measurement m{};
  copyString(m.nodeId, sizeof(m.nodeId), "CAUCE-T");
  copyString(m.sensorId, sizeof(m.sensorId), "S1");
  m.variable = var;
  m.value = value;
  m.sequence = seq;
  m.timestampUtcMs = tsMs;
  m.timeUncertain = false;
  return m;
}

ValidationEngine& engine() {
  static ValidationEngine e(defaultThresholds());
  return e;
}

}  // namespace

void test_valid_temperature_passes() {
  ValidationContext ctx;
  ctx.timeValid = true;
  const auto r =
      engine().evaluate(makeMeasurement(Variable::AirTemperature, 21.5f, 1, 60000),
                        ctx);
  TEST_ASSERT_EQUAL(Quality::Valid, r.quality);
  TEST_ASSERT_EQUAL(kReasonNone, r.reasonBits);
  TEST_ASSERT_FALSE(r.timeUncertain);
}

void test_future_timestamp_beyond_skew_is_time_uncertain() {
  ValidationContext ctx;
  ctx.timeValid = true;
  ctx.nowUtcMs = 1787356800000ULL;
  const auto r = engine().evaluate(
      makeMeasurement(Variable::AirTemperature, 21.5f, 9,
                      1787356800000ULL + 120000ULL + 1ULL),
      ctx);
  TEST_ASSERT_TRUE(r.timeUncertain);
  TEST_ASSERT_TRUE(r.reasonBits & kReasonTimeUncertain);
}

void test_timestamp_within_skew_stays_trusted() {
  ValidationContext ctx;
  ctx.timeValid = true;
  ctx.nowUtcMs = 1787356800000ULL;
  const auto r = engine().evaluate(
      makeMeasurement(Variable::AirTemperature, 21.5f, 9,
                      1787356800000ULL + 60000ULL),
      ctx);
  TEST_ASSERT_FALSE(r.timeUncertain);
  TEST_ASSERT_EQUAL(Quality::Valid, r.quality);
}

void test_non_finite_value_is_invalid() {
  ValidationContext ctx;
  ctx.timeValid = true;
  const auto r = engine().evaluate(
      makeMeasurement(Variable::RelativeHumidity, NAN, 2, 120000), ctx);
  TEST_ASSERT_EQUAL(Quality::Invalid, r.quality);
  TEST_ASSERT_TRUE(r.reasonBits & kReasonNonFinite);
}

void test_impossible_range_is_invalid() {
  ValidationContext ctx;
  ctx.timeValid = true;
  const auto r = engine().evaluate(
      makeMeasurement(Variable::AirTemperature, 140.0f, 3, 180000), ctx);
  TEST_ASSERT_EQUAL(Quality::Invalid, r.quality);
  TEST_ASSERT_TRUE(r.reasonBits & kReasonOutOfRange);
}

void test_humidity_above_100_is_invalid() {
  ValidationContext ctx;
  ctx.timeValid = true;
  const auto r = engine().evaluate(
      makeMeasurement(Variable::RelativeHumidity, 105.0f, 4, 240000), ctx);
  TEST_ASSERT_EQUAL(Quality::Invalid, r.quality);
}

void test_abrupt_jump_is_suspect() {
  ValidationContext ctx;
  ctx.timeValid = true;
  ctx.hasPreviousValue = true;
  ctx.previousValue = 20.0f;
  ctx.previousTimestampMs = 60000;
  const auto r = engine().evaluate(
      makeMeasurement(Variable::AirTemperature, 40.0f, 5, 120000), ctx);
  TEST_ASSERT_EQUAL(Quality::Suspect, r.quality);
  TEST_ASSERT_TRUE(r.reasonBits & kReasonRateOfChange);
}

void test_gradual_change_is_valid() {
  ValidationContext ctx;
  ctx.timeValid = true;
  ctx.hasPreviousValue = true;
  ctx.previousValue = 20.0f;
  ctx.previousTimestampMs = 60000;
  const auto r = engine().evaluate(
      makeMeasurement(Variable::AirTemperature, 20.4f, 6, 120000), ctx);
  TEST_ASSERT_EQUAL(Quality::Valid, r.quality);
}

void test_frozen_sensor_becomes_suspect_after_limit() {
  ValidationContext ctx;
  ctx.timeValid = true;
  Quality lastQuality = Quality::Valid;
  for (uint32_t i = 1; i <= 7; ++i) {
    const auto r = engine().evaluate(
        makeMeasurement(Variable::AirTemperature, 22.0f, i, i * 60000), ctx);
    lastQuality = r.quality;
    if (r.newIdenticalStreak == 1) ctx.streakValue = 22.0f;
    ctx.identicalStreak = r.newIdenticalStreak;
    ctx.hasPreviousValue = true;
    ctx.previousValue = 22.0f;
    ctx.previousTimestampMs = i * 60000;
  }
  TEST_ASSERT_EQUAL(Quality::Suspect, lastQuality);
}

void test_duplicate_sequence_is_invalid() {
  ValidationContext ctx;
  ctx.timeValid = true;
  ctx.hasLastSequence = true;
  ctx.lastSeenSequence = 10;
  const auto r = engine().evaluate(
      makeMeasurement(Variable::AirTemperature, 21.0f, 9, 300000), ctx);
  TEST_ASSERT_EQUAL(Quality::Invalid, r.quality);
  TEST_ASSERT_TRUE(r.reasonBits & kReasonDuplicateSequence);
}

void test_time_uncertainty_is_flagged_not_rejected() {
  ValidationContext ctx;
  ctx.timeValid = false;
  const auto r =
      engine().evaluate(makeMeasurement(Variable::AirTemperature, 21.0f, 11, 0),
                        ctx);
  TEST_ASSERT_TRUE(r.timeUncertain);
  TEST_ASSERT_TRUE(r.reasonBits & kReasonTimeUncertain);
  TEST_ASSERT_NOT_EQUAL(Quality::Invalid, r.quality);
}

void registerValidationTests() {

  RUN_TEST(test_valid_temperature_passes);
  RUN_TEST(test_non_finite_value_is_invalid);
  RUN_TEST(test_impossible_range_is_invalid);
  RUN_TEST(test_humidity_above_100_is_invalid);
  RUN_TEST(test_abrupt_jump_is_suspect);
  RUN_TEST(test_gradual_change_is_valid);
  RUN_TEST(test_frozen_sensor_becomes_suspect_after_limit);
  RUN_TEST(test_duplicate_sequence_is_invalid);
  RUN_TEST(test_time_uncertainty_is_flagged_not_rejected);
}

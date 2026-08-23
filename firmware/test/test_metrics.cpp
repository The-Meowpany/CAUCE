#include <cmath>
#include <cstring>

#include <unity.h>

#include "cauce/core/Metrics.h"

using namespace cauce;

namespace {

Measurement makeM(Variable var, float value, uint64_t tsMs) {
  Measurement m{};
  copyString(m.nodeId, sizeof(m.nodeId), "CAUCE-001");
  copyString(m.sensorId, sizeof(m.sensorId), "S1");
  m.variable = var;
  m.value = value;
  m.timestampUtcMs = tsMs;
  m.quality = Quality::Valid;
  return m;
}

}  // namespace

void test_sample_stats_known_vector() {
  const float values[] = {5.0f, 1.0f, 3.0f, 2.0f, 4.0f};
  float scratch[8];
  SampleStats stats;
  TEST_ASSERT_TRUE(computeSampleStats(values, 5, stats, scratch, 8));
  TEST_ASSERT_EQUAL_UINT32(5, stats.count);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, stats.minValue);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, stats.maxValue);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.0f, stats.mean);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.0f, stats.median);
  TEST_ASSERT_FLOAT_WITHIN(0.002f, 1.58114f, stats.stdDev);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.2f, stats.p05);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, stats.p25);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.0f, stats.p75);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 4.8f, stats.p95);
}

void test_sample_stats_even_count_median() {
  const float values[] = {40.0f, 10.0f, 30.0f, 20.0f};
  float scratch[8];
  SampleStats stats;
  TEST_ASSERT_TRUE(computeSampleStats(values, 4, stats, scratch, 8));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, stats.median);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 12.90994f, stats.stdDev);
}

void test_sample_stats_single_value() {
  const float values[] = {7.25f};
  float scratch[4];
  SampleStats stats;
  TEST_ASSERT_TRUE(computeSampleStats(values, 1, stats, scratch, 4));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.25f, stats.median);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, stats.stdDev);
}

void test_extract_filters_variable_and_nan() {
  Measurement samples[5] = {
      makeM(Variable::AirTemperature, 21.0f, 1000),
      makeM(Variable::RelativeHumidity, 55.0f, 2000),
      makeM(Variable::AirTemperature, NAN, 3000),
      makeM(Variable::AirTemperature, 22.5f, 4000),
      makeM(Variable::AirTemperature, 0.0f, 5000),
  };
  samples[4].quality = Quality::Missing;
  float out[5];
  const uint32_t n = extractVariableValues(samples, 5, Variable::AirTemperature,
                                           out, 5);
  TEST_ASSERT_EQUAL_UINT32(2, n);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.0f, out[0]);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 22.5f, out[1]);
}

void test_aggregation_two_windows() {
  const uint64_t base = 1787356800000ULL;
  Measurement samples[6] = {
      makeM(Variable::AirTemperature, 20.0f, base + 10000),
      makeM(Variable::AirTemperature, 22.0f, base + 20000),
      makeM(Variable::AirTemperature, 24.0f, base + 30000),
      makeM(Variable::AirTemperature, 26.0f, base + 70000),
      makeM(Variable::RelativeHumidity, 50.0f, base + 75000),
      makeM(Variable::AirTemperature, 28.0f, base + 80000),
  };
  AggregateBucket buckets[4];
  const uint32_t n = aggregateBuckets(samples, 6, Variable::AirTemperature, 60,
                                      buckets, 4);
  TEST_ASSERT_EQUAL_UINT32(2, n);
  TEST_ASSERT_EQUAL_UINT64(base, buckets[0].startMs);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 22.0f, buckets[0].mean);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, buckets[0].min);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 24.0f, buckets[0].max);
  TEST_ASSERT_EQUAL_UINT32(3, buckets[0].count);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 27.0f, buckets[1].mean);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 26.0f, buckets[1].min);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 28.0f, buckets[1].max);
}

void test_aggregation_respects_capacity() {
  const uint64_t base = 1787356800000ULL;
  Measurement samples[3] = {
      makeM(Variable::Light, 100.0f, base),
      makeM(Variable::Light, 200.0f, base + 60000),
      makeM(Variable::Light, 300.0f, base + 120000),
  };
  AggregateBucket buckets[1];
  const uint32_t n = aggregateBuckets(samples, 3, Variable::Light, 60,
                                      buckets, 1);
  TEST_ASSERT_EQUAL_UINT32(1, n);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, buckets[0].mean);
}

void test_exposure_hours_above_threshold() {
  const uint64_t minute = 60000ULL;
  const uint64_t base = 1787356800000ULL;
  Measurement temps[4] = {
      makeM(Variable::AirTemperature, 30.0f, base),
      makeM(Variable::AirTemperature, 35.0f, base + minute),
      makeM(Variable::AirTemperature, 36.5f, base + 2 * minute),
      makeM(Variable::AirTemperature, 29.0f, base + 3 * minute),
  };
  const double hours =
      exposureHoursAbove(temps, 4, 32.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0 / 60.0, hours);
}

void test_exposure_none_when_below() {
  const uint64_t minute = 60000ULL;
  const uint64_t base = 1787356800000ULL;
  Measurement temps[3] = {
      makeM(Variable::AirTemperature, 25.0f, base),
      makeM(Variable::AirTemperature, 31.9f, base + minute),
      makeM(Variable::AirTemperature, 25.0f, base + 2 * minute),
  };
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0,
                           exposureHoursAbove(temps, 3, 32.0f));
}

void registerMetricsTests() {
  UNITY_BEGIN();
  RUN_TEST(test_sample_stats_known_vector);
  RUN_TEST(test_sample_stats_even_count_median);
  RUN_TEST(test_sample_stats_single_value);
  RUN_TEST(test_extract_filters_variable_and_nan);
  RUN_TEST(test_aggregation_two_windows);
  RUN_TEST(test_aggregation_respects_capacity);
  RUN_TEST(test_exposure_hours_above_threshold);
  RUN_TEST(test_exposure_none_when_below);
  
  UNITY_END();
}

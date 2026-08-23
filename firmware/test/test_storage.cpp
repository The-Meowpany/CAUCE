#include <cstring>

#include <unity.h>

#include "cauce/core/LogStorageRepository.h"
#include "cauce/core/RecordCodec.h"
#include "cauce/hal/MemoryFileSystem.h"

using namespace cauce;

namespace {

hal::MemoryFileSystem fs;
LogStorageRepository* store = nullptr;

Measurement makeMeasurement(uint32_t seq, uint64_t tsMs, Variable var,
                            float value, Quality quality) {
  Measurement m{};
  copyString(m.nodeId, sizeof(m.nodeId), "CAUCE-001");
  copyString(m.sensorId, sizeof(m.sensorId), "S1");
  m.sequence = seq;
  m.timestampUtcMs = tsMs;
  m.variable = var;
  m.value = value;
  m.quality = quality;
  return m;
}

void resetStore(const char* dir, uint32_t segmentBytes, bool wipeFiles = true) {
  static bool cleaned = false;
  if (!cleaned || wipeFiles) {
    char paths[16][64];
    const int n = fs.listFiles(dir, paths, 16);
    for (int i = 0; i < n; ++i) fs.removeFile(paths[i]);
    cleaned = true;
  }
  delete store;
  store = new LogStorageRepository(fs, dir, segmentBytes);
  TEST_ASSERT_TRUE(store->open());
}

}  // namespace

void test_append_and_query_roundtrip() {
  resetStore("data", 256 * 1024);
  for (uint32_t i = 1; i <= 10; ++i) {
    TEST_ASSERT_TRUE(store->append(
        makeMeasurement(i, 1787356800000ULL + i * 60000ULL,
                        Variable::AirTemperature, 20.0f + i, Quality::Valid)));
  }
  TEST_ASSERT_EQUAL_UINT32(10, store->totalRecords());
  TEST_ASSERT_EQUAL_UINT32(10, store->lastSequence());

  Measurement page[4];
  QueryStats stats{};
  const size_t got = store->query(1787356800000ULL + 3 * 60000ULL,
                                  1787356800000ULL + 6 * 60000ULL, 0, page, 4,
                                  stats);
  TEST_ASSERT_EQUAL_UINT32(4, stats.matched);
  TEST_ASSERT_EQUAL_UINT32(4, got);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 23.0f, page[0].value);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 26.0f, page[3].value);
}

void test_query_pagination_with_skip() {
  resetStore("data", 256 * 1024);
  for (uint32_t i = 1; i <= 8; ++i) {
    store->append(makeMeasurement(i, 1000000000ULL + i, Variable::Pressure,
                                  1013.0f, Quality::Valid));
  }
  Measurement page[2];
  QueryStats stats{};
  store->query(0, UINT64_MAX, 5, page, 2, stats);
  TEST_ASSERT_EQUAL_UINT32(8, stats.matched);
  TEST_ASSERT_EQUAL_UINT32(2, stats.returned);
  TEST_ASSERT_EQUAL_UINT32(6, page[0].sequence);
  TEST_ASSERT_EQUAL_UINT32(7, page[1].sequence);
}

void test_latest_and_reopen_recovery() {
  resetStore("data", 256 * 1024);
  for (uint32_t i = 1; i <= 5; ++i) {
    store->append(makeMeasurement(i, 2000000000ULL + i, Variable::Light,
                                  300.0f * i, Quality::Valid));
  }
  Measurement latest{};
  TEST_ASSERT_TRUE(store->latest(latest));
  TEST_ASSERT_EQUAL_UINT32(5, latest.sequence);

  resetStore("data", 256 * 1024, false);
  TEST_ASSERT_EQUAL_UINT32(5, store->totalRecords());
  TEST_ASSERT_EQUAL_UINT32(5, store->lastSequence());
  TEST_ASSERT_TRUE(store->latest(latest));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1500.0f, latest.value);
}

void test_corrupted_tail_is_isolated_on_reopen() {
  resetStore("data", 256 * 1024);
  for (uint32_t i = 1; i <= 3; ++i) {
    store->append(makeMeasurement(i, 3000000000ULL + i, Variable::AirTemperature,
                                  21.0f, Quality::Valid));
  }
  uint8_t garbage[37] = {};
  garbage[0] = 0xDE;
  TEST_ASSERT_TRUE(fs.appendBytes("data/meas_000001.clog", garbage, sizeof(garbage)));

  resetStore("data", 256 * 1024, false);

  TEST_ASSERT_EQUAL_UINT32(3, store->totalRecords());
  TEST_ASSERT_TRUE(store->corruptedFrames() >= 1);

  store->append(makeMeasurement(4, 3000000100ULL, Variable::AirTemperature, 22.0f,
                                Quality::Valid));
  Measurement latest{};
  TEST_ASSERT_TRUE(store->latest(latest));
  TEST_ASSERT_EQUAL_UINT32(4, latest.sequence);

  resetStore("data", 256 * 1024, false);

  TEST_ASSERT_EQUAL_UINT32(4, store->totalRecords());
}

void test_segment_rotation_occurs_when_limit_reached() {
  resetStore("data", kFrameSize * 3 + 1);

  for (uint32_t i = 1; i <= 10; ++i) {
    store->append(makeMeasurement(i, 4000000000ULL + i, Variable::BatteryVoltage,
                                  3.9f, Quality::Valid));
  }
  TEST_ASSERT_TRUE(store->segments().size() >= 3);
  TEST_ASSERT_EQUAL_UINT32(10, store->totalRecords());
}

void test_retention_removes_oldest_but_keeps_one_segment() {
  resetStore("data", kFrameSize * 2);

  for (uint32_t i = 1; i <= 12; ++i) {
    store->append(makeMeasurement(i, 5000000000ULL + i, Variable::RelativeHumidity,
                                  60.0f, Quality::Valid));
  }
  TEST_ASSERT_TRUE(store->segments().size() >= 4);
  store->applyRetentionPolicy(kFrameSize * 3);
  TEST_ASSERT_TRUE(store->segments().size() >= 1);
  TEST_ASSERT_TRUE(store->totalBytes() <= kFrameSize * 3);
  TEST_ASSERT_TRUE(store->totalRecords() >= 1);
}

void registerStorageTests() {

  RUN_TEST(test_append_and_query_roundtrip);
  RUN_TEST(test_query_pagination_with_skip);
  RUN_TEST(test_latest_and_reopen_recovery);
  RUN_TEST(test_corrupted_tail_is_isolated_on_reopen);
  RUN_TEST(test_segment_rotation_occurs_when_limit_reached);
  RUN_TEST(test_retention_removes_oldest_but_keeps_one_segment);
}

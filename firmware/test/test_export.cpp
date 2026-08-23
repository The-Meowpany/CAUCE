#include <cstdio>
#include <cstring>
#include <string>

#include <unity.h>

#include "cauce/core/DataExporter.h"
#include "cauce/core/LogStorageRepository.h"
#include "cauce/hal/MemoryFileSystem.h"

using namespace cauce;

namespace {

hal::MemoryFileSystem fs;
const char* kDir = "data_export";

void wipeDir() {
  char paths[16][64];
  const int n = fs.listFiles(kDir, paths, 16);
  for (int i = 0; i < n; ++i) fs.removeFile(paths[i]);
}

Measurement makeM(uint32_t seq, uint64_t tsMs, Variable var, float value,
                  Quality q) {
  Measurement m{};
  copyString(m.nodeId, sizeof(m.nodeId), "CAUCE-001");
  copyString(m.sensorId, sizeof(m.sensorId), "BME280-1");
  m.sequence = seq;
  m.timestampUtcMs = tsMs;
  m.variable = var;
  m.value = value;
  m.quality = q;
  m.timeUncertain = false;
  return m;
}

std::string collectAll(ChunkedExporter& exporter, size_t chunkCapacity) {
  std::string out;
  char buffer[1024];
  if (chunkCapacity > sizeof(buffer)) return "";
  while (!exporter.done()) {
    const size_t n = exporter.next(buffer, chunkCapacity);
    if (n == 0) break;
    out.append(buffer, n);
  }
  return out;
}

}  // namespace

void test_csv_export_contains_header_and_rows() {
  wipeDir();
  LogStorageRepository store(fs, kDir);
  store.open();
  const uint64_t t0 = 1787356800000ULL;
  TEST_ASSERT_TRUE(store.append(makeM(1, t0 + 60000, Variable::AirTemperature,
                                      21.5f, Quality::Valid)));
  TEST_ASSERT_TRUE(store.append(makeM(2, t0 + 120000,
                                      Variable::RelativeHumidity, 63.25f,
                                      Quality::Suspect)));

  ChunkedExporter exporter(store, ChunkedExporter::Format::Csv, 0, UINT64_MAX);
  const std::string csv = collectAll(exporter, 512);

  TEST_ASSERT_TRUE(csv.find("node_id,sensor_id,sequence") == 0);
  TEST_ASSERT_TRUE(csv.find(",2026-08-22T00:01:00Z,air_temperature,21.50,C,VALID,0,0\n") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(csv.find(",relative_humidity,63.25,%RH,SUSPECT,0,0") !=
                   std::string::npos);
}

void test_csv_escapes_special_characters_per_rfc4180() {
  wipeDir();
  LogStorageRepository store(fs, kDir);
  store.open();
  Measurement m = makeM(9, 1787356890000ULL, Variable::Pressure, 1013.2f,
                        Quality::Valid);
  copyString(m.sensorId, sizeof(m.sensorId), "BME,\"280\"");
  TEST_ASSERT_TRUE(store.append(m));

  ChunkedExporter exporter(store, ChunkedExporter::Format::Csv, 0, UINT64_MAX);
  const std::string csv = collectAll(exporter, 512);
  TEST_ASSERT_TRUE(csv.find("\"BME,\"\"280\"\"\"") != std::string::npos);
}

void test_json_array_is_complete_and_typed() {
  wipeDir();
  LogStorageRepository store(fs, kDir);
  store.open();
  store.append(makeM(1, 1787356860000ULL, Variable::AirTemperature, 18.05f,
                     Quality::Valid));
  store.append(makeM(2, 1787356920000ULL, Variable::BatteryVoltage, 3.91f,
                     Quality::Uncalibrated));

  ChunkedExporter exporter(store, ChunkedExporter::Format::JsonArray, 0,
                           UINT64_MAX);
  const std::string json = collectAll(exporter, 512);

  TEST_ASSERT_TRUE(json.front() == '[');
  TEST_ASSERT_TRUE(json.back() == ']');
  TEST_ASSERT_TRUE(json.find("\"variable\":\"air_temperature\"") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"value\":18.05") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"quality\":\"UNCALIBRATED\"") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"time_uncertain\":false") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"timestamp\":\"2026-08-22T00:02:00Z\"") !=
                   std::string::npos);
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(json.find('[')));
  TEST_ASSERT_NULL(strchr(json.c_str() + 1, '['));
}

void test_json_empty_store_yields_empty_array() {
  wipeDir();
  LogStorageRepository store(fs, kDir);
  store.open();
  ChunkedExporter exporter(store, ChunkedExporter::Format::JsonArray, 0,
                           UINT64_MAX);
  const std::string json = collectAll(exporter, 512);
  TEST_ASSERT_EQUAL_STRING("[]", json.c_str());
}

void test_chunked_output_equals_single_shot() {
  wipeDir();
  std::fflush(stdout);
  LogStorageRepository store(fs, kDir);
  store.open();
  const uint64_t t0 = 1787356800000ULL;
  for (uint32_t i = 1; i <= 12; ++i) {
    store.append(makeM(i, t0 + i * 60000ULL,
                       i % 2 ? Variable::AirTemperature : Variable::Light,
                       20.0f + i, Quality::Valid));
  }
  std::fflush(stdout);

  ChunkedExporter big(store, ChunkedExporter::Format::Csv, 0, UINT64_MAX);
  const std::string oneShot = collectAll(big, 1024);

  ChunkedExporter small(store, ChunkedExporter::Format::Csv, 0, UINT64_MAX);
  const std::string chunked = collectAll(small, 400);

  TEST_ASSERT_EQUAL_STRING(oneShot.c_str(), chunked.c_str());
  TEST_ASSERT_TRUE(oneShot.size() > 1100);
}

void test_time_range_filters_records() {
  wipeDir();
  LogStorageRepository store(fs, kDir);
  store.open();
  const uint64_t t0 = 1787356800000ULL;
  store.append(makeM(1, t0, Variable::AirTemperature, 10.0f, Quality::Valid));
  store.append(makeM(2, t0 + 3600000ULL, Variable::AirTemperature, 11.0f,
                     Quality::Valid));
  store.append(makeM(3, t0 + 7200000ULL, Variable::AirTemperature, 12.0f,
                     Quality::Valid));

  ChunkedExporter exporter(store, ChunkedExporter::Format::Csv,
                           t0 + 1800000ULL, t0 + 5400000ULL);
  const std::string csv = collectAll(exporter, 512);
  TEST_ASSERT_TRUE(csv.find(",10.00,C") == std::string::npos);
  TEST_ASSERT_TRUE(csv.find(",12.00,C") == std::string::npos);
  TEST_ASSERT_TRUE(csv.find(",11.00,C") != std::string::npos);
}

void registerExportTests() {
  UNITY_BEGIN();
  RUN_TEST(test_csv_export_contains_header_and_rows);
  RUN_TEST(test_csv_escapes_special_characters_per_rfc4180);
  RUN_TEST(test_json_array_is_complete_and_typed);
  RUN_TEST(test_json_empty_store_yields_empty_array);
  RUN_TEST(test_chunked_output_equals_single_shot);
  RUN_TEST(test_time_range_filters_records);
  
  UNITY_END();
}

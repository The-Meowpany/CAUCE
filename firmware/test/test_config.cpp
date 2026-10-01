#include <cmath>
#include <cstring>

#include <unity.h>

#include "cauce/core/ConfigManager.h"
#include "cauce/hal/MemoryFileSystem.h"

using namespace cauce;

namespace {

hal::MemoryFileSystem fs;
const char* kPath = "/config/cauce.conf";

NodeConfig sampleConfig() {
  NodeConfig c{};
  copyString(c.nodeId, sizeof(c.nodeId), "CAUCE-007");
  copyString(c.siteId, sizeof(c.siteId), "PLAZA-CENTRAL");
  c.latitude = -34.54f;
  c.longitude = -56.29f;
  c.elevationM = 42.0f;
  copyString(c.landCover, sizeof(c.landCover), "urban");
  copyString(c.shadeCondition, sizeof(c.shadeCondition), "partial");
  c.samplingIntervalS = 300;
  c.syncIntervalS = 1800;
  c.timezoneOffsetMin = -180;
  c.wifiEnabled = false;
  c.storageMaxBytes = 256 * 1024;
  c.segmentMaxBytes = 32 * 1024;
  return c;
}

}  // namespace

void test_load_on_empty_filesystem_creates_defaults() {
  ConfigManager mgr(fs, kPath);
  NodeConfig config{};
  TEST_ASSERT_EQUAL(ConfigLoadStatus::CreatedDefaults, mgr.load(config));
  TEST_ASSERT_EQUAL_STRING("CAUCE-001", config.nodeId);
  TEST_ASSERT_EQUAL_UINT32(60, config.samplingIntervalS);
}

void test_serialize_parse_roundtrip_preserves_fields() {
  const NodeConfig original = sampleConfig();
  char text[4096];
  TEST_ASSERT_TRUE(serializeConfig(original, text, sizeof(text)));

  NodeConfig parsed{};
  TEST_ASSERT_TRUE(parseConfig(text, parsed));
  TEST_ASSERT_EQUAL_STRING("CAUCE-007", parsed.nodeId);
  TEST_ASSERT_EQUAL_STRING("PLAZA-CENTRAL", parsed.siteId);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -34.54f, parsed.latitude);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 42.0f, parsed.elevationM);
  TEST_ASSERT_EQUAL_UINT32(300, parsed.samplingIntervalS);
  TEST_ASSERT_EQUAL_INT16(-180, parsed.timezoneOffsetMin);
  TEST_ASSERT_FALSE(parsed.wifiEnabled);
}

void test_unset_geo_serializes_as_nan() {
  NodeConfig original = sampleConfig();
  original.latitude = NAN;
  original.longitude = NAN;
  char text[4096];
  serializeConfig(original, text, sizeof(text));
  NodeConfig parsed{};
  TEST_ASSERT_TRUE(parseConfig(text, parsed));
  TEST_ASSERT_TRUE(std::isnan(parsed.latitude));
  TEST_ASSERT_TRUE(std::isnan(parsed.longitude));
}

void test_thresholds_survive_roundtrip() {
  NodeConfig original = sampleConfig();
  original.thresholds.rangeMax[static_cast<uint8_t>(Variable::AirTemperature)] =
      70.0f;
  char text[8192];
  serializeConfig(original, text, sizeof(text));
  NodeConfig parsed{};
  TEST_ASSERT_TRUE(parseConfig(text, parsed));
  TEST_ASSERT_FLOAT_WITHIN(
      0.01f, 70.0f,
      parsed.thresholds.rangeMax[static_cast<uint8_t>(Variable::AirTemperature)]);
  TEST_ASSERT_FLOAT_WITHIN(
      0.01f, defaultThresholds().rangeMin[static_cast<uint8_t>(
                 Variable::AirTemperature)],
      parsed.thresholds.rangeMin[static_cast<uint8_t>(Variable::AirTemperature)]);
}

void test_save_then_load_roundtrip() {
  fs.writeWholeFile("/config/.keep", nullptr, 0);
  ConfigManager mgr(fs, "/config/node2.conf");
  const NodeConfig original = sampleConfig();
  TEST_ASSERT_TRUE(mgr.save(original));

  NodeConfig loaded{};
  TEST_ASSERT_EQUAL(ConfigLoadStatus::Loaded, mgr.load(loaded));
  TEST_ASSERT_EQUAL_STRING("CAUCE-007", loaded.nodeId);
  TEST_ASSERT_EQUAL_UINT32(300, loaded.samplingIntervalS);
}

void test_corrupted_main_restores_from_backup() {
  ConfigManager mgr(fs, "/config/n3.conf");
  TEST_ASSERT_TRUE(mgr.save(sampleConfig()));

  NodeConfig modified = sampleConfig();
  modified.samplingIntervalS = 900;
  TEST_ASSERT_TRUE(mgr.save(modified));

  const uint8_t junk[] = "\x00\xFFgarbage without equals signs\n\n\n\n";
  fs.writeWholeFile("/config/n3.conf", junk, sizeof(junk) - 1);

  NodeConfig restored{};
  TEST_ASSERT_EQUAL(ConfigLoadStatus::RestoredFromBackup, mgr.load(restored));
  TEST_ASSERT_EQUAL_UINT32(300, restored.samplingIntervalS);
}

void test_validation_rejects_bad_intervals() {
  NodeConfig config = sampleConfig();
  config.samplingIntervalS = 3;
  const auto result = ConfigManager::validate(config);
  TEST_ASSERT_FALSE(result.ok);
  TEST_ASSERT_TRUE(result.errorCount > 0);

  config.samplingIntervalS = 600;
  TEST_ASSERT_TRUE(ConfigManager::validate(config).ok);
}

void test_validation_rejects_bad_node_id_charset() {
  NodeConfig config = sampleConfig();
  copyString(config.nodeId, sizeof(config.nodeId), "BAD ID!");
  TEST_ASSERT_FALSE(ConfigManager::validate(config).ok);

  copyString(config.nodeId, sizeof(config.nodeId), "CAUCE-001_A");
  TEST_ASSERT_TRUE(ConfigManager::validate(config).ok);
}

void test_validation_rejects_inverted_thresholds() {
  NodeConfig config = sampleConfig();
  config.thresholds.rangeMin[static_cast<uint8_t>(Variable::Pressure)] = 1200.0f;
  config.thresholds.rangeMax[static_cast<uint8_t>(Variable::Pressure)] = 800.0f;
  TEST_ASSERT_FALSE(ConfigManager::validate(config).ok);
}

void test_garbage_input_rejected() {
  NodeConfig out{};
  TEST_ASSERT_FALSE(parseConfig("\n\n   \nnot a config", out));
  TEST_ASSERT_FALSE(parseConfig("", out));
}

void registerConfigTests() {

  RUN_TEST(test_load_on_empty_filesystem_creates_defaults);
  RUN_TEST(test_serialize_parse_roundtrip_preserves_fields);
  RUN_TEST(test_unset_geo_serializes_as_nan);
  RUN_TEST(test_thresholds_survive_roundtrip);
  RUN_TEST(test_save_then_load_roundtrip);
  RUN_TEST(test_corrupted_main_restores_from_backup);
  RUN_TEST(test_validation_rejects_bad_intervals);
  RUN_TEST(test_validation_rejects_bad_node_id_charset);
  RUN_TEST(test_validation_rejects_inverted_thresholds);
  RUN_TEST(test_garbage_input_rejected);
}

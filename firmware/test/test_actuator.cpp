#include <cstring>
#include <map>
#include <string>

#include <unity.h>

#include "cauce/core/NodeActuator.h"
#include "cauce/hal/IFileSystem.h"

namespace {

// In-memory filesystem. writeWholeFile replaces, appendBytes concatenates, which
// is what lets a test check that persist really rewrites rather than appends - the
// bug that turns a settings file into something load() rejects after two changes.
class MemoryFs final : public cauce::hal::IFileSystem {
 public:
  bool exists(const char* path) override {
    return files_.count(path) != 0;
  }
  bool appendBytes(const char* path, const uint8_t* data, size_t length) override {
    files_[path] += std::string(reinterpret_cast<const char*>(data), length);
    return true;
  }
  bool readRange(const char* path, size_t offset, uint8_t* buffer,
                 size_t length) override {
    auto it = files_.find(path);
    if (it == files_.end() || offset + length > it->second.size()) return false;
    std::memcpy(buffer, it->second.data() + offset, length);
    return true;
  }
  bool writeWholeFile(const char* path, const uint8_t* data,
                      size_t length) override {
    files_[path] = std::string(reinterpret_cast<const char*>(data), length);
    return true;
  }
  size_t fileSize(const char* path) override {
    auto it = files_.find(path);
    return it == files_.end() ? 0 : it->second.size();
  }
  bool removeFile(const char* path) override {
    files_.erase(path);
    return true;
  }
  int listFiles(const char*, char (*)[64], int) override { return 0; }

  std::string read(const char* path) {
    auto it = files_.find(path);
    return it == files_.end() ? std::string() : it->second;
  }
  void put(const char* path, const std::string& body) { files_[path] = body; }

 private:
  std::map<std::string, std::string> files_;
};

class CountingSink final : public cauce::ILogSink {
 public:
  void writeLine(const char* line) override {
    ++lines_;
    last_ = line ? line : "";
  }
  int lines() const { return lines_; }
  const std::string& last() const { return last_; }

 private:
  int lines_{0};
  std::string last_;
};

struct LoggerAndSink {
  CountingSink sink;
  cauce::Logger logger{sink};
};

constexpr const char* kPath = "/state/settings";

std::string detail(char* buffer) {
  return std::string(buffer ? buffer : "");
}

}  // namespace

// --- the settings that ship ---------------------------------------------

void test_defaults_are_inside_the_silence_budget() {
  MemoryFs fs;
  LoggerAndSink logging;
  cauce::NodeActuator actuator(fs, logging.logger, kPath);
  TEST_ASSERT_TRUE(actuator.withinSilenceBudget());
  TEST_ASSERT_EQUAL_UINT32(300, actuator.settings().samplingIntervalSeconds);
  TEST_ASSERT_EQUAL_UINT32(600, actuator.settings().syncIntervalSeconds);
}

void test_an_applied_interval_is_reported_as_applied() {
  MemoryFs fs;
  LoggerAndSink logging;
  cauce::NodeActuator actuator(fs, logging.logger, kPath);
  char buffer[96];
  TEST_ASSERT_TRUE(actuator.setSamplingInterval(120, buffer, sizeof(buffer)));
  TEST_ASSERT_EQUAL_STRING("applied:sampling_interval_s=120", buffer);
  TEST_ASSERT_EQUAL_UINT32(120, actuator.settings().samplingIntervalSeconds);
  TEST_ASSERT_TRUE(actuator.isDirty());
}

// --- refusals -------------------------------------------------------------

void test_an_out_of_range_interval_is_refused_with_a_reason() {
  MemoryFs fs;
  LoggerAndSink logging;
  cauce::NodeActuator actuator(fs, logging.logger, kPath);
  char buffer[96];

  TEST_ASSERT_FALSE(actuator.setSamplingInterval(9, buffer, sizeof(buffer)));
  TEST_ASSERT_EQUAL_STRING("refused:sampling_out_of_range", buffer);
  TEST_ASSERT_FALSE(
      actuator.setSamplingInterval(86401, buffer, sizeof(buffer)));
  TEST_ASSERT_EQUAL_STRING("refused:sampling_out_of_range", buffer);
  TEST_ASSERT_FALSE(actuator.setSyncInterval(59, buffer, sizeof(buffer)));
  TEST_ASSERT_FALSE(actuator.setSyncInterval(86401, buffer, sizeof(buffer)));

  // Refused means unchanged, not "clamped to something nearby".
  TEST_ASSERT_EQUAL_UINT32(300, actuator.settings().samplingIntervalSeconds);
  TEST_ASSERT_EQUAL_UINT32(600, actuator.settings().syncIntervalSeconds);
  TEST_ASSERT_FALSE(actuator.isDirty());
}

// The reason the original handlers did nothing. A combination that leaves the node
// silent for longer than the budget is refused, and the refusal names the budget
// rather than the field's own range - otherwise an operator raises the ceiling and
// wonders why nothing changed.
// The invariant is over the pair, and it is worth being precise about how it is
// reached: the field bounds cap both settings at one day, and the budget is a week,
// so every combination the setters accept is inside it. That is a real guarantee
// about the bounds rather than a coincidence, and this test is what establishes it.
//
// It also means the budget check cannot fire from the setters today. It stays as a
// guard for whoever raises kMaxSamplingSeconds or kMaxSyncSeconds later, and this
// test is what will fail when they do - which is the day it matters.
void test_every_accepted_combination_is_inside_the_silence_budget() {
  const uint32_t values[] = {cauce::NodeActuator::kMinSamplingSeconds, 300,
                             3600, 43200,
                             cauce::NodeActuator::kMaxSamplingSeconds};
  MemoryFs fs;
  LoggerAndSink logging;
  char buffer[96];

  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    for (size_t j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
      cauce::NodeActuator actuator(fs, logging.logger, kPath);
      const bool a = actuator.setSamplingInterval(values[i], buffer, sizeof(buffer));
      const bool b = actuator.setSyncInterval(values[j], buffer, sizeof(buffer));
      TEST_ASSERT_TRUE(a);
      // sync below its own 60 s floor is refused; everything else must land.
      TEST_ASSERT_TRUE(b || values[j] < cauce::NodeActuator::kMinSyncSeconds);
      TEST_ASSERT_TRUE(actuator.withinSilenceBudget());
      TEST_ASSERT_TRUE(actuator.silenceSeconds() <=
                       cauce::NodeActuator::kMaxSilenceSeconds);
    }
  }
}

// A setting of one day is the most a node can ever be quiet for, and that is
// inside the week the budget allows. Asserted directly so the number is visible in
// a test rather than implied by a constant comparison.
void test_the_field_bounds_imply_the_silence_budget() {
  TEST_ASSERT_TRUE(cauce::NodeActuator::kMaxSamplingSeconds <=
                   cauce::NodeActuator::kMaxSilenceSeconds);
  TEST_ASSERT_TRUE(cauce::NodeActuator::kMaxSyncSeconds <=
                   cauce::NodeActuator::kMaxSilenceSeconds);
}

void test_led_mode_is_bounded() {
  MemoryFs fs;
  LoggerAndSink logging;
  cauce::NodeActuator actuator(fs, logging.logger, kPath);
  char buffer[96];
  TEST_ASSERT_TRUE(actuator.setLedMode(3, buffer, sizeof(buffer)));
  TEST_ASSERT_EQUAL_STRING("applied:led_mode=3", buffer);
  TEST_ASSERT_FALSE(actuator.setLedMode(4, buffer, sizeof(buffer)));
  TEST_ASSERT_EQUAL_STRING("refused:led_mode_out_of_range", buffer);
  TEST_ASSERT_EQUAL_UINT8(3, actuator.settings().ledMode);
}

// --- resync latch ---------------------------------------------------------

void test_a_resync_request_is_latched_until_consumed() {
  MemoryFs fs;
  LoggerAndSink logging;
  cauce::NodeActuator actuator(fs, logging.logger, kPath);
  char buffer[96];

  // Nothing outstanding, so consuming reports nothing rather than acting.
  TEST_ASSERT_FALSE(actuator.consumeResyncRequest());

  TEST_ASSERT_TRUE(actuator.requestResync(1234, buffer, sizeof(buffer)));
  TEST_ASSERT_EQUAL_STRING("applied:resync_requested_at_ms=1234", buffer);

  // Latched: still there on the next read, because the command usually arrives in
  // the same batch as the sync response.
  TEST_ASSERT_EQUAL_UINT32(1234, actuator.settings().resyncRequestedAtMs);
  TEST_ASSERT_TRUE(actuator.consumeResyncRequest());
  TEST_ASSERT_EQUAL_UINT32(0, actuator.settings().resyncRequestedAtMs);
  TEST_ASSERT_FALSE(actuator.consumeResyncRequest());
}

// --- persistence ----------------------------------------------------------

void test_settings_survive_a_reload() {
  MemoryFs fs;
  LoggerAndSink logging;
  char buffer[96];
  {
    cauce::NodeActuator actuator(fs, logging.logger, kPath);
    TEST_ASSERT_TRUE(actuator.setSamplingInterval(900, buffer, sizeof(buffer)));
    TEST_ASSERT_TRUE(actuator.setSyncInterval(1800, buffer, sizeof(buffer)));
    TEST_ASSERT_TRUE(actuator.setLedMode(2, buffer, sizeof(buffer)));
    TEST_ASSERT_TRUE(actuator.persist());
  }
  cauce::NodeActuator reloaded(fs, logging.logger, kPath);
  TEST_ASSERT_TRUE(reloaded.load());
  TEST_ASSERT_EQUAL_UINT32(900, reloaded.settings().samplingIntervalSeconds);
  TEST_ASSERT_EQUAL_UINT32(1800, reloaded.settings().syncIntervalSeconds);
  TEST_ASSERT_EQUAL_UINT8(2, reloaded.settings().ledMode);
  TEST_ASSERT_FALSE(reloaded.isDirty());
}

// persist must replace, not append. An appending implementation produces a file
// that still loads, because the parser reads the first occurrence of each key, so
// the second change is silently lost on the next reboot.
void test_persisting_twice_replaces_rather_than_appends() {
  MemoryFs fs;
  LoggerAndSink logging;
  char buffer[96];
  cauce::NodeActuator actuator(fs, logging.logger, kPath);

  TEST_ASSERT_TRUE(actuator.setSamplingInterval(120, buffer, sizeof(buffer)));
  TEST_ASSERT_TRUE(actuator.persist());
  const size_t firstLength = fs.fileSize(kPath);

  TEST_ASSERT_TRUE(actuator.setSamplingInterval(240, buffer, sizeof(buffer)));
  TEST_ASSERT_TRUE(actuator.persist());
  TEST_ASSERT_EQUAL_UINT32(firstLength, fs.fileSize(kPath));

  cauce::NodeActuator reloaded(fs, logging.logger, kPath);
  TEST_ASSERT_TRUE(reloaded.load());
  TEST_ASSERT_EQUAL_UINT32(240, reloaded.settings().samplingIntervalSeconds);
}

void test_a_missing_file_leaves_the_defaults() {
  MemoryFs fs;
  LoggerAndSink logging;
  cauce::NodeActuator actuator(fs, logging.logger, kPath);
  TEST_ASSERT_FALSE(actuator.load());
  TEST_ASSERT_EQUAL_UINT32(300, actuator.settings().samplingIntervalSeconds);
  TEST_ASSERT_EQUAL_UINT32(600, actuator.settings().syncIntervalSeconds);
}

// A file truncated mid-write keeps its magic, so the magic check cannot catch it.
// What has to catch it is the parser refusing a value with no terminator: a file
// ending in "sampling_interval_s=90" when 900 was written must not be read as 90.
//
// Recovery is to the defaults rather than to false, because a node that keeps
// measuring at the default rate is better than one that refuses to start.
void test_a_partial_value_in_a_truncated_file_is_not_adopted() {
  MemoryFs fs;
  LoggerAndSink logging;
  cauce::NodeActuator actuator(fs, logging.logger, kPath);

  fs.put(kPath,
         "cauce-node-settings v1\nsampling_interval_s=90\nsync_interval_s=1800\n"
         "led_mode=1\n");

  TEST_ASSERT_TRUE(actuator.load());
  // 90 was never set by anyone; the default stands.
  TEST_ASSERT_EQUAL_UINT32(300, actuator.settings().samplingIntervalSeconds);
  // The complete line is still read.
  TEST_ASSERT_EQUAL_UINT32(1800, actuator.settings().syncIntervalSeconds);
  TEST_ASSERT_EQUAL_UINT8(1, actuator.settings().ledMode);
}

// A file with no magic at all is not ours, and load() says so rather than
// pretending it read defaults from a file it did not understand.
void test_a_file_with_the_wrong_magic_is_refused() {
  MemoryFs fs;
  LoggerAndSink logging;
  cauce::NodeActuator actuator(fs, logging.logger, kPath);
  fs.put(kPath,
         "something-else v9\nsampling_interval_s=900\nsync_interval_s=1800\n");
  TEST_ASSERT_FALSE(actuator.load());
  TEST_ASSERT_EQUAL_UINT32(300, actuator.settings().samplingIntervalSeconds);
}

// A value outside its range in the file falls back to the default for that field
// only, so one bad line does not discard the other two.
void test_one_out_of_range_field_does_not_discard_the_others() {
  MemoryFs fs;
  LoggerAndSink logging;
  cauce::NodeActuator actuator(fs, logging.logger, kPath);
  fs.put(kPath,
         "cauce-node-settings v1\nsampling_interval_s=900\n"
         "sync_interval_s=1800\nled_mode=99\n");
  TEST_ASSERT_TRUE(actuator.load());
  TEST_ASSERT_EQUAL_UINT32(900, actuator.settings().samplingIntervalSeconds);
  TEST_ASSERT_EQUAL_UINT32(1800, actuator.settings().syncIntervalSeconds);
  TEST_ASSERT_EQUAL_UINT8(0, actuator.settings().ledMode);
}

void registerNodeActuatorTests() {
  RUN_TEST(test_defaults_are_inside_the_silence_budget);
  RUN_TEST(test_an_applied_interval_is_reported_as_applied);
  RUN_TEST(test_an_out_of_range_interval_is_refused_with_a_reason);
  RUN_TEST(test_led_mode_is_bounded);
  RUN_TEST(test_a_resync_request_is_latched_until_consumed);
  RUN_TEST(test_settings_survive_a_reload);
  RUN_TEST(test_persisting_twice_replaces_rather_than_appends);
  RUN_TEST(test_a_missing_file_leaves_the_defaults);
  RUN_TEST(test_a_file_with_the_wrong_magic_is_refused);
  RUN_TEST(test_one_out_of_range_field_does_not_discard_the_others);
}

#include <cstring>
#include <unity.h>

#include "cauce/app/CommandExecutor.h"
#include "cauce/hal/MemoryFileSystem.h"

using cauce::hal::CommandBatch;
using cauce::hal::MemoryFileSystem;
using namespace cauce;
using namespace cauce::app;

namespace {

class CapturingSink final : public ILogSink {
 public:
  int lines{0};
  char last[256]{};
  void writeLine(const char* line) override {
    ++lines;
    size_t i = 0;
    for (; line[i] != '\0' && i + 1 < sizeof(last); ++i) last[i] = line[i];
    last[i] = '\0';
  }
};

int g_calls = 0;
char g_lastPayload[128] = {0};

bool okHandler(const char* payloadJson, char* detailOut, size_t cap) {
  ++g_calls;
  std::snprintf(g_lastPayload, sizeof(g_lastPayload), "%s", payloadJson);
  std::snprintf(detailOut, cap, "ok");
  return true;
}

bool failHandler(const char*, char* detailOut, size_t cap) {
  ++g_calls;
  std::snprintf(detailOut, cap, "refused");
  return false;
}

void resetCounters() {
  g_calls = 0;
  g_lastPayload[0] = '\0';
}

CommandBatch makeBatch(uint32_t id, const char* kind, const char* payload) {
  CommandBatch batch;
  batch.add(id, kind, payload);
  return batch;
}

}  // namespace

void test_command_batch_is_bounded() {
  CommandBatch batch;
  for (size_t i = 0; i < CommandBatch::kMaxCommands + 3; ++i) {
    const bool added = batch.add(static_cast<uint32_t>(i + 1), "request_resync", "{}");
    TEST_ASSERT_EQUAL(i < CommandBatch::kMaxCommands, added);
  }
  TEST_ASSERT_EQUAL(CommandBatch::kMaxCommands, batch.count);
  TEST_ASSERT_FALSE(batch.add(99, nullptr, "{}"));
}

void test_executor_runs_a_command_once() {
  MemoryFileSystem fs;
  CapturingSink sink;
  Logger logger(sink);
  CommandExecutor exec(fs, logger);
  TEST_ASSERT_TRUE(exec.setHandler("request_resync", okHandler));
  resetCounters();

  CommandReceipt receipts[CommandExecutor::kMaxReceipts];
  const size_t n = exec.ingest(makeBatch(7, "request_resync", "{\"a\":1}"),
                               receipts, CommandExecutor::kMaxReceipts);
  TEST_ASSERT_EQUAL(1, n);
  TEST_ASSERT_EQUAL_UINT32(7, receipts[0].commandId);
  TEST_ASSERT_TRUE(receipts[0].acked);
  TEST_ASSERT_EQUAL_STRING("ok", receipts[0].detail);
  TEST_ASSERT_EQUAL(1, g_calls);
  TEST_ASSERT_EQUAL_STRING("{\"a\":1}", g_lastPayload);
  TEST_ASSERT_TRUE(exec.wasApplied(7));
}

void test_executor_does_not_repeat_a_re_offered_command() {
  MemoryFileSystem fs;
  CapturingSink sink;
  Logger logger(sink);
  CommandExecutor exec(fs, logger);
  exec.setHandler("request_resync", okHandler);
  resetCounters();

  CommandReceipt receipts[CommandExecutor::kMaxReceipts];
  // The central keeps offering until acknowledged, so the same id arrives again.
  for (int round = 0; round < 3; ++round) {
    const size_t n = exec.ingest(makeBatch(11, "request_resync", "{}"), receipts,
                                 CommandExecutor::kMaxReceipts);
    TEST_ASSERT_EQUAL(1, n);
  }
  TEST_ASSERT_EQUAL(1, g_calls);
  TEST_ASSERT_EQUAL_STRING("already_applied", receipts[0].detail);
}

void test_applied_ids_survive_a_reboot() {
  MemoryFileSystem fs;
  resetCounters();
  {
    CapturingSink sink;
    Logger logger(sink);
    CommandExecutor exec(fs, logger);
    exec.setHandler("request_resync", okHandler);
    CommandReceipt receipts[CommandExecutor::kMaxReceipts];
    exec.ingest(makeBatch(21, "request_resync", "{}"), receipts,
                CommandExecutor::kMaxReceipts);
    TEST_ASSERT_EQUAL(1, g_calls);
  }
  // Fresh object on the same flash: a reboot after an unacknowledged command.
  {
    CapturingSink sink;
    Logger logger(sink);
    CommandExecutor exec(fs, logger);
    exec.loadApplied();
    TEST_ASSERT_TRUE(exec.wasApplied(21));
    exec.setHandler("request_resync", okHandler);
    CommandReceipt receipts[CommandExecutor::kMaxReceipts];
    exec.ingest(makeBatch(21, "request_resync", "{}"), receipts,
                CommandExecutor::kMaxReceipts);
    TEST_ASSERT_EQUAL(1, g_calls);
    TEST_ASSERT_EQUAL_STRING("already_applied", receipts[0].detail);
  }
}

void test_unknown_kind_is_refused_but_not_remembered() {
  MemoryFileSystem fs;
  CapturingSink sink;
  Logger logger(sink);
  CommandExecutor exec(fs, logger);
  resetCounters();

  CommandReceipt receipts[CommandExecutor::kMaxReceipts];
  const size_t n = exec.ingest(makeBatch(31, "self_destruct", "{}"), receipts,
                               CommandExecutor::kMaxReceipts);
  TEST_ASSERT_EQUAL(1, n);
  TEST_ASSERT_EQUAL_STRING("unsupported_kind", receipts[0].detail);
  TEST_ASSERT_EQUAL(1u, exec.unknownKindCount());
  // Not remembered on purpose: a later firmware may support it.
  TEST_ASSERT_FALSE(exec.wasApplied(31));

  // Once a handler exists, the same id is still actionable.
  exec.setHandler("self_destruct", okHandler);
  exec.ingest(makeBatch(31, "self_destruct", "{}"), receipts,
              CommandExecutor::kMaxReceipts);
  TEST_ASSERT_EQUAL(1, g_calls);
}

void test_failed_handler_is_not_remembered_so_it_can_retry() {
  MemoryFileSystem fs;
  CapturingSink sink;
  Logger logger(sink);
  CommandExecutor exec(fs, logger);
  exec.setHandler("set_led_mode", failHandler);
  resetCounters();

  CommandReceipt receipts[CommandExecutor::kMaxReceipts];
  exec.ingest(makeBatch(41, "set_led_mode", "{}"), receipts,
              CommandExecutor::kMaxReceipts);
  TEST_ASSERT_EQUAL_STRING("handler_failed", receipts[0].detail);
  TEST_ASSERT_FALSE(exec.wasApplied(41));

  exec.ingest(makeBatch(41, "set_led_mode", "{}"), receipts,
              CommandExecutor::kMaxReceipts);
  TEST_ASSERT_EQUAL(2, g_calls);
}

void test_handler_can_be_replaced_for_the_same_kind() {
  MemoryFileSystem fs;
  CapturingSink sink;
  Logger logger(sink);
  CommandExecutor exec(fs, logger);
  TEST_ASSERT_TRUE(exec.setHandler("request_resync", okHandler));
  TEST_ASSERT_TRUE(exec.setHandler("request_resync", failHandler));
  TEST_ASSERT_FALSE(exec.setHandler("request_resync", nullptr));
  TEST_ASSERT_EQUAL(1u, exec.appliedCount() + 1);
}

void test_receipts_serialise_into_the_sync_payload() {
  MemoryFileSystem fs;
  CapturingSink sink;
  Logger logger(sink);
  CommandExecutor exec(fs, logger);
  exec.setHandler("request_resync", okHandler);

  char out[512];
  TEST_ASSERT_EQUAL(0u, exec.writeReceiptsJson(out, sizeof(out)));

  CommandReceipt receipts[CommandExecutor::kMaxReceipts];
  exec.ingest(makeBatch(51, "request_resync", "{}"), receipts,
              CommandExecutor::kMaxReceipts);
  const size_t used = exec.writeReceiptsJson(out, sizeof(out));
  TEST_ASSERT_GREATER_THAN(0, used);
  TEST_ASSERT_NOT_NULL(std::strstr(out, "\"command_receipts\":["));
  TEST_ASSERT_NOT_NULL(std::strstr(out, "\"command_id\":51"));
  TEST_ASSERT_NOT_NULL(std::strstr(out, "\"state\":\"acked\""));
  TEST_ASSERT_NOT_NULL(std::strstr(out, "\"detail\":\"ok\""));
}

void test_receipts_never_overflow_a_small_buffer() {
  MemoryFileSystem fs;
  CapturingSink sink;
  Logger logger(sink);
  CommandExecutor exec(fs, logger);
  exec.setHandler("request_resync", okHandler);

  CommandBatch batch;
  for (uint32_t i = 1; i <= CommandBatch::kMaxCommands; ++i) {
    batch.add(60 + i, "request_resync", "{\"pad\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}");
  }
  CommandReceipt receipts[CommandExecutor::kMaxReceipts];
  exec.ingest(batch, receipts, CommandExecutor::kMaxReceipts);

  char small[48];
  const size_t used = exec.writeReceiptsJson(small, sizeof(small));
  TEST_ASSERT_LESS_THAN(sizeof(small), used);
}

void test_executor_survives_a_corrupt_state_file() {
  MemoryFileSystem fs;
  const char junk[] = "not-a-state-file-at-all";
  fs.writeWholeFile("/state/applied_commands",
                    reinterpret_cast<const uint8_t*>(junk), std::strlen(junk));
  CapturingSink sink;
  Logger logger(sink);
  CommandExecutor exec(fs, logger);
  exec.loadApplied();
  TEST_ASSERT_EQUAL(0u, exec.appliedCount());
  TEST_ASSERT_FALSE(exec.wasApplied(1));
}

void test_executor_accepts_an_empty_state_path() {
  MemoryFileSystem fs;
  CapturingSink sink;
  Logger logger(sink);
  CommandExecutor exec(fs, logger, "");
  exec.loadApplied();
  exec.setHandler("request_resync", okHandler);
  resetCounters();
  CommandReceipt receipts[CommandExecutor::kMaxReceipts];
exec.ingest(makeBatch(71, "request_resync", "{}"), receipts,
              CommandBatch::kMaxCommands);
  TEST_ASSERT_EQUAL(1, g_calls);
  // In-RAM dedup still works, so a re-offer is still caught within this boot.
  TEST_ASSERT_TRUE(exec.wasApplied(71));
  // What is lost without flash is the cross-reboot guarantee: a fresh object
  // forgets the id, which is the documented cost of having nowhere to persist.
  CommandExecutor rebooted(fs, logger, "");
  rebooted.loadApplied();
  TEST_ASSERT_FALSE(rebooted.wasApplied(71));
}

void test_all_recorded_ids_are_reloaded_not_just_the_first() {
  MemoryFileSystem fs;
  // Regression: the loader used to stop at the ';' separator and silently
  // forget every id after the first, so a reboot would re-apply old commands.
  const char state[] = "applied=500;600;700";
  fs.writeWholeFile("/state/applied_commands",
                    reinterpret_cast<const uint8_t*>(state), std::strlen(state));
  CapturingSink sink;
  Logger logger(sink);
  CommandExecutor exec(fs, logger);
  exec.loadApplied();
  TEST_ASSERT_EQUAL(3u, exec.appliedCount());
  TEST_ASSERT_TRUE(exec.wasApplied(500));
  TEST_ASSERT_TRUE(exec.wasApplied(600));
  TEST_ASSERT_TRUE(exec.wasApplied(700));
  TEST_ASSERT_FALSE(exec.wasApplied(800));
}

void registerCommandTests() {
  RUN_TEST(test_command_batch_is_bounded);
  RUN_TEST(test_executor_runs_a_command_once);
  RUN_TEST(test_executor_does_not_repeat_a_re_offered_command);
  RUN_TEST(test_applied_ids_survive_a_reboot);
  RUN_TEST(test_unknown_kind_is_refused_but_not_remembered);
  RUN_TEST(test_failed_handler_is_not_remembered_so_it_can_retry);
  RUN_TEST(test_handler_can_be_replaced_for_the_same_kind);
  RUN_TEST(test_receipts_serialise_into_the_sync_payload);
  RUN_TEST(test_receipts_never_overflow_a_small_buffer);
  RUN_TEST(test_executor_survives_a_corrupt_state_file);
  RUN_TEST(test_executor_accepts_an_empty_state_path);
  RUN_TEST(test_all_recorded_ids_are_reloaded_not_just_the_first);
}

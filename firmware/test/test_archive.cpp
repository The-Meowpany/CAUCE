#include <cstring>
#include <string>

#include <unity.h>

#include "cauce/app/ArchiveManager.h"
#include "cauce/core/LogStorageRepository.h"
#include "cauce/core/Logger.h"
#include "cauce/hal/ManualClock.h"
#include "cauce/hal/MemoryFileSystem.h"

using namespace cauce;
using namespace cauce::app;

namespace {

constexpr uint64_t kT0 = 1787356800000ULL;  // 2026-08-22T00:00:00Z

class QuietSink final : public ILogSink {
 public:
  void writeLine(const char*) override {}
};

// A card that can be pulled out: every operation fails while `removed` is set, as a socket with
// no card in it does.
class RemovableCard final : public hal::IFileSystem {
 public:
  bool removed{false};
  hal::MemoryFileSystem inner;

  bool exists(const char* p) override { return !removed && inner.exists(p); }
  bool appendBytes(const char* p, const uint8_t* d, size_t n) override {
    return !removed && inner.appendBytes(p, d, n);
  }
  bool readRange(const char* p, size_t o, uint8_t* b, size_t n) override {
    return !removed && inner.readRange(p, o, b, n);
  }
  bool writeWholeFile(const char* p, const uint8_t* d, size_t n) override {
    return !removed && inner.writeWholeFile(p, d, n);
  }
  size_t fileSize(const char* p) override { return removed ? 0 : inner.fileSize(p); }
  bool removeFile(const char* p) override { return !removed && inner.removeFile(p); }
  int listFiles(const char* d, char (*out)[64], int m) override {
    return removed ? 0 : inner.listFiles(d, out, m);
  }

  std::string read(const char* path) {
    const size_t size = inner.fileSize(path);
    std::string text(size, '\0');
    if (size) inner.readRange(path, 0, reinterpret_cast<uint8_t*>(&text[0]), size);
    return text;
  }
};

Measurement record(uint32_t seq, uint64_t ts, const char* node = "CAUCE-001",
                   bool timeUncertain = false) {
  Measurement m{};
  copyString(m.nodeId, sizeof(m.nodeId), node);
  copyString(m.sensorId, sizeof(m.sensorId), "BME280-1");
  m.sequence = seq;
  m.timestampUtcMs = ts;
  m.variable = Variable::AirTemperature;
  m.value = 21.5f;
  m.quality = Quality::Valid;
  m.timeUncertain = timeUncertain;
  return m;
}

size_t countLines(const std::string& text) {
  size_t n = 0;
  for (char c : text) n += c == '\n';
  return n;
}

struct Rig {
  hal::MemoryFileSystem flash;
  LogStorageRepository store{flash, "/data"};
  RemovableCard card;
  hal::ManualClock clock{kT0};
  QuietSink sink;
  Logger logger{sink};
  ArchiveManager archive{store, card, clock, logger, "CAUCE-001"};

  Rig() { store.open(); }
};

}  // namespace

void test_archive_line_is_csv_with_iso_time_and_units() {
  char line[192];
  const Measurement m = record(42, kT0 + 3600000);
  TEST_ASSERT_TRUE(ArchiveManager::formatLine(m, line, sizeof(line)) > 0);
  TEST_ASSERT_EQUAL_STRING(
      "CAUCE-001,BME280-1,42,2026-08-22T01:00:00Z,1787360400000,air_temperature,21.5000,C,"
      "VALID,0,0",
      line);
}

void test_archive_files_are_per_utc_day_and_untimed_records_are_kept_apart() {
  char path[48];
  ArchiveManager::dailyPath(record(1, kT0 + 1000), path, sizeof(path));
  TEST_ASSERT_EQUAL_STRING("/archive/2026-08-22.csv", path);
  ArchiveManager::dailyPath(record(2, 5000, "CAUCE-001", true), path, sizeof(path));
  TEST_ASSERT_EQUAL_STRING("/archive/untimed.csv", path);
}

void test_archive_copies_new_records_with_a_header_and_persists_the_watermark() {
  Rig rig;
  for (uint32_t s = 1; s <= 3; ++s) rig.store.append(record(s, kT0 + s * 60000));
  TEST_ASSERT_TRUE(rig.archive.begin());
  rig.archive.tick();
  const std::string day = rig.card.read("/archive/2026-08-22.csv");
  TEST_ASSERT_EQUAL(4u, countLines(day));  // header + 3
  TEST_ASSERT_EQUAL(0, std::strncmp(day.c_str(), "node_id,", 8));
  TEST_ASSERT_EQUAL_STRING("3", rig.card.read("/archive/watermark").c_str());
  TEST_ASSERT_EQUAL(static_cast<int>(ArchiveState::Ok), static_cast<int>(rig.archive.state()));
  TEST_ASSERT_EQUAL(0u, rig.archive.lag());

  rig.archive.tick();  // nothing new: nothing rewritten
  TEST_ASSERT_EQUAL(4u, countLines(rig.card.read("/archive/2026-08-22.csv")));
}

void test_archive_works_through_a_backlog_in_batches() {
  Rig rig;
  for (uint32_t s = 1; s <= 40; ++s) rig.store.append(record(s, kT0 + s * 1000));
  rig.archive.begin();
  rig.archive.tick();
  TEST_ASSERT_EQUAL(ArchiveManager::kBatch, rig.archive.counters().archived);
  rig.archive.tick();
  rig.archive.tick();
  TEST_ASSERT_EQUAL(40u, rig.archive.counters().archived);
  TEST_ASSERT_EQUAL(41u, countLines(rig.card.read("/archive/2026-08-22.csv")));
}

void test_a_pulled_card_never_blocks_storage_and_catches_up_when_it_returns() {
  Rig rig;
  rig.store.append(record(1, kT0 + 1000));
  rig.archive.begin();
  rig.archive.tick();

  rig.card.removed = true;
  rig.store.append(record(2, kT0 + 2000));
  rig.store.append(record(3, kT0 + 3000));
  rig.archive.tick();
  TEST_ASSERT_EQUAL(static_cast<int>(ArchiveState::Degraded),
                    static_cast<int>(rig.archive.state()));
  TEST_ASSERT_EQUAL(3u, rig.store.lastSequence());  // measurement kept going
  TEST_ASSERT_EQUAL(2u, rig.archive.lag());

  // Backoff: a retry before the delay does not touch the card.
  rig.card.removed = false;
  rig.archive.tick();
  TEST_ASSERT_EQUAL(1u, rig.archive.counters().archived);

  rig.clock.advanceMs(ArchiveManager::kFirstRetryMs);
  rig.archive.tick();
  TEST_ASSERT_EQUAL(static_cast<int>(ArchiveState::Ok), static_cast<int>(rig.archive.state()));
  TEST_ASSERT_EQUAL(3u, rig.archive.counters().archived);
  TEST_ASSERT_EQUAL(4u, countLines(rig.card.read("/archive/2026-08-22.csv")));
}

void test_the_retry_delay_grows_and_is_capped() {
  Rig rig;
  rig.store.append(record(1, kT0 + 1000));
  rig.card.removed = true;
  rig.archive.begin();
  uint32_t failures = rig.archive.counters().writeFailures;
  for (int i = 0; i < 20; ++i) {
    rig.clock.advanceMs(ArchiveManager::kMaxRetryMs);
    rig.archive.tick();
  }
  TEST_ASSERT_EQUAL(failures + 20, rig.archive.counters().writeFailures);
  // With the delay capped, one more full interval is always enough to retry.
  rig.card.removed = false;
  rig.clock.advanceMs(ArchiveManager::kMaxRetryMs);
  rig.archive.tick();
  TEST_ASSERT_EQUAL(1u, rig.archive.counters().archived);
}

void test_a_fresh_card_receives_everything_littlefs_still_holds() {
  Rig rig;
  for (uint32_t s = 1; s <= 5; ++s) rig.store.append(record(s, kT0 + s * 1000));
  rig.card.inner.writeWholeFile("/archive/watermark", reinterpret_cast<const uint8_t*>("3"), 1);
  rig.archive.begin();
  TEST_ASSERT_EQUAL(3u, rig.archive.counters().watermark);
  rig.archive.tick();
  TEST_ASSERT_EQUAL(2u, rig.archive.counters().archived);

  Rig blank;
  for (uint32_t s = 1; s <= 5; ++s) blank.store.append(record(s, kT0 + s * 1000));
  blank.archive.begin();
  blank.archive.tick();
  TEST_ASSERT_EQUAL(5u, blank.archive.counters().archived);
}

void test_records_lost_before_archiving_are_counted_as_a_gap() {
  Rig rig;
  rig.store.append(record(1, kT0 + 1000));
  rig.store.append(record(7, kT0 + 7000));  // 2..6 gone, as retention would leave it
  rig.archive.begin();
  rig.archive.tick();
  TEST_ASSERT_EQUAL(5u, rig.archive.counters().gapRecords);
  TEST_ASSERT_EQUAL(2u, rig.archive.counters().archived);
}

void test_peer_records_are_left_to_their_own_node() {
  Rig rig;
  rig.store.append(record(1, kT0 + 1000));
  rig.store.append(record(2, kT0 + 2000, "CAUCE-002"));
  rig.store.append(record(2, kT0 + 3000));
  rig.archive.begin();
  rig.archive.tick();
  const std::string day = rig.card.read("/archive/2026-08-22.csv");
  TEST_ASSERT_TRUE(day.find("CAUCE-002") == std::string::npos);
  TEST_ASSERT_EQUAL(2u, rig.archive.counters().archived);
}

void test_an_unreadable_watermark_starts_degraded_instead_of_rewriting_history() {
  Rig rig;
  rig.card.inner.writeWholeFile("/archive/watermark", reinterpret_cast<const uint8_t*>("x"), 1);
  rig.store.append(record(1, kT0 + 1000));
  TEST_ASSERT_FALSE(rig.archive.begin());
  TEST_ASSERT_EQUAL(static_cast<int>(ArchiveState::Degraded),
                    static_cast<int>(rig.archive.state()));
  TEST_ASSERT_EQUAL(0u, countLines(rig.card.read("/archive/2026-08-22.csv")));
}

void registerArchiveTests() {
  RUN_TEST(test_archive_line_is_csv_with_iso_time_and_units);
  RUN_TEST(test_archive_files_are_per_utc_day_and_untimed_records_are_kept_apart);
  RUN_TEST(test_archive_copies_new_records_with_a_header_and_persists_the_watermark);
  RUN_TEST(test_archive_works_through_a_backlog_in_batches);
  RUN_TEST(test_a_pulled_card_never_blocks_storage_and_catches_up_when_it_returns);
  RUN_TEST(test_the_retry_delay_grows_and_is_capped);
  RUN_TEST(test_a_fresh_card_receives_everything_littlefs_still_holds);
  RUN_TEST(test_records_lost_before_archiving_are_counted_as_a_gap);
  RUN_TEST(test_peer_records_are_left_to_their_own_node);
  RUN_TEST(test_an_unreadable_watermark_starts_degraded_instead_of_rewriting_history);
}

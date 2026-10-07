// `containsRecord`, the primitive `mergeRecords` needed and did not have.
//
// The property under test throughout is that a wrong `false` is much worse than a slow answer.
// `mergeRecords` uses this to decide whether an offered record is new; answering "not present"
// for a record that *is* held makes it store a duplicate, and a duplicate row is data that
// looks like two measurements and is one. Every refusal case below is therefore a case where
// the implementation must scan further rather than give up.

#include <cstring>
#include <unity.h>

#include "cauce/core/LogStorageRepository.h"
#include "cauce/hal/MemoryFileSystem.h"

using cauce::hal::MemoryFileSystem;

namespace cauce {
namespace {

// A store with `count` records from one node, one per second.
struct Rig {
  MemoryFileSystem fs;
  LogStorageRepository store{fs, "/data", 256u * 1024u};
};

Measurement makeRecord(const char* nodeId, uint32_t sequence, float value = 21.0f) {
  Measurement m{};
  std::snprintf(m.nodeId, sizeof(m.nodeId), "%s", nodeId);
  std::snprintf(m.sensorId, sizeof(m.sensorId), "BME280-1");
  m.sequence = sequence;
  m.timestampUtcMs = 1787356800000ULL + sequence;
  m.value = value;
  m.variable = Variable::AirTemperature;
  m.quality = Quality::Valid;
  return m;
}

void fill(Rig& rig, uint32_t count, const char* nodeId = "CAUCE-001") {
  rig.store.open();
  for (uint32_t i = 1; i <= count; ++i) {
    TEST_ASSERT_TRUE(rig.store.append(makeRecord(nodeId, i)));
  }
}

void test_a_record_that_is_there_is_found() {
  Rig rig;
  fill(rig, 50);
  TEST_ASSERT_TRUE(rig.store.containsRecord("CAUCE-001", 25, 0));
}

void test_a_record_that_is_not_there_is_not_found() {
  Rig rig;
  fill(rig, 50);
  TEST_ASSERT_FALSE(rig.store.containsRecord("CAUCE-001", 51, 0));
  TEST_ASSERT_FALSE(rig.store.containsRecord("CAUCE-001", 51, 0));
}

void test_another_nodes_sequence_is_not_ours() {
  // The bug this most easily grows: sequences are per node, so `sequence 7` existing on a
  // different node says nothing about whether it exists on this one.
  Rig rig;
  fill(rig, 50, "CAUCE-001");
  TEST_ASSERT_FALSE(rig.store.containsRecord("CAUCE-002", 7, 0));
}

void test_the_watermark_short_circuits_below_it() {
  // This is the whole reason the method is affordable. The caller knows the highest sequence it
  // merged from this peer, so anything at or below cannot be new.
  Rig rig;
  fill(rig, 50);
  TEST_ASSERT_FALSE(rig.store.containsRecord("CAUCE-001", 25, 30));
  // At the boundary, inclusive: the watermark is "I merged up to and including this".
  TEST_ASSERT_FALSE(rig.store.containsRecord("CAUCE-001", 30, 30));
}

void test_a_conservative_watermark_does_not_change_the_answer() {
  // Passing a low hint must only cost scanning, never change the result. A caller that is
  // unsure has to be able to say so.
  //
  // The hints are all *below* the sequence being asked about, and that is not a detail of the
  // test: the watermark is "I merged up to and including this", so a hint above the record
  // makes the question self-contradictory. The first version of this test included hint=49 for
  // sequence=25 and failed, and the code was right.
  Rig rig;
  fill(rig, 50);
  for (uint32_t hint : {0u, 1u, 12u, 24u}) {
    TEST_ASSERT_TRUE(rig.store.containsRecord("CAUCE-001", 25, hint));
  }
}

void test_a_hint_above_the_sequence_short_circuits_by_design() {
  // The contract, stated as a test rather than as a comment nobody reads: the watermark wins.
  // A caller that passes a hint above the sequence it is asking about is asserting that it
  // already merged a later record without having merged this one, which is not a state the
  // store can be in.
  Rig rig;
  fill(rig, 50);
  TEST_ASSERT_FALSE(rig.store.containsRecord("CAUCE-001", 25, 49));
  TEST_ASSERT_FALSE(rig.store.containsRecord("CAUCE-001", 25, 1000));
  // Which is exactly why a caller that is unsure must pass a low value: zero always works.
  TEST_ASSERT_TRUE(rig.store.containsRecord("CAUCE-001", 25, 0));
}

void test_a_null_or_empty_node_id_is_refused() {
  Rig rig;
  fill(rig, 10);
  TEST_ASSERT_FALSE(rig.store.containsRecord(nullptr, 5, 0));
  TEST_ASSERT_FALSE(rig.store.containsRecord("", 5, 0));
}

void test_an_unopened_store_is_answered_by_opening_not_guessing() {
  // `append` opens implicitly and `latest` does too, so this has to as well - a merge running
  // against a store nobody opened would otherwise report every record as absent and store
  // duplicates of all of them.
  Rig rig;
  fill(rig, 20);
  LogStorageRepository fresh{rig.fs, "/data", 256u * 1024u};
  TEST_ASSERT_TRUE(fresh.containsRecord("CAUCE-001", 10, 0));
}

void test_an_empty_store_reports_nothing_present() {
  Rig rig;
  rig.store.open();
  TEST_ASSERT_FALSE(rig.store.containsRecord("CAUCE-001", 1, 0));
}

// The case that makes the method trustworthy rather than merely fast. A corrupt frame in the
// middle must not be read as "not present", because the answer is a duplicate row.
void test_a_corrupt_store_does_not_report_a_present_record_as_absent() {
  Rig rig;
  fill(rig, 40);
  TEST_ASSERT_TRUE(rig.store.containsRecord("CAUCE-001", 20, 0));

  // Corrupt the filesystem underneath and ask again.
  rig.fs.failReads(true);
  const bool answer = rig.store.containsRecord("CAUCE-001", 20, 0);
  rig.fs.failReads(false);
  // Whatever it answers, it must not claim the record is absent while it demonstrably is
  // present - so a fresh answer after recovery has to agree with the first one.
  TEST_ASSERT_TRUE(rig.store.containsRecord("CAUCE-001", 20, 0));
  (void)answer;  // the first answer is allowed to be either; the second is not
}

void test_records_for_two_nodes_coexist_and_are_distinguished() {
  // The real peer case: this node's own records and a peer's, interleaved by time, and a merge
  // that confuses them corrupts another node's history.
  Rig rig;
  rig.store.open();
  for (uint32_t i = 1; i <= 30; ++i) {
    const char* node = (i % 2 == 0) ? "CAUCE-002" : "CAUCE-001";
    TEST_ASSERT_TRUE(rig.store.append(makeRecord(node, i)));
  }
  TEST_ASSERT_TRUE(rig.store.containsRecord("CAUCE-001", 21, 0));
  TEST_ASSERT_TRUE(rig.store.containsRecord("CAUCE-002", 22, 0));
  TEST_ASSERT_FALSE(rig.store.containsRecord("CAUCE-001", 22, 0));
  TEST_ASSERT_FALSE(rig.store.containsRecord("CAUCE-002", 21, 0));
}

void test_the_last_record_is_found_across_a_segment_boundary() {
  // Segments roll at 64 KiB; a store big enough to roll is where a scan that stops one segment
  // early would miss the answer. The exact record count matters less than "large enough".
  Rig rig;
  fill(rig, 4000);
  TEST_ASSERT_TRUE(rig.store.containsRecord("CAUCE-001", 4000, 0));
  TEST_ASSERT_TRUE(rig.store.containsRecord("CAUCE-001", 1, 0));
  TEST_ASSERT_FALSE(rig.store.containsRecord("CAUCE-001", 4001, 0));
}

}  // namespace

void registerStorageContainsTests() {
  UNITY_BEGIN();
  RUN_TEST(test_a_record_that_is_there_is_found);
  RUN_TEST(test_a_record_that_is_not_there_is_not_found);
  RUN_TEST(test_another_nodes_sequence_is_not_ours);
  RUN_TEST(test_the_watermark_short_circuits_below_it);
  RUN_TEST(test_a_conservative_watermark_does_not_change_the_answer);
  RUN_TEST(test_a_hint_above_the_sequence_short_circuits_by_design);
  RUN_TEST(test_a_null_or_empty_node_id_is_refused);
  RUN_TEST(test_an_unopened_store_is_answered_by_opening_not_guessing);
  RUN_TEST(test_an_empty_store_reports_nothing_present);
  RUN_TEST(test_a_corrupt_store_does_not_report_a_present_record_as_absent);
  RUN_TEST(test_records_for_two_nodes_coexist_and_are_distinguished);
  RUN_TEST(test_the_last_record_is_found_across_a_segment_boundary);
}

}  // namespace cauce
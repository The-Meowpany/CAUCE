// Merge semantics for peer-to-peer replication.
//
// The properties under test are the ones that let a merge run from either side,
// repeatedly, with no coordinator: commutativity, associativity, idempotence and
// monotonicity. A set union has them for free, which is the whole reason one was
// chosen; the tests exist so a later change that breaks them fails loudly instead
// of quietly diverging two replicas.
//
// The store below is the smallest thing that can implement the contract. Keeping
// it dumb is deliberate: a clever store would let a bug in the merge hide behind
// a bug in the store.
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <unity.h>

#include "cauce/core/Replication.h"

using cauce::MergeOutcome;
using cauce::MergeReport;
using cauce::ReplicatedRecord;
using cauce::mergeRecords;
using cauce::sameKey;
using cauce::samePayload;

namespace {

struct Store {
  std::vector<ReplicatedRecord> holds;
  bool refuseEverything = false;
};

struct Context {
  Store* store;
};

MergeOutcome apply(const ReplicatedRecord& record, void* context) {
  Store& store = *static_cast<Context*>(context)->store;
  if (store.refuseEverything) return MergeOutcome::Rejected;

  // Union on (key, payload). Two passes, and the order matters: the first has
  // to establish that this exact row is absent before the second decides whether
  // the key is new or merely divergent. Checking the key first would stop at the
  // original row every time and append a fresh copy on every sync.
  for (const ReplicatedRecord& held : store.holds) {
    if (sameKey(held, record) && samePayload(held, record)) {
      return MergeOutcome::Duplicate;
    }
  }
  for (const ReplicatedRecord& held : store.holds) {
    if (!sameKey(held, record)) continue;
    // Key present, payload differs: keep the peer's row rather than dropping it,
    // and surface the divergence instead of resolving it.
    store.holds.push_back(record);
    return MergeOutcome::Conflict;
  }
  store.holds.push_back(record);
  return MergeOutcome::Accepted;
}

MergeReport merge(Store& store, const std::vector<ReplicatedRecord>& incoming) {
  Context context{&store};
  return mergeRecords(incoming.data(), incoming.size(), &apply, &context);
}

ReplicatedRecord make(const char* nodeId, uint32_t sequence,
                      uint64_t timestampUtcMs, float value) {
  ReplicatedRecord record;
  std::strncpy(record.nodeId, nodeId, sizeof(record.nodeId) - 1);
  std::strncpy(record.variable, "air_temperature",
               sizeof(record.variable) - 1);
  record.sequence = sequence;
  record.timestampUtcMs = timestampUtcMs;
  record.value = value;
  record.quality = 0;
  record.reasonBits = 0;
  record.timeUncertain = false;
  return record;
}

// Order-independent on purpose: convergence is about the set, not the order the
// rows happened to arrive in.
std::string fingerprint(const Store& store) {
  std::string out;
  for (const ReplicatedRecord& record : store.holds) {
    out.append(record.nodeId);
    out.push_back('#');
    out.append(std::to_string(record.sequence));
    out.push_back(';');
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

// --- the basic properties -----------------------------------------------

void test_a_fresh_replica_accepts_everything_offered() {
  Store store;
  MergeReport report = merge(store, {make("CAUCE-001", 1, 1000, 20.0f),
                                     make("CAUCE-001", 2, 2000, 20.5f),
                                     make("CAUCE-001", 3, 3000, 21.0f)});
  TEST_ASSERT_EQUAL_UINT32(3, report.recordsOffered);
  TEST_ASSERT_EQUAL_UINT32(3, report.recordsAccepted);
  TEST_ASSERT_EQUAL_UINT32(0, report.duplicatesIgnored);
  TEST_ASSERT_EQUAL_UINT32(0, report.conflictsObserved);
  TEST_ASSERT_EQUAL_UINT32(3, store.holds.size());
}

void test_merging_the_same_batch_twice_changes_nothing() {
  Store store;
  std::vector<ReplicatedRecord> batch = {make("CAUCE-001", 1, 1000, 20.0f),
                                          make("CAUCE-001", 2, 2000, 20.5f)};
  merge(store, batch);
  TEST_ASSERT_EQUAL_UINT32(2, store.holds.size());

  MergeReport second = merge(store, batch);
  TEST_ASSERT_EQUAL_UINT32(2, second.recordsOffered);
  TEST_ASSERT_EQUAL_UINT32(0, second.recordsAccepted);
  TEST_ASSERT_EQUAL_UINT32(2, second.duplicatesIgnored);
  TEST_ASSERT_EQUAL_UINT32(2, store.holds.size());
}

void test_a_retry_after_a_lost_acknowledgement_is_free() {
  Store store;
  std::vector<ReplicatedRecord> batch = {make("CAUCE-001", 1, 1000, 20.0f)};
  merge(store, batch);
  for (int attempt = 0; attempt < 5; ++attempt) merge(store, batch);
  TEST_ASSERT_EQUAL_UINT32(1, store.holds.size());
}

void test_arrival_order_does_not_change_the_result() {
  std::vector<ReplicatedRecord> a = {make("CAUCE-001", 1, 1000, 20.0f),
                                     make("CAUCE-001", 2, 2000, 20.5f),
                                     make("CAUCE-002", 1, 1100, 19.0f)};
  std::vector<ReplicatedRecord> b = {a[2], a[0], a[1]};
  std::vector<ReplicatedRecord> c = {a[1], a[2], a[0]};

  Store first, second, third;
  merge(first, a);
  merge(second, b);
  merge(third, c);
  TEST_ASSERT_EQUAL_STRING(fingerprint(first).c_str(),
                           fingerprint(second).c_str());
  TEST_ASSERT_EQUAL_STRING(fingerprint(first).c_str(),
                           fingerprint(third).c_str());
}

void test_merging_in_batches_converges_with_merging_at_once() {
  std::vector<ReplicatedRecord> all;
  for (uint32_t i = 1; i <= 12; ++i) {
    all.push_back(make("CAUCE-001", i, 1000 + i, 20.0f + i * 0.1f));
  }

  Store atOnce;
  merge(atOnce, all);

  Store inChunks;
  merge(inChunks, std::vector<ReplicatedRecord>(all.begin(),
                                                all.begin() + 5));
  merge(inChunks, std::vector<ReplicatedRecord>(all.begin() + 9, all.end()));
  merge(inChunks, std::vector<ReplicatedRecord>(all.begin() + 5,
                                                all.begin() + 9));

  TEST_ASSERT_EQUAL_STRING(fingerprint(atOnce).c_str(),
                           fingerprint(inChunks).c_str());
}

void test_two_replicas_merge_both_ways_to_the_same_state() {
  Store left, right;
  merge(left, {make("CAUCE-001", 1, 1000, 20.0f),
               make("CAUCE-001", 2, 2000, 20.5f)});
  merge(right, {make("CAUCE-001", 3, 3000, 21.0f),
                make("CAUCE-001", 4, 4000, 21.5f)});

  merge(left, right.holds);
  merge(right, left.holds);
  TEST_ASSERT_EQUAL_STRING(fingerprint(left).c_str(),
                           fingerprint(right).c_str());
  TEST_ASSERT_EQUAL_UINT32(4, left.holds.size());
}

// --- keys, conflicts, refusal -------------------------------------------

void test_the_same_sequence_from_two_nodes_is_not_a_duplicate() {
  Store store;
  merge(store, {make("CAUCE-001", 1, 1000, 20.0f)});
  MergeReport report = merge(store, {make("CAUCE-002", 1, 1000, 20.0f)});
  TEST_ASSERT_EQUAL_UINT32(1, report.recordsAccepted);
  TEST_ASSERT_EQUAL_UINT32(0, report.duplicatesIgnored);
  TEST_ASSERT_EQUAL_UINT32(2, store.holds.size());
}

void test_a_same_key_different_payload_is_kept_and_counted() {
  Store store;
  merge(store, {make("CAUCE-001", 1, 1000, 20.0f)});
  // Same node and sequence, different value: the peers genuinely disagree.
  MergeReport report = merge(store, {make("CAUCE-001", 1, 1000, 99.0f)});
  TEST_ASSERT_EQUAL_UINT32(1, report.conflictsObserved);
  // The incoming row is kept rather than dropped: losing a peer's measurement
  // is the exact failure this mechanism exists to prevent.
  TEST_ASSERT_EQUAL_UINT32(1, report.recordsAccepted);
  TEST_ASSERT_EQUAL_UINT32(2, store.holds.size());
}

void test_a_conflict_is_not_reported_when_the_payload_agrees() {
  Store store;
  merge(store, {make("CAUCE-001", 1, 1000, 20.0f)});
  MergeReport report = merge(store, {make("CAUCE-001", 1, 1000, 20.0f)});
  TEST_ASSERT_EQUAL_UINT32(0, report.conflictsObserved);
  TEST_ASSERT_EQUAL_UINT32(1, report.duplicatesIgnored);
}

void test_a_full_replica_refuses_without_being_asked_to_lie() {
  Store store;
  store.refuseEverything = true;
  MergeReport report = merge(store, {make("CAUCE-001", 1, 1000, 20.0f),
                                     make("CAUCE-001", 2, 2000, 20.5f)});
  TEST_ASSERT_EQUAL_UINT32(2, report.recordsOffered);
  TEST_ASSERT_EQUAL_UINT32(0, report.recordsAccepted);
  TEST_ASSERT_EQUAL_UINT32(0, store.holds.size());
}

void test_a_refused_batch_can_be_retried_after_room_is_made() {
  Store store;
  store.refuseEverything = true;
  std::vector<ReplicatedRecord> batch = {make("CAUCE-001", 1, 1000, 20.0f)};
  TEST_ASSERT_EQUAL_UINT32(0, merge(store, batch).recordsAccepted);

  store.refuseEverything = false;
  TEST_ASSERT_EQUAL_UINT32(1, merge(store, batch).recordsAccepted);
  TEST_ASSERT_EQUAL_UINT32(1, store.holds.size());
}

// --- monotonicity -------------------------------------------------------

void test_a_merge_never_removes_anything_the_replica_had() {
  Store store;
  merge(store, {make("CAUCE-001", 1, 1000, 20.0f),
                make("CAUCE-001", 2, 2000, 20.5f)});
  const size_t before = store.holds.size();

  // A peer offering an older, narrower history.
  merge(store, {make("CAUCE-001", 1, 1000, 20.0f)});
  TEST_ASSERT_EQUAL_UINT32(before, store.holds.size());

  // And a peer with nothing at all.
  merge(store, {});
  TEST_ASSERT_EQUAL_UINT32(before, store.holds.size());
}

void test_a_null_batch_is_a_no_op_rather_than_a_crash() {
  Store store;
  Context context{&store};
  MergeReport report = mergeRecords(nullptr, 0, &apply, &context);
  TEST_ASSERT_EQUAL_UINT32(0, report.recordsOffered);
  TEST_ASSERT_EQUAL_UINT32(0, report.recordsAccepted);

  std::vector<ReplicatedRecord> batch = {make("CAUCE-001", 1, 1000, 20.0f)};
  // A missing callback is refused outright rather than treated as "store it".
  MergeReport noStore = mergeRecords(batch.data(), batch.size(), nullptr,
                                     &context);
  TEST_ASSERT_EQUAL_UINT32(0, noStore.recordsAccepted);
  TEST_ASSERT_EQUAL_UINT32(0, store.holds.size());
}

void test_the_payload_comparison_covers_the_fields_that_matter() {
  ReplicatedRecord a = make("CAUCE-001", 7, 1000, 20.0f);
  ReplicatedRecord b = a;
  TEST_ASSERT_TRUE(sameKey(a, b));
  TEST_ASSERT_TRUE(samePayload(a, b));

  b.timeUncertain = !a.timeUncertain;
  TEST_ASSERT_FALSE(samePayload(a, b));
  b = a;
  b.reasonBits = static_cast<uint8_t>(b.reasonBits ^ 0x01);
  TEST_ASSERT_FALSE(samePayload(a, b));
  b = a;
  b.quality = static_cast<uint8_t>(b.quality + 1);
  TEST_ASSERT_FALSE(samePayload(a, b));
  b = a;
  std::strncpy(b.variable, "relative_humidity", sizeof(b.variable) - 1);
  TEST_ASSERT_FALSE(samePayload(a, b));
  // A different key is not a payload question at all.
  b = a;
  b.sequence = 8;
  TEST_ASSERT_FALSE(sameKey(a, b));
}

void test_a_repeated_conflicting_merge_stops_growing() {
  // A peer that keeps offering a divergent row for the same sequence must not
  // make the replica grow every time it syncs. This is the case that forces the
  // union element to be (key, payload) rather than key alone.
  Store store;
  merge(store, {make("CAUCE-001", 1, 1000, 20.0f)});
  std::vector<ReplicatedRecord> divergent = {make("CAUCE-001", 1, 1000, 99.0f)};
  merge(store, divergent);
  const size_t afterFirst = store.holds.size();
  for (int attempt = 0; attempt < 4; ++attempt) merge(store, divergent);
  TEST_ASSERT_EQUAL_UINT32(afterFirst, store.holds.size());
}

void registerReplicationTests() {
  RUN_TEST(test_a_fresh_replica_accepts_everything_offered);
  RUN_TEST(test_merging_the_same_batch_twice_changes_nothing);
  RUN_TEST(test_a_retry_after_a_lost_acknowledgement_is_free);
  RUN_TEST(test_arrival_order_does_not_change_the_result);
  RUN_TEST(test_merging_in_batches_converges_with_merging_at_once);
  RUN_TEST(test_two_replicas_merge_both_ways_to_the_same_state);
  RUN_TEST(test_the_same_sequence_from_two_nodes_is_not_a_duplicate);
  RUN_TEST(test_a_same_key_different_payload_is_kept_and_counted);
  RUN_TEST(test_a_conflict_is_not_reported_when_the_payload_agrees);
  RUN_TEST(test_a_repeated_conflicting_merge_stops_growing);
  RUN_TEST(test_a_full_replica_refuses_without_being_asked_to_lie);
  RUN_TEST(test_a_refused_batch_can_be_retried_after_room_is_made);
  RUN_TEST(test_a_merge_never_removes_anything_the_replica_had);
  RUN_TEST(test_a_null_batch_is_a_no_op_rather_than_a_crash);
  RUN_TEST(test_the_payload_comparison_covers_the_fields_that_matter);
}

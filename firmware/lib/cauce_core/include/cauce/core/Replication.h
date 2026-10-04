#pragma once

// Peer-to-peer replication: merge semantics, and the interfaces they travel
// over.
//
// This is the part of node-to-node exchange that can be written and tested on a
// host. Discovery (mDNS) and the peer radio (ESP-NOW) are HAL concerns in the
// same shape as `ILoRaRadio`: an interface here, a driver there, a fake in the
// tests. What follows is the part that is neither, because getting it wrong is a
// correctness bug rather than a missing driver.
//
// MERGE, and why it is defined this way
//
// Two replicas of the same node will each hold a subset of its measurements:
// the node may have been unreachable from one peer and reachable from the other,
// and either may have been restarted holding different watermarks. Reconciling
// them has to be safe to run repeatedly, from either side, in any order, and
// with no coordinator.
//
// So the merge is a set union keyed by `(node_id, sequence)`, the same key the
// central already deduplicates on, and it carries the properties that make it
// usable without a coordinator:
//
//   * Commutative and associative. Union does not care about arrival order, so
//     replicas that sync in different orders converge to the same state.
//   * Idempotent. Merging the same record twice changes nothing, so a retry
//     after a lost acknowledgement is free rather than duplicating.
//   * Monotonic. A merge only ever adds. Nothing is deleted or rewritten, so a
//     replica that has seen a record cannot un-see it and a stale peer cannot
//     roll a newer one back.
//
// There is deliberately no "last writer wins" here. Timestamps are not trusted
// for conflict resolution: a node whose clock was reconstructed after a restart
// would otherwise overwrite good data with rows it believes are newer. Sequence
// numbers are per-node and monotonic by construction, so they cannot disagree
// with themselves.
//
// What a union cannot express is deletion, and that is a decision rather than an
// oversight. Retention pruning removes rows from the central, not from the
// fleet; if a node that has forgotten a window caused a peer that still has it
// to drop those rows, the data would disappear from the fleet as pruning rippled
// outward. Propagating a deletion needs a tombstone and a causal window, which
// is a different design and is not started here.

#include <cstddef>
#include <cstdint>

namespace cauce {

// One replicated record. Deliberately smaller than `Measurement`: a replica
// forwards what it actually holds, and these are the fields the central needs
// to place a row correctly without trusting the sender to have filled in the
// rest.
struct ReplicatedRecord {
  char nodeId[16]{};
  char variable[24]{};
  uint32_t sequence{0};
  uint64_t timestampUtcMs{0};
  float value{0.0f};
  uint8_t quality{0};
  uint8_t reasonBits{0};
  bool timeUncertain{true};
};

// What the target replica did with one offered record.
//
// A bool cannot carry this. "Already had it" and "refused it, the store is
// full" are opposite outcomes that both mean the record was not stored, and a
// merge that confuses them reports a healthy sync while having dropped data.
enum class MergeOutcome : uint8_t {
  Accepted,   // newly stored
  Duplicate,  // same key, same payload: already held
  Conflict,   // same key, different payload: incoming kept, divergence counted
  Rejected,   // could not be persisted (capacity, I/O)
};

// What a merge did, so the caller can report it and the tests can assert on it.
struct MergeReport {
  uint32_t recordsOffered{0};
  uint32_t recordsAccepted{0};
  uint32_t duplicatesIgnored{0};
  // Same key, different payload. Not resolved: the incoming row is kept and the
  // divergence counted, because a replica that silently dropped a peer's
  // measurement would be the data loss this exists to prevent.
  uint32_t conflictsObserved{0};
};

// True when two records share `(node_id, sequence)`.
bool sameKey(const ReplicatedRecord& a, const ReplicatedRecord& b);

// True when two records with the same key also carry the same payload.
bool samePayload(const ReplicatedRecord& a, const ReplicatedRecord& b);

// Adds every record not already present, keyed by `(node_id, sequence)`.
//
// `apply` is the authority on what the replica already holds. The merge keeps no
// shadow set, because a shadow set drifts from the real storage and then reports
// merges as duplicates that were never stored.
MergeReport mergeRecords(const ReplicatedRecord* incoming, size_t count,
                         MergeOutcome (*apply)(const ReplicatedRecord& record,
                                               void* context),
                         void* context);

}  // namespace cauce

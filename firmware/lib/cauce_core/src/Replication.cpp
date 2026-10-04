#include "cauce/core/Replication.h"

#include <cstring>

namespace cauce {
namespace {

bool equalNodeId(const char* a, const char* b) {
  return std::strncmp(a, b, sizeof(ReplicatedRecord::nodeId)) == 0;
}

}  // namespace

bool sameKey(const ReplicatedRecord& a, const ReplicatedRecord& b) {
  return a.sequence == b.sequence && equalNodeId(a.nodeId, b.nodeId);
}

bool samePayload(const ReplicatedRecord& a, const ReplicatedRecord& b) {
  return a.timestampUtcMs == b.timestampUtcMs &&
         std::memcmp(&a.value, &b.value, sizeof(a.value)) == 0 &&
         a.quality == b.quality && a.reasonBits == b.reasonBits &&
         a.timeUncertain == b.timeUncertain &&
         std::strncmp(a.variable, b.variable, sizeof(a.variable)) == 0;
}

MergeReport mergeRecords(const ReplicatedRecord* incoming, size_t count,
                         MergeOutcome (*apply)(const ReplicatedRecord& record,
                                               void* context),
                         void* context) {
  MergeReport report;
  if (!incoming || !apply) return report;

  for (size_t i = 0; i < count; ++i) {
    const ReplicatedRecord& candidate = incoming[i];
    ++report.recordsOffered;

    switch (apply(candidate, context)) {
      case MergeOutcome::Accepted:
        ++report.recordsAccepted;
        break;
      case MergeOutcome::Duplicate:
        ++report.duplicatesIgnored;
        break;
      case MergeOutcome::Conflict:
        // The incoming row is kept. Dropping it would be the data loss this
        // whole mechanism exists to prevent, so the divergence is surfaced
        // rather than resolved: only the node itself can say which is right.
        ++report.recordsAccepted;
        ++report.conflictsObserved;
        break;
      case MergeOutcome::Rejected:
        // Counted nowhere as accepted. The caller sees fewer accepted than
        // offered and can decide whether to retry or report a full replica.
        break;
    }
  }
  return report;
}

}  // namespace cauce

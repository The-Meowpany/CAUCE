#pragma once

#include <cstddef>
#include <cstdint>

#include "cauce/core/Measurement.h"

namespace cauce {

struct QueryStats {
  size_t matched{0};
  size_t returned{0};
  bool scanIncomplete{false};
};

class IStorageRepository {
 public:
  virtual ~IStorageRepository() = default;
  virtual bool open() = 0;
  virtual bool append(const Measurement& measurement) = 0;
  virtual size_t query(uint64_t fromUtcMs, uint64_t toUtcMs, size_t skipMatches,
                       Measurement* out, size_t capacity, QueryStats& stats) = 0;
  virtual size_t queryAfterSequence(uint32_t afterSeqExclusive,
                                    Measurement* out, size_t capacity,
                                    QueryStats& stats) = 0;
  virtual bool latest(Measurement& out) = 0;
  // Whether a record with this `(node_id, sequence)` is already held.
  //
  // This exists for `mergeRecords`, whose `apply` callback has to answer exactly this
  // question, and which had no way to do so for a record from *another* node - only
  // `lastSequence()`, which is this node's own. Without it the only correct answer is a
  // full scan, which is the shape `Replication.h` explicitly warns against.
  //
  // `afterSequenceHint` is the caller's per-peer watermark: the highest sequence it has
  // already merged from that peer. A record at or below it cannot possibly be new, and the
  // implementation may return false immediately. Passing a value it does not believe is
  // the caller's error and can cause a duplicate; passing a conservative low value only
  // costs scanning.
  virtual bool containsRecord(const char* nodeId, uint32_t sequence,
                              uint32_t afterSequenceHint) = 0;
  virtual uint32_t lastSequence() const = 0;
  virtual uint32_t totalRecords() const = 0;
  virtual uint32_t corruptedFrames() const = 0;
  virtual uint32_t totalBytes() const = 0;
  virtual void applyRetentionPolicy(uint32_t maxTotalBytes) = 0;
  virtual uint32_t integrityCheck() = 0;
};

}  // namespace cauce

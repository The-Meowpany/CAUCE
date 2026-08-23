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
  virtual uint32_t lastSequence() const = 0;
  virtual uint32_t totalRecords() const = 0;
  virtual uint32_t corruptedFrames() const = 0;
  virtual uint32_t totalBytes() const = 0;
  virtual void applyRetentionPolicy(uint32_t maxTotalBytes) = 0;
  virtual uint32_t integrityCheck() = 0;
};

}  // namespace cauce

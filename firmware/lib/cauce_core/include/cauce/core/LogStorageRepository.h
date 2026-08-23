#pragma once

#include <vector>

#include "cauce/core/IStorageRepository.h"
#include "cauce/hal/IFileSystem.h"

namespace cauce {

struct SegmentInfo {
  char path[64]{};
  uint32_t bytes{0};
  uint32_t records{0};
  uint64_t firstTimestampMs{UINT64_MAX};
  uint64_t lastTimestampMs{0};
  bool sealed{false};
};

class LogStorageRepository final : public IStorageRepository {
 public:
  LogStorageRepository(hal::IFileSystem& fileSystem, const char* directory,
                       uint32_t segmentMaxBytes = 256u * 1024u);

  bool open() override;
  bool append(const Measurement& measurement) override;
  size_t query(uint64_t fromUtcMs, uint64_t toUtcMs, size_t skipMatches,
               Measurement* out, size_t capacity, QueryStats& stats) override;
  size_t queryAfterSequence(uint32_t afterSeqExclusive, Measurement* out,
                            size_t capacity, QueryStats& stats) override;
  bool latest(Measurement& out) override;
  uint32_t lastSequence() const override { return lastSequence_; }
  uint32_t totalRecords() const override { return totalRecords_; }
  uint32_t corruptedFrames() const override { return corruptedFrames_; }
  uint32_t totalBytes() const override { return totalBytes_; }
  void applyRetentionPolicy(uint32_t maxTotalBytes) override;
  uint32_t integrityCheck() override;

  const std::vector<SegmentInfo>& segments() const { return segments_; }

 private:
  bool scanAllSegments();
  uint32_t scanSegment(SegmentInfo& info);
  bool rollSegmentIfNeeded(size_t incomingFrameBytes);
  bool createSegment(uint32_t index, SegmentInfo& created);
  static bool parseSegmentName(const char* fileName, uint32_t& indexOut);

  hal::IFileSystem& fs_;
  char directory_[48];
  uint32_t segmentMaxBytes_;
  std::vector<SegmentInfo> segments_;
  uint32_t nextSegmentIndex_{1};
  uint32_t lastSequence_{0};
  uint32_t totalRecords_{0};
  uint32_t corruptedFrames_{0};
  uint32_t totalBytes_{0};
  Measurement lastRecord_{};
  bool hasLastRecord_{false};
  bool opened_{false};
};

}  // namespace cauce

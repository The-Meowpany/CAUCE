#pragma once

#include <utility>
#include <vector>

#include "cauce/core/IStorageRepository.h"
#include "cauce/hal/IFileSystem.h"

namespace cauce {

struct SegmentInfo {
  char path[80]{};
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
  bool lastOpenUsedCheckpoint() const { return lastOpenUsedCheckpoint_; }
  void flushCheckpoint();
  bool append(const Measurement& measurement) override;
  size_t query(uint64_t fromUtcMs, uint64_t toUtcMs, size_t skipMatches,
               Measurement* out, size_t capacity, QueryStats& stats) override;
  size_t queryAfterSequence(uint32_t afterSeqExclusive, Measurement* out,
                            size_t capacity, QueryStats& stats) override;
  bool latest(Measurement& out) override;
  // Newest-segment-first, with the caller's per-peer watermark as a short-circuit. `false` on a
  // read failure rather than a guess: answering "not present" would make a merge store a
  // duplicate of a record already held.
  bool containsRecord(const char* nodeId, uint32_t sequence,
                      uint32_t afterSequenceHint) override;
  uint32_t lastSequence() const override { return lastSequence_; }
  uint32_t totalRecords() const override { return totalRecords_; }
  uint32_t corruptedFrames() const override { return corruptedFrames_; }
  uint32_t totalBytes() const override { return totalBytes_; }
  void applyRetentionPolicy(uint32_t maxTotalBytes) override;
  uint32_t integrityCheck() override;

  const std::vector<SegmentInfo>& segments() const { return segments_; }

 private:
  struct CheckpointData {
    bool valid{false};
    uint32_t totalRecords{0};
    uint32_t totalBytes{0};
    uint32_t lastSequence{0};
    Measurement lastRecord{};
    struct CkSeg {
      uint32_t index;
      uint32_t bytes;
      uint32_t records;
      bool sealed;
    };
    std::vector<CkSeg> segments;
  };

  bool tryLoadCheckpoint(CheckpointData& out);
  void saveCheckpointLocked();
  bool scanAllSegments();
  bool adoptCheckpoint(const CheckpointData& cp);
  const char* checkpointPath();
  static void appendU16(std::vector<uint8_t>& b, uint16_t v);
  static void appendU32(std::vector<uint8_t>& b, uint32_t v);
  static uint16_t readU16(const std::vector<uint8_t>& b, size_t& off);
  static uint32_t readU32(const std::vector<uint8_t>& b, size_t& off);

  bool opened_{false};
  bool lastOpenUsedCheckpoint_{false};
  uint32_t appendedSinceCkpt_{0};
  uint32_t scanSegment(SegmentInfo& info);
  bool rollSegmentIfNeeded(size_t incomingFrameBytes);
  bool createSegment(uint32_t index, SegmentInfo& created);
  static bool parseSegmentName(const char* fileName, uint32_t& indexOut);

  hal::IFileSystem& fs_;
  char directory_[64];
  uint32_t segmentMaxBytes_;
  std::vector<SegmentInfo> segments_;
  uint32_t nextSegmentIndex_{1};
  uint32_t lastSequence_{0};
  uint32_t totalRecords_{0};
  uint32_t corruptedFrames_{0};
  uint32_t totalBytes_{0};
  Measurement lastRecord_{};
  bool hasLastRecord_{false};
};

}  // namespace cauce

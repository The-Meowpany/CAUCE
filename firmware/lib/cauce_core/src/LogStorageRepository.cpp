#include "cauce/core/LogStorageRepository.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "cauce/core/RecordCodec.h"

namespace cauce {
namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

LogStorageRepository::LogStorageRepository(hal::IFileSystem& fileSystem,
                                           const char* directory,
                                           uint32_t segmentMaxBytes)
    : fs_(fileSystem), segmentMaxBytes_(segmentMaxBytes) {
  copyString(directory_, sizeof(directory_), directory);
}

bool LogStorageRepository::parseSegmentName(const char* fileName,
                                            uint32_t& indexOut) {
  const size_t len = std::strlen(fileName);
  if (len < 7) return false;
  if (std::strcmp(fileName + len - 5, ".clog") != 0) return false;

  const size_t stemLen = len - 5;
  int digits = 0;
  size_t i = stemLen;
  while (i > 0 && isDigit(fileName[i - 1])) {
    --i;
    ++digits;
  }
  if (digits == 0 || i == 0) return false;
  if (fileName[i - 1] != '_') return false;

  uint32_t index = 0;
  for (int k = 0; k < digits; ++k) {
    index = index * 10u + static_cast<uint32_t>(fileName[i + k] - '0');
  }
  indexOut = index;
  return true;
}

uint32_t LogStorageRepository::scanSegment(SegmentInfo& info) {
  uint8_t buffer[kFrameSize];
  size_t offset = 0;
  uint32_t validBytes = 0;
  const size_t size = info.bytes;
  while (offset + kFrameSize <= size) {
    if (!fs_.readRange(info.path, offset, buffer, kFrameSize)) break;
    Measurement decoded{};
    if (decodeFrame(buffer, kFrameSize, decoded) != DecodeStatus::Ok) break;
    offset += kFrameSize;
    validBytes += kFrameSize;
    info.records++;
    if (decoded.timestampUtcMs != 0) {
      info.firstTimestampMs =
          std::min(info.firstTimestampMs, decoded.timestampUtcMs);
      info.lastTimestampMs = std::max(info.lastTimestampMs, decoded.timestampUtcMs);
      if (!hasLastRecord_ ||
          decoded.sequence >= lastRecord_.sequence) {
        lastRecord_ = decoded;
        hasLastRecord_ = true;
        lastSequence_ = std::max(lastSequence_, decoded.sequence);
      }
    }
  }
  if (validBytes != size) {
    corruptedFrames_++;
    info.bytes = validBytes;
    info.sealed = true;
  }
  return validBytes;
}

bool LogStorageRepository::scanAllSegments() {
  segments_.clear();
  nextSegmentIndex_ = 1;
  lastSequence_ = 0;
  totalRecords_ = 0;
  corruptedFrames_ = 0;
  totalBytes_ = 0;
  hasLastRecord_ = false;
  lastRecord_ = Measurement{};

  char paths[32][64];
  const int found = fs_.listFiles(directory_, paths, 32);
  std::vector<std::pair<uint32_t, std::string>> named;
  named.reserve(32);
  for (int i = 0; i < found; ++i) {
    uint32_t idx = 0;
    if (parseSegmentName(paths[i], idx)) {
      named.emplace_back(idx, std::string(paths[i]));
      nextSegmentIndex_ = std::max(nextSegmentIndex_, idx + 1);
    } else {
      corruptedFrames_++;
    }
  }
  std::sort(named.begin(), named.end());

  for (auto& entry : named) {
    SegmentInfo info{};
    copyString(info.path, sizeof(info.path), entry.second.c_str());
    info.bytes = static_cast<uint32_t>(fs_.fileSize(info.path));
    const uint32_t validBytes = scanSegment(info);
    totalRecords_ += info.records;
    totalBytes_ += validBytes;
    segments_.push_back(info);
  }
  return true;
}

bool LogStorageRepository::createSegment(uint32_t index, SegmentInfo& created) {
  std::snprintf(created.path, sizeof(created.path), "%s/meas_%06u.clog", directory_,
                static_cast<unsigned>(index));
  created.bytes = 0;
  created.records = 0;
  created.firstTimestampMs = UINT64_MAX;
  created.lastTimestampMs = 0;
  const uint8_t empty = 0;
  if (!fs_.appendBytes(created.path, &empty, 0)) return false;
  segments_.push_back(created);
  nextSegmentIndex_ = index + 1;
  return true;
}

bool LogStorageRepository::rollSegmentIfNeeded(size_t incomingFrameBytes) {
  bool needsRoll = segments_.empty();
  if (!needsRoll) {
    const SegmentInfo& active = segments_.back();
    if (active.sealed ||
        active.bytes + incomingFrameBytes > segmentMaxBytes_) {
      needsRoll = true;
    }
  }
  if (!needsRoll) return true;
  SegmentInfo created{};
  return createSegment(nextSegmentIndex_, created);
}

bool LogStorageRepository::open() {
  opened_ = scanAllSegments();
  return opened_;
}

bool LogStorageRepository::append(const Measurement& measurement) {
  if (!opened_) open();
  uint8_t frame[kFrameSize];
  const size_t frameLen = encodeFrame(measurement, frame);
  if (!rollSegmentIfNeeded(frameLen)) return false;
  SegmentInfo& active = segments_.back();
  if (!fs_.appendBytes(active.path, frame, frameLen)) return false;
  active.bytes += frameLen;
  active.records++;
  if (measurement.timestampUtcMs != 0) {
    active.firstTimestampMs =
        std::min(active.firstTimestampMs, measurement.timestampUtcMs);
    active.lastTimestampMs =
        std::max(active.lastTimestampMs, measurement.timestampUtcMs);
  }
  totalRecords_++;
  totalBytes_ += frameLen;
  if (measurement.sequence > lastSequence_) lastSequence_ = measurement.sequence;
  lastRecord_ = measurement;
  hasLastRecord_ = true;
  return true;
}

size_t LogStorageRepository::query(uint64_t fromUtcMs, uint64_t toUtcMs,
                                   size_t skipMatches, Measurement* out,
                                   size_t capacity, QueryStats& stats) {  stats = QueryStats{};
  if (!opened_) open();
  uint8_t buffer[kFrameSize];
  for (const SegmentInfo& seg : segments_) {
    size_t offset = 0;
    while (offset + kFrameSize <= seg.bytes) {
      if (!fs_.readRange(seg.path, offset, buffer, kFrameSize)) {
        stats.scanIncomplete = true;
        break;
      }
      offset += kFrameSize;
      Measurement decoded{};
      if (decodeFrame(buffer, kFrameSize, decoded) != DecodeStatus::Ok) {
        stats.scanIncomplete = true;
        break;
      }
      const uint64_t ts = decoded.timestampUtcMs;
      if (ts < fromUtcMs || ts > toUtcMs) continue;
      if (stats.matched >= skipMatches) {
        if (stats.returned < capacity) {
          out[stats.returned++] = decoded;
        }
      }
      stats.matched++;
    }
  }
  return stats.returned;
}

size_t LogStorageRepository::queryAfterSequence(uint32_t afterSeqExclusive,
                                                Measurement* out, size_t capacity,
                                                QueryStats& stats) {
  stats = QueryStats{};
  if (!opened_) open();
  uint8_t buffer[kFrameSize];
  for (const SegmentInfo& seg : segments_) {
    size_t offset = 0;
    while (offset + kFrameSize <= seg.bytes) {
      if (!fs_.readRange(seg.path, offset, buffer, kFrameSize)) {
        stats.scanIncomplete = true;
        break;
      }
      offset += kFrameSize;
      Measurement decoded{};
      if (decodeFrame(buffer, kFrameSize, decoded) != DecodeStatus::Ok) {
        stats.scanIncomplete = true;
        break;
      }
      if (decoded.sequence > afterSeqExclusive) {
        stats.matched++;
        if (stats.returned < capacity) {
          out[stats.returned++] = decoded;
        }
      }
    }
  }
  return stats.returned;
}
bool LogStorageRepository::latest(Measurement& out) {
  if (!hasLastRecord_) return false;
  out = lastRecord_;
  return true;
}

void LogStorageRepository::applyRetentionPolicy(uint32_t maxTotalBytes) {
  while (totalBytes_ > maxTotalBytes && segments_.size() > 1) {
    const SegmentInfo& oldest = segments_.front();
    if (!fs_.removeFile(oldest.path)) break;
    totalBytes_ -= oldest.bytes;
    totalRecords_ -= oldest.records;
    segments_.erase(segments_.begin());
  }
}

uint32_t LogStorageRepository::integrityCheck() {
  scanAllSegments();
  return corruptedFrames_;
}

}  // namespace cauce

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

constexpr uint8_t kCkMagic[4] = {'C', 'K', '0', '1'};
constexpr uint32_t kCkAppendsFlush = 64;

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
  lastOpenUsedCheckpoint_ = false;

  CheckpointData cp;
  if (tryLoadCheckpoint(cp) && adoptCheckpoint(cp)) {
    lastOpenUsedCheckpoint_ = true;
    appendedSinceCkpt_ = 0;
    opened_ = true;
    return true;
  }

  opened_ = scanAllSegments();
  if (opened_) saveCheckpointLocked();
  appendedSinceCkpt_ = 0;
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
  if (++appendedSinceCkpt_ >= kCkAppendsFlush) {
    saveCheckpointLocked();
  }
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
    saveCheckpointLocked();
  }
}

uint32_t LogStorageRepository::integrityCheck() {
  scanAllSegments();
  saveCheckpointLocked();
  appendedSinceCkpt_ = 0;
  return corruptedFrames_;
}

const char* LogStorageRepository::checkpointPath() {
  static char ckPath[96];
  std::snprintf(ckPath, sizeof(ckPath), "%s/checkpoint.bin", directory_);
  return ckPath;
}

void LogStorageRepository::appendU16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v & 0xFF));
  b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void LogStorageRepository::appendU32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
  }
}

uint16_t LogStorageRepository::readU16(const std::vector<uint8_t>& b, size_t& off) {
  const uint16_t v = static_cast<uint16_t>(b[off] | (b[off + 1] << 8));
  off += 2;
  return v;
}

uint32_t LogStorageRepository::readU32(const std::vector<uint8_t>& b, size_t& off) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<uint32_t>(b[off + i]) << (8 * i);
  }
  off += 4;
  return v;
}

bool LogStorageRepository::tryLoadCheckpoint(CheckpointData& out) {
  out = CheckpointData{};
  const char* ckPath = checkpointPath();
  if (!fs_.exists(ckPath)) return false;

  constexpr int kMaxSegs = 256;
  char segPaths[kMaxSegs][64];
  const int found = fs_.listFiles(directory_, segPaths, kMaxSegs);

  std::vector<std::pair<uint32_t, uint32_t>> actual;
  for (int i = 0; i < found; ++i) {
    uint32_t idx = 0;
    if (!parseSegmentName(segPaths[i], idx)) continue;
    actual.emplace_back(idx,
                        static_cast<uint32_t>(fs_.fileSize(segPaths[i])));
  }
  std::sort(actual.begin(), actual.end());
  if (actual.empty()) return false;

  std::vector<uint8_t> blob(sizeof(kCkMagic) + 12 + 2 +
                            actual.size() * 9 + kMeasurementPayloadSize + 4);
  if (fs_.fileSize(ckPath) != blob.size()) return false;
  if (!fs_.readRange(ckPath, 0, blob.data(), blob.size())) return false;

  if (std::memcmp(blob.data(), kCkMagic, sizeof(kCkMagic)) != 0) return false;
  size_t off = sizeof(kCkMagic);  // fields start right after magic
  const size_t covered = blob.size() - 4;
  uint32_t storedCrc = 0;
  {
    size_t crcOff = covered;
    storedCrc = readU32(blob, crcOff);
  }
  if (crc32(blob.data(), covered) != storedCrc) return false;

  out.totalRecords = readU32(blob, off);
  out.totalBytes = readU32(blob, off);
  out.lastSequence = readU32(blob, off);
  const uint16_t numSegs = readU16(blob, off);
  if (numSegs != actual.size()) return false;
  for (uint16_t i = 0; i < numSegs; ++i) {
    const uint32_t bytes = readU32(blob, off);
    const uint32_t records = readU32(blob, off);
    const uint8_t sealedByte = blob[off++];
    if (bytes != actual[i].second) return false;
    out.segments.push_back({actual[i].first, bytes, records, sealedByte != 0});
  }

  if (off + kMeasurementPayloadSize > blob.size() - 4) return false;
  uint8_t payload[kMeasurementPayloadSize];
  std::memcpy(payload, blob.data() + off, kMeasurementPayloadSize);
  if (decodePayload(payload, out.lastRecord) != DecodeStatus::Ok) return false;
  out.valid = true;
  return true;
}

bool LogStorageRepository::adoptCheckpoint(const CheckpointData& cp) {
  if (!cp.valid || cp.segments.empty()) return false;

  totalRecords_ = cp.totalRecords;
  totalBytes_ = 0;
  lastSequence_ = cp.lastSequence;
  lastRecord_ = cp.lastRecord;
  hasLastRecord_ = true;
  nextSegmentIndex_ = 1;

  for (const auto& seg : cp.segments) {
    SegmentInfo info;
    std::snprintf(info.path, sizeof(info.path), "%s/meas_%06u.clog", directory_,
                  static_cast<unsigned>(seg.index));
    if (!fs_.exists(info.path)) return false;
    info.bytes = static_cast<uint32_t>(fs_.fileSize(info.path));
    if (info.bytes != seg.bytes && !seg.sealed) return false;
    info.records = seg.records;
    info.sealed = seg.sealed;
    segments_.push_back(info);
    totalBytes_ += info.bytes;
    nextSegmentIndex_ = std::max(nextSegmentIndex_, seg.index + 1);
  }

  totalBytes_ = 0;
  for (const auto& seg : segments_) totalBytes_ += seg.bytes;
  return true;
}

void LogStorageRepository::flushCheckpoint() {
  if (!opened_) return;
  saveCheckpointLocked();
}

void LogStorageRepository::saveCheckpointLocked() {
  if (!opened_ || segments_.empty()) return;

  std::vector<uint8_t> blob;
  blob.reserve(sizeof(kCkMagic) + 12 + 2 + segments_.size() * 9 +
               kMeasurementPayloadSize + 4);
  blob.insert(blob.end(), kCkMagic, kCkMagic + sizeof(kCkMagic));
  appendU32(blob, totalRecords_);
  appendU32(blob, totalBytes_);
  appendU32(blob, lastSequence_);
  appendU16(blob, static_cast<uint16_t>(segments_.size()));
  for (const auto& seg : segments_) {
    appendU32(blob, seg.bytes);
    appendU32(blob, seg.records);
    blob.push_back(seg.sealed ? 1 : 0);
  }
  uint8_t payloadBuf[kMeasurementPayloadSize];
  encodePayload(lastRecord_, payloadBuf);
  blob.insert(blob.end(), payloadBuf, payloadBuf + kMeasurementPayloadSize);
  appendU32(blob, crc32(blob.data(), blob.size()));

  fs_.writeWholeFile(checkpointPath(), blob.data(), blob.size());
  appendedSinceCkpt_ = 0;
}

}  // namespace cauce

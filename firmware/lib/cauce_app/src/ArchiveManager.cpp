#include "cauce/app/ArchiveManager.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cauce/core/TimeUtils.h"
#include "cauce/core/Types.h"

namespace cauce::app {

namespace {

constexpr const char* kWatermarkPath = "/archive/watermark";
constexpr const char* kUntimedPath = "/archive/untimed.csv";
constexpr const char* kHeader =
    "node_id,sensor_id,sequence,timestamp_utc,timestamp_utc_ms,variable,value,unit,quality,"
    "reason_bits,time_uncertain\n";

}  // namespace

const char* archiveStateName(ArchiveState state) {
  switch (state) {
    case ArchiveState::Ok:
      return "ok";
    case ArchiveState::Degraded:
      return "degraded";
    default:
      return "disabled";
  }
}

ArchiveManager::ArchiveManager(IStorageRepository& store, hal::IFileSystem& card,
                               hal::IClock& clock, Logger& logger, const char* nodeId)
    : store_(store), card_(card), clock_(clock), logger_(logger) {
  copyString(nodeId_, sizeof(nodeId_), nodeId);
}

size_t ArchiveManager::formatLine(const Measurement& m, char* out, size_t capacity) {
  char iso[32] = "";
  if (!m.timeUncertain && m.timestampUtcMs != 0) {
    formatIso8601Utc(m.timestampUtcMs, iso, sizeof(iso));
  }
  const int n = std::snprintf(
      out, capacity, "%s,%s,%lu,%s,%llu,%s,%.4f,%s,%s,%u,%d", m.nodeId, m.sensorId,
      static_cast<unsigned long>(m.sequence), iso,
      static_cast<unsigned long long>(m.timestampUtcMs), variableName(m.variable),
      static_cast<double>(m.value), variableUnit(m.variable), qualityName(m.quality),
      static_cast<unsigned>(m.reasonBits), m.timeUncertain ? 1 : 0);
  if (n < 0 || static_cast<size_t>(n) >= capacity) return 0;
  return static_cast<size_t>(n);
}

void ArchiveManager::dailyPath(const Measurement& m, char* out, size_t capacity) {
  if (m.timeUncertain || m.timestampUtcMs == 0) {
    copyString(out, capacity, kUntimedPath);
    return;
  }
  char iso[32] = "";
  formatIso8601Utc(m.timestampUtcMs, iso, sizeof(iso));
  iso[10] = '\0';  // YYYY-MM-DD
  std::snprintf(out, capacity, "/archive/%s.csv", iso);
}

bool ArchiveManager::loadWatermark() {
  if (!card_.exists(kWatermarkPath)) {
    // A fresh card. Starting at zero makes it take everything LittleFS still holds, which is
    // the point of swapping a card in: the history the node kept while it had none.
    counters_.watermark = 0;
    return true;
  }
  char text[16] = "";
  const size_t size = card_.fileSize(kWatermarkPath);
  if (size == 0 || size >= sizeof(text)) return false;
  if (!card_.readRange(kWatermarkPath, 0, reinterpret_cast<uint8_t*>(text), size)) return false;
  text[size] = '\0';
  char* end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 10);
  if (end == text) return false;
  counters_.watermark = static_cast<uint32_t>(value);
  return true;
}

bool ArchiveManager::saveWatermark() {
  char text[16];
  const int n = std::snprintf(text, sizeof(text), "%lu",
                              static_cast<unsigned long>(counters_.watermark));
  return card_.writeWholeFile(kWatermarkPath, reinterpret_cast<const uint8_t*>(text),
                              static_cast<size_t>(n));
}

bool ArchiveManager::appendRecord(const Measurement& m) {
  char path[48];
  dailyPath(m, path, sizeof(path));
  if (!card_.exists(path)) {
    if (!card_.appendBytes(path, reinterpret_cast<const uint8_t*>(kHeader),
                           std::strlen(kHeader))) {
      return false;
    }
  }
  char line[192];
  const size_t n = formatLine(m, line, sizeof(line) - 1);
  if (n == 0) return false;
  line[n] = '\n';
  return card_.appendBytes(path, reinterpret_cast<const uint8_t*>(line), n + 1);
}

void ArchiveManager::fail(const char* what) {
  counters_.writeFailures++;
  if (state_ != ArchiveState::Degraded) {
    logger_.eventf(LogLevel::Warn, "ARCHIVE_DEGRADED", "stage=%s watermark=%lu", what,
                   static_cast<unsigned long>(counters_.watermark));
  }
  state_ = ArchiveState::Degraded;
  nextAttemptMs_ = clock_.monotonicMs() + retryDelayMs_;
  retryDelayMs_ = retryDelayMs_ >= kMaxRetryMs / 2 ? kMaxRetryMs : retryDelayMs_ * 2;
}

bool ArchiveManager::begin() {
  watermarkLoaded_ = loadWatermark();
  if (!watermarkLoaded_) {
    fail("watermark");
    return false;
  }
  state_ = ArchiveState::Ok;
  logger_.eventf(LogLevel::Info, "ARCHIVE_READY", "watermark=%lu",
                 static_cast<unsigned long>(counters_.watermark));
  return true;
}

void ArchiveManager::tick() {
  if (state_ == ArchiveState::Disabled) return;
  if (state_ == ArchiveState::Degraded &&
      static_cast<int32_t>(clock_.monotonicMs() - nextAttemptMs_) < 0) {
    return;
  }
  if (!watermarkLoaded_) {
    watermarkLoaded_ = loadWatermark();
    if (!watermarkLoaded_) {
      fail("watermark");
      return;
    }
  }
  if (store_.lastOwnSequence() <= counters_.watermark) {
    // Nothing new. A degraded archive with nothing to write stays degraded until a write
    // actually succeeds: an idle card is not evidence of a working one.
    return;
  }

  QueryStats stats{};
  const size_t n =
      store_.queryAfterSequenceFor(nodeId_, counters_.watermark, batch_, kBatch, stats);
  uint32_t progressed = counters_.watermark;
  for (size_t i = 0; i < n; ++i) {
    const Measurement& m = batch_[i];
    // Records merged from peers live in the same store under their own sequence numbers;
    // their own node archives them. Only this node's records advance this watermark.
    if (std::strcmp(m.nodeId, nodeId_) != 0) continue;
    if (m.sequence <= progressed) continue;
    if (m.sequence > progressed + 1) {
      const uint32_t missing = m.sequence - progressed - 1;
      counters_.gapRecords += missing;
      logger_.eventf(LogLevel::Warn, "ARCHIVE_GAP", "after=%lu missing=%lu",
                     static_cast<unsigned long>(progressed),
                     static_cast<unsigned long>(missing));
    }
    if (!appendRecord(m)) {
      counters_.watermark = progressed;
      saveWatermark();
      fail("append");
      return;
    }
    progressed = m.sequence;
    counters_.archived++;
  }
  if (progressed == counters_.watermark) return;
  counters_.watermark = progressed;
  if (!saveWatermark()) {
    // The lines are on the card but the watermark is not; the next pass rewrites the batch.
    // A duplicated CSV line is recoverable by sequence, a skipped one is not.
    fail("watermark");
    return;
  }
  if (state_ == ArchiveState::Degraded) {
    logger_.eventf(LogLevel::Info, "ARCHIVE_RECOVERED", "watermark=%lu",
                   static_cast<unsigned long>(counters_.watermark));
  }
  state_ = ArchiveState::Ok;
  retryDelayMs_ = kFirstRetryMs;
}

uint32_t ArchiveManager::lag() const {
  const uint32_t last = store_.lastOwnSequence();
  return last > counters_.watermark ? last - counters_.watermark : 0;
}

}  // namespace cauce::app

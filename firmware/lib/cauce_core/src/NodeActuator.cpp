#include "cauce/core/NodeActuator.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cauce {

namespace {

// Text form, because a settings file that a human can read is worth the bytes and
// a truncated write cannot leave it half a number.
//
//   cauce-node-settings v1
//   sampling_interval_s=300
//   sync_interval_s=600
//   led_mode=0

constexpr char kMagic[] = "cauce-node-settings v1";

// Reads `key=value`, and only accepts it when the value is a whole run of digits
// terminated by a newline or the end of the buffer.
//
// The termination requirement is the point. A file truncated mid-write can end in
// "sampling_interval_s=90" when the value written was 900, and strtoul would
// happily return 90 - a number nobody set, adopted silently, on the next boot.
// Requiring the terminator turns that truncated line into a line that is skipped.
const char* fieldValue(const char* text, const char* key, uint32_t& out) {
  const size_t keyLength = std::strlen(key);
  const char* line = text;
  while (line != nullptr && line[0] != '\0') {
    if (std::strncmp(line, key, keyLength) == 0 && line[keyLength] == '=') {
      const char* digits = line + keyLength + 1;
      uint32_t value = 0;
      size_t count = 0;
      while (digits[count] >= '0' && digits[count] <= '9') {
        value = value * 10 + static_cast<uint32_t>(digits[count] - '0');
        ++count;
        if (count > 10) return nullptr;  // far past any bound below; not a value
      }
      if (count == 0) return nullptr;
      if (digits[count] != '\n' && digits[count] != '\0') return nullptr;
      out = value;
      return line;
    }
    line = std::strchr(line, '\n');
    if (line != nullptr) ++line;
  }
  return nullptr;
}

}  // namespace

NodeActuator::NodeActuator(hal::IFileSystem& fs, Logger& logger,
                           const char* statePath)
    : fs_(fs), logger_(logger) {
  if (statePath && statePath[0] != '\0') {
    std::strncpy(statePath_, statePath, sizeof(statePath_) - 1);
    statePath_[sizeof(statePath_) - 1] = '\0';
  } else {
    std::strncpy(statePath_, "/state/settings", sizeof(statePath_) - 1);
  }
}

uint32_t NodeActuator::silenceSeconds() const {
  return settings_.samplingIntervalSeconds > settings_.syncIntervalSeconds
             ? settings_.samplingIntervalSeconds
             : settings_.syncIntervalSeconds;
}

bool NodeActuator::wouldExceedSilenceBudget(uint32_t sampling,
                                            uint32_t sync) const {
  const uint32_t silence = sampling > sync ? sampling : sync;
  return silence > kMaxSilenceSeconds;
}

bool NodeActuator::load() {
  if (!fs_.exists(statePath_)) return false;
  const size_t size = fs_.fileSize(statePath_);
  if (size == 0 || size > 512) return false;

  char buffer[513];
  if (!fs_.readRange(statePath_, 0, reinterpret_cast<uint8_t*>(buffer),
                     size)) {
    return false;
  }
  buffer[size] = '\0';

  // The magic is checked rather than the line count: a file that got truncated
  // mid-write can still contain the first two lines, and reading a sampling
  // interval from it while the sync interval falls back to its default would give
  // a pair that was never asked for.
  if (std::strncmp(buffer, kMagic, std::strlen(kMagic)) != 0) {
    logger_.eventf(LogLevel::Warn, "settings_corrupt",
                   "path=%s bytes=%u", statePath_, static_cast<unsigned>(size));
    return false;
  }

  uint32_t value = 0;
  if (fieldValue(buffer, "sampling_interval_s", value) &&
      value >= kMinSamplingSeconds && value <= kMaxSamplingSeconds) {
    settings_.samplingIntervalSeconds = value;
  }
  if (fieldValue(buffer, "sync_interval_s", value) && value >= kMinSyncSeconds &&
      value <= kMaxSyncSeconds) {
    settings_.syncIntervalSeconds = value;
  }
  if (fieldValue(buffer, "led_mode", value) && value <= kMaxLedMode) {
    settings_.ledMode = static_cast<uint8_t>(value);
  }

  // A file that loads into a combination outside the budget is not trusted. The
  // bounds above make that unreachable for a well-formed file, which is precisely
  // why the check is here rather than assumed.
  if (!withinSilenceBudget()) {
    logger_.event(LogLevel::Warn, "settings_outside_budget");
    settings_ = NodeSettings{};
    return false;
  }

  dirty_ = false;
  return true;
}

bool NodeActuator::persist() const {
  char body[192];
  const int written = std::snprintf(
      body, sizeof(body),
      "%s\nsampling_interval_s=%u\nsync_interval_s=%u\nled_mode=%u\n", kMagic,
      static_cast<unsigned>(settings_.samplingIntervalSeconds),
      static_cast<unsigned>(settings_.syncIntervalSeconds),
      static_cast<unsigned>(settings_.ledMode));
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(body)) return false;
  return fs_.writeWholeFile(statePath_,
                            reinterpret_cast<const uint8_t*>(body),
                            static_cast<size_t>(written));
}

bool NodeActuator::setSamplingInterval(uint32_t seconds, char* detailOut,
                                       size_t capacity) {
  if (seconds < kMinSamplingSeconds || seconds > kMaxSamplingSeconds) {
    std::snprintf(detailOut, capacity, "refused:sampling_out_of_range");
    return false;
  }
  if (wouldExceedSilenceBudget(seconds, settings_.syncIntervalSeconds)) {
    // The reason the command is refused is the invariant, not the field's own
    // range, so the detail says so. An operator who sees only
    // "out_of_range" would raise the ceiling and wonder why nothing changed.
    std::snprintf(detailOut, capacity, "refused:silence_budget_s=%u",
                  static_cast<unsigned>(kMaxSilenceSeconds));
    return false;
  }
  settings_.samplingIntervalSeconds = seconds;
  dirty_ = true;
  std::snprintf(detailOut, capacity, "applied:sampling_interval_s=%u",
                static_cast<unsigned>(seconds));
  return true;
}

bool NodeActuator::setSyncInterval(uint32_t seconds, char* detailOut,
                                   size_t capacity) {
  if (seconds < kMinSyncSeconds || seconds > kMaxSyncSeconds) {
    std::snprintf(detailOut, capacity, "refused:sync_out_of_range");
    return false;
  }
  if (wouldExceedSilenceBudget(settings_.samplingIntervalSeconds, seconds)) {
    std::snprintf(detailOut, capacity, "refused:silence_budget_s=%u",
                  static_cast<unsigned>(kMaxSilenceSeconds));
    return false;
  }
  settings_.syncIntervalSeconds = seconds;
  dirty_ = true;
  std::snprintf(detailOut, capacity, "applied:sync_interval_s=%u",
                static_cast<unsigned>(seconds));
  return true;
}

bool NodeActuator::setLedMode(uint32_t mode, char* detailOut, size_t capacity) {
  if (mode > kMaxLedMode) {
    std::snprintf(detailOut, capacity, "refused:led_mode_out_of_range");
    return false;
  }
  settings_.ledMode = static_cast<uint8_t>(mode);
  dirty_ = true;
  std::snprintf(detailOut, capacity, "applied:led_mode=%u",
                static_cast<unsigned>(mode));
  return true;
}

bool NodeActuator::requestResync(uint32_t nowMs, char* detailOut,
                                 size_t capacity) {
  settings_.resyncRequestedAtMs = nowMs == 0 ? 1 : nowMs;
  std::snprintf(detailOut, capacity, "applied:resync_requested_at_ms=%u",
                static_cast<unsigned>(settings_.resyncRequestedAtMs));
  return true;
}

bool NodeActuator::consumeResyncRequest() {
  if (settings_.resyncRequestedAtMs == 0) return false;
  settings_.resyncRequestedAtMs = 0;
  return true;
}

}  // namespace cauce

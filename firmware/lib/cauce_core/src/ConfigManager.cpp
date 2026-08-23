#include "cauce/core/ConfigManager.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace cauce {

namespace {
constexpr size_t kMaxConfigText = 8192;
}

ConfigManager::ConfigManager(hal::IFileSystem& fileSystem, const char* configPath)
    : fs_(fileSystem) {
  copyString(path_, sizeof(path_), configPath);
  std::snprintf(backupPath_, sizeof(backupPath_), "%s.bak", configPath);
}

bool ConfigManager::readTextFile(const char* path, char* buffer, size_t capacity) {
  if (!fs_.exists(path)) return false;
  const size_t size = fs_.fileSize(path);
  if (size == 0 || size >= capacity) return false;
  if (!fs_.readRange(path, 0, reinterpret_cast<uint8_t*>(buffer), size)) return false;
  buffer[size] = '\0';
  return true;
}

ConfigLoadStatus ConfigManager::load(NodeConfig& outConfig) {
  static char text[kMaxConfigText];

  if (readTextFile(path_, text, kMaxConfigText) && parseConfig(text, outConfig)) {
    current_ = outConfig;
    return ConfigLoadStatus::Loaded;
  }
  if (readTextFile(backupPath_, text, kMaxConfigText) && parseConfig(text, outConfig)) {
    current_ = outConfig;
    return ConfigLoadStatus::RestoredFromBackup;
  }
  current_ = NodeConfig{};
  outConfig = current_;
  return ConfigLoadStatus::CreatedDefaults;
}

bool ConfigManager::save(const NodeConfig& config) {
  static char text[kMaxConfigText];
  if (!serializeConfig(config, text, kMaxConfigText)) return false;

  if (fs_.exists(path_)) {
    const size_t size = fs_.fileSize(path_);
    if (size > 0 && size < kMaxConfigText) {
      static uint8_t previous[kMaxConfigText];
      if (fs_.readRange(path_, 0, previous, size)) {
        fs_.writeWholeFile(backupPath_, previous, size);
      }
    }
  }
  current_ = config;
  const size_t textLen = std::strlen(text);
  return fs_.writeWholeFile(path_, reinterpret_cast<const uint8_t*>(text), textLen);
}

ConfigValidation ConfigManager::validate(const NodeConfig& config) {
  ConfigValidation result;

  if (config.nodeId[0] == '\0') result.addError("node_id must not be empty");
  if (std::strlen(config.nodeId) >= sizeof(config.nodeId))
    result.addError("node_id too long");

  if (config.samplingIntervalS < 10 || config.samplingIntervalS > 3600)
    result.addError("sampling_interval_s out of [10..3600]");
  if (config.syncIntervalS < 60 || config.syncIntervalS > 86400)
    result.addError("sync_interval_s out of [60..86400]");
  if (config.timezoneOffsetMin < -720 || config.timezoneOffsetMin > 840)
    result.addError("tz_offset_min out of [-720..840]");

  if (config.segmentMaxBytes < 4096)
    result.addError("segment_max_bytes must be >= 4096");
  if (config.storageMaxBytes < 2u * config.segmentMaxBytes)
    result.addError("storage_max_bytes must be >= 2x segment_max_bytes");

  if (!std::isnan(config.latitude) &&
      (config.latitude < -90.0f || config.latitude > 90.0f))
    result.addError("latitude out of [-90..90]");
  if (!std::isnan(config.longitude) &&
      (config.longitude < -180.0f || config.longitude > 180.0f))
    result.addError("longitude out of [-180..180]");
  if (!std::isnan(config.elevationM) &&
      (config.elevationM < -100.0f || config.elevationM > 6000.0f))
    result.addError("elevation_m out of [-100..6000]");

  for (uint8_t v = 0; v < kVariableCount; ++v) {
    if (config.thresholds.rangeMin[v] >= config.thresholds.rangeMax[v]) {
      result.addError("threshold range_min must be < range_max");
      break;
    }
  }
  if (config.adminTokenSha256[0] != '\0' &&
      std::strlen(config.adminTokenSha256) != 64)
    result.addError("admin_token_sha256 must be 64 hex chars");

  return result;
}

}  // namespace cauce

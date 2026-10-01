#include "cauce/core/ConfigManager.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace cauce {

namespace {
constexpr size_t kMaxConfigText = 8192;
constexpr size_t kCopyChunk = 1024;

bool validNodeIdChars(const char* id) {
  for (const char* p = id; *p; ++p) {
    const char c = *p;
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}
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
  if (readTextFile(path_, scratch_, sizeof(scratch_)) &&
      parseConfig(scratch_, outConfig)) {
    current_ = outConfig;
    return ConfigLoadStatus::Loaded;
  }
  if (readTextFile(backupPath_, scratch_, sizeof(scratch_)) &&
      parseConfig(scratch_, outConfig)) {
    current_ = outConfig;
    return ConfigLoadStatus::RestoredFromBackup;
  }
  current_ = NodeConfig{};
  outConfig = current_;
  return ConfigLoadStatus::CreatedDefaults;
}

bool ConfigManager::save(const NodeConfig& config) {
  static_assert(sizeof(scratch_) >= kMaxConfigText, "scratch too small");
  if (!serializeConfig(config, scratch_, kMaxConfigText)) return false;

  if (fs_.exists(path_)) {
    const size_t size = fs_.fileSize(path_);
    if (size > 0 && size < kMaxConfigText) {
      fs_.removeFile(backupPath_);
      size_t copied = 0;
      bool backupOk = true;
      while (copied < size && backupOk) {
        size_t chunk = size - copied;
        if (chunk > kCopyChunk) chunk = kCopyChunk;
        if (!fs_.readRange(path_, copied, backupScratch_, chunk)) {
          backupOk = false;
          break;
        }
        if (!fs_.appendBytes(backupPath_, backupScratch_, chunk)) {
          backupOk = false;
          break;
        }
        copied += chunk;
      }
      if (!backupOk) fs_.removeFile(backupPath_);
    }
  }
  current_ = config;
  const size_t textLen = std::strlen(scratch_);
  return fs_.writeWholeFile(path_,
                            reinterpret_cast<const uint8_t*>(scratch_), textLen);
}

ConfigValidation ConfigManager::validate(const NodeConfig& config) {
  ConfigValidation result;

  if (config.nodeId[0] == '\0') result.addError("node_id must not be empty");
  if (std::strlen(config.nodeId) >= sizeof(config.nodeId))
    result.addError("node_id too long");
  if (!validNodeIdChars(config.nodeId))
    result.addError("node_id charset is [A-Za-z0-9_-]");

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
  if (config.syncServerUrl[0] != '\0' &&
      std::strlen(config.syncServerUrl) >= 128)
    result.addError("sync_server_url too long");
  if (config.otaManifestUrl[0] != '\0' &&
      std::strlen(config.otaManifestUrl) >= 160)
    result.addError("ota_manifest_url too long");
  if (config.loraSyncIntervalS < 60 || config.loraSyncIntervalS > 86400)
    result.addError("lora_sync_interval_s out of [60..86400]");

  return result;
}

}  // namespace cauce

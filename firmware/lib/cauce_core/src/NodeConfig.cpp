#include "cauce/core/NodeConfig.h"

#include <cstdarg>
#include <new>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cauce {
namespace {

// 32 bytes of seed as hex, plus the terminator.
constexpr size_t kEd25519SeedHexChars = 64;

// One hex digit as a value, or -1. Accepts either case: an operator typing a
// seed by hand will not remember which case the generator printed.
int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

// Copies `value` only when it is exactly `expectedLength` hex characters.
//
// Refusing a wrong-length or non-hex seed is the point. A 63-character seed is a
// provisioning typo, and a copy-paste that mangles one character produces a seed
// that is *valid* and belongs to nobody: the node would boot, sign challenges with a
// key the central does not know, and fail as an invalid signature - which points at
// the certificate rather than at the config line that is wrong.
bool copyIfHexOfLength(const char* value, size_t expectedLength, char* out,
                       size_t capacity) {
  if (value == nullptr || out == nullptr) return false;
  if (std::strlen(value) != expectedLength) return false;
  for (size_t i = 0; i < expectedLength; ++i) {
    const char c = value[i];
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F');
    if (!hex) return false;
  }
  copyString(out, capacity, value);
  return true;
}

struct Cursor {
  char* pos;
  size_t remaining;
  bool overflow;

  Cursor(char* initialPos, size_t initialRemaining)
      : pos(initialPos), remaining(initialRemaining), overflow(false) {}

  void write(const char* fmt, ...) {
    if (remaining == 0) {
      overflow = true;
      return;
    }
    va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(pos, remaining, fmt, args);
    va_end(args);
    if (written < 0) {
      overflow = true;
      return;
    }
    if (static_cast<size_t>(written) >= remaining) {
      overflow = true;
      pos += remaining - 1;
      remaining = 0;
      return;
    }
    pos += written;
    remaining -= static_cast<size_t>(written);
  }

  void writeFloat(const char* key, float v) {
    if (v != v) {
      write("%s=\n", key);
    } else {
      write("%s=%.4f\n", key, static_cast<double>(v));
    }
  }
};

void appendThresholdKeys(Cursor& c, const Thresholds& t, Variable v) {
  const char* name = variableName(v);
  const uint8_t i = static_cast<uint8_t>(v);
  c.write("thr_range_min_%s=%.2f\n", name, static_cast<double>(t.rangeMin[i]));
  c.write("thr_range_max_%s=%.2f\n", name, static_cast<double>(t.rangeMax[i]));
  c.write("thr_rise_pm_%s=%.2f\n", name, static_cast<double>(t.maxRisePerMinute[i]));
  c.write("thr_drop_pm_%s=%.2f\n", name, static_cast<double>(t.maxDropPerMinute[i]));
}

char* trim(char* s) {
  while (*s == ' ' || *s == '\t' || *s == '\r') ++s;
  char* end = s + std::strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) --end;
  *end = '\0';
  return s;
}

bool parseFloat(const char* text, float& out) {
  if (*text == '\0') {
    out = NAN;
    return true;
  }
  char* endPtr = nullptr;
  const double parsed = std::strtod(text, &endPtr);
  if (endPtr == text || *endPtr != '\0') return false;
  out = static_cast<float>(parsed);
  return true;
}

bool parseInt(const char* text, long long& out) {
  if (*text == '\0') return false;
  char* endPtr = nullptr;
  out = std::strtoll(text, &endPtr, 10);
  return endPtr != text && *endPtr == '\0';
}

}  // namespace

bool serializeConfig(const NodeConfig& config, char* out, size_t capacity) {
  Cursor c{out, capacity};
  c.write("# CAUCE node configuration v%u\n", Versions::kConfigSchema);
  c.write("schema_version=%u\n", config.schemaVersion);
  c.write("node_id=%s\n", config.nodeId);
  c.write("site_id=%s\n", config.siteId);
  c.writeFloat("latitude", config.latitude);
  c.writeFloat("longitude", config.longitude);
  c.writeFloat("elevation_m", config.elevationM);
  c.write("land_cover=%s\n", config.landCover);
  c.write("shade_condition=%s\n", config.shadeCondition);
  c.write("sampling_interval_s=%lu\n",
          static_cast<unsigned long>(config.samplingIntervalS));
  c.write("sync_interval_s=%lu\n",
          static_cast<unsigned long>(config.syncIntervalS));
  c.write("tz_offset_min=%d\n", static_cast<int>(config.timezoneOffsetMin));
  c.write("wifi_enabled=%d\n", config.wifiEnabled ? 1 : 0);
  c.write("wifi_ssid=%s\n", config.wifiSsid);
  c.write("wifi_password=%s\n", config.wifiPassword);
  c.write("ntp_server=%s\n", config.ntpServer);
  c.write("sync_server_url=%s\n", config.syncServerUrl);
  c.write("sync_device_key=%s\n", config.syncDeviceKey);
  c.write("sync_auth_seed=%s\n", config.syncAuthSeedHex);
  c.write("sync_certificate=%s\n", config.syncCertificate);
  c.write("ota_manifest_key=%s\n", config.otaManifestKey);
  c.write("ota_manifest_url=%s\n", config.otaManifestUrl);
  c.write("lora_enabled=%d\n", config.loraEnabled ? 1 : 0);
  c.write("lora_sync_interval_s=%lu\n",
          static_cast<unsigned long>(config.loraSyncIntervalS));
  c.write("lora_region=%s\n", config.loraRegion);
  c.write("storage_max_bytes=%lu\n",
          static_cast<unsigned long>(config.storageMaxBytes));
  c.write("segment_max_bytes=%lu\n",
          static_cast<unsigned long>(config.segmentMaxBytes));
  c.write("deep_sleep_enabled=%d\n", config.deepSleepEnabled ? 1 : 0);
  c.write("admin_token_sha256=%s\n", config.adminTokenSha256);

  for (uint8_t v = 0; v < kVariableCount; ++v) {
    appendThresholdKeys(c, config.thresholds, static_cast<Variable>(v));
  }
  if (c.overflow) {
    out[capacity > 0 ? capacity - 1 : 0] = '\0';
    return false;
  }
  return true;
}

bool decodeSeedHex(const char* hex, uint8_t* out, size_t capacity) {
  if (hex == nullptr || out == nullptr || capacity < kEd25519SeedHexChars / 2) return false;
  if (std::strlen(hex) != kEd25519SeedHexChars) return false;

  uint8_t decoded[kEd25519SeedHexChars / 2];
  for (size_t i = 0; i < kEd25519SeedHexChars; i += 2) {
    const int hi = hexNibble(hex[i]);
    const int lo = hexNibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      // Nothing written. A caller that ignored the false would otherwise be
      // authenticating with whatever was in the buffer, which on a first call
      // is uninitialised stack.
      return false;
    }
    decoded[i / 2] = static_cast<uint8_t>((hi << 4) | lo);
  }
  for (size_t i = 0; i < sizeof(decoded); ++i) {
    out[i] = decoded[i];
  }
  return true;
}

bool parseConfig(const char* text, NodeConfig& out) {
  if (!text) return false;
  NodeConfig cfg{};
  bool sawAnyKey = false;
  int malformed = 0;
  // A wrong credential fails the whole parse. See the assignment below for why this is not
  // the shared `malformed` counter.
  bool badCredentialLine = false;

  const char* cursor = text;
  while (*cursor != '\0') {
    const char* eol = std::strchr(cursor, '\n');
    const size_t lineLen = eol ? static_cast<size_t>(eol - cursor)
                               : std::strlen(cursor);
    char line[512];
    char* linePtr = line;
    if (lineLen >= sizeof(line)) {
      malformed++;
      linePtr = new (std::nothrow) char[lineLen + 1];
      if (!linePtr) break;
    }
    std::memcpy(linePtr, cursor, lineLen);
    linePtr[lineLen] = '\0';

    if (linePtr[0] != '#' && linePtr[0] != '\0') {
      char* eq = std::strchr(linePtr, '=');
      if (!eq) {
        malformed++;
      } else {
        *eq = '\0';
        const char* key = trim(linePtr);
        char* valueText = trim(eq + 1);
        sawAnyKey = true;

        long long intValue = 0;
        float floatValue = 0.0f;
      if (std::strcmp(key, "schema_version") == 0 && parseInt(valueText, intValue)) {
        cfg.schemaVersion = static_cast<uint8_t>(intValue);
      } else if (std::strcmp(key, "node_id") == 0) {
        copyString(cfg.nodeId, sizeof(cfg.nodeId), valueText);
      } else if (std::strcmp(key, "site_id") == 0) {
        copyString(cfg.siteId, sizeof(cfg.siteId), valueText);
      } else if (std::strcmp(key, "latitude") == 0 && parseFloat(valueText, floatValue)) {
        cfg.latitude = floatValue;
      } else if (std::strcmp(key, "longitude") == 0 && parseFloat(valueText, floatValue)) {
        cfg.longitude = floatValue;
      } else if (std::strcmp(key, "elevation_m") == 0 &&
                 parseFloat(valueText, floatValue)) {
        cfg.elevationM = floatValue;
      } else if (std::strcmp(key, "land_cover") == 0) {
        copyString(cfg.landCover, sizeof(cfg.landCover), valueText);
      } else if (std::strcmp(key, "shade_condition") == 0) {
        copyString(cfg.shadeCondition, sizeof(cfg.shadeCondition), valueText);
      } else if (std::strcmp(key, "sampling_interval_s") == 0 &&
                 parseInt(valueText, intValue)) {
        cfg.samplingIntervalS = static_cast<uint32_t>(intValue);
      } else if (std::strcmp(key, "sync_interval_s") == 0 &&
                 parseInt(valueText, intValue)) {
        cfg.syncIntervalS = static_cast<uint32_t>(intValue);
      } else if (std::strcmp(key, "tz_offset_min") == 0 &&
                 parseInt(valueText, intValue)) {
        cfg.timezoneOffsetMin = static_cast<int16_t>(intValue);
      } else if (std::strcmp(key, "wifi_enabled") == 0 &&
                 parseInt(valueText, intValue)) {
        cfg.wifiEnabled = intValue != 0;
      } else if (std::strcmp(key, "wifi_ssid") == 0) {
        copyString(cfg.wifiSsid, sizeof(cfg.wifiSsid), valueText);
      } else if (std::strcmp(key, "wifi_password") == 0) {
        copyString(cfg.wifiPassword, sizeof(cfg.wifiPassword), valueText);
      } else if (std::strcmp(key, "ntp_server") == 0) {
        copyString(cfg.ntpServer, sizeof(cfg.ntpServer), valueText);
      } else if (std::strcmp(key, "sync_server_url") == 0) {
        copyString(cfg.syncServerUrl, sizeof(cfg.syncServerUrl), valueText);
      } else if (std::strcmp(key, "ota_manifest_key") == 0) {
        // Not length-validated the way the Ed25519 seed is: an opaque shared secret rather
        // than a fixed-width seed, with its length the operator's choice. The central
        // enforces its own minimum when provisioning.
        copyString(cfg.otaManifestKey, sizeof(cfg.otaManifestKey), valueText);
      } else if (std::strcmp(key, "ota_manifest_url") == 0) {
        copyString(cfg.otaManifestUrl, sizeof(cfg.otaManifestUrl), valueText);
      } else if (std::strcmp(key, "sync_device_key") == 0) {
        copyString(cfg.syncDeviceKey, sizeof(cfg.syncDeviceKey), valueText);
      } else if (std::strcmp(key, "sync_auth_seed") == 0) {
        // The hex is validated, not just copied. A 63-character seed is a
        // provisioning typo, and accepting it here means the node boots,
        // authenticator reports NotConfigured, and the operator sees a node
        // that syncs with a fallback while its config file looks correct.
        if (valueText[0] != '\0' &&
            !copyIfHexOfLength(valueText, kEd25519SeedHexChars,
                               cfg.syncAuthSeedHex, sizeof(cfg.syncAuthSeedHex))) {
          // Its own flag rather than the shared `malformed` counter. That counter is
          // deliberately forgiving - up to three bad lines still load, because a config
          // written by a newer firmware should not brick an older one - and a bad
          // credential is the one line that must *not* be forgiven. It leaves a node that
          // boots looking correctly configured and fails every authentication as an invalid
          // signature, which points at the certificate instead of at the config line.
          badCredentialLine = true;
        }
      } else if (std::strcmp(key, "sync_certificate") == 0) {
        copyString(cfg.syncCertificate, sizeof(cfg.syncCertificate), valueText);
      } else if (std::strcmp(key, "lora_enabled") == 0 &&
                 parseInt(valueText, intValue)) {
        cfg.loraEnabled = intValue != 0;
      } else if (std::strcmp(key, "lora_sync_interval_s") == 0 &&
                 parseInt(valueText, intValue)) {
        cfg.loraSyncIntervalS = static_cast<uint32_t>(intValue);
      } else if (std::strcmp(key, "lora_region") == 0) {
        copyString(cfg.loraRegion, sizeof(cfg.loraRegion), valueText);
      } else if (std::strcmp(key, "storage_max_bytes") == 0 &&
                 parseInt(valueText, intValue)) {
        cfg.storageMaxBytes = static_cast<uint32_t>(intValue);
      } else if (std::strcmp(key, "segment_max_bytes") == 0 &&
                 parseInt(valueText, intValue)) {
        cfg.segmentMaxBytes = static_cast<uint32_t>(intValue);
      } else if (std::strcmp(key, "deep_sleep_enabled") == 0) {
        cfg.deepSleepEnabled = std::strcmp(valueText, "1") == 0 ||
                               std::strcmp(valueText, "true") == 0;
      } else if (std::strcmp(key, "admin_token_sha256") == 0) {
        copyString(cfg.adminTokenSha256, sizeof(cfg.adminTokenSha256), valueText);
      } else if (std::strncmp(key, "thr_", 4) == 0) {
        for (uint8_t v = 0; v < kVariableCount; ++v) {
          const Variable var = static_cast<Variable>(v);
          const char* name = variableName(var);
          const uint8_t idx = v;
          if (std::strncmp(key + 4, "range_min_", 10) == 0 &&
              std::strcmp(key + 14, name) == 0 && parseFloat(valueText, floatValue)) {
            cfg.thresholds.rangeMin[idx] = floatValue;
          } else if (std::strncmp(key + 4, "range_max_", 10) == 0 &&
                     std::strcmp(key + 14, name) == 0 &&
                     parseFloat(valueText, floatValue)) {
            cfg.thresholds.rangeMax[idx] = floatValue;
          } else if (std::strncmp(key + 4, "rise_pm_", 8) == 0 &&
                     std::strcmp(key + 12, name) == 0 &&
                     parseFloat(valueText, floatValue)) {
            cfg.thresholds.maxRisePerMinute[idx] = floatValue;
          } else if (std::strncmp(key + 4, "drop_pm_", 8) == 0 &&
                     std::strcmp(key + 12, name) == 0 &&
                     parseFloat(valueText, floatValue)) {
            cfg.thresholds.maxDropPerMinute[idx] = floatValue;
          }
        }
      }
    }
    }
    if (linePtr != line) delete[] linePtr;
    cursor = eol ? eol + 1 : cursor + lineLen;
  }
  if (!sawAnyKey || malformed > 3 || badCredentialLine) return false;
  out = cfg;
  return true;
}

}  // namespace cauce

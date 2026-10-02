#include "cauce/app/FieldDiagnostics.h"

#include <cstdio>
#include <cstring>

#include "cauce/core/SecurityUtils.h"

namespace cauce::app {
namespace {

const char* safeOr(const char* value, const char* fallback) {
  return (value && value[0] != '\0') ? value : fallback;
}

void appendEscaped(char* out, size_t capacity, size_t& pos,
                   const char* value) {
  if (out == nullptr || capacity == 0) return;
  if (pos >= capacity) return;
  out[pos++] = '"';
  if (value != nullptr) {
    for (const char* p = value; *p != '\0'; ++p) {
      if (pos + 8 >= capacity) break;
      const unsigned char c = static_cast<unsigned char>(*p);
      switch (c) {
        case '"': out[pos++] = '\\'; out[pos++] = '"'; break;
        case '\\': out[pos++] = '\\'; out[pos++] = '\\'; break;
        case '\n': out[pos++] = '\\'; out[pos++] = 'n'; break;
        case '\r': out[pos++] = '\\'; out[pos++] = 'r'; break;
        case '\t': out[pos++] = '\\'; out[pos++] = 't'; break;
        default:
          if (c < 0x20) {
            pos += static_cast<size_t>(std::snprintf(
                out + pos, capacity - pos, "\\u%04x", c));
          } else {
            out[pos++] = static_cast<char>(c);
          }
      }
    }
  }
  if (pos + 1 < capacity) out[pos++] = '"';
  out[pos < capacity ? pos : capacity - 1] = '\0';
}

}  // namespace

size_t buildDiagnosticsJson(char* out, size_t capacity,
                            const DiagnosticsInput& in) {
  if (out == nullptr || capacity == 0) return 0;

  char nodeId[40], siteId[40], firmware[24], hardware[24], nodeState[24],
      netState[24], lastError[80];
  size_t pos = 0;
  appendEscaped(nodeId, sizeof(nodeId), pos, safeOr(in.nodeId, "unknown"));
  pos = 0;
  appendEscaped(siteId, sizeof(siteId), pos,
                in.siteId ? in.siteId : "");
  pos = 0;
  appendEscaped(firmware, sizeof(firmware), pos,
                safeOr(in.firmwareVersion, "0.0.0"));
  pos = 0;
  appendEscaped(hardware, sizeof(hardware), pos,
                in.hardwareRevision ? in.hardwareRevision : "");
  pos = 0;
  appendEscaped(nodeState, sizeof(nodeState), pos, safeOr(in.nodeState, "?"));
  pos = 0;
  appendEscaped(netState, sizeof(netState), pos, safeOr(in.netState, "?"));
  pos = 0;
  appendEscaped(lastError, sizeof(lastError), pos,
                in.lastError ? in.lastError : "");

  const int n = std::snprintf(
      out, capacity,
      "{\"schema\":\"cauce.diag/1\",\"identity\":{\"node_id\":%s,\"site_id\":%s,"
      "\"firmware\":%s,\"hardware\":%s},"
      "\"health\":{\"firmware\":%s,\"node_state\":%s,"
      "\"net_state\":%s,\"uptime_ms\":%lu,\"utc_time_valid\":%s,"
      "\"rssi_dbm\":%d,\"battery_v\":%.2f,\"measurements\":%lu,"
      "\"stored\":%lu,\"invalid\":%lu,\"suspect\":%lu,\"read_failures\":%lu,"
      "\"storage_failures\":%lu,\"storage_records\":%lu,\"storage_bytes\":%lu,"
      "\"corrupted_frames\":%lu,\"last_success_utc_ms\":%llu,"
      "\"sample_interval_s\":%lu,\"reset_reason\":%lu,\"boot_count\":%lu},"
      "\"sync\":{\"attempts\":%lu,\"failures\":%lu},"
      "\"radio\":{\"lora_enabled\":%s,\"rssi_dbm\":%d,\"snr_db\":%d,"
      "\"sf\":%d},"
      "\"errors\":{\"last\":%s}}",
      nodeId, siteId, firmware, hardware, firmware, nodeState, netState,
      static_cast<unsigned long>(in.uptimeMs), in.clockValid ? "true" : "false",
      static_cast<int>(in.rssiDbm), static_cast<double>(in.batteryVoltageV),
      static_cast<unsigned long>(in.measurementCount),
      static_cast<unsigned long>(in.storedCount),
      static_cast<unsigned long>(in.invalidCount),
      static_cast<unsigned long>(in.suspectCount),
      static_cast<unsigned long>(in.readFailures),
      static_cast<unsigned long>(in.storageFailures),
      static_cast<unsigned long>(in.storageRecords),
      static_cast<unsigned long>(in.storageBytes),
      static_cast<unsigned long>(in.corruptedFrames),
      static_cast<unsigned long long>(in.lastSuccessUtcMs),
      static_cast<unsigned long>(in.sampleIntervalS),
      static_cast<unsigned long>(in.resetReason),
      static_cast<unsigned long>(in.bootCount),
      static_cast<unsigned long>(in.syncAttempts),
      static_cast<unsigned long>(in.syncFailures),
      in.loraEnabled ? "true" : "false", static_cast<int>(in.loraRssiDbm),
      static_cast<int>(in.loraSnrDb), static_cast<int>(in.loraSf), lastError);

  if (n < 0 || static_cast<size_t>(n) >= capacity) {
    if (capacity > 0) out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(n);
}

size_t diagnosticsSignatureHex(const char* deviceKey, size_t keyLen,
                               const char* body, size_t bodyLen,
                               char outHex[65]) {
  if (outHex == nullptr) return 0;
  outHex[0] = '\0';
  if (deviceKey == nullptr || body == nullptr) return 0;
  if (keyLen == 0 || bodyLen == 0) return 0;
  uint8_t digest[32];
  hmacSha256(reinterpret_cast<const uint8_t*>(deviceKey), keyLen,
             reinterpret_cast<const uint8_t*>(body), bodyLen, digest);
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; ++i) {
    outHex[i * 2] = kHex[(digest[i] >> 4) & 0x0F];
    outHex[i * 2 + 1] = kHex[digest[i] & 0x0F];
  }
  outHex[64] = '\0';
  return 64;
}

}  // namespace cauce::app

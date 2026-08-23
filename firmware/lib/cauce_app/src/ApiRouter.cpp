#include "cauce/app/ApiRouter.h"

#include <cstdio>
#include <cstdlib>
#include <new>
#include <cstring>

#include "cauce/app/NetworkManager.h"
#include "cauce/app/WebAssets.h"
#include "cauce/core/SecurityUtils.h"
#include "cauce/core/TimeUtils.h"

namespace cauce::app {

namespace {

bool pathEquals(const char* target, const char* literal) {
  const size_t len = std::strlen(literal);
  return std::strncmp(target, literal, len) == 0 &&
         (target[len] == '\0' || target[len] == '?');
}

bool queryParam(const char* query, const char* name, char* out, size_t capacity) {
  if (!query || !name || capacity == 0) return false;
  out[0] = '\0';
  const size_t nameLen = std::strlen(name);
  const char* p = query;
  while (*p) {
    if (std::strncmp(p, name, nameLen) == 0 && p[nameLen] == '=') {
      const char* v = p + nameLen + 1;
      size_t used = 0;
      while (*v && *v != '&' && used + 1 < capacity) {
        if (*v == '%' && v[1] != '\0' && v[2] != '\0') {
          auto hexVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
          };
          const int hi = hexVal(v[1]);
          const int lo = hexVal(v[2]);
          if (hi >= 0 && lo >= 0) {
            out[used++] = static_cast<char>((hi << 4) | lo);
            v += 3;
            continue;
          }
        }
        out[used++] = *v++;
      }
      out[used] = '\0';
      return true;
    }
    while (*p && *p != '&') ++p;
    if (*p == '&') ++p;
  }
  return false;
}

bool parseEpochParam(const char* value, uint64_t& outMs) {
  if (!value || !*value) return false;
  bool allDigits = true;
  for (const char* p = value; *p; ++p) {
    if (*p < '0' || *p > '9') {
      allDigits = false;
      break;
    }
  }
  if (allDigits) {
    outMs = std::strtoull(value, nullptr, 10);
    return true;
  }
  return parseIso8601Utc(value, outMs);
}

void appendJsonText(char*& c, size_t& rem, const char* text) {
  if (rem <= 1) return;
  const int written = std::snprintf(c, rem, "%s", text);
  if (written <= 0) return;
  const size_t use = static_cast<size_t>(written) < rem
                         ? static_cast<size_t>(written)
                         : rem - 1;
  c += use;
  rem -= use;
}

}  // namespace

ApiRouter::ApiRouter(IStorageRepository& store, ConfigManager& config,
                     hal::IClock& clock, Logger& logger, SystemHealth& health)
    : store_(store),
      config_(config),
      clock_(clock),
      logger_(logger),
      health_(health),
      exporter_(nullptr),
      streamKind_(StreamKind::None) {}

bool ApiRouter::authorized(const Request& req) const {
  const char* expected = config_.current().adminTokenSha256;
  if (!expected[0]) return false;
  if (!req.authorization || std::strncmp(req.authorization, "Bearer ", 7) != 0)
    return false;
  char hashed[65];
  sha256Hex(req.authorization + 7, hashed);
  return secureEquals(hashed, expected);
}

ApiRouter::Response ApiRouter::respondError(uint16_t code, const char* body, char* out,
                                 size_t capacity) {
  return respond(code, body, std::strlen(body), out, capacity);
}
ApiRouter::Response ApiRouter::respond(uint16_t code, const char* body, size_t len,
                            char* out, size_t capacity) {
  Response r{};
  r.statusCode = code;
  r.streamActive = false;
  r.streamDone = true;
  if (len >= capacity) len = capacity ? capacity - 1 : 0;
  std::memcpy(out, body, len);
  out[len] = '\0';
  r.bytesWritten = len;
  return r;
}

bool ApiRouter::startMeasurementStream(StreamKind kind, uint64_t fromMs,
                                       uint64_t toMs) {
  if (exporter_) {
    exporter_->~ChunkedExporter();
    exporter_ = nullptr;
  }
  streamKind_ = kind;
  exporter_ = new (exporterStorage_) ChunkedExporter(
      store_,
      kind == StreamKind::Csv ? ChunkedExporter::Format::Csv
                              : ChunkedExporter::Format::JsonArray,
      fromMs, toMs);
  return true;
}

void ApiRouter::startHtmlStream() {
  if (exporter_) {
    exporter_->~ChunkedExporter();
    exporter_ = nullptr;
  }
  htmlData_ = kIndexHtml;
  htmlLen_ = kIndexHtmlLen;
  htmlPos_ = 0;
  streamKind_ = StreamKind::Html;
}

ApiRouter::Response ApiRouter::pumpStream(char* out, size_t capacity) {
  Response r{};
  r.streamActive = true;
  r.statusCode = 200;
  r.contentType = streamKind_ == StreamKind::Csv   ? "text/csv"
                  : streamKind_ == StreamKind::Html
                      ? "text/html; charset=utf-8"
                      : "application/json";
  if (streamKind_ == StreamKind::Html) {
    const size_t remaining = htmlLen_ - htmlPos_;
    r.bytesWritten = remaining < capacity ? remaining : capacity;
    std::memcpy(out, htmlData_ + htmlPos_, r.bytesWritten);
    htmlPos_ += r.bytesWritten;
    r.streamDone = htmlPos_ >= htmlLen_;
    if (r.streamDone) {
      streamKind_ = StreamKind::None;
      htmlData_ = nullptr;
    }
    return r;
  }
  if (!exporter_) {
    r.streamDone = true;
    streamKind_ = StreamKind::None;
    r.bytesWritten = 0;
    return r;
  }
  r.bytesWritten = exporter_->next(out, capacity);
  r.streamDone = exporter_->done();
  if (r.streamDone) {
    exporter_->~ChunkedExporter();
    exporter_ = nullptr;
    streamKind_ = StreamKind::None;
  }
  return r;
}

ApiRouter::Response ApiRouter::routeGet(const char* path, const char* query, char* out,
                             size_t capacity) {
  char param[96];

  if (pathEquals(path, "/api/v1/node")) {
    const NodeConfig& cfg = config_.current();
    char lat[24] = "null", lon[24] = "null", ele[24] = "null";
    if (!std::isnan(cfg.latitude))
      std::snprintf(lat, sizeof(lat), "%.4f", static_cast<double>(cfg.latitude));
    if (!std::isnan(cfg.longitude))
      std::snprintf(lon, sizeof(lon), "%.4f",
                    static_cast<double>(cfg.longitude));
    if (!std::isnan(cfg.elevationM))
      std::snprintf(ele, sizeof(ele), "%.1f",
                    static_cast<double>(cfg.elevationM));
    char nodeJson[32], siteJson[48], landJson[40], shadeJson[40];
    escapeJsonString(cfg.nodeId, nodeJson, sizeof(nodeJson));
    escapeJsonString(cfg.siteId, siteJson, sizeof(siteJson));
    escapeJsonString(cfg.landCover, landJson, sizeof(landJson));
    escapeJsonString(cfg.shadeCondition, shadeJson, sizeof(shadeJson));
    char body[512];
    const int n = std::snprintf(
        body, sizeof(body),
        "{\"protocol_version\":%u,\"firmware_version\":\"%s\","
        "\"hardware_revision\":\"%s\",\"node_id\":%s,\"site_id\":%s,"
        "\"latitude\":%s,\"longitude\":%s,\"elevation_m\":%s,"
        "\"land_cover\":%s,\"shade_condition\":%s}",
        Versions::kProtocol, Versions::kFirmware, Versions::kHardwareRevision,
        nodeJson, siteJson, lat, lon, ele, landJson, shadeJson);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(body))
      return respondError(500, "{\"error\":\"internal\"}", out, capacity);
    return respond(200, body, static_cast<size_t>(n), out, capacity);
  }

  if (pathEquals(path, "/api/v1/status")) {
    Measurement latest{};
    char latestJson[320];
    if (store_.latest(latest)) {
      measurementToJson(latest, latestJson, sizeof(latestJson));
    } else {
      copyString(latestJson, sizeof(latestJson), "null");
    }
    char body[512];
    const int n = std::snprintf(
        body, sizeof(body),
        "{\"node_state\":\"%s\",\"net_state\":\"%s\",\"time_valid\":%s,"
        "\"uptime_ms\":%lu,\"latest\":%s}",
        nodeStateName(health_.nodeState), netStateName(health_.netState),
        clock_.utcTimeValid() ? "true" : "false",
        static_cast<unsigned long>(health_.uptimeMs), latestJson);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(body))
      return respondError(500, "{\"error\":\"internal\"}", out, capacity);
    return respond(200, body, static_cast<size_t>(n), out, capacity);
  }

  if (pathEquals(path, "/api/v1/measurements/latest")) {
    Measurement latest{};
    if (!store_.latest(latest))
      return respondError(404, "{\"error\":\"no_measurements\"}", out, capacity);
    char body[384];
    const size_t n = measurementToJson(latest, body, sizeof(body));
    return respond(200, body, n, out, capacity);
  }

  if (pathEquals(path, "/api/v1/measurements")) {
    uint64_t from = 0;
    uint64_t to = UINT64_MAX;
    if (queryParam(query, "from", param, sizeof(param)) &&
        !parseEpochParam(param, from))
      return respondError(400, "{\"error\":\"bad_from\"}", out, capacity);
    if (queryParam(query, "to", param, sizeof(param)) &&
        !parseEpochParam(param, to))
      return respondError(400, "{\"error\":\"bad_to\"}", out, capacity);
    startMeasurementStream(StreamKind::JsonArray, from, to);
    return pumpStream(out, capacity);
  }

  if (pathEquals(path, "/api/v1/export")) {
    StreamKind kind = StreamKind::Csv;
    if (queryParam(query, "format", param, sizeof(param))) {
      if (std::strcmp(param, "json") == 0) kind = StreamKind::JsonArray;
      else if (std::strcmp(param, "csv") != 0)
        return respondError(400, "{\"error\":\"bad_format\"}", out, capacity);
    }
    uint64_t from = 0;
    uint64_t to = UINT64_MAX;
    if (queryParam(query, "from", param, sizeof(param)) &&
        !parseEpochParam(param, from))
      return respondError(400, "{\"error\":\"bad_from\"}", out, capacity);
    if (queryParam(query, "to", param, sizeof(param)) &&
        !parseEpochParam(param, to))
      return respondError(400, "{\"error\":\"bad_to\"}", out, capacity);
    startMeasurementStream(kind, from, to);
    return pumpStream(out, capacity);
  }

  if (pathEquals(path, "/api/v1/health")) {
    char body[640];
    const int n = std::snprintf(
        body, sizeof(body),
        "{\"firmware\":\"%s\",\"node_state\":\"%s\",\"net_state\":\"%s\","
        "\"uptime_ms\":%lu,\"utc_time_valid\":%s,\"rssi_dbm\":%d,"
        "\"battery_v\":%.2f,"
        "\"measurements\":%lu,\"stored\":%lu,\"invalid\":%lu,\"suspect\":%lu,"
        "\"read_failures\":%lu,\"storage_failures\":%lu,"
        "\"last_success_utc_ms\":%llu,"
        "\"storage_records\":%lu,\"storage_bytes\":%lu,\"corrupted_frames\":%lu}",
        health_.firmwareVersion, nodeStateName(health_.nodeState),
        netStateName(health_.netState),
        static_cast<unsigned long>(health_.uptimeMs),
        clock_.utcTimeValid() ? "true" : "false",
        static_cast<int>(health_.rssiDbm),
        static_cast<double>(health_.batteryVoltageV),
        static_cast<unsigned long>(health_.measurementCount),
        static_cast<unsigned long>(health_.storedCount),
        static_cast<unsigned long>(health_.invalidCount),
        static_cast<unsigned long>(health_.suspectCount),
        static_cast<unsigned long>(health_.readFailures),
        static_cast<unsigned long>(health_.storageFailures),
        static_cast<unsigned long long>(health_.lastSuccessUtcMs),
        static_cast<unsigned long>(health_.storageRecords),
        static_cast<unsigned long>(health_.storageBytes),
        static_cast<unsigned long>(health_.corruptedFrames));
    if (n < 0 || static_cast<size_t>(n) >= sizeof(body))
      return respondError(500, "{\"error\":\"internal\"}", out, capacity);
    return respond(200, body, static_cast<size_t>(n), out, capacity);
  }

  if (pathEquals(path, "/api/v1/config")) {
    const NodeConfig& cfg = config_.current();
    char body[1600];
    char nodeId[32], siteId[48], land[32], shade[32], ssid[72], ntp[96];
    escapeJsonString(cfg.nodeId, nodeId, sizeof(nodeId));
    escapeJsonString(cfg.siteId, siteId, sizeof(siteId));
    escapeJsonString(cfg.landCover, land, sizeof(land));
    escapeJsonString(cfg.shadeCondition, shade, sizeof(shade));
    escapeJsonString(cfg.wifiSsid, ssid, sizeof(ssid));
    escapeJsonString(cfg.ntpServer, ntp, sizeof(ntp));
    char geo[96] = "null";
    if (!std::isnan(cfg.latitude))
      std::snprintf(geo, sizeof(geo), "[%.4f,%.4f,%.1f]",
                    static_cast<double>(cfg.latitude),
                    static_cast<double>(cfg.longitude),
                    static_cast<double>(cfg.elevationM));
    const int n = std::snprintf(
        body, sizeof(body),
        "{\"schema_version\":%u,\"node_id\":%s,\"site_id\":%s,"
        "\"location\":%s,\"land_cover\":%s,\"shade_condition\":%s,"
        "\"sampling_interval_s\":%lu,\"sync_interval_s\":%lu,"
        "\"tz_offset_min\":%d,\"wifi_enabled\":%s,\"wifi_ssid\":%s,"
        "\"wifi_password\":null,\"ntp_server\":%s,"
        "\"admin_token_configured\":%s,"
        "\"thresholds\":{\"air_temperature\":[%.1f,%.1f],"
        "\"relative_humidity\":[%.1f,%.1f],\"pressure\":[%.1f,%.1f]}}",
        cfg.schemaVersion, nodeId, siteId, geo, land, shade,
        static_cast<unsigned long>(cfg.samplingIntervalS),
        static_cast<unsigned long>(cfg.syncIntervalS),
        static_cast<int>(cfg.timezoneOffsetMin),
        cfg.wifiEnabled ? "true" : "false", ssid, ntp,
        cfg.adminTokenSha256[0] ? "true" : "false",
        static_cast<double>(cfg.thresholds.rangeMin[static_cast<uint8_t>(
            Variable::AirTemperature)]),
        static_cast<double>(cfg.thresholds.rangeMax[static_cast<uint8_t>(
            Variable::AirTemperature)]),
        static_cast<double>(cfg.thresholds.rangeMin[static_cast<uint8_t>(
            Variable::RelativeHumidity)]),
        static_cast<double>(cfg.thresholds.rangeMax[static_cast<uint8_t>(
            Variable::RelativeHumidity)]),
        static_cast<double>(cfg.thresholds.rangeMin[static_cast<uint8_t>(
            Variable::Pressure)]),
        static_cast<double>(cfg.thresholds.rangeMax[static_cast<uint8_t>(
            Variable::Pressure)]));
    if (n < 0 || static_cast<size_t>(n) >= sizeof(body))
      return respondError(500, "{\"error\":\"internal\"}", out, capacity);
    return respond(200, body, static_cast<size_t>(n), out, capacity);
  }

  if (pathEquals(path, "/")) {
    startHtmlStream();
    return pumpStream(out, capacity);
  }
  if (pathEquals(path, "/favicon.ico")) {
    Response r{};
    r.statusCode = 204;
    r.streamDone = true;
    r.bytesWritten = 0;
    return r;
  }

  return respondError(404, "{\"error\":\"not_found\"}", out, capacity);
}

ApiRouter::Response ApiRouter::routePostConfig(const Request& req, char* out,
                                    size_t capacity) {
  if (!req.body || req.bodyLen == 0)
    return respondError(400, "{\"error\":\"empty_body\"}", out, capacity);

  NodeConfig candidate{};
  char text[2048];
  size_t copyLen = req.bodyLen < sizeof(text) - 1 ? req.bodyLen
                                                  : sizeof(text) - 1;
  std::memcpy(text, req.body, copyLen);
  text[copyLen] = '\0';

  if (!parseConfig(text, candidate))
    return respondError(400, "{\"error\":\"unparsable_body\"}", out, capacity);

  const ConfigValidation validation = ConfigManager::validate(candidate);
  if (!validation.ok) {
    char errors[512];
    size_t used = 0;
    used += static_cast<size_t>(
        std::snprintf(errors + used, sizeof(errors) - used, "{\"errors\":["));
    for (uint8_t i = 0; i < validation.errorCount && i < 4; ++i) {
      char errEsc[140];
      escapeJsonString(validation.errors[i], errEsc, sizeof(errEsc));
      used += static_cast<size_t>(std::snprintf(
          errors + used, sizeof(errors) - used, "%s%s", i ? "," : "", errEsc));
    }
    used += static_cast<size_t>(
        std::snprintf(errors + used, sizeof(errors) - used, "]}"));
    return respond(422, errors, used, out, capacity);
  }

  if (!config_.save(candidate))
    return respondError(500, "{\"error\":\"persist_failed\"}", out, capacity);

  logger_.event(LogLevel::Info, "CONFIG_APPLIED_VIA_API");
  return respondError(200, "{\"status\":\"applied\"}", out, capacity);
}

ApiRouter::Response ApiRouter::handle(const Request& request, char* out,
                           size_t capacity) {
  if (capacity < kMinBodyCapacity) {
    Response r{};
    r.statusCode = 500;
    r.bytesWritten = 0;
    r.streamDone = true;
    return r;
  }
  if (exporter_) {
    exporter_->~ChunkedExporter();
    exporter_ = nullptr;
    streamKind_ = StreamKind::None;
  }
  if (streamKind_ == StreamKind::Html) {
    streamKind_ = StreamKind::None;
    htmlData_ = nullptr;
  }

  const char* path = request.target ? request.target : "/";
  char queryBuf[192] = "";
  const char* query = nullptr;
  const char* qmark = std::strchr(path, '?');
  if (qmark) {
    const size_t qlen = std::strlen(qmark + 1);
    copyString(queryBuf, sizeof(queryBuf), qmark + 1);
    query = queryBuf;
    const size_t plen = static_cast<size_t>(qmark - path) <
                                sizeof(routePathBuf_) - 1
                            ? static_cast<size_t>(qmark - path)
                            : sizeof(routePathBuf_) - 1;
    std::memcpy(routePathBuf_, path, plen);
    routePathBuf_[plen] = '\0';
    path = routePathBuf_;
  }

  const bool isGet = std::strcmp(request.method ? request.method : "GET",
                                  "GET") == 0;

  if (isGet) return routeGet(path, query, out, capacity);

  if (std::strcmp(request.method, "POST") == 0 &&
      pathEquals(path, "/api/v1/config")) {
    if (!authorized(request)) {
      if (!config_.current().adminTokenSha256[0])
        return respondError(503, "{\"error\":\"admin_not_configured\"}", out,
                            capacity);
      logger_.event(LogLevel::Warn, "CONFIG_WRITE_UNAUTHORIZED");
      return respondError(401, "{\"error\":\"unauthorized\"}", out, capacity);
    }
return routePostConfig(request, out, capacity);
  }

  if (std::strcmp(request.method, "POST") == 0)
    return respondError(404, "{\"error\":\"not_found\"}", out, capacity);

  return respondError(405, "{\"error\":\"method_not_allowed\"}", out, capacity);
}

ApiRouter::Response ApiRouter::continueStream(char* out, size_t capacity) {
  if (capacity < kMinBodyCapacity) {
    Response r{};
    r.statusCode = 500;
    r.streamActive = true;
    r.streamDone = true;
    return r;
  }
  if (streamKind_ == StreamKind::Html) return pumpStream(out, capacity);
  if (!exporter_) {
    Response r{};
    r.streamDone = true;
    r.streamActive = false;
    return r;
  }
  return pumpStream(out, capacity);
}

}  // namespace cauce::app

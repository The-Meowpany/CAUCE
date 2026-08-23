#include "cauce/core/DataExporter.h"

#include <cstdio>
#include <cstring>

#include "cauce/core/TimeUtils.h"

namespace cauce {

namespace {
bool fieldNeedsCsvQuoting(const char* s) {
  for (const char* p = s; *p; ++p) {
    if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') return true;
  }
  return false;
}

void appendText(char*& cursor, size_t& remaining, const char* text) {
  if (!text || remaining == 0) return;
  const size_t len = std::strlen(text);
  const size_t use = len < remaining ? len : remaining - 1;
  std::memcpy(cursor, text, use);
  cursor += use;
  remaining -= use;
}
}  // namespace

size_t escapeCsvField(const char* field, char* out, size_t capacity) {
  if (!out || capacity == 0) return 0;
  out[0] = '\0';
  if (!field) return 0;
  if (capacity < 3) return 0;
  if (!fieldNeedsCsvQuoting(field)) {
    const size_t len = std::strlen(field);
    if (len >= capacity) return 0;
    std::memcpy(out, field, len + 1);
    return len;
  }
  size_t used = 0;
  out[used++] = '"';
  for (const char* p = field; *p; ++p) {
    if (*p == '"') {
      if (used + 2 >= capacity) return 0;
      out[used++] = '"';
    }
    if (used + 1 >= capacity) return 0;
    out[used++] = *p;
  }
  if (used + 2 >= capacity) return 0;
  out[used++] = '"';
  out[used] = '\0';
  return used;
}

size_t escapeJsonString(const char* text, char* out, size_t capacity) {
  if (!out || capacity == 0) return 0;
  out[0] = '\0';
  if (!text) return 0;
  size_t used = 0;
  auto put = [&](char c) -> bool {
    if (used + 1 >= capacity) return false;
    out[used++] = c;
    return true;
  };
  if (!put('"')) return 0;
  for (const char* p = text; *p; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    switch (c) {
      case '"':
        if (!put('\\') || !put('"')) return 0;
        break;
      case '\\':
        if (!put('\\') || !put('\\')) return 0;
        break;
      case '\n':
        if (!put('\\') || !put('n')) return 0;
        break;
      case '\r':
        if (!put('\\') || !put('r')) return 0;
        break;
      case '\t':
        if (!put('\\') || !put('t')) return 0;
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          for (const char* q = buf; *q; ++q)
            if (!put(*q)) return 0;
        } else if (!put(static_cast<char>(c))) {
          return 0;
        }
    }
  }
  if (!put('"')) return 0;
  out[used] = '\0';
  return used;
}

ChunkedExporter::ChunkedExporter(IStorageRepository& store, Format format,
                                 uint64_t fromUtcMs, uint64_t toUtcMs)
    : store_(store), format_(format), fromMs_(fromUtcMs), toMs_(toUtcMs) {}

bool ChunkedExporter::fillBatch() {
  batchCount_ = static_cast<uint32_t>(
      store_.query(fromMs_, toMs_, skipMatches_, batch_, kBatchSize, stats_));
  batchIndex_ = 0;
  skipMatches_ += batchCount_;
  return batchCount_ > 0;
}

size_t ChunkedExporter::writeCsvHeaderIfNeeded(char* out, size_t capacity) {
  if (headerWritten_ || format_ != Format::Csv) return 0;
  headerWritten_ = true;
  return static_cast<size_t>(
      std::snprintf(out, capacity,
                    "node_id,sensor_id,sequence,timestamp_utc_ms,timestamp_iso,"
                    "variable,value,unit,quality,reason_bits,time_uncertain\n"));
}

size_t ChunkedExporter::encodeCsv(const Measurement& m, char* out,
                                  size_t capacity) {
  char iso[24];
  formatIso8601Utc(m.timestampUtcMs, iso, sizeof(iso));

  char nodeField[40], sensorField[56];
  escapeCsvField(m.nodeId, nodeField, sizeof(nodeField));
  escapeCsvField(m.sensorId, sensorField, sizeof(sensorField));

  char* c = out;
  size_t rem = capacity;

  appendText(c, rem, nodeField);
  appendText(c, rem, ",");
  appendText(c, rem, sensorField);
  char line[160];
  const int n = std::snprintf(line, sizeof(line),
                              ",%lu,%llu,%s,%s,%.2f,%s,%s,%u,%u\n",
                              static_cast<unsigned long>(m.sequence),
                              static_cast<unsigned long long>(m.timestampUtcMs),
                              iso, variableName(m.variable),
                              static_cast<double>(m.value), variableUnit(m.variable),
                              qualityName(m.quality),
                              static_cast<unsigned>(m.reasonBits),
                              m.timeUncertain ? 1u : 0u);
  (void)n;
  appendText(c, rem, line);
  return static_cast<size_t>(c - out);
}

size_t measurementToJson(const Measurement& m, char* out, size_t capacity) {
  char iso[24];
  formatIso8601Utc(m.timestampUtcMs, iso, sizeof(iso));
  char nodeJson[48], sensorJson[64];
  escapeJsonString(m.nodeId, nodeJson, sizeof(nodeJson));
  escapeJsonString(m.sensorId, sensorJson, sizeof(sensorJson));
  return static_cast<size_t>(std::snprintf(
      out, capacity,
      "{\"node_id\":%s,\"sensor_id\":%s,\"sequence\":%lu,"
      "\"timestamp\":\"%s\",\"timestamp_utc_ms\":%llu,"
      "\"variable\":\"%s\",\"value\":%.2f,\"unit\":\"%s\","
      "\"quality\":\"%s\",\"reason_bits\":%u,\"time_uncertain\":%s}",
      nodeJson, sensorJson, static_cast<unsigned long>(m.sequence), iso,
      static_cast<unsigned long long>(m.timestampUtcMs),
      variableName(m.variable), static_cast<double>(m.value),
      variableUnit(m.variable), qualityName(m.quality),
      static_cast<unsigned>(m.reasonBits), m.timeUncertain ? "true" : "false"));
}

size_t ChunkedExporter::encodeJson(const Measurement& m, char* out,
                                   size_t capacity) {
  return measurementToJson(m, out, capacity);
}

size_t ChunkedExporter::encodeOne(const Measurement& m, char* out,
                                  size_t capacity) {
  return format_ == Format::Csv ? encodeCsv(m, out, capacity)
                                : encodeJson(m, out, capacity);
}

size_t ChunkedExporter::next(char* out, size_t capacity) {
  if (done_) return 0;
  if (!out || capacity < kMinChunkCapacity) {
    done_ = true;
    return 0;
  }

  size_t used = 0;
  if (format_ == Format::Csv) {
    used += writeCsvHeaderIfNeeded(out + used, capacity - used);
  } else if (!elementEmitted_) {
    out[used++] = '[';
    elementEmitted_ = true;
  }

  while (true) {
    if (batchIndex_ >= batchCount_) {
      if (!fillBatch()) break;
    }
    char record[320];
    const size_t recordLen = encodeOne(batch_[batchIndex_], record, sizeof(record));
    if (recordLen == 0) {
      done_ = true;
      break;
    }
    size_t separatorLen = 0;
    if (format_ == Format::JsonArray) separatorLen = 1;
    else if (recordLen == 0 || record[recordLen - 1] != '\n') separatorLen = 1;

    if (used + recordLen + separatorLen + 2 > capacity) {
      if (used == 0) {
        done_ = true;
        return 0;
      }
      return used;
    }
    std::memcpy(out + used, record, recordLen);
    used += recordLen;
    if (format_ == Format::JsonArray) {
      out[used++] = ',';
    } else if (record[recordLen - 1] != '\n') {
      out[used++] = '\n';
    }
    ++batchIndex_;
  }

  if (format_ == Format::JsonArray) {
    if (used + 2 > capacity) return used;
    if (elementEmitted_ && used > 1 && out[used - 1] == ',') --used;
    out[used++] = ']';
    out[used] = '\0';
    done_ = true;
    return used;
  }
  done_ = true;
  return used;
}

}  // namespace cauce

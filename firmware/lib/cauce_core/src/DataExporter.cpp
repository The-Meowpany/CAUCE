#include "cauce/core/DataExporter.h"

#include <cstdio>
#include <cstring>

#include "cauce/core/TimeUtils.h"

namespace cauce {

namespace {
// The four characters that decide whether a field is quoted at all.
//
// WHY THE LEADING-CHARACTER CHECK IS SEPARATE AND COMES FIRST
//
// Quoting a field does not make it safe in a spreadsheet. Excel and LibreOffice both treat a
// cell whose text begins with `=`, `+`, `-` or `@` as a formula, *inside the quotes*. Quoting
// only handles commas, quotes and newlines, so `=cmd|'/C calc'!A0` came out of this escaper
// looking like a plain field and was evaluated by whoever opened the file.
//
// The central has the same bug in its own CSV export, fixed there at the same time; this is
// the device-side half and both write `node_id` into the first column.
//
// The mitigation is to prefix an apostrophe, which every spreadsheet treats as "this is text"
// and which no consumer of the file sees once the cell is displayed. It changes the field
// rather than refusing it: refusing would drop the row, and a node with a `=` in its id
// should report its data rather than vanish from the export. The apostrophe is only added
// when the field would otherwise be quoted, because an unquoted `=x` is evaluated too - the
// check has to run on every field, not only the ones containing structural characters.
// True when the text is a number.
//
// The central's escaper has the same carve-out and it is not optional there: without it every
// negative `calibration_offset` came out as `'-1.5`, which a backend test caught the moment
// the fix landed. A device with a negative correction would have written the same thing, and
// an apostrophe in front of a number is a lie about the data.
bool fieldIsNumber(const char* s) {
    if (!s || !*s) return false;
    const char* p = s;
    if (*p == '+' || *p == '-') ++p;
    bool digits = false;
    bool dot = false;
    for (; *p; ++p) {
      if (*p >= '0' && *p <= '9') {
        digits = true;
        continue;
      }
      if (*p == '.' && !dot) {
        dot = true;
        continue;
      }
      // An exponent, so -1.5e3 counts. Deliberately narrow: `strtod` would also accept
      // "nan", "inf" and hex, and prefixing those is harmless but surprising, while a
      // hand-rolled check cannot be surprised by a locale.
      if ((*p == 'e' || *p == 'E') && digits) {
        const char* q = p + 1;
        if (*q == '+' || *q == '-') ++q;
        if (*q < '0' || *q > '9') return false;
        digits = false;  // the mantissa digits do not count as exponent digits
        continue;
      }
      return false;
    }
    return digits;
  }

  bool fieldLooksLikeFormula(const char* s) {
    return s[0] == '=' || s[0] == '+' || s[0] == '-' || s[0] == '@';
  }

  bool fieldNeedsCsvQuoting(const char* s) {
    // Leading whitespace defeats the check: Excel trims it and evaluates what follows. So the
    // formula test skips spaces, tabs and quotes, which is the other way a leading `=` gets
    // past a naive check.
    const char* p = s;
    while (*p == ' ' || *p == '\t' || *p == '"') ++p;
    if (fieldIsNumber(p)) return false;
    if (fieldLooksLikeFormula(p)) return true;
    for (const char* q = s; *q; ++q) {
      if (*q == ',' || *q == '"' || *q == '\n' || *q == '\r') return true;
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
  // Everything below writes in place, so a failure has to leave the buffer empty rather
  // than half a cell. The lambda does that and is called at every early return.
  const auto fail = [&]() -> size_t {
    out[0] = '\0';
    return 0;
  };
  // Room for the shortest quoted output, `""`, plus the terminator.
  if (capacity < 3) return fail();
  if (!fieldNeedsCsvQuoting(field)) {
    const size_t len = std::strlen(field);
    if (len >= capacity) return fail();
    std::memcpy(out, field, len + 1);
    return len;
  }
// The formula guard goes inside the quotes, and AFTER any leading whitespace rather than
  // at the very front of the cell.
  //
  // Both halves matter. Outside the quotes an apostrophe would be part of the value instead
  // of a spreadsheet directive; in front of the whitespace it would do nothing at all,
  // because the spreadsheet trims `"  '=1+1"` back to `=1+1` and evaluates that. The cell
  // has to read `"  '=1+1"`.
  //
  // Numbers are exempt. The leading `-` of a negative value is the same byte as the leading
  // `-` of a formula, and prefixing every negative value would be a lie in the data column -
  // the central's writer has the identical carve-out and its test is what caught the same
  // mistake there.
  const char* formulaAt = field;
  while (*formulaAt == ' ' || *formulaAt == '\t' || *formulaAt == '"') ++formulaAt;
  const bool guard = !fieldIsNumber(formulaAt) && fieldLooksLikeFormula(formulaAt);

  size_t used = 0;
  out[used++] = '"';
  for (const char* p = field; *p; ++p) {
    // The apostrophe is inserted at the position the scan reached, which is after any
    // leading whitespace and before the `=`.
    if (p == formulaAt && guard) {
      if (used + 2 > capacity) return fail();
      out[used++] = '\'';
    }
    if (*p == '"') {
      if (used + 2 > capacity) return fail();
      out[used++] = '"';
    }
    if (used + 2 > capacity) return fail();
    out[used++] = *p;
  }
  if (used + 2 > capacity) return fail();
  out[used++] = '"';
  out[used] = '\0';
  return used;
}

size_t escapeJsonString(const char* text, char* out, size_t capacity) {
  // Same contract as `escapeCsvField`, and for the same reason: a caller that ignores the
  // return value must not be handed a half-written string. Restoring that by hand at seven
  // return sites is discipline that lasts exactly until somebody adds an eighth.
  const auto fail = [&]() -> size_t {
    out[0] = '\0';
    return 0;
  };
  if (!out || capacity == 0) return 0;
  out[0] = '\0';
  if (!text) return 0;
  size_t used = 0;
  auto put = [&](char c) -> bool {
    if (used + 1 >= capacity) return false;
    out[used++] = c;
    return true;
  };
  if (!put('"')) return fail();
  for (const char* p = text; *p; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    switch (c) {
      case '"':
        if (!put('\\') || !put('"')) return fail();
        break;
      case '\\':
        if (!put('\\') || !put('\\')) return fail();
        break;
      case '\n':
        if (!put('\\') || !put('n')) return fail();
        break;
      case '\r':
        if (!put('\\') || !put('r')) return fail();
        break;
      case '\t':
        if (!put('\\') || !put('t')) return fail();
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          for (const char* q = buf; *q; ++q)
            if (!put(*q)) return fail();
        } else if (!put(static_cast<char>(c))) {
          return fail();
        }
    }
  }
  if (!put('"')) return fail();
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

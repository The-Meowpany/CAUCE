#include "cauce/app/OtaJson.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cauce::app {

namespace {

bool findValue(const char* json, const char* key, const char*& valStart,
               bool& quoted) {
  char pattern[40];
  std::snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char* p = std::strstr(json, pattern);
  if (!p) return false;
  p = std::strchr(p + std::strlen(pattern), ':');
  if (!p) return false;
  ++p;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
  if (*p == '"') {
    quoted = true;
    valStart = p + 1;
    return true;
  }
  if ((*p >= '0' && *p <= '9') || *p == '-') {
    quoted = false;
    valStart = p;
    return true;
  }
  return false;
}

bool copyJsonString(const char* start, char* out, size_t capacity) {
  const char* end = std::strchr(start, '"');
  if (!end) return false;
  const size_t len = static_cast<size_t>(end - start);
  if (len == 0 || len >= capacity) return false;
  std::memcpy(out, start, len);
  out[len] = '\0';
  return true;
}

bool isHex64(const char* text) {
  for (int i = 0; i < 64; ++i) {
    const char c = text[i];
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F');
    if (!hex) return false;
  }
  return true;
}

bool isHttpUrl(const char* text) {
  return std::strncmp(text, "http://", 7) == 0 ||
         std::strncmp(text, "https://", 8) == 0;
}

}  // namespace

bool parseOtaManifestJson(const char* json, OtaRelease& out) {
  if (!json) return false;
  OtaRelease tmp{};
  const char* val = nullptr;
  bool quoted = false;
  if (!findValue(json, "version", val, quoted) || !quoted) return false;
  if (!copyJsonString(val, tmp.version, sizeof(tmp.version))) return false;
  if (!findValue(json, "sha256", val, quoted) || !quoted) return false;
  if (!copyJsonString(val, tmp.sha256Hex, sizeof(tmp.sha256Hex))) return false;
  if (!isHex64(tmp.sha256Hex)) return false;
  if (!findValue(json, "url", val, quoted) || !quoted) return false;
  if (!copyJsonString(val, tmp.url, sizeof(tmp.url))) return false;
  if (!isHttpUrl(tmp.url)) return false;
  if (!findValue(json, "total_size", val, quoted) || quoted) return false;
  char* endPtr = nullptr;
  const unsigned long size = std::strtoul(val, &endPtr, 10);
  if (endPtr == val || size == 0 || size > 0xFFFFFFFFUL) return false;
  tmp.totalSize = static_cast<uint32_t>(size);
  tmp.manifestHmacHex[0] = '\0';
  if (findValue(json, "hmac", val, quoted) && quoted) {
    if (!copyJsonString(val, tmp.manifestHmacHex,
                        sizeof(tmp.manifestHmacHex))) {
      return false;
    }
  }
  out = tmp;
  return true;
}

}  // namespace cauce::app

#include "cauce/core/TimeUtils.h"

#include <cstdio>
#include <cstring>

namespace cauce {
namespace {

struct CivilDate {
  int year;
  unsigned month;
  unsigned day;
};

CivilDate civilFromDays(int64_t z) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint64_t doe = static_cast<uint64_t>(z - era * 146097);
  const uint64_t yoe =
      (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint64_t mp = (5 * doy + 2) / 153;
  CivilDate d{};
  d.day = static_cast<unsigned>(doy - (153 * mp + 2) / 5 + 1);
  d.month = static_cast<unsigned>(mp < 10 ? mp + 3 : mp - 9);
  d.year = static_cast<int>(y + (d.month <= 2 ? 1 : 0));
  return d;
}

int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2 ? 1 : 0;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy =
      (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool digitsToInt(const char*& p, int count, int& out) {
  int value = 0;
  for (int i = 0; i < count; ++i) {
    if (*p < '0' || *p > '9') return false;
    value = value * 10 + (*p - '0');
    ++p;
  }
  out = value;
  return true;
}

}  // namespace

void formatIso8601Utc(uint64_t epochMs, char* out, size_t capacity) {
  if (!out || capacity < 21) {
    if (out && capacity > 0) out[0] = '\0';
    return;
  }
  const int64_t totalSeconds = static_cast<int64_t>(epochMs / 1000ULL);
  const unsigned msPart = static_cast<unsigned>(epochMs % 1000ULL);
  int64_t daySeconds = totalSeconds;
  const int64_t days = daySeconds >= 0 ? daySeconds / 86400
                                       : (daySeconds - 86399) / 86400;
  daySeconds -= days * 86400;
  const CivilDate date = civilFromDays(days);
  std::snprintf(out, capacity, "%04d-%02u-%02uT%02u:%02u:%02uZ",
                date.year, date.month, date.day,
                static_cast<unsigned>(daySeconds / 3600),
                static_cast<unsigned>((daySeconds / 60) % 60),
                static_cast<unsigned>(daySeconds % 60));
  (void)msPart;
}

bool parseIso8601Utc(const char* text, uint64_t& outEpochMs) {
  if (!text) return false;
  const char* p = text;
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (!digitsToInt(p, 4, year)) return false;
  if (*p++ != '-') return false;
  if (!digitsToInt(p, 2, month)) return false;
  if (*p++ != '-') return false;
  if (!digitsToInt(p, 2, day)) return false;
  if (*p == '\0') {
    hour = minute = second = 0;
  } else {
    if (*p++ != 'T' && *(p - 1) != ' ') return false;
    if (!digitsToInt(p, 2, hour)) return false;
    if (*p++ != ':') return false;
    if (!digitsToInt(p, 2, minute)) return false;
    if (*p++ != ':') return false;
    if (!digitsToInt(p, 2, second)) return false;
    while (*p == '.') ++p;
    while (*p >= '0' && *p <= '9') ++p;
    if (*p == 'Z') ++p;
  }
  if (*p != '\0') return false;
  if (month < 1 || month > 12 || day < 1 || day > 31) return false;
  if (hour > 23 || minute > 59 || second > 60) return false;

  const int64_t days =
      daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  const int64_t seconds =
      days * 86400LL + hour * 3600LL + minute * 60LL + second;
  if (seconds < 0) return false;
  outEpochMs = static_cast<uint64_t>(seconds) * 1000ULL;
  return true;
}

}  // namespace cauce

#include "cauce/drivers/Ds3231Rtc.h"

namespace cauce::drivers {

namespace {

constexpr uint8_t kRegSeconds = 0x00;
constexpr uint8_t kRegStatus = 0x0F;
constexpr uint8_t kOscillatorStopFlag = 0x80;

uint8_t fromBcd(uint8_t v) { return static_cast<uint8_t>((v >> 4) * 10 + (v & 0x0F)); }
uint8_t toBcd(int v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }

// Days from 1970-01-01 to a civil date (Howard Hinnant's algorithm, proleptic Gregorian).
int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

}  // namespace

Ds3231Rtc::Ds3231Rtc(hal::II2cBus& bus) : bus_(bus) {}

uint64_t Ds3231Rtc::toEpochMs(int year, int month, int day, int hour, int minute, int second) {
  const int64_t days = daysFromCivil(year, month, day);
  const int64_t seconds = days * 86400 + hour * 3600 + minute * 60 + second;
  return static_cast<uint64_t>(seconds) * 1000ULL;
}

void Ds3231Rtc::fromEpochMs(uint64_t epochMs, int& year, int& month, int& day, int& hour,
                            int& minute, int& second) {
  const int64_t total = static_cast<int64_t>(epochMs / 1000ULL);
  int64_t days = total / 86400;
  int64_t rem = total % 86400;
  hour = static_cast<int>(rem / 3600);
  minute = static_cast<int>((rem % 3600) / 60);
  second = static_cast<int>(rem % 60);
  days += 719468;
  const int64_t era = days / 146097;
  const int64_t doe = days - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  year = static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0));
}

bool Ds3231Rtc::present() { return bus_.isPresent(kAddress); }

bool Ds3231Rtc::timeTrustworthy() {
  uint8_t status = 0;
  if (!bus_.readRegisters(kAddress, kRegStatus, &status, 1)) return false;
  return (status & kOscillatorStopFlag) == 0;
}

bool Ds3231Rtc::readUtcMs(uint64_t& outEpochMs) {
  uint8_t r[7];
  if (!bus_.readRegisters(kAddress, kRegSeconds, r, sizeof(r))) return false;
  const int second = fromBcd(r[0] & 0x7F);
  const int minute = fromBcd(r[1] & 0x7F);
  // 12-hour mode is never written by this driver; a chip found in it is treated as corrupt.
  if (r[2] & 0x40) return false;
  const int hour = fromBcd(r[2] & 0x3F);
  const int day = fromBcd(r[4] & 0x3F);
  const int month = fromBcd(r[5] & 0x1F);
  const int year = 2000 + fromBcd(r[6]);
  if (second > 59 || minute > 59 || hour > 23 || day < 1 || day > 31 || month < 1 ||
      month > 12) {
    return false;
  }
  outEpochMs = toEpochMs(year, month, day, hour, minute, second);
  return true;
}

bool Ds3231Rtc::writeUtcMs(uint64_t epochMs) {
  int year, month, day, hour, minute, second;
  fromEpochMs(epochMs, year, month, day, hour, minute, second);
  if (year < 2000 || year > 2099) return false;
  // Day of week is required by the register map but unused here; 1 keeps it in range.
  const uint8_t r[7] = {toBcd(second), toBcd(minute), toBcd(hour), 1,
                        toBcd(day),    toBcd(month),  toBcd(year - 2000)};
  if (!bus_.writeRegisters(kAddress, kRegSeconds, r, sizeof(r))) return false;
  uint8_t status = 0;
  if (!bus_.readRegisters(kAddress, kRegStatus, &status, 1)) return false;
  status = static_cast<uint8_t>(status & ~kOscillatorStopFlag);
  return bus_.writeRegisters(kAddress, kRegStatus, &status, 1);
}

}  // namespace cauce::drivers

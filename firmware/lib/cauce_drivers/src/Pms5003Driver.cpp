#include "cauce/drivers/Pms5003Driver.h"

#include "cauce/core/Types.h"

namespace cauce::drivers {

namespace {

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

}  // namespace

Pms5003Driver::Pms5003Driver(hal::IUart& uart, hal::IClock& clock, const char* id)
    : uart_(uart), clock_(clock) {
  copyString(metadata_.id, sizeof(metadata_.id), id);
  copyString(metadata_.model, sizeof(metadata_.model), "PMS5003");
  metadata_.variables[0] = Variable::Pm1;
  metadata_.variables[1] = Variable::Pm25;
  metadata_.variables[2] = Variable::Pm10;
  metadata_.variableCount = 3;
  // Datasheet: +/-10 ug/m3 below 100 ug/m3, +/-10 % above.
  metadata_.accuracyNote = 10.0f;
  metadata_.resolution = 1.0f;
}

bool Pms5003Driver::parse(const uint8_t* data, size_t length, Frame& out) {
  if (data == nullptr || length < kFrameBytes) return false;
  // Scanned from the end: when the buffer holds more than one frame, the newest one wins.
  for (size_t start = length - kFrameBytes + 1; start-- > 0;) {
    const uint8_t* f = data + start;
    if (f[0] != 0x42 || f[1] != 0x4D || be16(f + 2) != kFrameBytes - 4) continue;
    uint16_t sum = 0;
    for (size_t i = 0; i < kFrameBytes - 2; ++i) sum = static_cast<uint16_t>(sum + f[i]);
    if (sum != be16(f + kFrameBytes - 2)) continue;
    out.pm1 = be16(f + 10);
    out.pm25 = be16(f + 12);
    out.pm10 = be16(f + 14);
    return true;
  }
  return false;
}

bool Pms5003Driver::begin() {
  if (!uart_.begin(kBaud)) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  uart_.discardInput();
  startedAtMs_ = clock_.monotonicMs();
  // The sensor streams on its own; one frame within the timeout proves it is there.
  uint8_t buffer[2 * kFrameBytes];
  const size_t got = uart_.read(buffer, sizeof(buffer), kReadTimeoutMs);
  Frame frame;
  if (!parse(buffer, got, frame)) {
    lastStatus_ = SensorStatus::NotConnected;
    return false;
  }
  lastStatus_ = SensorStatus::Initializing;
  return true;
}

SensorStatus Pms5003Driver::health() { return lastStatus_; }

bool Pms5003Driver::refresh() {
  const uint32_t now = clock_.monotonicMs();
  if (haveFrame_ && now - frameAtMs_ < kFrameReuseMs) return true;
  // Whatever piled up since the last cycle is stale; the next frame is at most a second away.
  uart_.discardInput();
  uint8_t buffer[2 * kFrameBytes];
  const size_t got = uart_.read(buffer, sizeof(buffer), kReadTimeoutMs);
  Frame frame;
  if (!parse(buffer, got, frame)) {
    lastStatus_ = got == 0 ? SensorStatus::NotConnected : SensorStatus::InvalidData;
    haveFrame_ = false;
    return false;
  }
  frame_ = frame;
  frameAtMs_ = now;
  haveFrame_ = true;
  return true;
}

bool Pms5003Driver::read(Variable variable, Reading& outReading) {
  outReading = Reading{};
  if (variable != Variable::Pm1 && variable != Variable::Pm25 && variable != Variable::Pm10) {
    outReading.status = SensorStatus::InvalidData;
    return false;
  }
  if (clock_.monotonicMs() - startedAtMs_ < kWarmupMs) {
    lastStatus_ = SensorStatus::Initializing;
    outReading.status = lastStatus_;
    return false;
  }
  if (!refresh()) {
    outReading.status = lastStatus_;
    return false;
  }
  lastStatus_ = SensorStatus::Ok;
  outReading.ok = true;
  outReading.status = SensorStatus::Ok;
  outReading.value = static_cast<float>(variable == Variable::Pm1    ? frame_.pm1
                                        : variable == Variable::Pm25 ? frame_.pm25
                                                                     : frame_.pm10);
  return true;
}

const SensorMetadata& Pms5003Driver::metadata() const { return metadata_; }

}  // namespace cauce::drivers

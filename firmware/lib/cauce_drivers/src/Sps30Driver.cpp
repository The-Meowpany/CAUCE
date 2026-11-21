#include "cauce/drivers/Sps30Driver.h"

#include <cmath>
#include <cstring>

#include "cauce/core/Types.h"

namespace cauce::drivers {

namespace {

constexpr uint16_t kStartMeasurement = 0x0010;
constexpr uint16_t kReadDataReady = 0x0202;
constexpr uint16_t kReadMeasuredValues = 0x0300;
constexpr uint8_t kFloatFormat = 0x03;  // big-endian IEEE754 floats

int indexFor(Variable v) {
  switch (v) {
    case Variable::Pm1:
      return 0;
    case Variable::Pm25:
      return 1;
    case Variable::Pm4:
      return 2;
    case Variable::Pm10:
      return 3;
    default:
      return -1;
  }
}

}  // namespace

Sps30Driver::Sps30Driver(hal::II2cBus& bus, hal::IClock& clock, const char* id)
    : bus_(bus), clock_(clock) {
  copyString(metadata_.id, sizeof(metadata_.id), id);
  copyString(metadata_.model, sizeof(metadata_.model), "SPS30");
  metadata_.variables[0] = Variable::Pm1;
  metadata_.variables[1] = Variable::Pm25;
  metadata_.variables[2] = Variable::Pm4;
  metadata_.variables[3] = Variable::Pm10;
  metadata_.variableCount = 4;
  // Datasheet: +/-10 ug/m3 below 100 ug/m3, +/-10 % above.
  metadata_.accuracyNote = 10.0f;
  metadata_.resolution = 0.1f;
}

uint8_t Sps30Driver::crc8(const uint8_t* data, uint8_t length) {
  uint8_t crc = 0xFF;
  for (uint8_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc;
}

bool Sps30Driver::sendCommand(uint16_t command, const uint8_t* args, uint8_t argWords) {
  uint8_t buffer[2 + 3 * 2];
  uint8_t n = 0;
  buffer[n++] = static_cast<uint8_t>(command >> 8);
  buffer[n++] = static_cast<uint8_t>(command & 0xFF);
  for (uint8_t w = 0; w < argWords && w < 2; ++w) {
    buffer[n++] = args[2 * w];
    buffer[n++] = args[2 * w + 1];
    buffer[n++] = crc8(args + 2 * w, 2);
  }
  return bus_.writeBytes(kAddress, buffer, n);
}

bool Sps30Driver::readWords(uint16_t command, uint8_t* out, uint8_t words) {
  if (!sendCommand(command)) return false;
  uint8_t raw[60];
  const uint8_t rawLength = static_cast<uint8_t>(words * 3);
  if (rawLength > sizeof(raw)) return false;
  if (!bus_.readBytes(kAddress, raw, rawLength)) return false;
  for (uint8_t w = 0; w < words; ++w) {
    // A word with a bad CRC poisons the whole frame: the four values are only meaningful
    // together, and a half-checked frame would store one corrupt number next to good ones.
    if (crc8(raw + 3 * w, 2) != raw[3 * w + 2]) return false;
    out[2 * w] = raw[3 * w];
    out[2 * w + 1] = raw[3 * w + 1];
  }
  return true;
}

bool Sps30Driver::begin() {
  if (!bus_.isPresent(kAddress)) {
    lastStatus_ = SensorStatus::NotConnected;
    return false;
  }
  const uint8_t args[2] = {kFloatFormat, 0x00};
  if (!sendCommand(kStartMeasurement, args, 1)) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  started_ = true;
  startedAtMs_ = clock_.monotonicMs();
  haveFrame_ = false;
  lastStatus_ = SensorStatus::Initializing;
  return true;
}

SensorStatus Sps30Driver::health() {
  if (!started_) return lastStatus_;
  if (!bus_.isPresent(kAddress)) {
    lastStatus_ = SensorStatus::NotConnected;
    return lastStatus_;
  }
  if (clock_.monotonicMs() - startedAtMs_ < kWarmupMs) {
    lastStatus_ = SensorStatus::Initializing;
  } else if (lastStatus_ == SensorStatus::Initializing) {
    lastStatus_ = SensorStatus::Ok;
  }
  return lastStatus_;
}

bool Sps30Driver::fetchFrame() {
  uint8_t ready[2] = {0, 0};
  if (!readWords(kReadDataReady, ready, 1)) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  if (ready[1] != 0x01) {
    lastStatus_ = SensorStatus::Initializing;
    return false;
  }
  // Ten floats (mass x4, number x5, typical size), two words each. Only mass is kept.
  uint8_t data[40];
  if (!readWords(kReadMeasuredValues, data, 20)) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    const uint32_t bits = (static_cast<uint32_t>(data[4 * i]) << 24) |
                          (static_cast<uint32_t>(data[4 * i + 1]) << 16) |
                          (static_cast<uint32_t>(data[4 * i + 2]) << 8) |
                          static_cast<uint32_t>(data[4 * i + 3]);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    if (!std::isfinite(value)) {
      lastStatus_ = SensorStatus::InvalidData;
      return false;
    }
    mass_[i] = value;
  }
  haveFrame_ = true;
  frameAtMs_ = clock_.monotonicMs();
  lastStatus_ = SensorStatus::Ok;
  return true;
}

bool Sps30Driver::read(Variable variable, Reading& outReading) {
  outReading = Reading{};
  const int index = indexFor(variable);
  if (index < 0) {
    outReading.status = SensorStatus::InvalidData;
    return false;
  }
  if (!started_ && !begin()) {
    outReading.status = lastStatus_;
    return false;
  }
  const uint32_t now = clock_.monotonicMs();
  if (now - startedAtMs_ < kWarmupMs) {
    // The fan has not settled: values exist but describe the start-up transient.
    outReading.status = SensorStatus::Initializing;
    return false;
  }
  if (!haveFrame_ || now - frameAtMs_ >= kFrameReuseMs) {
    if (!fetchFrame()) {
      outReading.status = lastStatus_;
      return false;
    }
  }
  outReading.ok = true;
  outReading.status = SensorStatus::Ok;
  outReading.value = mass_[index];
  return true;
}

const SensorMetadata& Sps30Driver::metadata() const { return metadata_; }

}  // namespace cauce::drivers

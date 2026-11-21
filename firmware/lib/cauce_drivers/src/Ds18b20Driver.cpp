#include "cauce/drivers/Ds18b20Driver.h"

#include "cauce/core/Types.h"

namespace cauce::drivers {

namespace {

constexpr uint8_t kSkipRom = 0xCC;
constexpr uint8_t kConvertT = 0x44;
constexpr uint8_t kReadScratchpad = 0xBE;
// The register value at power-up, before any conversion has completed.
constexpr int16_t kPowerOnRaw = 0x0550;  // 85.0 C

}  // namespace

uint8_t Ds18b20Driver::crc8(const uint8_t* data, uint8_t length) {
  uint8_t crc = 0;
  for (uint8_t i = 0; i < length; ++i) {
    uint8_t byte = data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint8_t mix = static_cast<uint8_t>((crc ^ byte) & 0x01);
      crc >>= 1;
      if (mix) crc ^= 0x8C;
      byte >>= 1;
    }
  }
  return crc;
}

Ds18b20Driver::Ds18b20Driver(hal::IOneWire& bus, const char* id, Variable variable)
    : bus_(bus), variable_(variable) {
  copyString(metadata_.id, sizeof(metadata_.id), id);
  copyString(metadata_.model, sizeof(metadata_.model), "DS18B20");
  metadata_.variables[0] = variable;
  metadata_.variableCount = 1;
  metadata_.accuracyNote = 0.5f;  // datasheet, -10..85 C
  metadata_.resolution = 0.0625f;
}

bool Ds18b20Driver::startConversion() {
  if (!bus_.reset()) return false;
  bus_.writeByte(kSkipRom);
  bus_.writeByte(kConvertT);
  conversionPending_ = true;
  return true;
}

bool Ds18b20Driver::begin() {
  if (!startConversion()) {
    lastStatus_ = SensorStatus::NotConnected;
    return false;
  }
  lastStatus_ = SensorStatus::Initializing;
  return true;
}

SensorStatus Ds18b20Driver::health() { return lastStatus_; }

bool Ds18b20Driver::read(Variable variable, Reading& outReading) {
  outReading = Reading{};
  if (variable != variable_) {
    outReading.status = SensorStatus::InvalidData;
    return false;
  }
  if (!conversionPending_) {
    // Nothing to collect yet: start one and report that the value comes next interval.
    lastStatus_ = startConversion() ? SensorStatus::Initializing : SensorStatus::NotConnected;
    outReading.status = lastStatus_;
    return false;
  }
  if (!bus_.reset()) {
    conversionPending_ = false;
    lastStatus_ = SensorStatus::NotConnected;
    outReading.status = lastStatus_;
    return false;
  }
  bus_.writeByte(kSkipRom);
  bus_.writeByte(kReadScratchpad);
  uint8_t pad[9];
  for (uint8_t& b : pad) b = bus_.readByte();
  // Start the next conversion before judging this one, so a single bad frame does not cost
  // two intervals.
  const bool restarted = startConversion();

  if (crc8(pad, 8) != pad[8]) {
    lastStatus_ = SensorStatus::InvalidData;
    outReading.status = lastStatus_;
    return false;
  }
  const int16_t raw = static_cast<int16_t>(pad[0] | (static_cast<uint16_t>(pad[1]) << 8));
  if (raw == kPowerOnRaw) {
    // 85.0 C is what the part holds until a conversion finishes. A real 85 C soil reading is
    // not a case this deployment has to support; a probe that browned out is.
    lastStatus_ = SensorStatus::Initializing;
    outReading.status = lastStatus_;
    return false;
  }
  lastStatus_ = restarted ? SensorStatus::Ok : SensorStatus::NotConnected;
  outReading.ok = true;
  outReading.status = SensorStatus::Ok;
  outReading.value = static_cast<float>(raw) / 16.0f;
  return true;
}

const SensorMetadata& Ds18b20Driver::metadata() const { return metadata_; }

}  // namespace cauce::drivers

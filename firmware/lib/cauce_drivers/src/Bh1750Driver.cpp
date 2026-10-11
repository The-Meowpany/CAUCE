#include "cauce/drivers/Bh1750Driver.h"

#include "cauce/core/Types.h"

namespace cauce::drivers {

Bh1750Driver::Bh1750Driver(hal::II2cBus& bus, hal::IClock& clock, const char* id,
                           uint8_t i2cAddress)
    : bus_(bus), clock_(clock), address_(i2cAddress) {
  copyString(metadata_.id, sizeof(metadata_.id), id);
  copyString(metadata_.model, sizeof(metadata_.model), "BH1750");
  metadata_.variables[0] = Variable::Light;
  metadata_.variableCount = 1;
  // Datasheet accuracy is +/-20 %, which is the number the field procedure has to plan around.
  metadata_.accuracyNote = 20.0f;
  metadata_.resolution = 1.0f;
}

bool Bh1750Driver::configure() {
  const uint8_t on = static_cast<uint8_t>(Register::PowerOn);
  if (!bus_.writeRegisters(address_, 0x00, &on, 1)) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  const uint8_t reset = static_cast<uint8_t>(Register::Reset);
  if (!bus_.writeRegisters(address_, 0x00, &reset, 1)) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  clock_.sleepMs(5);
  // High-resolution continuous at 1 lx. Mode2 would give 0.5 lx but saturates an outdoor node
  // far earlier, and 1 lx is already finer than the +/-20 % the sensor actually holds.
  const uint8_t mode = static_cast<uint8_t>(Register::ContinuousHighRes);
  if (!bus_.writeRegisters(address_, 0x00, &mode, 1)) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  clock_.sleepMs(180);
  configured_ = true;
  lastStatus_ = SensorStatus::Ok;
  return true;
}

bool Bh1750Driver::begin() {
  if (!bus_.isPresent(address_)) {
    lastStatus_ = SensorStatus::NotConnected;
    return false;
  }
  return configure();
}

bool Bh1750Driver::readRaw(uint16_t& outRaw) {
  uint8_t buffer[2] = {0, 0};
  if (!bus_.readRegisters(address_, 0x00, buffer, 2)) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  outRaw = static_cast<uint16_t>((static_cast<uint16_t>(buffer[0]) << 8) | buffer[1]);
  return true;
}

SensorStatus Bh1750Driver::health() {
  if (!configured_) return lastStatus_;
  if (!bus_.isPresent(address_)) {
    lastStatus_ = SensorStatus::NotConnected;
    return lastStatus_;
  }
  lastStatus_ = SensorStatus::Ok;
  return lastStatus_;
}

bool Bh1750Driver::read(Variable variable, Reading& outReading) {
  outReading = Reading{};
  if (variable != Variable::Light) {
    outReading.status = SensorStatus::InvalidData;
    return false;
  }
  if (!configured_ && !configure()) return false;

  uint16_t raw = 0;
  if (!readRaw(raw)) {
    outReading.status = lastStatus_;
    return false;
  }

  // The datasheet's own conversion for high-resolution mode. The subtraction is the dark
  // current the part expects to be compensated away; it is what keeps a reading at night near
  // zero instead of sitting at a small positive floor that would never trigger anything.
  const float lux = static_cast<float>(raw) / 1.2f;
  lastLux_ = lux;
  outReading.ok = true;
  outReading.status = SensorStatus::Ok;
  outReading.value = lux;
  return true;
}

const SensorMetadata& Bh1750Driver::metadata() const { return metadata_; }

}  // namespace cauce::drivers
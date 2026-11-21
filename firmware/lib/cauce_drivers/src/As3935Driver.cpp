#include "cauce/drivers/As3935Driver.h"

#include "cauce/core/Types.h"

namespace cauce::drivers {

namespace {

constexpr uint8_t kRegAfeGain = 0x00;
constexpr uint8_t kRegInterrupt = 0x03;
constexpr uint8_t kRegDistance = 0x07;
constexpr uint8_t kRegPresetDefault = 0x3C;
constexpr uint8_t kRegCalibRco = 0x3D;
constexpr uint8_t kDirectCommand = 0x96;

constexpr uint8_t kGainIndoor = 0x12;
constexpr uint8_t kGainOutdoor = 0x0E;

constexpr uint8_t kIntNoise = 0x01;
constexpr uint8_t kIntDisturber = 0x04;
constexpr uint8_t kIntLightning = 0x08;
constexpr uint8_t kMaskDisturber = 0x20;

}  // namespace

As3935Driver::As3935Driver(hal::II2cBus& bus, const char* id, bool indoor, uint8_t address)
    : bus_(bus), address_(address), indoor_(indoor) {
  copyString(metadata_.id, sizeof(metadata_.id), id);
  copyString(metadata_.model, sizeof(metadata_.model), "AS3935");
  metadata_.variables[0] = Variable::LightningStrikes;
  metadata_.variables[1] = Variable::LightningDistance;
  metadata_.variableCount = 2;
  metadata_.resolution = 1.0f;
}

bool As3935Driver::writeRegister(uint8_t reg, uint8_t value) {
  return bus_.writeRegisters(address_, reg, &value, 1);
}

bool As3935Driver::readRegister(uint8_t reg, uint8_t& value) {
  return bus_.readRegisters(address_, reg, &value, 1);
}

bool As3935Driver::begin() {
  if (!bus_.isPresent(address_)) {
    lastStatus_ = SensorStatus::NotConnected;
    return false;
  }
  // Restore defaults, then calibrate the internal RC oscillators. Without the calibration the
  // timing of the signal validation drifts and the strike/disturber classification degrades.
  if (!writeRegister(kRegPresetDefault, kDirectCommand) ||
      !writeRegister(kRegCalibRco, kDirectCommand)) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  const uint8_t gain = static_cast<uint8_t>((indoor_ ? kGainIndoor : kGainOutdoor) << 1);
  if (!writeRegister(kRegAfeGain, gain)) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  // Disturbers (man-made pulses) are counted for diagnostics but kept off the IRQ line, so a
  // noisy site does not spend its loop servicing events that are never data.
  uint8_t intReg = 0;
  if (!readRegister(kRegInterrupt, intReg) ||
      !writeRegister(kRegInterrupt, static_cast<uint8_t>(intReg | kMaskDisturber))) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  configured_ = true;
  lastStatus_ = SensorStatus::Ok;
  return true;
}

SensorStatus As3935Driver::health() {
  if (!configured_) return lastStatus_;
  lastStatus_ = bus_.isPresent(address_) ? SensorStatus::Ok : SensorStatus::NotConnected;
  return lastStatus_;
}

void As3935Driver::service() {
  if (!pending_ || !configured_) return;
  pending_ = false;
  uint8_t cause = 0;
  if (!readRegister(kRegInterrupt, cause)) {
    lastStatus_ = SensorStatus::BusError;
    return;
  }
  cause &= 0x0F;
  if (cause & kIntLightning) {
    uint8_t distance = 0;
    if (readRegister(kRegDistance, distance)) {
      distance &= 0x3F;
      ++strikes_;
      if (distance != kOutOfRange && distance < nearestKm_) nearestKm_ = distance;
    }
  } else if (cause & kIntDisturber) {
    ++disturbers_;
  } else if (cause & kIntNoise) {
    ++noiseEvents_;
  }
}

bool As3935Driver::read(Variable variable, Reading& outReading) {
  outReading = Reading{};
  if (!configured_ && !begin()) {
    outReading.status = lastStatus_;
    return false;
  }
  if (variable == Variable::LightningStrikes) {
    outReading.ok = true;
    outReading.status = SensorStatus::Ok;
    outReading.value = static_cast<float>(strikes_);
    strikesTaken_ = true;
    return true;
  }
  if (variable == Variable::LightningDistance) {
    const uint8_t nearest = nearestKm_;
    const bool any = strikes_ > 0 && nearest != kOutOfRange;
    // The distance read closes the interval opened by the strike read.
    if (strikesTaken_) {
      strikes_ = 0;
      nearestKm_ = kOutOfRange;
      strikesTaken_ = false;
    }
    if (!any) {
      // No strike in range: there is no distance to report, and zero would mean "overhead".
      outReading.status = SensorStatus::NoEvent;
      return false;
    }
    outReading.ok = true;
    outReading.status = SensorStatus::Ok;
    outReading.value = static_cast<float>(nearest);
    return true;
  }
  outReading.status = SensorStatus::InvalidData;
  return false;
}

const SensorMetadata& As3935Driver::metadata() const { return metadata_; }

}  // namespace cauce::drivers

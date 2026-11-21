#include "cauce/drivers/GypsumAcDriver.h"

#include <cmath>

#include "cauce/core/Types.h"

namespace cauce::drivers {

namespace {
// In the two phases the divider's halves swap places, so their voltages add up to the supply.
// A sum that is far off means one end is not being driven: the expander lost its outputs, a
// wire is off, or the ADC pin is not on the divider at all.
constexpr float kSupplyTolerance = 0.15f;
}  // namespace

GypsumAcDriver::GypsumAcDriver(hal::II2cBus& bus, hal::IAnalogInput& adc, uint8_t adcChannel,
                               const char* id, float kpaPerMv, float offsetKpa, float supplyMv,
                               uint8_t address)
    : bus_(bus),
      adc_(adc),
      channel_(adcChannel),
      kpaPerMv_(kpaPerMv),
      offsetKpa_(offsetKpa),
      supplyMv_(supplyMv),
      address_(address) {
  copyString(metadata_.id, sizeof(metadata_.id), id);
  copyString(metadata_.model, sizeof(metadata_.model), "GYPSUM-AC");
  metadata_.variables[0] = Variable::SoilWaterPotential;
  metadata_.variableCount = 1;
}

bool GypsumAcDriver::drive(uint8_t pins) {
  return bus_.writeRegisters(address_, kRegOlat, &pins, 1);
}

bool GypsumAcDriver::phase(uint8_t pins, float& outMv) {
  if (!drive(pins)) return false;
  return adc_.readMillivolts(channel_, outMv);
}

bool GypsumAcDriver::begin() {
  if (!bus_.isPresent(address_)) {
    lastStatus_ = SensorStatus::NotConnected;
    return false;
  }
  // Latch low before switching the pins to outputs, so they never come up driving.
  const uint8_t outputs = static_cast<uint8_t>(~(kPinA | kPinB));
  if (!drive(0) || !bus_.writeRegisters(address_, kRegIodir, &outputs, 1)) {
    lastStatus_ = SensorStatus::BusError;
    return false;
  }
  lastStatus_ = SensorStatus::Ok;
  return true;
}

SensorStatus GypsumAcDriver::health() { return lastStatus_; }

bool GypsumAcDriver::read(Variable variable, Reading& outReading) {
  outReading = Reading{};
  if (variable != Variable::SoilWaterPotential) {
    outReading.status = SensorStatus::InvalidData;
    return false;
  }
  float forward = 0.0f;
  float reverse = 0.0f;
  const bool ok = phase(kPinA, forward) && phase(kPinB, reverse);
  // Always end with both ends low, even after a failure: a pin left high is DC through the
  // block until the next read, which is what this driver exists to avoid.
  const bool idle = drive(0);
  if (!ok || !idle) {
    lastStatus_ = SensorStatus::BusError;
    outReading.status = lastStatus_;
    return false;
  }
  if (std::fabs(forward + reverse - supplyMv_) > kSupplyTolerance * supplyMv_) {
    lastStatus_ = SensorStatus::InvalidData;
    outReading.status = lastStatus_;
    return false;
  }
  // Forward is the DC circuit's voltage; reverse is its complement. Averaging the two
  // estimates halves the ADC noise and keeps the result in the DC fit's units.
  lastMv_ = (forward + (supplyMv_ - reverse)) * 0.5f;
  float value = kpaPerMv_ * lastMv_ + offsetKpa_;
  if (!std::isfinite(value)) {
    lastStatus_ = SensorStatus::InvalidData;
    outReading.status = lastStatus_;
    return false;
  }
  if (value < 0.0f) value = 0.0f;
  lastStatus_ = SensorStatus::Ok;
  outReading.ok = true;
  outReading.status = SensorStatus::Ok;
  outReading.value = value;
  return true;
}

const SensorMetadata& GypsumAcDriver::metadata() const { return metadata_; }

}  // namespace cauce::drivers

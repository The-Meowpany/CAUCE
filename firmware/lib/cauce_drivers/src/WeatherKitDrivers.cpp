#include "cauce/drivers/WeatherKitDrivers.h"

#include <cmath>

#include "cauce/core/Types.h"

namespace cauce::drivers {

// ---------------------------------------------------------------- rain gauge

RainGaugeDriver::RainGaugeDriver(const char* id, float mmPerTip) : mmPerTip_(mmPerTip) {
  copyString(metadata_.id, sizeof(metadata_.id), id);
  copyString(metadata_.model, sizeof(metadata_.model), "TIPPING-BUCKET");
  metadata_.variables[0] = Variable::Rainfall;
  metadata_.variableCount = 1;
  metadata_.resolution = mmPerTip;
}

void RainGaugeDriver::onTip(uint32_t nowMs) {
  if (sawTip_ && nowMs - lastTipMs_ < kDebounceMs) return;
  sawTip_ = true;
  lastTipMs_ = nowMs;
  totalTips_ = totalTips_ + 1;
}

bool RainGaugeDriver::begin() { return true; }

// A reed switch has no bus to probe. A broken cable reads exactly like a dry day, which is why
// the co-location plan checks every gauge with a measured volume of water before deployment.
SensorStatus RainGaugeDriver::health() { return SensorStatus::Ok; }

bool RainGaugeDriver::read(Variable variable, Reading& outReading) {
  outReading = Reading{};
  if (variable != Variable::Rainfall) {
    outReading.status = SensorStatus::InvalidData;
    return false;
  }
  const uint32_t total = totalTips_;
  const uint32_t tips = total - reportedTips_;
  reportedTips_ = total;
  outReading.ok = true;
  outReading.status = SensorStatus::Ok;
  outReading.value = static_cast<float>(tips) * mmPerTip_;
  return true;
}

const SensorMetadata& RainGaugeDriver::metadata() const { return metadata_; }

// ---------------------------------------------------------------- wind vane

namespace {

struct VanePosition {
  float degrees;
  float ohms;
};

// Resistance per heading for the common kit vane (Argent/Misol/SparkFun pattern).
constexpr VanePosition kVane[] = {
    {0.0f, 33000.0f},    {22.5f, 6570.0f},   {45.0f, 8200.0f},   {67.5f, 891.0f},
    {90.0f, 1000.0f},    {112.5f, 688.0f},   {135.0f, 2200.0f},  {157.5f, 1410.0f},
    {180.0f, 3900.0f},   {202.5f, 3140.0f},  {225.0f, 16000.0f}, {247.5f, 14120.0f},
    {270.0f, 120000.0f}, {292.5f, 42120.0f}, {315.0f, 64900.0f}, {337.5f, 21880.0f},
};

}  // namespace

WindVaneDriver::WindVaneDriver(hal::IAnalogInput& adc, uint8_t channel, const char* id,
                               float supplyMv, float pullUpOhms, float offsetDegrees)
    : adc_(adc),
      channel_(channel),
      supplyMv_(supplyMv),
      pullUpOhms_(pullUpOhms),
      offsetDegrees_(offsetDegrees) {
  copyString(metadata_.id, sizeof(metadata_.id), id);
  copyString(metadata_.model, sizeof(metadata_.model), "RESISTOR-VANE");
  metadata_.variables[0] = Variable::WindDirection;
  metadata_.variableCount = 1;
  metadata_.resolution = 22.5f;
}

float WindVaneDriver::headingForMillivolts(float mv) const {
  float bestError = 1e9f;
  float bestDegrees = -1.0f;
  for (const auto& p : kVane) {
    const float expected = supplyMv_ * p.ohms / (p.ohms + pullUpOhms_);
    const float error = std::fabs(expected - mv);
    if (error < bestError) {
      bestError = error;
      bestDegrees = p.degrees;
    }
  }
  if (bestError > kMaxErrorMv) return -1.0f;
  float heading = std::fmod(bestDegrees + offsetDegrees_, 360.0f);
  if (heading < 0.0f) heading += 360.0f;
  return heading;
}

bool WindVaneDriver::begin() {
  float mv = 0.0f;
  lastStatus_ = adc_.readMillivolts(channel_, mv) ? SensorStatus::Ok : SensorStatus::BusError;
  return lastStatus_ == SensorStatus::Ok;
}

SensorStatus WindVaneDriver::health() { return lastStatus_; }

bool WindVaneDriver::read(Variable variable, Reading& outReading) {
  outReading = Reading{};
  if (variable != Variable::WindDirection) {
    outReading.status = SensorStatus::InvalidData;
    return false;
  }
  float mv = 0.0f;
  if (!adc_.readMillivolts(channel_, mv)) {
    lastStatus_ = SensorStatus::BusError;
    outReading.status = lastStatus_;
    return false;
  }
  const float heading = headingForMillivolts(mv);
  if (heading < 0.0f) {
    // Near the supply rail is an open cable; near ground a short. Neither is a direction.
    lastStatus_ = mv > supplyMv_ * 0.97f ? SensorStatus::NotConnected : SensorStatus::InvalidData;
    outReading.status = lastStatus_;
    return false;
  }
  lastStatus_ = SensorStatus::Ok;
  outReading.ok = true;
  outReading.status = SensorStatus::Ok;
  outReading.value = heading;
  return true;
}

const SensorMetadata& WindVaneDriver::metadata() const { return metadata_; }

// ---------------------------------------------------------------- generic analogue probe

AnalogSensorDriver::AnalogSensorDriver(hal::IAnalogInput& adc, uint8_t channel, const char* id,
                                       const char* model, Variable variable, Mapping mapping,
                                       float a, float b)
    : adc_(adc), channel_(channel), variable_(variable), mapping_(mapping), a_(a), b_(b) {
  copyString(metadata_.id, sizeof(metadata_.id), id);
  copyString(metadata_.model, sizeof(metadata_.model), model);
  metadata_.variables[0] = variable;
  metadata_.variableCount = 1;
}

float AnalogSensorDriver::convert(float millivolts) const {
  if (mapping_ == Mapping::TwoPointPercent) {
    const float span = b_ - a_;
    if (std::fabs(span) < 1.0f) return NAN;  // a calibration with no span cannot map anything
    float percent = (millivolts - a_) / span * 100.0f;
    if (percent < 0.0f) percent = 0.0f;
    if (percent > 100.0f) percent = 100.0f;
    return percent;
  }
  const float value = a_ * millivolts + b_;
  return value < 0.0f ? 0.0f : value;
}

bool AnalogSensorDriver::begin() {
  float mv = 0.0f;
  lastStatus_ = adc_.readMillivolts(channel_, mv) ? SensorStatus::Ok : SensorStatus::BusError;
  return lastStatus_ == SensorStatus::Ok;
}

SensorStatus AnalogSensorDriver::health() { return lastStatus_; }

bool AnalogSensorDriver::read(Variable variable, Reading& outReading) {
  outReading = Reading{};
  if (variable != variable_) {
    outReading.status = SensorStatus::InvalidData;
    return false;
  }
  float mv = 0.0f;
  if (!adc_.readMillivolts(channel_, mv)) {
    lastStatus_ = SensorStatus::BusError;
    outReading.status = lastStatus_;
    return false;
  }
  const float value = convert(mv);
  if (!std::isfinite(value)) {
    lastStatus_ = SensorStatus::InvalidData;
    outReading.status = lastStatus_;
    return false;
  }
  lastStatus_ = SensorStatus::Ok;
  outReading.ok = true;
  outReading.status = SensorStatus::Ok;
  outReading.value = value;
  return true;
}

const SensorMetadata& AnalogSensorDriver::metadata() const { return metadata_; }

}  // namespace cauce::drivers

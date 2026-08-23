#include "cauce/drivers/SimulatedSensorDriver.h"

#include <cmath>

namespace cauce::drivers {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr uint64_t kDayMs = 86400000ULL;
}  // namespace

SimulatedSensorDriver::SimulatedSensorDriver(const char* id,
                                             const SimulationProfile& profile,
                                             uint64_t* sharedUtcMsClock)
    : profile_(profile), clockUtcMs_(sharedUtcMsClock), rngState_(profile.seed) {
  copyString(metadata_.id, sizeof(metadata_.id), id);
  copyString(metadata_.model, sizeof(metadata_.model), "SIM-ENV-A");
  copyString(metadata_.serial, sizeof(metadata_.serial), "SIM0001");
  metadata_.variables[0] = Variable::AirTemperature;
  metadata_.variables[1] = Variable::RelativeHumidity;
  metadata_.variableCount = 2;
  metadata_.resolution = 0.1f;
}

bool SimulatedSensorDriver::begin() {
  begun_ = true;
  return true;
}

SensorStatus SimulatedSensorDriver::health() {
  if (!begun_) return SensorStatus::Initializing;
  if (fault_ == Fault::Disconnect) return SensorStatus::NotConnected;
  if (fault_ == Fault::NotANumber) return SensorStatus::InvalidData;
  return SensorStatus::Ok;
}

float SimulatedSensorDriver::nextNoise() {
  rngState_ = rngState_ * 1664525u + 1013904223u;
  const float unit =
      static_cast<float>(rngState_ >> 8) / static_cast<float>(1u << 24);
  return (unit - 0.5f) * 2.0f * profile_.noiseScale;
}

float SimulatedSensorDriver::temperatureAt(uint64_t utcMs) const {
  const double phase =
      (static_cast<double>(utcMs % kDayMs) / static_cast<double>(kDayMs)) * 2.0 *
      kPi;
  const double diurnal = profile_.diurnalAmplitudeC * std::sin(phase - kPi / 2.0);
  return profile_.baseTemperatureC + static_cast<float>(diurnal);
}

void SimulatedSensorDriver::setUtcMs(uint64_t utcMs) { internalUtcMs_ = utcMs; }

void SimulatedSensorDriver::injectFault(Fault fault) { fault_ = fault; }

bool SimulatedSensorDriver::read(Variable variable, Reading& outReading) {
  outReading.ok = false;
  outReading.status = health();
  if (outReading.status != SensorStatus::Ok) return false;

  const uint64_t now = clockUtcMs_ ? *clockUtcMs_ : internalUtcMs_;

  switch (fault_) {
    case Fault::Frozen:
      if (frozenValue_ == -999.0f) frozenValue_ = temperatureAt(now);
      outReading.ok = true;
      outReading.status = SensorStatus::Ok;
      outReading.value = frozenValue_;
      return true;
    case Fault::OutOfRange:
      outReading.ok = false;
      outReading.status = SensorStatus::InvalidData;
      outReading.value = -999.0f;
      return false;
    default:
      break;
  }

  switch (variable) {
    case Variable::AirTemperature:
      outReading.value = temperatureAt(now) + nextNoise();
      break;
    case Variable::RelativeHumidity: {
      const float t = temperatureAt(now);
      float humidity =
          profile_.humidityBasePct - 2.5f * (t - profile_.baseTemperatureC);
      humidity += nextNoise();
      if (humidity < 0.0f) humidity = 0.0f;
      if (humidity > 100.0f) humidity = 100.0f;
      outReading.value = humidity;
      break;
    }
    default:
      outReading.status = SensorStatus::InvalidData;
      return false;
  }

  if (fault_ == Fault::NotANumber) {
    outReading.ok = true;
    outReading.status = SensorStatus::Ok;
    outReading.value = NAN;
    return true;
  }

  outReading.ok = true;
  outReading.status = SensorStatus::Ok;
  return true;
}

const SensorMetadata& SimulatedSensorDriver::metadata() const {
  return metadata_;
}

}  // namespace cauce::drivers

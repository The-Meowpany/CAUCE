#include "cauce/drivers/HallAnemometerDriver.h"

#include "cauce/core/Types.h"

namespace cauce::drivers {

HallAnemometerDriver::HallAnemometerDriver(hal::IClock& clock, const char* id,
                                           IPulseSource& source, float diameterMetres,
                                           uint8_t pulsesPerRevolution)
    : clock_(clock) {
  copyString(metadata_.id, sizeof(metadata_.id), id);
  copyString(metadata_.model, sizeof(metadata_.model), "HALL-ANEMOMETER");
  metadata_.variables[0] = Variable::WindSpeed;
  metadata_.variableCount = 1;
  metadata_.accuracyNote = 0.0f;  // a cup rotor is not calibrated; the co-location plan is
  metadata_.resolution = 0.1f;    // m/s

  accumulator_.setGeometry(diameterMetres, pulsesPerRevolution);
  source.attachPulseSink(&accumulator_);
}

bool HallAnemometerDriver::begin() {
  // Nothing to probe: the sensor is a magnetic switch closing on a passing magnet. The window
  // opens here on a timestamp rather than waiting for the first pulse, because a node sitting in
  // still air is the ordinary case and it still has to be able to say so.
  accumulator_.beginInterval(clock_.monotonicMs());
  lastStatus_ = SensorStatus::Initializing;
  return true;
}

SensorStatus HallAnemometerDriver::health() {
  if (!accumulator_.ready()) {
    lastStatus_ = SensorStatus::Initializing;
    return lastStatus_;
  }
  lastStatus_ = SensorStatus::Ok;
  return lastStatus_;
}

bool HallAnemometerDriver::read(Variable variable, Reading& outReading) {
  outReading = Reading{};
  if (variable != Variable::WindSpeed) {
    outReading.status = SensorStatus::InvalidData;
    return false;
  }

  const WindPulseAccumulator::Speed speed = accumulator_.takeSpeed(clock_.monotonicMs());
  if (!speed.valid) {
    // Too short an interval to say anything. Distinct from zero wind on purpose: a node that
    // has not been counting long enough has not established that the air is still.
    outReading.status = SensorStatus::Initializing;
    return false;
  }

  outReading.ok = true;
  outReading.status = SensorStatus::Ok;
  outReading.value = speed.metresPerSecond;
  return true;
}

const SensorMetadata& HallAnemometerDriver::metadata() const { return metadata_; }

}  // namespace cauce::drivers
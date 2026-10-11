#pragma once

#include <cstdint>

#include "cauce/drivers/ISensorDriver.h"
#include "cauce/drivers/WindAccumulator.h"
#include "cauce/hal/IClock.h"

namespace cauce::drivers {

// Source for the pulses, so that an ESP32 interrupt handler and a unit test can both feed the
// same counting logic. The driver below does not care which one it has.
struct IPulseSource {
  virtual ~IPulseSource() = default;
  virtual void attachPulseSink(WindPulseAccumulator* sink) = 0;
};

class HallAnemometerDriver final : public ISensorDriver {
 public:
  // A 3-cup rotor on a ~15 cm bracket is the common hobby build; the sweep diameter is the
  // distance across the tip circle, not the width of the rotor.
  static constexpr float kDefaultDiameterMetres = 0.15f;
  static constexpr uint8_t kDefaultPulsesPerRevolution = 3;

  HallAnemometerDriver(hal::IClock& clock, const char* id, IPulseSource& source,
                       float diameterMetres = kDefaultDiameterMetres,
                       uint8_t pulsesPerRevolution = kDefaultPulsesPerRevolution);

  bool begin() override;
  SensorStatus health() override;
  bool read(Variable variable, Reading& outReading) override;
  const SensorMetadata& metadata() const override;

  // Exposed so an interrupt can push an edge straight into the accumulator without going
  // through a full read.
  WindPulseAccumulator& accumulator() { return accumulator_; }

 private:
  hal::IClock& clock_;
  SensorMetadata metadata_{};
  SensorStatus lastStatus_{SensorStatus::Initializing};
  WindPulseAccumulator accumulator_{};
};

}  // namespace cauce::drivers
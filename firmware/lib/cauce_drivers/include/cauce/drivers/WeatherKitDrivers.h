#pragma once

#include <cstdint>

#include "cauce/drivers/ISensorDriver.h"
#include "cauce/hal/IClock.h"
#include "cauce/hal/IPeripherals.h"

namespace cauce::drivers {

// Tipping-bucket rain gauge on a reed switch. Each tip is one fixed depth of rain; the driver
// reports the depth accumulated since the previous read, so a stored value is "rain in this
// sampling interval" and daily totals are a sum on the central.
class RainGaugeDriver final : public ISensorDriver {
 public:
  // A reed switch bounces for a few milliseconds; a real bucket cannot tip twice in 50 ms.
  static constexpr uint32_t kDebounceMs = 50;

  RainGaugeDriver(const char* id, float mmPerTip);

  // Call from the pin interrupt. Only touches two integers.
  void onTip(uint32_t nowMs);

  bool begin() override;
  SensorStatus health() override;
  bool read(Variable variable, Reading& outReading) override;
  const SensorMetadata& metadata() const override;

  uint32_t totalTips() const { return totalTips_; }

 private:
  float mmPerTip_;
  SensorMetadata metadata_{};
  // Only the interrupt writes these, and only the loop writes reportedTips_, so the two sides
  // never read-modify-write the same word and no tip is lost between them.
  volatile uint32_t totalTips_{0};
  volatile uint32_t lastTipMs_{0};
  volatile bool sawTip_{false};
  uint32_t reportedTips_{0};
};

// Resistor-ladder wind vane read through a pull-up divider on an ADC pin. The common kit vane
// closes one or two reed switches per heading, giving sixteen distinct resistances.
class WindVaneDriver final : public ISensorDriver {
 public:
  // A reading further than this from every heading is an open or shorted vane, not a wind.
  static constexpr float kMaxErrorMv = 120.0f;

  WindVaneDriver(hal::IAnalogInput& adc, uint8_t channel, const char* id,
                 float supplyMv = 3300.0f, float pullUpOhms = 10000.0f,
                 float offsetDegrees = 0.0f);

  bool begin() override;
  SensorStatus health() override;
  bool read(Variable variable, Reading& outReading) override;
  const SensorMetadata& metadata() const override;

  // Nearest heading for a divider voltage, or a negative value when none is close enough.
  float headingForMillivolts(float mv) const;

 private:
  hal::IAnalogInput& adc_;
  uint8_t channel_;
  float supplyMv_;
  float pullUpOhms_;
  float offsetDegrees_;
  SensorMetadata metadata_{};
  SensorStatus lastStatus_{SensorStatus::Initializing};
};

// One analogue probe mapped to one variable. Covers the capacitive soil moisture probe, the
// leaf wetness grid, the BPW34 irradiance stage, the gypsum block and the battery divider:
// each is a voltage at a pin and a calibration, and none deserves its own class.
class AnalogSensorDriver final : public ISensorDriver {
 public:
  enum class Mapping : uint8_t {
    // 0 % at `a` mV, 100 % at `b` mV, clamped. Moisture probes read lower when wetter, so
    // `a` > `b` is the normal case and is handled.
    TwoPointPercent = 0,
    // value = a * mV + b, clamped below at zero.
    Linear = 1,
  };

  AnalogSensorDriver(hal::IAnalogInput& adc, uint8_t channel, const char* id, const char* model,
                     Variable variable, Mapping mapping, float a, float b);

  bool begin() override;
  SensorStatus health() override;
  bool read(Variable variable, Reading& outReading) override;
  const SensorMetadata& metadata() const override;

  float convert(float millivolts) const;

 private:
  hal::IAnalogInput& adc_;
  uint8_t channel_;
  Variable variable_;
  Mapping mapping_;
  float a_;
  float b_;
  SensorMetadata metadata_{};
  SensorStatus lastStatus_{SensorStatus::Initializing};
};

}  // namespace cauce::drivers

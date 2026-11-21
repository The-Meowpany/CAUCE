#pragma once

#include <cstdint>

#include "cauce/drivers/ISensorDriver.h"
#include "cauce/hal/II2cBus.h"
#include "cauce/hal/IPeripherals.h"

namespace cauce::drivers {

// Gypsum block read with alternating polarity.
//
// A DC divider pushes current through the block in one direction on every read, which
// polarises it and drifts the reading over a season. Here the two ends of the divider are
// driven by an MCP23008 I2C expander instead of 3V3 and ground, because the node has no free
// GPIO left for them:
//
//   GP0 --[ R_ref ]--+-- GP1 side: [ block ] -- GP1
//                    |
//                  ADC (the pin the DC read used)
//
// Phase A drives GP0 high and GP1 low, which is exactly the old DC circuit. Phase B swaps them.
// Both phases last one ADC conversion, then both pins go low, so the net charge through the
// block is zero. The two readings are folded back into the millivolts the DC circuit would have
// shown, so a per-soil fit made against the DC read still applies unchanged.
class GypsumAcDriver final : public ISensorDriver {
 public:
  static constexpr uint8_t kDefaultAddress = 0x20;  // A2..A0 tied to ground
  static constexpr uint8_t kRegIodir = 0x00;
  static constexpr uint8_t kRegOlat = 0x0A;
  static constexpr uint8_t kPinA = 0x01;  // GP0, reference-resistor end
  static constexpr uint8_t kPinB = 0x02;  // GP1, far end of the block

  GypsumAcDriver(hal::II2cBus& bus, hal::IAnalogInput& adc, uint8_t adcChannel, const char* id,
                 float kpaPerMv, float offsetKpa, float supplyMv = 3300.0f,
                 uint8_t address = kDefaultAddress);

  bool begin() override;
  SensorStatus health() override;
  bool read(Variable variable, Reading& outReading) override;
  const SensorMetadata& metadata() const override;

  // The DC-equivalent millivolts of the last good read, for diagnostics and tests.
  float lastEquivalentMv() const { return lastMv_; }

 private:
  bool drive(uint8_t pins);
  bool phase(uint8_t pins, float& outMv);

  hal::II2cBus& bus_;
  hal::IAnalogInput& adc_;
  uint8_t channel_;
  float kpaPerMv_;
  float offsetKpa_;
  float supplyMv_;
  uint8_t address_;
  float lastMv_{0.0f};
  SensorMetadata metadata_{};
  SensorStatus lastStatus_{SensorStatus::Initializing};
};

}  // namespace cauce::drivers

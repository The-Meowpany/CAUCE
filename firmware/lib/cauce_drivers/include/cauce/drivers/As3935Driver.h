#pragma once

#include <cstdint>

#include "cauce/drivers/ISensorDriver.h"
#include "cauce/hal/II2cBus.h"

namespace cauce::drivers {

// AS3935 lightning detector over I2C. Strikes are events, not samples: the chip raises its IRQ
// pin, the interrupt sets a flag, and `service()` - called from the main loop, never from the
// interrupt - reads the cause register and counts the strike. `read()` then reports the count
// since the previous read and the distance to the nearest strike in that interval.
class As3935Driver final : public ISensorDriver {
 public:
  static constexpr uint8_t kDefaultAddress = 0x03;
  // Distance code for "storm out of range" in the chip's estimator.
  static constexpr uint8_t kOutOfRange = 0x3F;

  As3935Driver(hal::II2cBus& bus, const char* id, bool indoor,
               uint8_t address = kDefaultAddress);

  bool begin() override;
  SensorStatus health() override;
  bool read(Variable variable, Reading& outReading) override;
  const SensorMetadata& metadata() const override;

  // Safe to call from an ISR: only sets a flag.
  void onInterrupt() { pending_ = true; }
  // Reads the interrupt source when one is pending. Call from the loop.
  void service();

  uint32_t disturberCount() const { return disturbers_; }
  uint32_t noiseCount() const { return noiseEvents_; }

 private:
  bool writeRegister(uint8_t reg, uint8_t value);
  bool readRegister(uint8_t reg, uint8_t& value);

  hal::II2cBus& bus_;
  uint8_t address_;
  bool indoor_;
  SensorMetadata metadata_{};
  SensorStatus lastStatus_{SensorStatus::Initializing};
  bool configured_{false};
  volatile bool pending_{false};
  uint32_t strikes_{0};
  uint8_t nearestKm_{kOutOfRange};
  uint32_t disturbers_{0};
  uint32_t noiseEvents_{0};
  // Both variables come from one interval; the distance read closes it.
  bool strikesTaken_{false};
};

}  // namespace cauce::drivers

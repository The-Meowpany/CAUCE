#pragma once

#include <cstdint>

#include "cauce/drivers/ISensorDriver.h"
#include "cauce/hal/IClock.h"
#include "cauce/hal/II2cBus.h"

namespace cauce::drivers {

// Ambient light. The `illuminance` channel has been in the data model since the beginning with
// no driver behind it, so this fills a hole rather than widening the schema.
class Bh1750Driver final : public ISensorDriver {
 public:
  static constexpr uint8_t kDefaultAddress = 0x23;
  static constexpr uint8_t kAlternateAddress = 0x5C;

  enum class Mode : uint8_t {
    HighResolution = 0x20,   // 1 lx,   ~120 ms
    HighResMode2 = 0x21,     // 0.5 lx, ~120 ms
    LowResolution = 0x10,    // 4 lx,   ~16 ms
  };

  Bh1750Driver(hal::II2cBus& bus, hal::IClock& clock, const char* id,
               uint8_t i2cAddress = kDefaultAddress);

  bool begin() override;
  SensorStatus health() override;
  bool read(Variable variable, Reading& outReading) override;
  const SensorMetadata& metadata() const override;

 private:
  enum class Register : uint8_t {
    PowerOn = 0x01,
    Reset = 0x07,
    ContinuousHighRes = 0x10,
    ContinuousHighResMode2 = 0x11,
    ContinuousLowRes = 0x13,
    OneTimeHighRes = 0x20,
    OneTimeHighResMode2 = 0x21,
    OneTimeLowRes = 0x23,
  };

  bool configure();
  bool readRaw(uint16_t& outRaw);

  hal::II2cBus& bus_;
  hal::IClock& clock_;
  uint8_t address_;
  SensorMetadata metadata_{};
  SensorStatus lastStatus_{SensorStatus::Initializing};
  bool configured_{false};
  float lastLux_{0.0f};
};

}  // namespace cauce::drivers
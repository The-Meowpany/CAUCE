#pragma once

#include <cstdint>

#include "cauce/drivers/Bme280Math.h"
#include "cauce/drivers/ISensorDriver.h"
#include "cauce/hal/IClock.h"
#include "cauce/hal/II2cBus.h"

namespace cauce::drivers {

class Bme280Driver final : public ISensorDriver {
 public:
  static constexpr uint8_t kDefaultAddress = 0x76;

  Bme280Driver(hal::II2cBus& bus, hal::IClock& clock, const char* id,
               uint8_t i2cAddress = kDefaultAddress);

  bool begin() override;
  SensorStatus health() override;
  bool read(Variable variable, Reading& outReading) override;
  const SensorMetadata& metadata() const override;

 private:
  enum class Register : uint8_t {
    ChipId = 0xD0,
    Reset = 0xE0,
    CtrlHum = 0xF2,
    Status = 0xF3,
    CtrlMeas = 0xF4,
    Config = 0xF5,
    PressMsb = 0xF7,
  };

  bool readTrimming();
  bool triggerConversion();
  bool waitForConversion();
  bool burstRead(Bme280Raw& raw);

  hal::II2cBus& bus_;
  hal::IClock& clock_;
  uint8_t address_;
  Bme280Trim trim_{};
  SensorMetadata metadata_{};
  SensorStatus lastStatus_{SensorStatus::Initializing};
};

}  // namespace cauce::drivers

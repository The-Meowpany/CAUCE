#pragma once

#include <cstdint>

#include "cauce/drivers/ISensorDriver.h"
#include "cauce/hal/IPeripherals.h"

namespace cauce::drivers {

// DS18B20 waterproof probe on a 1-Wire bus with a single device, used for soil temperature.
//
// A 12-bit conversion takes 750 ms. Instead of blocking the scheduler for it, each read
// collects the conversion started by the previous read and immediately starts the next one, so
// the value is at most one sampling interval old and the loop never stalls.
class Ds18b20Driver final : public ISensorDriver {
 public:
  Ds18b20Driver(hal::IOneWire& bus, const char* id, Variable variable = Variable::SoilTemperature);

  bool begin() override;
  SensorStatus health() override;
  bool read(Variable variable, Reading& outReading) override;
  const SensorMetadata& metadata() const override;

  // Dallas/Maxim CRC-8 (reflected polynomial 0x8C) over the scratchpad.
  static uint8_t crc8(const uint8_t* data, uint8_t length);

 private:
  bool startConversion();

  hal::IOneWire& bus_;
  Variable variable_;
  SensorMetadata metadata_{};
  SensorStatus lastStatus_{SensorStatus::Initializing};
  bool conversionPending_{false};
};

}  // namespace cauce::drivers

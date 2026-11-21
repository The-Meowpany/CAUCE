#pragma once

#include <cstdint>

#include "cauce/drivers/ISensorDriver.h"
#include "cauce/hal/II2cBus.h"

namespace cauce::drivers {

// Vishay VEML6075 UVA/UVB sensor. Reports the UV index and its UVA and UVB contributions
// separately, which is the reason for this part: the LTR-390 gives only a single UV channel.
//
// The conversion follows Vishay's application note for an open-air sensor at 100 ms
// integration. Behind a cover window the coefficients change and must be refitted.
class Veml6075Driver final : public ISensorDriver {
 public:
  static constexpr uint8_t kAddress = 0x10;
  static constexpr uint16_t kDeviceId = 0x0026;

  struct Raw {
    uint16_t uva{0};
    uint16_t uvb{0};
    uint16_t comp1{0};
    uint16_t comp2{0};
  };
  struct Index {
    float uva{0.0f};
    float uvb{0.0f};
    float uvi{0.0f};
  };
  // Pure conversion, exposed so it is testable against the application note's numbers.
  static Index convert(const Raw& raw);

  Veml6075Driver(hal::II2cBus& bus, const char* id);

  bool begin() override;
  SensorStatus health() override;
  bool read(Variable variable, Reading& outReading) override;
  const SensorMetadata& metadata() const override;

 private:
  bool readWord(uint8_t reg, uint16_t& out);

  hal::II2cBus& bus_;
  SensorMetadata metadata_{};
  SensorStatus lastStatus_{SensorStatus::Initializing};
  bool configured_{false};
};

}  // namespace cauce::drivers

#pragma once

#include <cstdint>

#include "cauce/drivers/ISensorDriver.h"
#include "cauce/hal/II2cBus.h"

namespace cauce::drivers {

// Lite-On LTR-390UV-01 in UV mode. A single UV channel, so it reports the UV index and not
// the separate UVA and UVB contributions the VEML6075 gives, at a quarter of the price and
// still in production.
//
// Configured for gain 18 and 20-bit resolution, the setting the datasheet's UV sensitivity
// (2300 counts per UV index) is specified at. Behind a cover window the sensitivity changes
// and must be refitted.
class Ltr390Driver final : public ISensorDriver {
 public:
  static constexpr uint8_t kAddress = 0x53;
  static constexpr uint8_t kRegMainCtrl = 0x00;
  static constexpr uint8_t kRegMeasRate = 0x04;
  static constexpr uint8_t kRegGain = 0x05;
  static constexpr uint8_t kRegPartId = 0x06;
  static constexpr uint8_t kRegMainStatus = 0x07;
  static constexpr uint8_t kRegUvsData = 0x10;
  static constexpr uint8_t kPartIdHigh = 0xB0;        // upper nibble of PART_ID
  static constexpr uint8_t kMainCtrlUvsEnable = 0x0A;  // LS_EN | UVS_MODE
  static constexpr uint8_t kGain18 = 0x04;
  static constexpr uint8_t kMeasRate20Bit500Ms = 0x04;
  static constexpr uint8_t kDataReady = 0x08;
  static constexpr float kCountsPerUvIndex = 2300.0f;

  // Pure conversion from the 20-bit UVS count, exposed for tests.
  static float uvIndex(uint32_t counts);

  Ltr390Driver(hal::II2cBus& bus, const char* id);

  bool begin() override;
  SensorStatus health() override;
  bool read(Variable variable, Reading& outReading) override;
  const SensorMetadata& metadata() const override;

 private:
  bool writeRegister(uint8_t reg, uint8_t value);

  hal::II2cBus& bus_;
  SensorMetadata metadata_{};
  SensorStatus lastStatus_{SensorStatus::Initializing};
};

}  // namespace cauce::drivers

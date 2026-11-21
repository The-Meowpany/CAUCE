#pragma once

#include <cstdint>

#include "cauce/drivers/ISensorDriver.h"
#include "cauce/hal/IClock.h"
#include "cauce/hal/IPeripherals.h"

namespace cauce::drivers {

// Winsen MH-Z19C NDIR CO2 sensor over UART at 9600 baud.
//
// Automatic baseline correction is switched off at begin(). ABC assumes the sensor sees
// ~400 ppm fresh air at least once a day and recalibrates to it; that holds outdoors most
// days, but it silently shifts the baseline the co-location procedure is trying to measure.
// Calibration is a deliberate act in CAUCE, not a background one.
class Mhz19Driver final : public ISensorDriver {
 public:
  static constexpr uint32_t kBaud = 9600;
  // The datasheet's preheat time; readings before it are the lamp warming up.
  static constexpr uint32_t kPreheatMs = 180000;
  static constexpr uint32_t kReplyTimeoutMs = 200;

  Mhz19Driver(hal::IUart& uart, hal::IClock& clock, const char* id);

  bool begin() override;
  SensorStatus health() override;
  bool read(Variable variable, Reading& outReading) override;
  const SensorMetadata& metadata() const override;

  // Checksum over bytes 1..7 of a 9-byte frame: negated sum plus one.
  static uint8_t checksum(const uint8_t* frame);

 private:
  bool transact(uint8_t command, const uint8_t* args, uint8_t reply[9], bool expectReply);

  hal::IUart& uart_;
  hal::IClock& clock_;
  SensorMetadata metadata_{};
  SensorStatus lastStatus_{SensorStatus::Initializing};
  bool started_{false};
  uint32_t startedAtMs_{0};
};

}  // namespace cauce::drivers

#pragma once

#include <cstddef>
#include <cstdint>

#include "cauce/drivers/ISensorDriver.h"
#include "cauce/hal/IClock.h"
#include "cauce/hal/IPeripherals.h"

namespace cauce::drivers {

// Plantower PMS5003 / PMS7003 particulate sensor over UART at 9600 baud, in its power-on
// active mode: it streams a 32-byte frame roughly every second and needs no commands, so only
// its TX line is wired.
//
// A sixth of the price of an SPS30, and it reports PM1, PM2.5 and PM10 but not PM4. The
// "atmospheric environment" values are used rather than the CF=1 ones, which are calibrated
// for factory air and read high outdoors.
class Pms5003Driver final : public ISensorDriver {
 public:
  static constexpr uint32_t kBaud = 9600;
  static constexpr size_t kFrameBytes = 32;
  // The fan needs this long after power-up before the readings settle.
  static constexpr uint32_t kWarmupMs = 30000;
  // One frame answers all three variables of one measurement cycle.
  static constexpr uint32_t kFrameReuseMs = 2000;
  static constexpr uint32_t kReadTimeoutMs = 2500;

  struct Frame {
    uint16_t pm1{0};
    uint16_t pm25{0};
    uint16_t pm10{0};
  };
  // Finds the last complete, checksum-valid frame in `data`. Exposed for tests.
  static bool parse(const uint8_t* data, size_t length, Frame& out);

  Pms5003Driver(hal::IUart& uart, hal::IClock& clock, const char* id);

  bool begin() override;
  SensorStatus health() override;
  bool read(Variable variable, Reading& outReading) override;
  const SensorMetadata& metadata() const override;

 private:
  bool refresh();

  hal::IUart& uart_;
  hal::IClock& clock_;
  SensorMetadata metadata_{};
  SensorStatus lastStatus_{SensorStatus::Initializing};
  uint32_t startedAtMs_{0};
  bool haveFrame_{false};
  uint32_t frameAtMs_{0};
  Frame frame_{};
};

}  // namespace cauce::drivers

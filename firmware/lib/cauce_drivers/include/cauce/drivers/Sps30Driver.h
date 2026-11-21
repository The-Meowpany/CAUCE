#pragma once

#include <cstdint>

#include "cauce/drivers/ISensorDriver.h"
#include "cauce/hal/IClock.h"
#include "cauce/hal/II2cBus.h"

namespace cauce::drivers {

// Sensirion SPS30 particulate matter sensor over I2C. Reports mass concentration for PM1.0,
// PM2.5, PM4 and PM10 - the reason this part was chosen over cheaper optical counters is PM4,
// which they do not measure directly.
//
// One measurement frame carries all four values, so the first variable read in a cycle
// fetches the frame and the other three reuse it.
class Sps30Driver final : public ISensorDriver {
 public:
  static constexpr uint8_t kAddress = 0x69;

  Sps30Driver(hal::II2cBus& bus, hal::IClock& clock, const char* id);

  bool begin() override;
  SensorStatus health() override;
  bool read(Variable variable, Reading& outReading) override;
  const SensorMetadata& metadata() const override;

  // Sensirion's CRC-8 (polynomial 0x31, init 0xFF) over each 16-bit word.
  static uint8_t crc8(const uint8_t* data, uint8_t length);

  // The fan needs this long after start before the first frame is meaningful.
  static constexpr uint32_t kWarmupMs = 30000;
  // A frame younger than this is reused rather than read again.
  static constexpr uint32_t kFrameReuseMs = 2000;

 private:
  bool sendCommand(uint16_t command, const uint8_t* args = nullptr, uint8_t argWords = 0);
  bool readWords(uint16_t command, uint8_t* out, uint8_t words);
  bool fetchFrame();

  hal::II2cBus& bus_;
  hal::IClock& clock_;
  SensorMetadata metadata_{};
  SensorStatus lastStatus_{SensorStatus::Initializing};
  bool started_{false};
  uint32_t startedAtMs_{0};
  bool haveFrame_{false};
  uint32_t frameAtMs_{0};
  float mass_[4]{};
};

}  // namespace cauce::drivers

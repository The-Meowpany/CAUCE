#pragma once

#include <cstdint>

#include "cauce/drivers/ISensorDriver.h"
#include "cauce/hal/IClock.h"
#include "cauce/hal/II2cBus.h"

namespace cauce::drivers {

// Total VOC, and deliberately not CO2.
//
// The CCS811 does not measure carbon dioxide. It estimates an equivalent CO2 figure from the
// response of a metal-oxide sensor to volatile organic compounds, and it publishes that number
// as `eCO2` alongside `TVOC`. Treating the estimate as a measurement would put a value in the
// network that cannot be calibrated, drifts with humidity and temperature, reads high next to
// any vegetation or solvent, and moves during its own warm-up - which is exactly the kind of
// number a project that leads with "comparable data with the uncertainty shown" must not
// publish without saying so.
//
// So this driver reads TVOC and refuses the CO2 axis entirely. If a real CO2 number is wanted
// it needs an NDIR sensor - SCD4x or MH-Z19 - and that is a different part with a different
// calibration story, not a register on this one.
//
// The sensor is not ignored either: VOC is the quantity it actually senses, and a node that
// reports it honestly is more useful than one that reports a derived CO2 number nobody can
// defend.
class Ccs811Driver final : public ISensorDriver {
 public:
  static constexpr uint8_t kDefaultAddress = 0x5A;
  static constexpr uint8_t kAlternateAddress = 0x5B;

  // The part needs roughly ten minutes of continuous operation before its output means anything
  // stable. Readings taken before that are returned as Initializing rather than as data, so a
  // freshly booted node reports a warming sensor instead of a confident wrong number.
  static constexpr uint32_t kWarmupMs = 600000;

  Ccs811Driver(hal::II2cBus& bus, hal::IClock& clock, const char* id,
               uint8_t i2cAddress = kDefaultAddress);

  bool begin() override;
  SensorStatus health() override;
  bool read(Variable variable, Reading& outReading) override;
  const SensorMetadata& metadata() const override;

  bool warmedUp() const;

 private:
  enum class Register : uint8_t {
    Status = 0x00,
    MeasureMode = 0x01,
    AlgorithmResult = 0x02,
    RawData = 0x03,
    EnvData = 0x05,
    Nvm = 0x06,
    FeatureId = 0x07,
    ErrorId = 0xE0,
  };

  bool writeMode(uint8_t mode);
  bool readStatus(uint8_t& outStatus);
  bool readAlgorithmResult(uint16_t& outTvoc, uint16_t& outEco2);

  hal::II2cBus& bus_;
  hal::IClock& clock_;
  uint8_t address_;
  SensorMetadata metadata_{};
  SensorStatus lastStatus_{SensorStatus::Initializing};
  uint32_t startedMs_{0};
  bool configured_{false};
};

}  // namespace cauce::drivers
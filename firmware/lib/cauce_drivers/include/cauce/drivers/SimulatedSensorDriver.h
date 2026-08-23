#pragma once

#include <cstdint>

#include "cauce/drivers/ISensorDriver.h"

namespace cauce::drivers {

struct SimulationProfile {
  float baseTemperatureC{18.0f};
  float diurnalAmplitudeC{8.0f};
  float humidityBasePct{65.0f};
  float noiseScale{0.15f};
  uint32_t seed{42};
};

class SimulatedSensorDriver final : public ISensorDriver {
 public:
  explicit SimulatedSensorDriver(const char* id, const SimulationProfile& profile,
                                 uint64_t* sharedUtcMsClock);

  bool begin() override;
  SensorStatus health() override;
  bool read(Variable variable, Reading& outReading) override;
  const SensorMetadata& metadata() const override;

  void setUtcMs(uint64_t utcMs);
  enum class Fault : uint8_t { None = 0, Disconnect = 1, NotANumber = 2, OutOfRange = 3, Frozen = 4 };
  void injectFault(Fault fault);

 private:
  float nextNoise();
  float temperatureAt(uint64_t utcMs) const;

  SensorMetadata metadata_{};
  SimulationProfile profile_;
  uint64_t* clockUtcMs_;
  uint64_t internalUtcMs_{0};
  uint32_t rngState_;
  Fault fault_{Fault::None};
  float frozenValue_{-999.0f};
  bool begun_{false};
};

}  // namespace cauce::drivers

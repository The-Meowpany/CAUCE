#pragma once

#include <cstddef>
#include <cstdint>

#include "cauce/core/Types.h"

namespace cauce::drivers {

enum class SensorStatus : uint8_t {
  Ok = 0,
  Initializing = 1,
  NotConnected = 2,
  BusError = 3,
  InvalidData = 4,
};

struct SensorMetadata {
  char id[24]{};
  char model[24]{};
  char serial[16]{};
  Variable variables[4]{Variable::Unknown, Variable::Unknown, Variable::Unknown,
                        Variable::Unknown};
  uint8_t variableCount{0};
  float resolution{0.0f};
  float accuracyNote{0.0f};
};

struct Reading {
  bool ok{false};
  SensorStatus status{SensorStatus::NotConnected};
  float value{0.0f};
};

class ISensorDriver {
 public:
  virtual ~ISensorDriver() = default;
  virtual bool begin() = 0;
  virtual SensorStatus health() = 0;
  virtual bool read(Variable variable, Reading& outReading) = 0;
  virtual const SensorMetadata& metadata() const = 0;
};

}  // namespace cauce::drivers

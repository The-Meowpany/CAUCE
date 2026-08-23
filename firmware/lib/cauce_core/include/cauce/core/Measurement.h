#pragma once

#include <cstdint>

#include "cauce/core/Types.h"

namespace cauce {

struct Measurement {
  char nodeId[16]{};
  char sensorId[24]{};
  uint32_t sequence{};
  uint64_t timestampUtcMs{};
  float value{0.0f};
  Variable variable{Variable::Unknown};
  Quality quality{Quality::Uncalibrated};
  uint8_t reasonBits{0};
  bool timeUncertain{true};

  const char* unit() const { return variableUnit(variable); }
};

static_assert(sizeof(Measurement) <= 72, "Measurement must stay compact for ESP32 RAM");

}  // namespace cauce

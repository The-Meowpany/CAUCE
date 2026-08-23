#pragma once

#include <cstdint>

#include "cauce/core/Measurement.h"
#include "cauce/core/Types.h"

namespace cauce {

struct SampleStats {
  uint32_t count{0};
  float minValue{0.0f};
  float maxValue{0.0f};
  float mean{0.0f};
  float median{0.0f};
  float stdDev{0.0f};
  float p05{0.0f};
  float p25{0.0f};
  float p75{0.0f};
  float p95{0.0f};
};

bool computeSampleStats(const float* values, uint32_t count, SampleStats& out,
                        float* scratch, uint32_t scratchCapacity);

uint32_t extractVariableValues(const Measurement* measurements, uint32_t count,
                               Variable variable, float* out,
                               uint32_t capacity);

struct AggregateBucket {
  uint64_t startMs{0};
  float min{0.0f};
  float max{0.0f};
  float mean{0.0f};
  uint32_t count{0};
};

uint32_t aggregateBuckets(const Measurement* measurements, uint32_t count,
                          Variable variable, uint32_t windowSeconds,
                          AggregateBucket* out, uint32_t capacity);

double exposureHoursAbove(const Measurement* temperatures, uint32_t count,
                          float thresholdC);

}  // namespace cauce

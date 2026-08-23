#include "cauce/core/Metrics.h"

#include <cmath>
#include <cstring>

namespace cauce {
namespace {

void insertionSort(float* values, uint32_t count) {
  for (uint32_t i = 1; i < count; ++i) {
    const float key = values[i];
    int32_t j = static_cast<int32_t>(i) - 1;
    while (j >= 0 && values[j] > key) {
      values[j + 1] = values[j];
      --j;
    }
    values[j + 1] = key;
  }
}

float percentileFromSorted(const float* sorted, uint32_t count, double p) {
  if (count == 0) return NAN;
  if (count == 1) return sorted[0];
  const double position = p * static_cast<double>(count - 1);
  const uint32_t lower = static_cast<uint32_t>(position);
  const uint32_t upper =
      lower + 1 < count ? lower + 1 : count - 1;
  const double fraction = position - static_cast<double>(lower);
  return static_cast<float>(sorted[lower] * (1.0 - fraction) +
                            sorted[upper] * fraction);
}

}  // namespace

uint32_t extractVariableValues(const Measurement* measurements, uint32_t count,
                               Variable variable, float* out,
                               uint32_t capacity) {
  uint32_t extracted = 0;
  for (uint32_t i = 0; i < count && extracted < capacity; ++i) {
    if (measurements[i].variable != variable) continue;
    const Quality q = measurements[i].quality;
    if (q == Quality::Invalid || q == Quality::Missing ||
        q == Quality::Estimated) {
      continue;
    }
    if (std::isnan(measurements[i].value)) continue;
    out[extracted++] = measurements[i].value;
  }
  return extracted;
}

bool computeSampleStats(const float* values, uint32_t count, SampleStats& out,
                        float* scratch, uint32_t scratchCapacity) {
  out = SampleStats{};
  if (!values || !scratch || count == 0 || scratchCapacity < count) return false;

  std::memcpy(scratch, values, sizeof(float) * count);
  insertionSort(scratch, count);

  double sum = 0.0;
  for (uint32_t i = 0; i < count; ++i) sum += scratch[i];
  const double mean = sum / count;

  double varianceSum = 0.0;
  for (uint32_t i = 0; i < count; ++i) {
    const double delta = scratch[i] - mean;
    varianceSum += delta * delta;
  }
  const double variance =
      count >= 2 ? varianceSum / (count - 1) : 0.0;

  out.count = count;
  out.minValue = scratch[0];
  out.maxValue = scratch[count - 1];
  out.mean = static_cast<float>(mean);
  out.median = percentileFromSorted(scratch, count, 0.5);
  out.stdDev = static_cast<float>(std::sqrt(variance));
  out.p05 = percentileFromSorted(scratch, count, 0.05);
  out.p25 = percentileFromSorted(scratch, count, 0.25);
  out.p75 = percentileFromSorted(scratch, count, 0.75);
  out.p95 = percentileFromSorted(scratch, count, 0.95);
  return true;
}

uint32_t aggregateBuckets(const Measurement* measurements, uint32_t count,
                          Variable variable, uint32_t windowSeconds,
                          AggregateBucket* out, uint32_t capacity) {
  if (!measurements || !out || capacity == 0 || windowSeconds == 0) return 0;
  const uint64_t windowMs = static_cast<uint64_t>(windowSeconds) * 1000ULL;
  uint32_t bucketCount = 0;
  bool open = false;
  uint64_t currentStart = 0;
  double sum = 0.0;
  float minV = 0.0f;
  float maxV = 0.0f;
  uint32_t n = 0;

  auto closeBucket = [&]() {
    if (!open || n == 0 || bucketCount >= capacity) return;
    AggregateBucket& b = out[bucketCount++];
    b.startMs = currentStart;
    b.min = minV;
    b.max = maxV;
    b.mean = static_cast<float>(sum / n);
    b.count = n;
    open = false;
  };

  for (uint32_t i = 0; i < count; ++i) {
    const Measurement& m = measurements[i];
    if (m.variable != variable || m.timestampUtcMs == 0) continue;
    if (std::isnan(m.value)) continue;
    const uint64_t start = (m.timestampUtcMs / windowMs) * windowMs;
    if (!open || start != currentStart) {
      closeBucket();
      if (bucketCount >= capacity) return bucketCount;
      currentStart = start;
      sum = 0.0;
      minV = maxV = m.value;
      n = 0;
      open = true;
    }
    sum += m.value;
    if (m.value < minV) minV = m.value;
    if (m.value > maxV) maxV = m.value;
    ++n;
  }
  closeBucket();
  return bucketCount;
}

double exposureHoursAbove(const Measurement* temperatures, uint32_t count,
                          float thresholdC) {
  if (!temperatures || count < 2) return 0.0;
  double totalMs = 0.0;
  bool prevAbove = false;
  uint64_t prevTs = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const Measurement& m = temperatures[i];
    if (m.variable != Variable::AirTemperature) continue;
    if (std::isnan(m.value)) continue;
    const bool above = m.value >= thresholdC;
    if (prevAbove && above && m.timestampUtcMs > prevTs) {
      totalMs += static_cast<double>(m.timestampUtcMs - prevTs);
    }
    prevAbove = above;
    prevTs = m.timestampUtcMs;
  }
  return totalMs / 3600000.0;
}

}  // namespace cauce

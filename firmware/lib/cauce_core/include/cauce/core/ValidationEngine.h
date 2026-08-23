#pragma once

#include <cstdint>

#include "cauce/core/Measurement.h"
#include "cauce/core/Types.h"

namespace cauce {

enum ReasonBits : uint8_t {
  kReasonNone = 0,
  kReasonNonFinite = 1 << 0,
  kReasonOutOfRange = 1 << 1,
  kReasonRateOfChange = 1 << 2,
  kReasonStuckValue = 1 << 3,
  kReasonTimeUncertain = 1 << 4,
  kReasonDuplicateSequence = 1 << 5,
  kReasonSensorUnhealthy = 1 << 6,
  kReasonUnknownVariable = 1 << 7,
};

struct Thresholds {
  float rangeMin[kVariableCount];
  float rangeMax[kVariableCount];
  float maxRisePerMinute[kVariableCount];
  float maxDropPerMinute[kVariableCount];
  uint32_t stuckCountLimit;
  float stuckEpsilon;
  int64_t maxFutureSkewMs;

  float minFor(Variable v) const {
    return static_cast<uint8_t>(v) < kVariableCount
               ? rangeMin[static_cast<uint8_t>(v)]
               : -3.4e38f;
  }
  float maxFor(Variable v) const {
    return static_cast<uint8_t>(v) < kVariableCount
               ? rangeMax[static_cast<uint8_t>(v)]
               : 3.4e38f;
  }
};

Thresholds defaultThresholds();

struct ValidationContext {
  bool hasPreviousValue{false};
  float previousValue{0.0f};
  uint64_t previousTimestampMs{0};
  uint32_t identicalStreak{0};
  float streakValue{0.0f};
  bool timeValid{false};
  uint32_t lastSeenSequence{0};
  bool hasLastSequence{false};
};

struct ValidationResult {
  Quality quality{Quality::Suspect};
  uint8_t reasonBits{kReasonNone};
  bool timeUncertain{true};
  uint32_t newIdenticalStreak{0};
};

class ValidationEngine {
 public:
  explicit ValidationEngine(const Thresholds& thresholds);

  ValidationResult evaluate(const Measurement& candidate,
                            ValidationContext& context) const;

 private:
  Thresholds thresholds_;
};

}  // namespace cauce

#include "cauce/core/ValidationEngine.h"

#include <cmath>

namespace cauce {

Thresholds defaultThresholds() {
  Thresholds t{};
  for (uint8_t i = 0; i < kVariableCount; ++i) {
    t.rangeMin[i] = -3.4e38f;
    t.rangeMax[i] = 3.4e38f;
    t.maxRisePerMinute[i] = 3.4e38f;
    t.maxDropPerMinute[i] = 3.4e38f;
  }
  t.rangeMin[static_cast<uint8_t>(Variable::AirTemperature)] = -40.0f;
  t.rangeMax[static_cast<uint8_t>(Variable::AirTemperature)] = 85.0f;
  t.maxRisePerMinute[static_cast<uint8_t>(Variable::AirTemperature)] = 5.0f;
  t.maxDropPerMinute[static_cast<uint8_t>(Variable::AirTemperature)] = 5.0f;

  t.rangeMin[static_cast<uint8_t>(Variable::RelativeHumidity)] = 0.0f;
  t.rangeMax[static_cast<uint8_t>(Variable::RelativeHumidity)] = 100.0f;
  t.maxRisePerMinute[static_cast<uint8_t>(Variable::RelativeHumidity)] = 20.0f;
  t.maxDropPerMinute[static_cast<uint8_t>(Variable::RelativeHumidity)] = 20.0f;

  t.rangeMin[static_cast<uint8_t>(Variable::Pressure)] = 300.0f;
  t.rangeMax[static_cast<uint8_t>(Variable::Pressure)] = 1100.0f;
  t.maxRisePerMinute[static_cast<uint8_t>(Variable::Pressure)] = 2.0f;
  t.maxDropPerMinute[static_cast<uint8_t>(Variable::Pressure)] = 2.0f;

  t.rangeMin[static_cast<uint8_t>(Variable::Light)] = 0.0f;
  t.rangeMax[static_cast<uint8_t>(Variable::Light)] = 200000.0f;
  t.maxRisePerMinute[static_cast<uint8_t>(Variable::Light)] = 120000.0f;
  t.maxDropPerMinute[static_cast<uint8_t>(Variable::Light)] = 120000.0f;

  t.rangeMin[static_cast<uint8_t>(Variable::BatteryVoltage)] = 2.5f;
  t.rangeMax[static_cast<uint8_t>(Variable::BatteryVoltage)] = 4.5f;
  t.maxRisePerMinute[static_cast<uint8_t>(Variable::BatteryVoltage)] = 0.2f;
  t.maxDropPerMinute[static_cast<uint8_t>(Variable::BatteryVoltage)] = 0.2f;

  t.stuckCountLimit = 6;
  t.stuckEpsilon = 0.01f;
  t.maxFutureSkewMs = 120000;
  return t;
}

ValidationEngine::ValidationEngine(const Thresholds& thresholds)
    : thresholds_(thresholds) {}

ValidationResult ValidationEngine::evaluate(const Measurement& candidate,
                                            ValidationContext& context) const {
  ValidationResult result;
  result.timeUncertain =
      candidate.timeUncertain || !context.timeValid || candidate.timestampUtcMs == 0;
  if (context.timeValid && context.nowUtcMs != 0 &&
      candidate.timestampUtcMs >
          context.nowUtcMs + static_cast<uint64_t>(thresholds_.maxFutureSkewMs)) {
    result.timeUncertain = true;
  }
  uint8_t bits = candidate.reasonBits;
  Quality quality = Quality::Valid;

  if (result.timeUncertain) bits |= kReasonTimeUncertain;

  if (candidate.variable == Variable::Unknown) {
    bits |= kReasonUnknownVariable;
    quality = Quality::Invalid;
  }

  if (std::isnan(candidate.value) || std::isinf(candidate.value)) {
    bits |= kReasonNonFinite;
    quality = Quality::Invalid;
  } else if (quality != Quality::Invalid &&
             (candidate.value < thresholds_.minFor(candidate.variable) ||
              candidate.value > thresholds_.maxFor(candidate.variable))) {
    bits |= kReasonOutOfRange;
    quality = Quality::Invalid;
  }

  if (context.hasLastSequence && candidate.sequence != 0 &&
      candidate.sequence <= context.lastSeenSequence) {
    bits |= kReasonDuplicateSequence;
    quality = Quality::Invalid;
  }

  bool softViolation = false;

  if (!result.timeUncertain && context.hasPreviousValue &&
      candidate.timestampUtcMs > context.previousTimestampMs) {
    const double deltaMinutes =
        static_cast<double>(candidate.timestampUtcMs - context.previousTimestampMs) /
        60000.0;
    if (deltaMinutes > 0.0) {
      const float rate =
          static_cast<float>((candidate.value - context.previousValue) / deltaMinutes);
      const uint8_t varIdx = static_cast<uint8_t>(candidate.variable);
      if (rate > thresholds_.maxRisePerMinute[varIdx] ||
          rate < -thresholds_.maxDropPerMinute[varIdx]) {
        bits |= kReasonRateOfChange;
        softViolation = true;
      }
    }
  }

  if (std::fabs(candidate.value - context.streakValue) <= thresholds_.stuckEpsilon) {
    result.newIdenticalStreak = context.identicalStreak + 1;
  } else {
    result.newIdenticalStreak = 1;
  }
  if (result.newIdenticalStreak >= thresholds_.stuckCountLimit &&
      thresholds_.stuckCountLimit > 0) {
    bits |= kReasonStuckValue;
    softViolation = true;
  }

  if (quality == Quality::Invalid) {
    result.quality = Quality::Invalid;
  } else if (softViolation) {
    result.quality = Quality::Suspect;
  } else {
    result.quality = Quality::Valid;
  }

  result.reasonBits = bits;
  return result;
}

}  // namespace cauce

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


  // Wind: the ceiling is above the ~50 m/0 s a well-built cup anemometer reports before it is
  // physically overspeeding, and the rise budget is deliberately loose because a gust is real
  // data. A tight limit here would mark every gusty afternoon SUSPECT, which is worse than
  // letting a brief spike through for the outlier rule to catch.
  t.rangeMin[static_cast<uint8_t>(Variable::WindSpeed)] = 0.0f;
  t.rangeMax[static_cast<uint8_t>(Variable::WindSpeed)] = 60.0f;
  t.maxRisePerMinute[static_cast<uint8_t>(Variable::WindSpeed)] = 25.0f;
  t.maxDropPerMinute[static_cast<uint8_t>(Variable::WindSpeed)] = 25.0f;

  // TVOC: an estimated value, and a noisy one. The ceiling is the CCS811's own full scale with
  // headroom, and the rise budget is loose on purpose - VOC concentrations jump when someone
  // walks past the inlet, and calling that a fault would discard the signal the node exists to
  // capture.
  t.rangeMin[static_cast<uint8_t>(Variable::Tvoc)] = 0.0f;
  t.rangeMax[static_cast<uint8_t>(Variable::Tvoc)] = 65000.0f;
  t.maxRisePerMinute[static_cast<uint8_t>(Variable::Tvoc)] = 20000.0f;
  t.maxDropPerMinute[static_cast<uint8_t>(Variable::Tvoc)] = 20000.0f;

  auto set = [&t](Variable v, float lo, float hi, float ratePerMinute) {
    const uint8_t i = static_cast<uint8_t>(v);
    t.rangeMin[i] = lo;
    t.rangeMax[i] = hi;
    t.maxRisePerMinute[i] = ratePerMinute;
    t.maxDropPerMinute[i] = ratePerMinute;
  };

  // CO2: the MH-Z19C's own 400..5000 ppm span with margin below, because an NDIR cell that
  // reads 380 outdoors is a calibration question and not an impossible value.
  set(Variable::Co2, 300.0f, 10000.0f, 2000.0f);
  // Particulate mass: the SPS30 specifies 0..1000 ug/m3. Smoke and dust plumes are real and
  // abrupt, so the rate budget only catches a sensor that jumps across its whole scale.
  set(Variable::Pm1, 0.0f, 1000.0f, 800.0f);
  set(Variable::Pm25, 0.0f, 1000.0f, 800.0f);
  set(Variable::Pm4, 0.0f, 1000.0f, 800.0f);
  set(Variable::Pm10, 0.0f, 1000.0f, 800.0f);
  // UV index: 0..20 covers the highest values ever recorded at altitude. A cloud edge moves
  // the index by several units within a minute.
  set(Variable::UvIndex, 0.0f, 20.0f, 10.0f);
  set(Variable::UvaIndex, 0.0f, 20.0f, 10.0f);
  set(Variable::UvbIndex, 0.0f, 20.0f, 10.0f);
  // Irradiance: the solar constant is ~1361 W/m2; clear-sky surface values stay below ~1200.
  set(Variable::SolarIrradiance, 0.0f, 1500.0f, 1200.0f);
  // Direction is circular: 359 -> 1 is a 2 degree step, so a rate limit would flag every
  // northerly wind. Range only.
  set(Variable::WindDirection, 0.0f, 360.0f, 3.4e38f);
  // Rain per sampling interval. 300 mm is beyond any minute or hour on record; the rate
  // check is off because the value is already a per-interval total.
  set(Variable::Rainfall, 0.0f, 300.0f, 3.4e38f);
  set(Variable::SoilMoisture, 0.0f, 100.0f, 20.0f);
  // Gypsum blocks work from saturation to roughly -1500 kPa (wilting point); the value is
  // stored as positive suction.
  set(Variable::SoilWaterPotential, 0.0f, 2000.0f, 200.0f);
  // Soil a few centimetres down moves far more slowly than air.
  set(Variable::SoilTemperature, -20.0f, 70.0f, 2.0f);
  set(Variable::LeafWetness, 0.0f, 100.0f, 100.0f);
  // The AS3935 reports strikes as events per interval and distance in its own 1..40 km
  // bins; 63 is the chip's "out of range" code and is filtered by the driver.
  set(Variable::LightningStrikes, 0.0f, 1000.0f, 3.4e38f);
  set(Variable::LightningDistance, 0.0f, 40.0f, 3.4e38f);
  // Battery: wide enough for both a single LiPo cell and the 12 V lead-acid pack the pilot
  // station uses. A node on one chemistry narrows it with thr_range_* in its config.
  set(Variable::BatteryVoltage, 2.5f, 15.0f, 0.5f);

  t.stuckCountLimit = 6;
  t.stuckEpsilon = 0.01f;
  t.maxFutureSkewMs = 120000;
  return t;
}

bool isLegitimatelyConstant(Variable variable, float value, float epsilon) {
  switch (variable) {
    // A vane holds its last heading in calm air, so a constant direction is not a fault.
    case Variable::WindDirection:
      return true;
    // Zero is the resting state of these: no light at night, no wind in calm, no rain on a
    // dry day, no lightning most of the year. Six identical zeros are weather, not a frozen
    // sensor, and flagging them would mark most of every night SUSPECT.
    case Variable::Light:
    case Variable::WindSpeed:
    case Variable::UvIndex:
    case Variable::UvaIndex:
    case Variable::UvbIndex:
    case Variable::SolarIrradiance:
    case Variable::Rainfall:
    case Variable::LightningStrikes:
    case Variable::LightningDistance:
      return std::fabs(value) <= epsilon;
    default:
      return false;
  }
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
      thresholds_.stuckCountLimit > 0 &&
      !isLegitimatelyConstant(candidate.variable, candidate.value,
                              thresholds_.stuckEpsilon)) {
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

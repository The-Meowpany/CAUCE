#include "cauce/drivers/WindAccumulator.h"

#include <cmath>

namespace cauce::drivers {

namespace {
constexpr float kPi = 3.14159265358979323846f;
}  // namespace

float deriveMetresPerSecondPerPulsePerSecond(float diameterMetres,
                                              uint8_t pulsesPerRevolution) {
  if (diameterMetres <= 0.0f || pulsesPerRevolution == 0) return 0.0f;
  // One revolution advances the tip by pi * D, and pulsesPerRevolution pulses mark that
  // revolution, so a pulse is worth (pi * D) / N metres of travel.
  const float metresPerPulse = (kPi * diameterMetres) / static_cast<float>(pulsesPerRevolution);
  // Metres per second = metres per pulse * pulses per second.
  return metresPerPulse;
}

void WindPulseAccumulator::setGeometry(float diameterMetres, uint8_t pulsesPerRevolution) {
  metresPerSecondPerPulsePerSecond_ =
      deriveMetresPerSecondPerPulsePerSecond(diameterMetres, pulsesPerRevolution);
}

void WindPulseAccumulator::setCalibration(float metresPerSecondPerPulsePerSecond) {
  if (metresPerSecondPerPulsePerSecond_ <= 0.0f) return;  // refuse to calibrate an unbuilt sensor
  metresPerSecondPerPulsePerSecond_ = metresPerSecondPerPulsePerSecond;
}

void WindPulseAccumulator::beginInterval(uint32_t nowMs) {
  if (intervalOpen_) return;
  intervalStartMs_ = nowMs;
  intervalOpen_ = true;
}

void WindPulseAccumulator::addPulse(uint32_t nowMs) {
  if (!intervalOpen_) beginInterval(nowMs);
  ++pendingPulses_;
}

WindPulseAccumulator::Speed WindPulseAccumulator::takeSpeed(uint32_t nowMs) {
  Speed out;
  if (!intervalOpen_) {
    out.intervalMs = 0;
    return out;
  }
  const uint32_t elapsed = nowMs - intervalStartMs_;
  out.pulses = pendingPulses_;
  out.intervalMs = elapsed;

  // Reset first, unconditionally: an interval too short to report is still a consumed
  // interval, and keeping its pulses would silently fold them into the next one and overstate
  // it. Reporting the truth costs the node a few pulses a second, which is the right trade
  // against a wind speed that is quietly wrong.
  intervalStartMs_ = nowMs;
  pendingPulses_ = 0;

  if (elapsed < kMinIntervalMs || metresPerSecondPerPulsePerSecond_ <= 0.0f) {
    out.valid = false;
    out.metresPerSecond = 0.0f;
    return out;
  }

  const float pulsesPerSecond = static_cast<float>(out.pulses) * 1000.0f / static_cast<float>(elapsed);
  out.metresPerSecond = pulsesPerSecond * metresPerSecondPerPulsePerSecond_;
  out.valid = true;
  return out;
}

}  // namespace cauce::drivers
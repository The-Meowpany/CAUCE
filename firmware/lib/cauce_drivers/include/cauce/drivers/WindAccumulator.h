#pragma once

#include <cstdint>

namespace cauce::drivers {

// A three-cup anemometer is a pulse train, not a reading: the sensor produces one pulse per
// revolution and the wind speed is a function of how many pulses arrived over an interval. That
// makes the useful unit of work here the counter, and it means the driver cannot simply read a
// register on demand the way the BME280 does - it has to have been counting while nothing was
// asking.
//
// `PulsesPerRevolution` is 3 for a cup rotor. `DiameterMetres` is the sweep diameter, not the
// rotor's own diameter: the tip of a cup travels a circle of that diameter, and confusing the
// two under-reads speed by the ratio between them.
class WindPulseAccumulator {
 public:
  struct Speed {
    bool valid{false};
    float metresPerSecond{0.0f};
    uint32_t pulses{0};
    uint32_t intervalMs{0};
  };

  void setGeometry(float diameterMetres, uint8_t pulsesPerRevolution);
  void setCalibration(float metresPerSecondPerPulsePerSecond);

  // Opens an interval without waiting for a pulse.
  //
  // Calm is the common case for a cup anemometer, not an exceptional one, and an interval that
  // only ever opens on the first pulse means a node in still air reports "not yet measuring"
  // forever and never once records that the wind was zero. Starting the window on a timestamp
  // is what makes calm observable at all.
  void beginInterval(uint32_t nowMs);

  // Records one pulse at `nowMs`. Pulses are counted, not timestamped individually: the
  // conversion only ever needs the count over an interval, and keeping a history of every edge
  // would cost memory the node does not have for information nothing reads.
  void addPulse(uint32_t nowMs);

  // Latches the accumulated pulses into a speed and starts a new interval. Returns an invalid
  // speed - not zero - when the interval was too short to be meaningful, because "the wind is
  // calm" and "we did not measure long enough" are different claims and must not share a value.
  Speed takeSpeed(uint32_t nowMs);

  bool ready() const { return intervalOpen_; }

  static constexpr uint32_t kMinIntervalMs = 2000;

 private:
  float metresPerSecondPerPulsePerSecond_{0.0f};
  uint32_t pendingPulses_{0};
  uint32_t intervalStartMs_{0};
  bool intervalOpen_{false};
};

// The calibration constant that turns the geometry into a speed. Kept separate from the
// accumulator so the maths is testable without a clock and so a site can override the derived
// number: cup anemometers routinely read low next to a calibrated reference, and the honest
// response is a per-node factor with the disagreement recorded, not a different formula.
float deriveMetresPerSecondPerPulsePerSecond(float diameterMetres,
                                              uint8_t pulsesPerRevolution);

}  // namespace cauce::drivers
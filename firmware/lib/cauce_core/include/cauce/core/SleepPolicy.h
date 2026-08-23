#pragma once

#include <cstdint>

namespace cauce {

struct SleepAssessment {
  bool deepSleepAdvisable{false};
  uint32_t recommendedSleepS{0};
  const char* reason{"unknown"};
};

class SleepPolicy {
 public:
  struct Inputs {
    uint32_t samplingIntervalS{60};
    uint32_t syncIntervalS{900};
    uint32_t expectedBootConnectS{15};
    uint32_t expectedMeasureSettleS{2};
    float batteryV{0.0f};
    bool externalPower{false};
    bool storageHasPendingSync{false};
  };

  static SleepAssessment evaluate(const Inputs& in) {
    SleepAssessment a;
    if (in.externalPower) {
      a.reason = "external_power_no_sleep_needed";
      return a;
    }
    if (in.storageHasPendingSync && in.syncIntervalS < 4 * in.samplingIntervalS) {
      a.reason = "sync_cadence_too_tight_for_sleep";
      return a;
    }
    if (in.batteryV > 0.0f && in.batteryV < 3.3f) {
      a.reason = "battery_below_safe_threshold";
      a.deepSleepAdvisable = true;
      return a;
    }

    const uint32_t overhead =
        in.expectedBootConnectS + in.expectedMeasureSettleS;
    if (in.samplingIntervalS <= overhead * 2) {
      a.reason = "sampling_interval_too_short_for_sleep_overhead";
      return a;
    }

    a.deepSleepAdvisable = true;
    uint32_t sleep = in.samplingIntervalS - overhead;
    if (sleep > 3600) sleep = 3600;
    a.recommendedSleepS = sleep;
    a.reason = "ok";
    return a;
  }
};

}  // namespace cauce

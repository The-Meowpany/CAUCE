#pragma once

#include <cstdint>

namespace cauce::app {

struct DeepSleepInputs {
  bool enabled{false};
  uint32_t samplingIntervalS{60};
  uint32_t syncIntervalS{900};
  uint32_t expectedBootConnectS{15};
  uint32_t expectedMeasureSettleS{2};
  float batteryV{0.0f};
  bool externalPower{false};
  bool storageHasPendingSync{false};
  bool networkConnected{false};
  bool portalActive{false};
  bool otaBusy{false};
  uint32_t msSinceLastMeasurement{0};
  uint32_t msSinceBoot{0};
  uint32_t bootSettleS{20};
};

enum class SleepDecision : uint8_t { StayAwake = 0, Sleep = 1 };

struct SleepPlan {
  SleepDecision decision{SleepDecision::StayAwake};
  uint32_t sleepS{0};
  const char* reason{"disabled"};
};

class DeepSleepController {
 public:
  SleepPlan evaluate(const DeepSleepInputs& in) const;
};

}  // namespace cauce::app

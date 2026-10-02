#include "cauce/app/DeepSleepController.h"

#include "cauce/core/SleepPolicy.h"

namespace cauce::app {

SleepPlan DeepSleepController::evaluate(const DeepSleepInputs& in) const {
  SleepPlan plan;
  if (!in.enabled) {
    plan.reason = "disabled";
    return plan;
  }
  if (in.otaBusy) {
    plan.reason = "ota_in_progress";
    return plan;
  }
  if (in.networkConnected) {
    plan.reason = "link_up_stay_awake";
    return plan;
  }
  if (in.portalActive) {
    plan.reason = "portal_active";
    return plan;
  }
  if (in.msSinceBoot < in.bootSettleS * 1000u) {
    plan.reason = "boot_settle";
    return plan;
  }
  if (in.msSinceLastMeasurement < in.samplingIntervalS * 1000u) {
    plan.reason = "not_due_yet";
    return plan;
  }

  SleepPolicy::Inputs policyIn;
  policyIn.samplingIntervalS = in.samplingIntervalS;
  policyIn.syncIntervalS = in.syncIntervalS;
  policyIn.expectedBootConnectS = in.expectedBootConnectS;
  policyIn.expectedMeasureSettleS = in.expectedMeasureSettleS;
  policyIn.batteryV = in.batteryV;
  policyIn.externalPower = in.externalPower;
  policyIn.storageHasPendingSync = in.storageHasPendingSync;
  const SleepAssessment assessment = SleepPolicy::evaluate(policyIn);
  if (!assessment.deepSleepAdvisable) {
    plan.reason = assessment.reason;
    return plan;
  }
  plan.decision = SleepDecision::Sleep;
  plan.sleepS = assessment.recommendedSleepS;
  plan.reason = "ok";
  return plan;
}

}  // namespace cauce::app

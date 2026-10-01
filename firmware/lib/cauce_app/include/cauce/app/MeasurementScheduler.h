#pragma once

#include <cstdint>

#include "cauce/core/IStorageRepository.h"
#include "cauce/core/Logger.h"
#include "cauce/core/NodeState.h"
#include "cauce/core/ValidationEngine.h"
#include "cauce/drivers/ISensorDriver.h"
#include "cauce/hal/IClock.h"

namespace cauce::app {

struct SchedulerCounters {
  uint32_t measurementCount{0};
  uint32_t storedCount{0};
  uint32_t invalidCount{0};
  uint32_t suspectCount{0};
  uint32_t readFailures{0};
  uint32_t storageFailures{0};
  uint64_t lastSuccessUtcMs{0};
};

struct PerVariableState {
  bool hasPrevious{false};
  float previousValue{0.0f};
  uint64_t previousTimestampMs{0};
  uint32_t identicalStreak{0};
  float streakValue{-1e30f};
  bool sensorWasOk{false};
  uint32_t consecutiveMisses{0};
  bool missingRecordEmitted{false};
};

class MeasurementScheduler {
 public:
  MeasurementScheduler(hal::IClock& clock, IStorageRepository& store,
                       const ValidationEngine& validator, Logger& logger);

  void addSensor(drivers::ISensorDriver* sensor);
  void setNodeId(const char* nodeId);
  void setSamplingInterval(uint32_t intervalS);
  void setSequenceStart(uint32_t lastKnownSequence);
  void setRetentionBudget(uint32_t maxTotalBytes);
  void beginAllSensors();
  void tick();

  const SchedulerCounters& counters() const { return counters_; }
  NodeState currentState() const { return state_; }

 private:
  void processSensor(drivers::ISensorDriver* sensor);
  size_t sensorSlot(drivers::ISensorDriver* sensor) const;
  void emitMissingPlaceholder(Variable variable, drivers::ISensorDriver* sensor,
                              PerVariableState& varState, uint64_t timestampMs,
                              bool timeUncertain);
  bool appendMeasurement(const Measurement& measurement);

  hal::IClock& clock_;
  IStorageRepository& store_;
  const ValidationEngine& validator_;
  Logger& logger_;

  static constexpr size_t kMaxSensors = 4;
  drivers::ISensorDriver* sensors_[kMaxSensors]{};
  size_t sensorCount_{0};

  char nodeId_[16]{"CAUCE-001"};
  uint32_t samplingIntervalMs_{60000};
  uint64_t nextSampleAtMonotonicMs_{0};
  uint32_t sequenceCounter_{0};
  uint32_t retentionBudgetBytes_{0};
  uint64_t lastRetentionMonotonicMs_{0};
  SchedulerCounters counters_{};
  PerVariableState variableState_[kVariableCount][kMaxSensors]{};
  NodeState state_{NodeState::Boot};
};

}  // namespace cauce::app

#include "cauce/app/MeasurementScheduler.h"

#include <cstring>

namespace cauce::app {

MeasurementScheduler::MeasurementScheduler(hal::IClock& clock,
                                           IStorageRepository& store,
                                           const ValidationEngine& validator,
                                           Logger& logger)
    : clock_(clock), store_(store), validator_(validator), logger_(logger) {}

void MeasurementScheduler::addSensor(drivers::ISensorDriver* sensor) {
  if (!sensor) return;
  if (sensorCount_ >= kMaxSensors) {
    logger_.eventf(LogLevel::Warn, "SENSOR_CAPACITY_REACHED",
                   "max=%lu rejected_id=%s",
                   static_cast<unsigned long>(kMaxSensors),
                   sensor->metadata().id);
    return;
  }
  sensors_[sensorCount_++] = sensor;
}

void MeasurementScheduler::setNodeId(const char* nodeId) {
  copyString(nodeId_, sizeof(nodeId_), nodeId);
}

void MeasurementScheduler::setSamplingInterval(uint32_t intervalS) {
  if (intervalS < 10) intervalS = 10;
  if (intervalS > 3600) intervalS = 3600;
  samplingIntervalMs_ = intervalS * 1000u;
}

void MeasurementScheduler::setRetentionBudget(uint32_t maxTotalBytes) {
  retentionBudgetBytes_ = maxTotalBytes;
}

void MeasurementScheduler::setSequenceStart(uint32_t lastKnownSequence) {
  sequenceCounter_ = lastKnownSequence;
}

void MeasurementScheduler::beginAllSensors() {
  state_ = NodeState::SensorDiscovery;
  for (size_t i = 0; i < sensorCount_; ++i) {
    const drivers::SensorMetadata& meta = sensors_[i]->metadata();
    if (sensors_[i]->begin()) {
      logger_.eventf(LogLevel::Info, "SENSOR_DISCOVERED", "node=%s sensor=%s model=%s vars=%u",
                     nodeId_, meta.id, meta.model, meta.variableCount);
    } else {
      logger_.eventf(LogLevel::Warn, "SENSOR_BEGIN_FAILED",
                     "node=%s sensor=%s status=%u", nodeId_, meta.id,
                     static_cast<unsigned>(sensors_[i]->health()));
    }
  }
  state_ = NodeState::Ready;
  nextSampleAtMonotonicMs_ = clock_.monotonicMs();
  lastRetentionMonotonicMs_ = nextSampleAtMonotonicMs_;
}

void MeasurementScheduler::tick() {
  if (state_ != NodeState::Ready && state_ != NodeState::Storing) return;

  const uint32_t nowMs = clock_.monotonicMs();
  if (retentionBudgetBytes_ != 0 &&
      nowMs - lastRetentionMonotonicMs_ >= 3600000u) {
    lastRetentionMonotonicMs_ = nowMs;
    store_.applyRetentionPolicy(retentionBudgetBytes_);
  }
  if (nowMs < nextSampleAtMonotonicMs_) return;
  nextSampleAtMonotonicMs_ = nowMs + samplingIntervalMs_;
  state_ = NodeState::Measuring;

  for (size_t i = 0; i < sensorCount_; ++i) {
    processSensor(sensors_[i]);
  }
  state_ = NodeState::Ready;
}

void MeasurementScheduler::processSensor(drivers::ISensorDriver* sensor) {
  const drivers::SensorMetadata& meta = sensor->metadata();
  const size_t slot = sensorSlot(sensor);

  for (uint8_t v = 0; v < meta.variableCount; ++v) {
    const Variable variable = meta.variables[v];
    if (variable == Variable::Unknown) continue;

    PerVariableState& varState = variableState_[static_cast<uint8_t>(variable)][slot];
    drivers::Reading reading{};
    const bool readOk = sensor->read(variable, reading);

    if (!readOk) {
      counters_.readFailures++;
      varState.consecutiveMisses++;
      logger_.eventf(LogLevel::Warn, "SENSOR_READ_FAILED",
                     "node=%s sensor=%s var=%s status=%u misses=%u", nodeId_,
                     meta.id, variableName(variable),
                     static_cast<unsigned>(reading.status),
                     static_cast<unsigned>(varState.consecutiveMisses));
      if (varState.sensorWasOk && !varState.missingRecordEmitted &&
          varState.consecutiveMisses >= 2) {
        emitMissingPlaceholder(variable, sensor, varState, clock_.utcMs(),
                               !clock_.utcTimeValid());
      }
      continue;
    }

    Measurement candidate{};
    copyString(candidate.nodeId, sizeof(candidate.nodeId), nodeId_);
    copyString(candidate.sensorId, sizeof(candidate.sensorId), meta.id);
    candidate.sequence = ++sequenceCounter_;
    candidate.timestampUtcMs = clock_.utcMs();
    candidate.timeUncertain = !clock_.utcTimeValid();
    candidate.variable = variable;
    candidate.value = reading.value;
    candidate.quality = Quality::Uncalibrated;

    ValidationContext context;
    context.hasPreviousValue = varState.hasPrevious;
    context.previousValue = varState.previousValue;
    context.previousTimestampMs = varState.previousTimestampMs;
    context.identicalStreak = varState.identicalStreak;
    context.streakValue = varState.streakValue;
    context.timeValid = clock_.utcTimeValid();
    context.nowUtcMs = clock_.utcMs();
    context.hasLastSequence = false;

    const ValidationResult result = validator_.evaluate(candidate, context);
    candidate.quality = result.quality;
    candidate.reasonBits = result.reasonBits;
    candidate.timeUncertain = result.timeUncertain;

    varState.identicalStreak = result.newIdenticalStreak;
    if (result.newIdenticalStreak == 1) varState.streakValue = reading.value;
    varState.hasPrevious = true;
    varState.previousValue = reading.value;
    varState.previousTimestampMs = candidate.timestampUtcMs;
    varState.sensorWasOk = true;
    varState.consecutiveMisses = 0;
    varState.missingRecordEmitted = false;

    counters_.measurementCount++;
    if (result.quality == Quality::Invalid) counters_.invalidCount++;
    if (result.quality == Quality::Suspect) counters_.suspectCount++;

    if (appendMeasurement(candidate)) {
      counters_.storedCount++;
      counters_.lastSuccessUtcMs = candidate.timestampUtcMs;
      logger_.eventf(LogLevel::Info, "MEAS_STORED",
                     "node=%s seq=%lu var=%s value=%.2f q=%s%s", nodeId_,
                     static_cast<unsigned long>(candidate.sequence),
                     variableName(variable), static_cast<double>(candidate.value),
                     qualityName(candidate.quality),
                     candidate.timeUncertain ? " time=UNCERTAIN" : "");
    }
  }
}

size_t MeasurementScheduler::sensorSlot(drivers::ISensorDriver* sensor) const {
  for (size_t i = 0; i < kMaxSensors; ++i) {
    if (sensors_[i] == sensor) return i;
  }
  return 0;
}

void MeasurementScheduler::emitMissingPlaceholder(Variable variable,
                                                  drivers::ISensorDriver* sensor,
                                                  PerVariableState& varState,
                                                  uint64_t timestampMs,
                                                  bool timeUncertain) {
  Measurement missing{};
  copyString(missing.nodeId, sizeof(missing.nodeId), nodeId_);
  copyString(missing.sensorId, sizeof(missing.sensorId), sensor->metadata().id);
  missing.sequence = ++sequenceCounter_;
  missing.timestampUtcMs = timestampMs;
  missing.timeUncertain = timeUncertain || timestampMs == 0;
  missing.variable = variable;
  missing.value = 0.0f;
  missing.quality = Quality::Missing;
  missing.reasonBits = kReasonSensorUnhealthy;
  if (appendMeasurement(missing)) {
    varState.missingRecordEmitted = true;
    logger_.eventf(LogLevel::Info, "MISSING_RECORDED", "node=%s var=%s", nodeId_,
                   variableName(variable));
  }
}

bool MeasurementScheduler::appendMeasurement(const Measurement& measurement) {
  state_ = NodeState::Storing;
  bool ok = true;
  if (!store_.open()) ok = false;
  if (ok && !store_.append(measurement)) ok = false;

  if (!ok) {
    counters_.storageFailures++;
    logger_.eventf(LogLevel::Error, "STORAGE_APPEND_FAILED",
                   "node=%s seq=%lu var=%s", nodeId_,
                   static_cast<unsigned long>(measurement.sequence),
                   variableName(measurement.variable));
  }
  state_ = NodeState::Ready;
  return ok;
}

}  // namespace cauce::app

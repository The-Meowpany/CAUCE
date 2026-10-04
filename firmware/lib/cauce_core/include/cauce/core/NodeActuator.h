#pragma once

// Downlink actuation: the part where a command from the central actually changes
// what the node does.
//
// It exists as a core component rather than as handlers inside main.cpp for two
// reasons. The obvious failure of an in-main handler is that it is unreachable
// from a host test, and an unreachable actuator is an unverified actuator. And the
// safety rule below - the combination of intervals may never leave the node silent
// for longer than a week - is arithmetic over two settings, which is exactly the
// kind of rule that gets checked in a test and never in the field.
//
// WHAT THIS REFUSES TO DO
//
// It will not accept a command that stops the node reporting. That was the reason
// the original handlers validated and did nothing, and the worry was correct; it
// is answered here with bounds rather than with inaction. A refused command is
// reported to the central as an explicit refusal, so an operator learns that the
// node declined rather than inferring it from silence.
//
// Every receipt reports the value that was APPLIED, never the value that was
// requested. A command clamped to a bound has to say so, or the operator believes
// the node is sampling every 10 seconds when it is sampling every 60.

#include <cstddef>
#include <cstdint>

#include "cauce/core/Logger.h"
#include "cauce/hal/IFileSystem.h"

namespace cauce {

struct NodeSettings {
  uint32_t samplingIntervalSeconds{300};
  uint32_t syncIntervalSeconds{600};
  uint8_t ledMode{0};
  // Monotonic stamp of an outstanding resync request, 0 when there is none.
  uint32_t resyncRequestedAtMs{0};
};

class NodeActuator {
 public:
  // Bounds on a single setting.
  static constexpr uint32_t kMinSamplingSeconds = 10;
  static constexpr uint32_t kMaxSamplingSeconds = 86400;
  static constexpr uint32_t kMinSyncSeconds = 60;
  static constexpr uint32_t kMaxSyncSeconds = 86400;
  static constexpr uint32_t kMaxLedMode = 3;

  // The safety invariant. The node wakes to sample and to sync, so the longest it
  // can be quiet is the larger of the two intervals. A week is the point at which
  // silence stops being "offline" and starts being "lost", and the central's own
  // retention is far shorter than that, so a node silent for longer is already
  // outside the window its data would land in.
  static constexpr uint32_t kMaxSilenceSeconds = 7u * 86400u;

  NodeActuator(hal::IFileSystem& fs, Logger& logger,
               const char* statePath = "/state/settings");

  // Restores persisted settings. A missing or short or corrupt file leaves the
  // defaults in place and reports false, which is a recoverable state: the node
  // measures at the default rate rather than not measuring at all.
  bool load();

  bool persist() const;

  const NodeSettings& settings() const { return settings_; }

  // Each returns false when the command is refused, and writes a machine-readable
  // reason into `detailOut`. On success `detailOut` names the applied value.
  bool setSamplingInterval(uint32_t seconds, char* detailOut, size_t capacity);
  bool setSyncInterval(uint32_t seconds, char* detailOut, size_t capacity);
  bool setLedMode(uint32_t mode, char* detailOut, size_t capacity);

  // Latched rather than instantaneous: the command usually arrives in the same
  // batch as the sync response, and a flag set and cleared inside the handler
  // would be consumed before anything acted on it.
  bool requestResync(uint32_t nowMs, char* detailOut, size_t capacity);
  bool consumeResyncRequest();

  // How long the node can currently stay silent, in seconds.
  uint32_t silenceSeconds() const;

  // True when the current combination is within the safety invariant.
  bool withinSilenceBudget() const {
    return silenceSeconds() <= kMaxSilenceSeconds;
  }

  // True when a changed setting still needs writing to flash.
  bool isDirty() const { return dirty_; }

 private:
  bool wouldExceedSilenceBudget(uint32_t sampling, uint32_t sync) const;

  hal::IFileSystem& fs_;
  Logger& logger_;
  char statePath_[64];
  NodeSettings settings_{};
  bool dirty_{false};
};

}  // namespace cauce

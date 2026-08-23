#pragma once

#include <cstdint>

namespace cauce {

enum class NodeState : uint8_t {
  Boot,
  SelfTest,
  Initializing,
  TimeSync,
  SensorDiscovery,
  Ready,
  Measuring,
  Validating,
  Storing,
  Serving,
  Syncing,
  SensorError,
  StorageError,
  NetworkError,
  TimeError,
  Recovery,
  SafeMode,
};

const char* nodeStateName(NodeState state);

}  // namespace cauce

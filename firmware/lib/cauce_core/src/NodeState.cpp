#include "cauce/core/NodeState.h"

namespace cauce {

const char* nodeStateName(NodeState state) {
  switch (state) {
    case NodeState::Boot:
      return "BOOT";
    case NodeState::SelfTest:
      return "SELF_TEST";
    case NodeState::Initializing:
      return "INITIALIZING";
    case NodeState::TimeSync:
      return "TIME_SYNC";
    case NodeState::SensorDiscovery:
      return "SENSOR_DISCOVERY";
    case NodeState::Ready:
      return "READY";
    case NodeState::Measuring:
      return "MEASURING";
    case NodeState::Validating:
      return "VALIDATING";
    case NodeState::Storing:
      return "STORING";
    case NodeState::Serving:
      return "SERVING";
    case NodeState::Syncing:
      return "SYNCING";
    case NodeState::SensorError:
      return "SENSOR_ERROR";
    case NodeState::StorageError:
      return "STORAGE_ERROR";
    case NodeState::NetworkError:
      return "NETWORK_ERROR";
    case NodeState::TimeError:
      return "TIME_ERROR";
    case NodeState::Recovery:
      return "RECOVERY";
    case NodeState::SafeMode:
      return "SAFE_MODE";
  }
  return "UNKNOWN";
}

}  // namespace cauce

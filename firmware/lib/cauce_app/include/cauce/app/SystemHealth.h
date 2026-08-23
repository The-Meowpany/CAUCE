#pragma once

#include <cstdint>

#include "cauce/app/NetworkManager.h"
#include "cauce/core/NodeState.h"
#include "cauce/core/Types.h"

namespace cauce::app {

struct SystemHealth {
  const char* firmwareVersion{Versions::kFirmware};
  NodeState nodeState{NodeState::Boot};
  NetState netState{NetState::Disabled};
  uint32_t uptimeMs{0};
  bool utcTimeValid{false};
  int8_t rssiDbm{0};
  float batteryVoltageV{0.0f};

  uint32_t measurementCount{0};
  uint32_t storedCount{0};
  uint32_t invalidCount{0};
  uint32_t suspectCount{0};
  uint32_t readFailures{0};
  uint32_t storageFailures{0};

  uint64_t lastSuccessUtcMs{0};
  uint32_t storageRecords{0};
  uint32_t storageBytes{0};
  uint32_t corruptedFrames{0};
};

}  // namespace cauce::app

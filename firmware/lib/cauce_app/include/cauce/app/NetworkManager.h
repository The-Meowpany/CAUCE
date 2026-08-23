#pragma once

#include <cstdint>

#include "cauce/core/Logger.h"
#include "cauce/core/NodeConfig.h"
#include "cauce/hal/IClock.h"
#include "cauce/hal/INetworkController.h"

namespace cauce::app {

enum class NetState : uint8_t {
  Disabled = 0,
  WaitingToRetry = 1,
  Connecting = 2,
  Connected = 3,
  Degraded = 4,
  ApFallback = 5,
};

const char* netStateName(NetState state);

class NetworkManager {
 public:
  struct Tuning {
    uint32_t connectTimeoutS{30};
    uint32_t backoffBaseS{5};
    uint32_t backoffMaxS{300};
    uint32_t attemptsBeforeAp{5};
    int8_t degradedRssiDbm{-70};
    uint32_t apRetryPeriodS{900};
  };

  NetworkManager(hal::INetworkController& controller, hal::IClock& clock,
                 Logger& logger, const NodeConfig& config,
                 const char* apSsidBase);

  void tick();
  NetState state() const { return state_; }
  uint32_t attemptsSinceSuccess() const { return attempts_; }
  void forceRetry();

 private:
  void enterWaitingToRetry();
  void enterApFallback();
  bool credentialsPresent() const;

  hal::INetworkController& ctrl_;
  hal::IClock& clock_;
  Logger& logger_;
  NodeConfig config_;
  Tuning tuning_;
  char apSsid_[40];

  NetState state_{NetState::Disabled};
  uint32_t attempts_{0};
  uint64_t nextAttemptMonotonicMs_{0};
  uint64_t connectStartedMonotonicMs_{0};
};

}  // namespace cauce::app

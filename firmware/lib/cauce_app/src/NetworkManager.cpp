#include "cauce/app/NetworkManager.h"

#include <cstdio>
#include <cstring>

namespace cauce::app {

const char* netStateName(NetState state) {
  switch (state) {
    case NetState::Disabled:
      return "OFFLINE";
    case NetState::WaitingToRetry:
      return "WAITING_RETRY";
    case NetState::Connecting:
      return "CONNECTING";
    case NetState::Connected:
      return "CONNECTED";
    case NetState::Degraded:
      return "DEGRADED";
    case NetState::ApFallback:
      return "AP_MODE";
  }
  return "UNKNOWN";
}

NetworkManager::NetworkManager(hal::INetworkController& controller,
                               hal::IClock& clock, Logger& logger,
                               const NodeConfig& config, const char* apSsidBase)
    : ctrl_(controller), clock_(clock), logger_(logger), config_(config) {
  std::snprintf(apSsid_, sizeof(apSsid_), "%s-AP", apSsidBase);
}

bool NetworkManager::credentialsPresent() const {
  return config_.wifiSsid[0] != '\0';
}

void NetworkManager::enterWaitingToRetry() {
  ++attempts_;
  uint32_t backoffS = tuning_.backoffBaseS;
  for (uint32_t i = 1; i < attempts_ && backoffS < tuning_.backoffMaxS; ++i) {
    backoffS *= 2;
  }
  if (backoffS > tuning_.backoffMaxS) backoffS = tuning_.backoffMaxS;
  nextAttemptMonotonicMs_ =
      clock_.monotonicMs() + static_cast<uint64_t>(backoffS) * 1000ULL;
  state_ = NetState::WaitingToRetry;
  logger_.eventf(LogLevel::Warn, "NETWORK_RETRY_SCHEDULED",
                 "attempt=%lu backoff_s=%lu",
                 static_cast<unsigned long>(attempts_),
                 static_cast<unsigned long>(backoffS));
}

void NetworkManager::enterApFallback() {
  if (!ctrl_.startAp(apSsid_)) {
    logger_.event(LogLevel::Error, "AP_START_FAILED");
    state_ = NetState::WaitingToRetry;
    nextAttemptMonotonicMs_ =
        clock_.monotonicMs() +
        static_cast<uint64_t>(tuning_.apRetryPeriodS) * 1000ULL;
    return;
  }
  state_ = NetState::ApFallback;
  logger_.eventf(LogLevel::Info, "AP_STARTED", "ssid=%s", apSsid_);
}

void NetworkManager::forceRetry() {
  attempts_ = 0;
  nextAttemptMonotonicMs_ = 0;
  if (state_ == NetState::Connected || state_ == NetState::Degraded ||
      state_ == NetState::Connecting) {
    ctrl_.disconnectSta();
  }
  state_ = NetState::Disabled;
}

void NetworkManager::tick() {
  if (state_ == NetState::Disabled && !config_.wifiEnabled) return;

  const hal::NetEvent event = ctrl_.pollEvent();
  const uint32_t nowMs = clock_.monotonicMs();

  switch (state_) {
    case NetState::Disabled: {
      if (!config_.wifiEnabled) return;
      if (!credentialsPresent()) {
        enterApFallback();
        return;
      }
      state_ = NetState::WaitingToRetry;
      nextAttemptMonotonicMs_ = nowMs;
      break;
    }

    case NetState::WaitingToRetry: {
      if (nowMs < nextAttemptMonotonicMs_) return;
      if (attempts_ >= tuning_.attemptsBeforeAp) {
        enterApFallback();
        return;
      }
      if (!ctrl_.connectSta(config_.wifiSsid, config_.wifiPassword)) {
        logger_.event(LogLevel::Warn, "NETWORK_CONNECT_CALL_FAILED");
        enterWaitingToRetry();
        return;
      }
      connectStartedMonotonicMs_ = nowMs;
      state_ = NetState::Connecting;
      break;
    }

    case NetState::Connecting: {
      if (event == hal::NetEvent::GotIp) {
        attempts_ = 0;
        state_ = NetState::Connected;
        logger_.event(LogLevel::Info, "NETWORK_CONNECTED");
        return;
      }
      if (event == hal::NetEvent::ConnectFailed || event == hal::NetEvent::LinkLost) {
        ctrl_.disconnectSta();
        enterWaitingToRetry();
        return;
      }
      if (nowMs - connectStartedMonotonicMs_ >
          static_cast<uint64_t>(tuning_.connectTimeoutS) * 1000ULL) {
        ctrl_.disconnectSta();
        enterWaitingToRetry();
      }
      return;
    }

    case NetState::Connected: {
      if (event == hal::NetEvent::LinkLost) {
        logger_.event(LogLevel::Warn, "NETWORK_LOST");
        attempts_ = 0;
        enterWaitingToRetry();
        return;
      }
      if (ctrl_.rssiDbm() < tuning_.degradedRssiDbm) {
        state_ = NetState::Degraded;
        logger_.eventf(LogLevel::Warn, "NETWORK_DEGRADED", "rssi=%d",
                       static_cast<int>(ctrl_.rssiDbm()));
      }
      return;
    }

    case NetState::Degraded: {
      if (event == hal::NetEvent::LinkLost) {
        logger_.event(LogLevel::Warn, "NETWORK_LOST");
        enterWaitingToRetry();
        return;
      }
      if (ctrl_.rssiDbm() >= tuning_.degradedRssiDbm) {
        state_ = NetState::Connected;
        logger_.event(LogLevel::Info, "NETWORK_RECOVERED_QUALITY");
      }
      return;
    }

    case NetState::ApFallback: {
      if ((event == hal::NetEvent::LinkLost) &&
          credentialsPresent() &&
          nowMs - connectStartedMonotonicMs_ >
              static_cast<uint64_t>(tuning_.apRetryPeriodS) * 1000ULL) {
        ctrl_.stopAp();
        attempts_ = 0;
        state_ = NetState::WaitingToRetry;
        nextAttemptMonotonicMs_ = nowMs;
      }
      return;
    }
  }

  if (state_ == NetState::WaitingToRetry && nowMs >= nextAttemptMonotonicMs_ &&
      attempts_ < tuning_.attemptsBeforeAp) {
    if (ctrl_.connectSta(config_.wifiSsid, config_.wifiPassword)) {
      connectStartedMonotonicMs_ = nowMs;
      state_ = NetState::Connecting;
    } else {
      enterWaitingToRetry();
    }
  }
}

}  // namespace cauce::app

#include <cstring>
#include <vector>

#include <unity.h>

#include "cauce/app/NetworkManager.h"
#include "cauce/core/Logger.h"
#include "cauce/hal/ManualClock.h"

using namespace cauce;
using namespace cauce::app;

namespace {

class ScriptedNetController final : public hal::INetworkController {
 public:
  std::vector<hal::NetEvent> events;
  bool apStarted{false};
  int connectCalls{0};
  int disconnectCalls{0};
  int8_t rssi{-50};

  bool startAp(const char*) override {
    apStarted = true;
    return true;
  }
  void stopAp() override { apStarted = false; }
  bool connectSta(const char*, const char*) override {
    ++connectCalls;
    return true;
  }
  void disconnectSta() override { ++disconnectCalls; }
  hal::NetEvent pollEvent() override {
    if (events.empty()) return hal::NetEvent::None;
    const hal::NetEvent e = events.front();
    events.erase(events.begin());
    return e;
  }
  int8_t rssiDbm() override { return rssi; }

  void push(hal::NetEvent e) { events.push_back(e); }
};

class SilentSink final : public ILogSink {
 public:
  void writeLine(const char*) override {}
};

hal::ManualClock netClock(1787356800000ULL);
SilentSink sink;

NodeConfig configWithWifi() {
  NodeConfig c{};
  copyString(c.wifiSsid, sizeof(c.wifiSsid), "cauce-net");
  copyString(c.wifiPassword, sizeof(c.wifiPassword), "secret123");
  c.wifiEnabled = true;
  return c;
}

struct Rig {
  ScriptedNetController ctrl;
  Logger logger{sink};
  NetworkManager manager;

  explicit Rig(const NodeConfig& cfg)
      : manager(ctrl, netClock, logger, cfg, "CAUCE-001") {}
};

}  // namespace

void test_disabled_when_wifi_not_enabled() {
  netClock = hal::ManualClock(1787356800000ULL);
  NodeConfig cfg{};
  Rig rig(cfg);
  rig.manager.tick();
  TEST_ASSERT_EQUAL(NetState::Disabled, rig.manager.state());
}

void test_connects_and_reaches_connected_state() {
  netClock = hal::ManualClock(1787356800000ULL);
  Rig rig(configWithWifi());
  rig.manager.tick();
  TEST_ASSERT_EQUAL(1, rig.ctrl.connectCalls);
  TEST_ASSERT_EQUAL(NetState::Connecting, rig.manager.state());

  rig.ctrl.push(hal::NetEvent::GotIp);
  rig.manager.tick();
  TEST_ASSERT_EQUAL(NetState::Connected, rig.manager.state());
  TEST_ASSERT_EQUAL_UINT32(0, rig.manager.attemptsSinceSuccess());
}

void test_link_lost_schedules_retry_with_base_backoff() {
  netClock = hal::ManualClock(1787356800000ULL);
  Rig rig(configWithWifi());
  rig.manager.tick();
  rig.ctrl.push(hal::NetEvent::GotIp);
  rig.manager.tick();
  TEST_ASSERT_EQUAL(NetState::Connected, rig.manager.state());

  rig.ctrl.push(hal::NetEvent::LinkLost);
  rig.manager.tick();
  TEST_ASSERT_EQUAL(NetState::WaitingToRetry, rig.manager.state());
  TEST_ASSERT_EQUAL_UINT32(1, rig.manager.attemptsSinceSuccess());

  netClock.advanceMs(4000);
  rig.manager.tick();
  TEST_ASSERT_EQUAL(1, rig.ctrl.connectCalls);

  netClock.advanceMs(1500);
  rig.manager.tick();
  TEST_ASSERT_EQUAL(2, rig.ctrl.connectCalls);
  TEST_ASSERT_EQUAL(NetState::Connecting, rig.manager.state());
}

void test_backoff_doubles_between_failures() {
  netClock = hal::ManualClock(1787356800000ULL);
  Rig rig(configWithWifi());

  rig.manager.tick();
  TEST_ASSERT_EQUAL(NetState::Connecting, rig.manager.state());
  TEST_ASSERT_EQUAL(1, rig.ctrl.connectCalls);

  rig.ctrl.push(hal::NetEvent::ConnectFailed);
  rig.manager.tick();
  TEST_ASSERT_EQUAL(NetState::WaitingToRetry, rig.manager.state());
  TEST_ASSERT_EQUAL_UINT32(1, rig.manager.attemptsSinceSuccess());

  netClock.advanceMs(4900);
  rig.manager.tick();
  TEST_ASSERT_EQUAL(1, rig.ctrl.connectCalls);
  TEST_ASSERT_EQUAL(NetState::WaitingToRetry, rig.manager.state());

  netClock.advanceMs(200);
  rig.manager.tick();
  TEST_ASSERT_EQUAL(2, rig.ctrl.connectCalls);
  TEST_ASSERT_EQUAL(NetState::Connecting, rig.manager.state());
  const uint64_t gapAfterFirstFailure = netClock.monotonicMs() - 0;

  rig.ctrl.push(hal::NetEvent::ConnectFailed);
  rig.manager.tick();
  TEST_ASSERT_EQUAL(NetState::WaitingToRetry, rig.manager.state());
  TEST_ASSERT_EQUAL_UINT32(2, rig.manager.attemptsSinceSuccess());

  netClock.advanceMs(9900);
  rig.manager.tick();
  TEST_ASSERT_EQUAL(2, rig.ctrl.connectCalls);

  netClock.advanceMs(200);
  rig.manager.tick();
  TEST_ASSERT_EQUAL(3, rig.ctrl.connectCalls);
  TEST_ASSERT_TRUE(gapAfterFirstFailure >= 5000);
}

void test_falls_back_to_ap_after_max_attempts() {
  netClock = hal::ManualClock(1787356800000ULL);
  Rig rig(configWithWifi());
  for (int i = 0; i < 200 && !rig.ctrl.apStarted; ++i) {
    netClock.advanceMs(6000);
    rig.manager.tick();
    if (!rig.ctrl.apStarted) rig.ctrl.push(hal::NetEvent::ConnectFailed);
  }
  TEST_ASSERT_TRUE(rig.ctrl.apStarted);
  TEST_ASSERT_EQUAL(NetState::ApFallback, rig.manager.state());
}

void test_connect_timeout_triggers_retry() {
  netClock = hal::ManualClock(1787356800000ULL);
  Rig rig(configWithWifi());
  rig.manager.tick();
  TEST_ASSERT_EQUAL(NetState::Connecting, rig.manager.state());

  netClock.advanceMs(31000);
  rig.manager.tick();
  TEST_ASSERT_TRUE(rig.ctrl.disconnectCalls >= 1);
  TEST_ASSERT_EQUAL(NetState::WaitingToRetry, rig.manager.state());
}

void test_degraded_on_weak_signal_and_recovers() {
  netClock = hal::ManualClock(1787356800000ULL);
  Rig rig(configWithWifi());
  rig.manager.tick();
  rig.ctrl.push(hal::NetEvent::GotIp);
  rig.manager.tick();
  TEST_ASSERT_EQUAL(NetState::Connected, rig.manager.state());

  rig.ctrl.rssi = -82;
  rig.manager.tick();
  TEST_ASSERT_EQUAL(NetState::Degraded, rig.manager.state());

  rig.ctrl.rssi = -52;
  rig.manager.tick();
  TEST_ASSERT_EQUAL(NetState::Connected, rig.manager.state());
}

void test_no_credentials_starts_ap_immediately() {
  netClock = hal::ManualClock(1787356800000ULL);
  NodeConfig cfg{};
  cfg.wifiEnabled = true;
  Rig rig(cfg);
  rig.manager.tick();
  TEST_ASSERT_TRUE(rig.ctrl.apStarted);
  TEST_ASSERT_EQUAL(NetState::ApFallback, rig.manager.state());
}

void registerNetworkTests() {
  UNITY_BEGIN();
  RUN_TEST(test_disabled_when_wifi_not_enabled);
  RUN_TEST(test_connects_and_reaches_connected_state);
  RUN_TEST(test_link_lost_schedules_retry_with_base_backoff);
  RUN_TEST(test_backoff_doubles_between_failures);
  RUN_TEST(test_falls_back_to_ap_after_max_attempts);
  RUN_TEST(test_connect_timeout_triggers_retry);
  RUN_TEST(test_degraded_on_weak_signal_and_recovers);
  RUN_TEST(test_no_credentials_starts_ap_immediately);
  
  UNITY_END();
}

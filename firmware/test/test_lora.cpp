#include <cstring>
#include <string>
#include <vector>

#include <unity.h>

#include "cauce/app/LoRaSyncTransport.h"
#include "cauce/hal/ManualClock.h"

using namespace cauce;
using namespace cauce::app;

namespace {

class FakeRadio final : public hal::ILoRaRadio {
 public:
  bool canSend{true};
  bool sendOk{true};
  std::vector<std::string> sent;
  int16_t rssi{-80};

  bool canSendNow() override { return canSend; }
  bool send(const uint8_t* data, size_t length) override {
    if (!sendOk) return false;
    sent.push_back(std::string(reinterpret_cast<const char*>(data), length));
    return true;
  }
  int16_t lastRssiDbm() const override { return rssi; }
};

}  // namespace

void test_lora_rejects_oversize_payload() {
  hal::ManualClock clock(1787356800000ULL);
  FakeRadio radio;
  LoRaSyncTransport transport(radio, clock);
  transport.setMaxPayloadBytes(16);
  transport.setMinIntervalMs(0);
  uint32_t ack = 0;
  const char* big = "{\"protocol_version\":1}";
  const auto result = transport.postBatch(
      big, std::strlen(big), nullptr, 1000, ack);
  TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::NetworkError),
                    static_cast<int>(result));
  TEST_ASSERT_TRUE(radio.sent.empty());
}

void test_lora_enforces_min_interval() {
  hal::ManualClock clock(1787356800000ULL);
  FakeRadio radio;
  LoRaSyncTransport transport(radio, clock);
  transport.setMaxPayloadBytes(200);
  transport.setMinIntervalMs(600000);
  uint32_t ack = 0;
  const char* payload = "{\"a\":1}";
  const auto first = transport.postBatch(
      payload, std::strlen(payload), nullptr, 1000, ack);
  TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::Ok),
                    static_cast<int>(first));
  const auto second = transport.postBatch(
      payload, std::strlen(payload), nullptr, 1000, ack);
  TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::NetworkError),
                    static_cast<int>(second));
  clock.advanceMs(600000);
  const auto third = transport.postBatch(
      payload, std::strlen(payload), nullptr, 1000, ack);
  TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::Ok),
                    static_cast<int>(third));
  TEST_ASSERT_EQUAL(2, static_cast<int>(radio.sent.size()));
}

void test_lora_radio_down_maps_to_network_error() {
  hal::ManualClock clock(1787356800000ULL);
  FakeRadio radio;
  radio.canSend = false;
  LoRaSyncTransport transport(radio, clock);
  transport.setMaxPayloadBytes(200);
  transport.setMinIntervalMs(0);
  uint32_t ack = 0;
  const char* payload = "{\"a\":1}";
  const auto result = transport.postBatch(
      payload, std::strlen(payload), nullptr, 1000, ack);
  TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::NetworkError),
                    static_cast<int>(result));
  radio.canSend = true;
  radio.sendOk = false;
  const auto result2 = transport.postBatch(
      payload, std::strlen(payload), nullptr, 1000, ack);
  TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::NetworkError),
                    static_cast<int>(result2));
}

void test_lora_success_reports_optimistic_ack() {
  hal::ManualClock clock(1787356800000ULL);
  FakeRadio radio;
  LoRaSyncTransport transport(radio, clock);
  transport.setMaxPayloadBytes(200);
  transport.setMinIntervalMs(0);
  uint32_t ack = 7;
  const char* payload = "{\"a\":1}";
  const auto result = transport.postBatch(
      payload, std::strlen(payload), nullptr, 1000, ack);
  TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::Ok),
                    static_cast<int>(result));
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, ack);
  TEST_ASSERT_EQUAL(1, static_cast<int>(radio.sent.size()));
}

void registerLoRaTests() {
  RUN_TEST(test_lora_rejects_oversize_payload);
  RUN_TEST(test_lora_enforces_min_interval);
  RUN_TEST(test_lora_radio_down_maps_to_network_error);
  RUN_TEST(test_lora_success_reports_optimistic_ack);
}

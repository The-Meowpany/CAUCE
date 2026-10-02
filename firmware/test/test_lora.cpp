#include <cstring>
#include <deque>
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
  // Frames the gateway still owes us from earlier, readable before we send.
  std::deque<std::vector<uint8_t>> inbox;
  // Frames the gateway only emits once it has received an uplink, which is
  // what a real radio does: the answer cannot exist before the question.
  std::deque<std::vector<uint8_t>> afterSend;
  int receiveCalls{0};
  int readFailure{0};

  bool canSendNow() override { return canSend; }
  bool send(const uint8_t* data, size_t length) override {
    if (!sendOk) return false;
    sent.push_back(std::string(reinterpret_cast<const char*>(data), length));
    for (std::vector<uint8_t>& frame : afterSend) inbox.push_back(frame);
    afterSend.clear();
    return true;
  }
  int receive(uint8_t* buffer, size_t capacity) override {
    ++receiveCalls;
    if (readFailure != 0) return -1;
    if (inbox.empty()) return 0;
    const std::vector<uint8_t>& frame = inbox.front();
    if (frame.size() > capacity) return -1;
    std::memcpy(buffer, frame.data(), frame.size());
    inbox.pop_front();
    return static_cast<int>(frame.size());
  }
  int16_t lastRssiDbm() const override { return rssi; }

  void queueAck(uint32_t sequence) {
    uint8_t frame[LoRaSyncTransport::kAckFrameSize];
    LoRaSyncTransport::encodeAck(frame, sequence);
    afterSend.emplace_back(frame, frame + sizeof(frame));
  }
  // An answer that arrives before we transmit anything: a leftover.
  void queueStaleAck(uint32_t sequence) {
    uint8_t frame[LoRaSyncTransport::kAckFrameSize];
    LoRaSyncTransport::encodeAck(frame, sequence);
    inbox.emplace_back(frame, frame + sizeof(frame));
  }
  void queueRaw(std::vector<uint8_t> frame) { afterSend.push_back(frame); }
  void queueStaleRaw(std::vector<uint8_t> frame) { inbox.push_back(frame); }
};

LoRaSyncTransport makeTransport(FakeRadio& radio, hal::ManualClock& clock) {
  LoRaSyncTransport transport(radio, clock);
  transport.setMinIntervalMs(0);
  transport.setAckTimeoutMs(1000);
  transport.setAckPollIntervalMs(50);
  return transport;
}

}  // namespace

void test_lora_rejects_oversize_payload() {
  hal::ManualClock clock(1787356800000ULL);
  FakeRadio radio;
  LoRaSyncTransport transport = makeTransport(radio, clock);
  transport.setMaxPayloadBytes(16);
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
  transport.setAckTimeoutMs(1000);
  uint32_t ack = 0;
  const char* payload = "{\"a\":1}";
  radio.queueAck(1);
  TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::Ok),
                    static_cast<int>(transport.postBatch(
                        payload, std::strlen(payload), nullptr, 1000, ack)));
  const auto second = transport.postBatch(
      payload, std::strlen(payload), nullptr, 1000, ack);
  TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::NetworkError),
                    static_cast<int>(second));
  clock.advanceMs(600000);
  radio.queueAck(2);
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
  LoRaSyncTransport transport = makeTransport(radio, clock);
  uint32_t ack = 0;
  const char* payload = "{\"a\":1}";
  auto result = transport.postBatch(
      payload, std::strlen(payload), nullptr, 1000, ack);
  TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::NetworkError),
                    static_cast<int>(result));
  radio.canSend = true;
  radio.sendOk = false;
  result = transport.postBatch(
      payload, std::strlen(payload), nullptr, 1000, ack);
  TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::NetworkError),
                    static_cast<int>(result));
}

// The regression this whole change exists for: a frame the gateway never
// received must not be reported as delivered, because SyncManager would then
// advance the watermark and the records would be gone for good.
void test_lora_without_acknowledgement_reports_network_error() {
  hal::ManualClock clock(1787356800000ULL);
  FakeRadio radio;
  LoRaSyncTransport transport = makeTransport(radio, clock);
  uint32_t ack = 12345;
  const char* payload = "{\"a\":1}";
  const auto result = transport.postBatch(
      payload, std::strlen(payload), nullptr, 1000, ack);
  TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::NetworkError),
                    static_cast<int>(result));
  TEST_ASSERT_FALSE(transport.lastDeliveryConfirmed());
  TEST_ASSERT_EQUAL_UINT32(0, ack);
  TEST_ASSERT_EQUAL(1, static_cast<int>(radio.sent.size()));
  TEST_ASSERT_GREATER_THAN(0, radio.receiveCalls);
}

void test_lora_acknowledgement_carries_the_confirmed_sequence() {
  hal::ManualClock clock(1787356800000ULL);
  FakeRadio radio;
  LoRaSyncTransport transport = makeTransport(radio, clock);
  radio.queueAck(0x01020304u);
  uint32_t ack = 0;
  const char* payload = "{\"a\":1}";
  const auto result = transport.postBatch(
      payload, std::strlen(payload), nullptr, 1000, ack);
  TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::Ok),
                    static_cast<int>(result));
  TEST_ASSERT_TRUE(transport.lastDeliveryConfirmed());
  TEST_ASSERT_EQUAL_UINT32(0x01020304u, ack);
}

void test_lora_ignores_corrupt_acknowledgements() {
  hal::ManualClock clock(1787356800000ULL);
  {
    FakeRadio radio;
    LoRaSyncTransport transport = makeTransport(radio, clock);
    // Bad magic.
    radio.queueRaw({0x00, 0, 0, 0, 7, 0});
    uint32_t ack = 0;
    const char* payload = "{\"a\":1}";
    TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::NetworkError),
                      static_cast<int>(transport.postBatch(
                          payload, std::strlen(payload), nullptr, 1000, ack)));
  }
  {
    FakeRadio radio;
    LoRaSyncTransport transport = makeTransport(radio, clock);
    // Right magic, wrong checksum.
    radio.queueRaw({LoRaSyncTransport::kAckMagic, 0, 0, 0, 7, 0xFF});
    uint32_t ack = 0;
    const char* payload = "{\"a\":1}";
    TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::NetworkError),
                      static_cast<int>(transport.postBatch(
                          payload, std::strlen(payload), nullptr, 1000, ack)));
  }
  {
    FakeRadio radio;
    LoRaSyncTransport transport = makeTransport(radio, clock);
    // Right magic, truncated.
    radio.queueRaw({LoRaSyncTransport::kAckMagic, 0, 0});
    uint32_t ack = 0;
    const char* payload = "{\"a\":1}";
    TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::NetworkError),
                      static_cast<int>(transport.postBatch(
                          payload, std::strlen(payload), nullptr, 1000, ack)));
  }
}

void test_lora_drains_a_stale_acknowledgement_before_sending() {
  hal::ManualClock clock(1787356800000ULL);
  FakeRadio radio;
  LoRaSyncTransport transport = makeTransport(radio, clock);
  // An answer to a batch sent long ago, arriving now.
  radio.queueStaleAck(42);
  radio.queueStaleAck(43);
  uint32_t ack = 0;
  const char* payload = "{\"a\":1}";
  const auto result = transport.postBatch(
      payload, std::strlen(payload), nullptr, 1000, ack);
  TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::NetworkError),
                    static_cast<int>(result));
  TEST_ASSERT_FALSE(transport.lastDeliveryConfirmed());
  TEST_ASSERT_EQUAL_UINT32(0, ack);
}

void test_lora_ack_roundtrip_and_validation() {
  uint8_t frame[LoRaSyncTransport::kAckFrameSize];
  LoRaSyncTransport::encodeAck(frame, 0xDEADBEEF);
  uint32_t parsed = 0;
  TEST_ASSERT_TRUE(LoRaSyncTransport::parseAck(frame, sizeof(frame), parsed));
  TEST_ASSERT_EQUAL_UINT32(0xDEADBEEF, parsed);

  TEST_ASSERT_FALSE(LoRaSyncTransport::parseAck(frame, 0, parsed));
  TEST_ASSERT_FALSE(LoRaSyncTransport::parseAck(frame, 5, parsed));
  TEST_ASSERT_FALSE(LoRaSyncTransport::parseAck(nullptr, sizeof(frame), parsed));

  uint8_t bad[LoRaSyncTransport::kAckFrameSize];
  LoRaSyncTransport::encodeAck(bad, 1);
  bad[5] ^= 0x01;
  TEST_ASSERT_FALSE(LoRaSyncTransport::parseAck(bad, sizeof(bad), parsed));
}

void test_lora_read_failure_is_a_network_error() {
  hal::ManualClock clock(1787356800000ULL);
  FakeRadio radio;
  LoRaSyncTransport transport = makeTransport(radio, clock);
  radio.readFailure = -1;
  uint32_t ack = 0;
  const char* payload = "{\"a\":1}";
  TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::NetworkError),
                    static_cast<int>(transport.postBatch(
                        payload, std::strlen(payload), nullptr, 1000, ack)));
}

void registerLoRaTests() {
  RUN_TEST(test_lora_rejects_oversize_payload);
  RUN_TEST(test_lora_enforces_min_interval);
  RUN_TEST(test_lora_radio_down_maps_to_network_error);
  RUN_TEST(test_lora_without_acknowledgement_reports_network_error);
  RUN_TEST(test_lora_acknowledgement_carries_the_confirmed_sequence);
  RUN_TEST(test_lora_ignores_corrupt_acknowledgements);
  RUN_TEST(test_lora_drains_a_stale_acknowledgement_before_sending);
  RUN_TEST(test_lora_ack_roundtrip_and_validation);
  RUN_TEST(test_lora_read_failure_is_a_network_error);
}
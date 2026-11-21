#include <cstring>
#include <vector>

#include <unity.h>

#include "cauce/app/LoRaSyncTransport.h"
#include "cauce/core/SerialBridge.h"

using namespace cauce;

namespace {

// Same bytes as KNOWN_ACK_FRAME in backend/tests/test_gateway_runner.py: an acknowledgement for
// sequence 7, framed for transmission. Two independent implementations agreeing on this frame is
// what keeps the bridge and the runner speaking the same protocol.
const uint8_t kKnownAckFrame[] = {0xC5, 0xA0, 0x02, 0x00, 0x06, 0xA5, 0x00,
                                  0x00, 0x00, 0x07, 0xA2, 0xD1, 0x26};

std::vector<uint8_t> framed(bridge::FrameType type, const uint8_t* payload, size_t length) {
  std::vector<uint8_t> out(length + bridge::kOverheadBytes);
  const size_t n = bridge::encode(type, payload, length, out.data(), out.size());
  out.resize(n);
  return out;
}

}  // namespace

void test_serial_bridge_matches_the_python_known_answer() {
  uint8_t ack[app::LoRaSyncTransport::kAckFrameSize];
  app::LoRaSyncTransport::encodeAck(ack, 7);
  const std::vector<uint8_t> frame = framed(bridge::FrameType::RadioTransmit, ack, sizeof(ack));
  TEST_ASSERT_EQUAL(sizeof(kKnownAckFrame), frame.size());
  TEST_ASSERT_EQUAL_HEX8_ARRAY(kKnownAckFrame, frame.data(), sizeof(kKnownAckFrame));
}

void test_serial_bridge_decodes_after_garbage_and_rejects_a_bad_crc() {
  const char text[] = "HEARTBEAT radio=up";
  std::vector<uint8_t> good =
      framed(bridge::FrameType::Status, reinterpret_cast<const uint8_t*>(text), strlen(text));
  std::vector<uint8_t> corrupt = good;
  corrupt.back() ^= 0xFF;

  std::vector<uint8_t> stream = {0x00, 0xC5, 0x13, 0xC5};
  stream.insert(stream.end(), corrupt.begin(), corrupt.end());
  stream.insert(stream.end(), good.begin(), good.end());

  bridge::Decoder decoder;
  int completed = 0;
  for (uint8_t byte : stream) {
    if (decoder.push(byte)) {
      ++completed;
      TEST_ASSERT_EQUAL(static_cast<int>(bridge::FrameType::Status),
                        static_cast<int>(decoder.type()));
      TEST_ASSERT_EQUAL(strlen(text), decoder.length());
      TEST_ASSERT_EQUAL_MEMORY(text, decoder.payload(), strlen(text));
    }
  }
  TEST_ASSERT_EQUAL(1, completed);
  TEST_ASSERT_EQUAL(1u, decoder.errors());
}

void test_serial_bridge_refuses_what_it_cannot_carry() {
  std::vector<uint8_t> big(bridge::kMaxPayloadBytes + 1, 0x55);
  uint8_t out[bridge::kMaxPayloadBytes + bridge::kOverheadBytes + 8];
  TEST_ASSERT_EQUAL(0u, bridge::encode(bridge::FrameType::RadioReceived, big.data(), big.size(),
                                       out, sizeof(out)));
  // A length field larger than any real frame is dropped instead of waited for.
  bridge::Decoder decoder;
  const uint8_t bogus[] = {0xC5, 0xA0, 0x01, 0xEA, 0x60};
  for (uint8_t b : bogus) TEST_ASSERT_FALSE(decoder.push(b));
  TEST_ASSERT_EQUAL(1u, decoder.errors());
}

void test_serial_bridge_carries_a_full_lora_frame_with_its_rssi() {
  uint8_t payload[2 + 255];
  payload[0] = 0xFF;  // -97 dBm, big-endian
  payload[1] = 0x9F;
  for (size_t i = 2; i < sizeof(payload); ++i) payload[i] = static_cast<uint8_t>(i);
  const std::vector<uint8_t> frame =
      framed(bridge::FrameType::RadioReceived, payload, sizeof(payload));
  TEST_ASSERT_EQUAL(sizeof(payload) + bridge::kOverheadBytes, frame.size());
  bridge::Decoder decoder;
  bool done = false;
  for (uint8_t b : frame) done = decoder.push(b);
  TEST_ASSERT_TRUE(done);
  TEST_ASSERT_EQUAL_MEMORY(payload, decoder.payload(), sizeof(payload));
}

void registerSerialBridgeTests() {
  RUN_TEST(test_serial_bridge_matches_the_python_known_answer);
  RUN_TEST(test_serial_bridge_decodes_after_garbage_and_rejects_a_bad_crc);
  RUN_TEST(test_serial_bridge_refuses_what_it_cannot_carry);
  RUN_TEST(test_serial_bridge_carries_a_full_lora_frame_with_its_rssi);
}

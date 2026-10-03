#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include <unity.h>

#include "cauce/app/LoRaSyncTransport.h"
#include "cauce/core/SecurityUtils.h"
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

// The compact frame only exists if the JSON batch parses into records, so the
// tests have to send a realistic payload rather than a stub object.
std::string syncJson(size_t records) {
  std::string out = "{\"protocol_version\":1,\"node_id\":\"CAUCE-001\",\"batch_size\":";
  out += std::to_string(records);
  out += ",\"measurements\":[";
  for (size_t i = 0; i < records; ++i) {
    if (i) out += ",";
    out += "{\"node_id\":\"CAUCE-001\",\"sensor_id\":\"BME280-1\",\"sequence\":";
    out += std::to_string(i + 1);
    out += ",\"timestamp_utc_ms\":1787356800000,\"variable\":\"air_temperature\",\"value\":";
    out += std::to_string(20 + static_cast<int>(i));
    out += ",\"unit\":\"C\",\"quality\":\"VALID\",\"reason_bits\":0,\"time_uncertain\":false}";
  }
  out += "]}";
  return out;
}

LoRaSyncTransport makeTransport(FakeRadio& radio, hal::ManualClock& clock) {
  LoRaSyncTransport transport(radio, clock);
  // The JSON envelope is the input; the radio budget applies to the frame.
  transport.setMaxPayloadBytes(4096);
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
  transport.setMaxPayloadBytes(4096);
  transport.setMinIntervalMs(600000);
  transport.setAckTimeoutMs(1000);
  transport.setAckPollIntervalMs(50);
  uint32_t ack = 0;
  const std::string body = syncJson(1);
  radio.queueAck(1);
  TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::Ok),
                    static_cast<int>(transport.postBatch(
                        body.c_str(), body.size(), nullptr, 1000, ack)));
  const auto second = transport.postBatch(
      body.c_str(), body.size(), nullptr, 1000, ack);
  TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::NetworkError),
                    static_cast<int>(second));
  clock.advanceMs(600000);
  radio.queueAck(2);
  const auto third = transport.postBatch(
      body.c_str(), body.size(), nullptr, 1000, ack);
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
  const std::string body = syncJson(1);
  auto result = transport.postBatch(
      body.c_str(), body.size(), nullptr, 1000, ack);
  TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::NetworkError),
                    static_cast<int>(result));
  radio.canSend = true;
  radio.sendOk = false;
  result = transport.postBatch(
      body.c_str(), body.size(), nullptr, 1000, ack);
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
  const std::string body = syncJson(1);
  const auto result = transport.postBatch(
      body.c_str(), body.size(), nullptr, 1000, ack);
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
  const std::string body = syncJson(1);
  const auto result = transport.postBatch(
      body.c_str(), body.size(), nullptr, 1000, ack);
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
    const std::string body = syncJson(1);
    TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::NetworkError),
                      static_cast<int>(transport.postBatch(
                          body.c_str(), body.size(), nullptr, 1000, ack)));
  }
  {
    FakeRadio radio;
    LoRaSyncTransport transport = makeTransport(radio, clock);
    // Right magic, wrong checksum.
    radio.queueRaw({LoRaSyncTransport::kAckMagic, 0, 0, 0, 7, 0xFF});
    uint32_t ack = 0;
    const std::string body = syncJson(1);
    TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::NetworkError),
                      static_cast<int>(transport.postBatch(
                          body.c_str(), body.size(), nullptr, 1000, ack)));
  }
  {
    FakeRadio radio;
    LoRaSyncTransport transport = makeTransport(radio, clock);
    // Right magic, truncated.
    radio.queueRaw({LoRaSyncTransport::kAckMagic, 0, 0});
    uint32_t ack = 0;
    const std::string body = syncJson(1);
    TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::NetworkError),
                      static_cast<int>(transport.postBatch(
                          body.c_str(), body.size(), nullptr, 1000, ack)));
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
  const std::string body = syncJson(1);
  const auto result = transport.postBatch(
      body.c_str(), body.size(), nullptr, 1000, ack);
  TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::NetworkError),
                    static_cast<int>(result));
  TEST_ASSERT_FALSE(transport.lastDeliveryConfirmed());
  TEST_ASSERT_EQUAL_UINT32(0, ack);
}

// A node that signs its own frames is what lets a relay stay keyless, so the
// signature has to be byte-exact with what the central expects. The digest
// below is HMAC-SHA256("clave-dispositivo-0123456789", bytes 1..24), pinned
// against the Python implementation the server uses.
void test_signing_appends_a_known_hmac() {
  uint8_t frame[24];
  for (size_t i = 0; i < sizeof(frame); ++i) {
    frame[i] = static_cast<uint8_t>(i + 1);
  }
  const char* key = "clave-dispositivo-0123456789";

  uint8_t out[64] = {0};
  const size_t length = signFrame(frame, sizeof(frame),
                                  reinterpret_cast<const uint8_t*>(key),
                                  std::strlen(key), out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT32(sizeof(frame) + 32, length);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, out, sizeof(frame));

  static const uint8_t expected[32] = {
      0xa9, 0xe6, 0x47, 0xc7, 0x2c, 0x1a, 0xeb, 0xde, 0x5c, 0xa6, 0xb6,
      0x90, 0x9c, 0xce, 0x8d, 0x3a, 0xdf, 0xb4, 0x20, 0xa2, 0x4f, 0x2b,
      0xd2, 0xda, 0x7d, 0xd8, 0x09, 0x58, 0x75, 0xda, 0x88, 0x16};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out + sizeof(frame), 32);
}

void test_signing_refuses_a_missing_key_or_a_small_buffer() {
  uint8_t frame[24] = {0};
  uint8_t out[64] = {0};
  const char* key = "clave-dispositivo-0123456789";

  TEST_ASSERT_EQUAL_UINT32(
      0, signFrame(frame, sizeof(frame), nullptr, 0, out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT32(
      0, signFrame(frame, sizeof(frame),
                   reinterpret_cast<const uint8_t*>(key), 0, out, sizeof(out)));
  // One byte short of frame + signature: refuse rather than truncate.
  TEST_ASSERT_EQUAL_UINT32(
      0, signFrame(frame, sizeof(frame),
                   reinterpret_cast<const uint8_t*>(key), std::strlen(key),
                   out, sizeof(frame) + 31));
  TEST_ASSERT_EQUAL_UINT32(
      0, signFrame(nullptr, sizeof(frame),
                   reinterpret_cast<const uint8_t*>(key), std::strlen(key),
                   out, sizeof(out)));
}

void test_a_signed_frame_is_the_unsigned_frame_plus_thirty_two_bytes() {
  const char* key = "clave-dispositivo-0123456789";
  FakeRadio radio;
  hal::ManualClock clock;
  LoRaSyncTransport transport(radio, clock);

  std::string payload =
      "{\"node_id\":\"CAUCE-001\",\"measurements\":[{\"sequence\":1,"
      "\"timestamp_utc_ms\":1787356800000,\"variable\":\"air_temperature\","
      "\"value\":20.5,\"unit\":\"degC\",\"quality\":\"good\"}]}";

  uint32_t acked = 0;
  transport.postBatch(payload.c_str(), payload.size(), nullptr, 0, acked);
  TEST_ASSERT_TRUE(!radio.sent.empty());
  const std::string unsignedFrame = radio.sent.back();

  // A second transport, because the first is rate-limited and would refuse to
  // send a second time instead of letting this test observe both frames.
  LoRaSyncTransport signing(radio, clock);
  signing.setDeviceKey(reinterpret_cast<const uint8_t*>(key),
                       std::strlen(key));
  TEST_ASSERT_TRUE(signing.hasDeviceKey());
  signing.postBatch(payload.c_str(), payload.size(), nullptr, 0, acked);
  TEST_ASSERT_EQUAL_UINT32(radio.sent.size(), 2);
  const std::string signedFrame = radio.sent.back();

  TEST_ASSERT_EQUAL_UINT32(unsignedFrame.size() + 32, signedFrame.size());
  // Same framing and same CRC; only the trailing HMAC is added.
  TEST_ASSERT_EQUAL_UINT8_ARRAY(unsignedFrame.data(), signedFrame.data(),
                                unsignedFrame.size());
}

void test_tampering_with_a_signed_frame_breaks_its_hmac() {
  uint8_t frame[24];
  for (size_t i = 0; i < sizeof(frame); ++i) {
    frame[i] = static_cast<uint8_t>(i + 1);
  }
  const char* key = "clave-dispositivo-0123456789";
  uint8_t signedFrame[64] = {0};
  const size_t length =
      signFrame(frame, sizeof(frame),
                reinterpret_cast<const uint8_t*>(key), std::strlen(key),
                signedFrame, sizeof(signedFrame));
  TEST_ASSERT_EQUAL_UINT32(sizeof(frame) + 32, length);

  // Recomputing over a tampered body must not reproduce the signature that
  // travelled with it: that is what makes a forged frame detectable.
  uint8_t forged[24];
  std::memcpy(forged, frame, sizeof(frame));
  forged[3] = static_cast<uint8_t>(forged[3] ^ 0xFF);
  uint8_t mac[32];
  hmacSha256(reinterpret_cast<const uint8_t*>(key), std::strlen(key), forged,
             sizeof(forged), mac);
  TEST_ASSERT_TRUE(std::memcmp(mac, signedFrame + sizeof(frame), 32) != 0);
}

void test_the_signature_covers_the_crc() {
  // A frame is header + payload + CRC trailer, and the trailer is inside the
  // signed region. That matters because the CRC is not a security control: an
  // attacker can repair it after changing a value. The HMAC is what has to
  // notice.
  //
  // The frame is built by hand rather than through the encoder, because the
  // property under test is about which bytes are covered, not about framing.
  uint8_t frame[LoRaSyncTransport::LoRaBatchHeaderSize + 30 +
                LoRaSyncTransport::LoRaTrailerSize];
  for (size_t i = 0; i < sizeof(frame); ++i) {
    frame[i] = static_cast<uint8_t>(0x10 + i);
  }
  const size_t frameLength = sizeof(frame);
  const size_t trailerIndex = frameLength - LoRaSyncTransport::LoRaTrailerSize;

  const char* key = "clave-dispositivo-0123456789";
  uint8_t signedFrame[128] = {0};
  const size_t length =
      signFrame(frame, frameLength,
                reinterpret_cast<const uint8_t*>(key), std::strlen(key),
                signedFrame, sizeof(signedFrame));
  TEST_ASSERT_EQUAL_UINT32(frameLength + 32, length);

  // Repairing the CRC after a value change leaves a frame that still looks
  // intact; its HMAC no longer matches the one that travelled with it.
  uint8_t altered[128];
  std::memcpy(altered, frame, frameLength);
  altered[trailerIndex] = static_cast<uint8_t>(altered[trailerIndex] ^ 0xFF);
  uint8_t mac[32];
  hmacSha256(reinterpret_cast<const uint8_t*>(key), std::strlen(key), altered,
             frameLength, mac);
  TEST_ASSERT_TRUE(std::memcmp(mac, signedFrame + frameLength, 32) != 0);

  // And the same holds for a changed payload byte with the trailer left alone.
  uint8_t alteredValue[128];
  std::memcpy(alteredValue, frame, frameLength);
  alteredValue[LoRaSyncTransport::LoRaBatchHeaderSize] = static_cast<uint8_t>(
      alteredValue[LoRaSyncTransport::LoRaBatchHeaderSize] ^ 0xFF);
  hmacSha256(reinterpret_cast<const uint8_t*>(key), std::strlen(key),
             alteredValue, frameLength, mac);
  TEST_ASSERT_TRUE(std::memcmp(mac, signedFrame + frameLength, 32) != 0);
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
  const std::string body = syncJson(1);
  TEST_ASSERT_EQUAL(static_cast<int>(hal::ISyncTransport::Result::NetworkError),
                    static_cast<int>(transport.postBatch(
                        body.c_str(), body.size(), nullptr, 1000, ack)));
}

void test_lora_batch_json_parses_into_compact_records() {
  const std::string body = syncJson(3);
  Measurement out[8];
  const size_t n = LoRaSyncTransport::parseBatchJson(body.c_str(), body.size(),
                                                     out, 8);
  TEST_ASSERT_EQUAL(3, n);
  for (size_t i = 0; i < n; ++i) {
    TEST_ASSERT_EQUAL_UINT32(i + 1, out[i].sequence);
    TEST_ASSERT_EQUAL_UINT64(1787356800000ULL, out[i].timestampUtcMs);
    TEST_ASSERT_EQUAL_STRING("CAUCE-001", out[i].nodeId);
    TEST_ASSERT_EQUAL_STRING("BME280-1", out[i].sensorId);
    TEST_ASSERT_EQUAL_FLOAT(static_cast<float>(20 + i), out[i].value);
    TEST_ASSERT_EQUAL(static_cast<int>(Variable::AirTemperature),
                      static_cast<int>(out[i].variable));
    TEST_ASSERT_FALSE(out[i].timeUncertain);
  }
}

void test_lora_batch_json_refuses_what_it_cannot_carry() {
  Measurement out[8];
  TEST_ASSERT_EQUAL(
      0u, LoRaSyncTransport::parseBatchJson("{\"a\":1}", 7, out, 8));
  TEST_ASSERT_EQUAL(
      0u, LoRaSyncTransport::parseBatchJson(nullptr, 0, out, 8));
  TEST_ASSERT_EQUAL(
      0u, LoRaSyncTransport::parseBatchJson("{\"measurements\":[]}", 18, out, 8));
  // A record with no sequence is not a record.
  const std::string noSeq =
      "{\"measurements\":[{\"variable\":\"air_temperature\",\"value\":1}]}";
  TEST_ASSERT_EQUAL(0u, LoRaSyncTransport::parseBatchJson(
                            noSeq.c_str(), noSeq.size(), out, 8));
  // An unknown variable cannot be framed honestly either.
  const std::string oddVar =
      "{\"measurements\":[{\"sequence\":1,\"variable\":\"photon_flux\","
      "\"value\":1}]}";
  TEST_ASSERT_EQUAL(0u, LoRaSyncTransport::parseBatchJson(
                            oddVar.c_str(), oddVar.size(), out, 8));
}

void test_lora_batch_json_stops_at_the_capacity_it_is_given() {
  const std::string body = syncJson(5);
  Measurement out[2];
  const size_t n = LoRaSyncTransport::parseBatchJson(body.c_str(), body.size(),
                                                     out, 2);
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_EQUAL_UINT32(1, out[0].sequence);
  TEST_ASSERT_EQUAL_UINT32(2, out[1].sequence);
}

void test_lora_sends_compact_frames_not_json() {
  hal::ManualClock clock(1787356800000ULL);
  FakeRadio radio;
  LoRaSyncTransport transport = makeTransport(radio, clock);
  radio.queueAck(9);
  uint32_t ack = 0;
  // Four records at SF9 is one record per frame, so four uplinks.
  const std::string body = syncJson(4);
  TEST_ASSERT_EQUAL(
      static_cast<int>(hal::ISyncTransport::Result::Ok),
      static_cast<int>(transport.postBatch(body.c_str(), body.size(), nullptr,
                                           1000, ack)));
  TEST_ASSERT_EQUAL(4, static_cast<int>(radio.sent.size()));
  for (const std::string& frame : radio.sent) {
    // 12 B of header/trailer plus one 60 B record.
    TEST_ASSERT_EQUAL_UINT32(72, static_cast<uint32_t>(frame.size()));
    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        '"', frame[0],
        "a raw JSON batch was put on the radio instead of a binary frame");
  }
  TEST_ASSERT_EQUAL_UINT32(9, ack);
}

void test_lora_a_wider_budget_sends_fewer_uplinks() {
  hal::ManualClock clock(1787356800000ULL);
  FakeRadio radio;
  LoRaSyncTransport transport = makeTransport(radio, clock);
  // SF7 fits three records per uplink, so four records take two.
  transport.setRadioPayloadBytes(222);
  radio.queueAck(9);
  uint32_t ack = 0;
  const std::string body = syncJson(4);
  TEST_ASSERT_EQUAL(
      static_cast<int>(hal::ISyncTransport::Result::Ok),
      static_cast<int>(transport.postBatch(body.c_str(), body.size(), nullptr,
                                           1000, ack)));
  TEST_ASSERT_EQUAL(2, static_cast<int>(radio.sent.size()));
  TEST_ASSERT_EQUAL_UINT32(192, static_cast<uint32_t>(radio.sent[0].size()));
  TEST_ASSERT_EQUAL_UINT32(72, static_cast<uint32_t>(radio.sent[1].size()));
}

void test_lora_refuses_a_payload_it_cannot_frame_honestly() {
  hal::ManualClock clock(1787356800000ULL);
  FakeRadio radio;
  LoRaSyncTransport transport = makeTransport(radio, clock);
  transport.setRadioPayloadBytes(51);  // SF10: too small for a 60 B record
  radio.queueAck(9);
  uint32_t ack = 0;
  const std::string body = syncJson(1);
  TEST_ASSERT_EQUAL(
      static_cast<int>(hal::ISyncTransport::Result::NetworkError),
      static_cast<int>(transport.postBatch(body.c_str(), body.size(), nullptr,
                                           1000, ack)));
  TEST_ASSERT_TRUE(radio.sent.empty());
}

void test_lora_ack_matches_the_vectors_the_gateway_produces() {
  // Pinned cross-language vectors. `backend/cauce_server/lora_frames.py`
  // builds the acknowledgement the gateway sends; these are the exact bytes it
  // produces for the same sequences. If either side changes byte order or
  // checksum convention, the node stops understanding its gateway, and nothing
  // else in the suite would notice because both sides test in isolation.
  struct Vector {
    uint32_t sequence;
    const char* hex;
  };
  const Vector vectors[] = {
      {0u, "a500000000a5"},
      {1u, "a500000001a4"},
      {4242u, "a50000109227"},
      {0xFFFFFFFFu, "a5ffffffffa5"},
      {0xDEADBEEFu, "a5deadbeef87"},
  };

  for (const Vector& v : vectors) {
    uint8_t frame[LoRaSyncTransport::kAckFrameSize];
    LoRaSyncTransport::encodeAck(frame, v.sequence);
    char produced[2 * LoRaSyncTransport::kAckFrameSize + 1] = {0};
    static const char* digits = "0123456789abcdef";
    for (size_t i = 0; i < LoRaSyncTransport::kAckFrameSize; ++i) {
      produced[i * 2] = digits[(frame[i] >> 4) & 0xF];
      produced[i * 2 + 1] = digits[frame[i] & 0xF];
    }
    TEST_ASSERT_EQUAL_STRING(v.hex, produced);

    uint32_t parsed = 0;
    TEST_ASSERT_TRUE(LoRaSyncTransport::parseAck(
        frame, LoRaSyncTransport::kAckFrameSize, parsed));
    TEST_ASSERT_EQUAL_UINT32(v.sequence, parsed);
  }
}

void registerLoRaTests() {
  RUN_TEST(test_lora_batch_json_parses_into_compact_records);
  RUN_TEST(test_lora_batch_json_refuses_what_it_cannot_carry);
  RUN_TEST(test_lora_batch_json_stops_at_the_capacity_it_is_given);
  RUN_TEST(test_lora_ack_matches_the_vectors_the_gateway_produces);
  RUN_TEST(test_lora_sends_compact_frames_not_json);
  RUN_TEST(test_lora_a_wider_budget_sends_fewer_uplinks);
  RUN_TEST(test_lora_refuses_a_payload_it_cannot_frame_honestly);
  RUN_TEST(test_lora_rejects_oversize_payload);
  RUN_TEST(test_lora_enforces_min_interval);
  RUN_TEST(test_lora_radio_down_maps_to_network_error);
  RUN_TEST(test_lora_without_acknowledgement_reports_network_error);
  RUN_TEST(test_lora_acknowledgement_carries_the_confirmed_sequence);
  RUN_TEST(test_lora_ignores_corrupt_acknowledgements);
  RUN_TEST(test_lora_drains_a_stale_acknowledgement_before_sending);
  RUN_TEST(test_lora_ack_roundtrip_and_validation);
  RUN_TEST(test_lora_read_failure_is_a_network_error);
  RUN_TEST(test_signing_appends_a_known_hmac);
  RUN_TEST(test_signing_refuses_a_missing_key_or_a_small_buffer);
  RUN_TEST(test_a_signed_frame_is_the_unsigned_frame_plus_thirty_two_bytes);
  RUN_TEST(test_tampering_with_a_signed_frame_breaks_its_hmac);
  RUN_TEST(test_the_signature_covers_the_crc);
}
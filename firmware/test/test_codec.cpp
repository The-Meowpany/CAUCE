#include <cmath>
#include <cstring>

#include <unity.h>

#include "cauce/core/RecordCodec.h"
#include "cauce/core/ValidationEngine.h"

using namespace cauce;

namespace {

Measurement sampleMeasurement() {
  Measurement m{};
  copyString(m.nodeId, sizeof(m.nodeId), "CAUCE-001");
  copyString(m.sensorId, sizeof(m.sensorId), "BME280-1");
  m.sequence = 1842;
  m.timestampUtcMs = 1787356800000ULL + 12345;
  m.value = -4.75f;
  m.variable = Variable::AirTemperature;
  m.quality = Quality::Suspect;
  m.reasonBits = kReasonRateOfChange | kReasonTimeUncertain;
  m.timeUncertain = true;
  return m;
}

}  // namespace

void test_crc32_known_vector() {
  const uint8_t data[] = "123456789";
  TEST_ASSERT_EQUAL_UINT32(0xCBF43926u, crc32(data, 9));
}

void test_frame_roundtrip_preserves_fields() {
  const Measurement original = sampleMeasurement();
  uint8_t frame[kFrameSize];
  TEST_ASSERT_EQUAL_UINT(kFrameSize, encodeFrame(original, frame));

  Measurement decoded{};
  TEST_ASSERT_EQUAL(DecodeStatus::Ok, decodeFrame(frame, kFrameSize, decoded));
  TEST_ASSERT_EQUAL(original.sequence, decoded.sequence);
  TEST_ASSERT_EQUAL(original.timestampUtcMs, decoded.timestampUtcMs);
  TEST_ASSERT_EQUAL(original.variable, decoded.variable);
  TEST_ASSERT_EQUAL(original.quality, decoded.quality);
  TEST_ASSERT_EQUAL(original.reasonBits, decoded.reasonBits);
  TEST_ASSERT_TRUE(decoded.timeUncertain);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, original.value, decoded.value);
  TEST_ASSERT_EQUAL_STRING(original.nodeId, decoded.nodeId);
  TEST_ASSERT_EQUAL_STRING(original.sensorId, decoded.sensorId);
}

void test_flipped_bit_detected_by_crc() {
  const Measurement original = sampleMeasurement();
  uint8_t frame[kFrameSize];
  encodeFrame(original, frame);
  frame[10] ^= 0x40;
  Measurement decoded{};
  TEST_ASSERT_EQUAL(DecodeStatus::BadCrc, decodeFrame(frame, kFrameSize, decoded));
}

void test_bad_magic_rejected() {
  uint8_t frame[kFrameSize];
  encodeFrame(sampleMeasurement(), frame);
  frame[0] = 0x00;
  Measurement out{};
  TEST_ASSERT_EQUAL(DecodeStatus::BadMagic, decodeFrame(frame, kFrameSize, out));
}

void test_truncated_frame_rejected() {
  uint8_t frame[kFrameSize];
  encodeFrame(sampleMeasurement(), frame);
  Measurement out{};
  TEST_ASSERT_EQUAL(DecodeStatus::BadLength,
                    decodeFrame(frame, kFrameSize - 1, out));
}

void test_unterminated_node_id_is_terminated_on_decode() {
  const Measurement original = sampleMeasurement();
  uint8_t payload[kMeasurementPayloadSize];
  encodePayload(original, payload);
  std::memset(payload + 20, 'A', 16);

  Measurement decoded{};
  TEST_ASSERT_EQUAL(DecodeStatus::Ok, decodePayload(payload, decoded));
  TEST_ASSERT_EQUAL('\0', decoded.nodeId[15]);
  TEST_ASSERT_EQUAL(0, std::strncmp(decoded.nodeId, "AAAAAAAAAAAAAAA", 15));
}

void registerCodecTests() {

  RUN_TEST(test_crc32_known_vector);
  RUN_TEST(test_frame_roundtrip_preserves_fields);
  RUN_TEST(test_flipped_bit_detected_by_crc);
  RUN_TEST(test_bad_magic_rejected);
  RUN_TEST(test_truncated_frame_rejected);
  RUN_TEST(test_unterminated_node_id_is_terminated_on_decode);
}

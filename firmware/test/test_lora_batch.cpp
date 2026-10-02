#include <cstring>
#include <unity.h>

#include "cauce/core/LoRaBatchCodec.h"

using namespace cauce;

namespace {

void copyStr(char* dst, size_t cap, const char* src) {
  size_t i = 0;
  for (; src[i] != '\0' && i + 1 < cap; ++i) dst[i] = src[i];
  dst[i] = '\0';
}

Measurement makeM(uint32_t seq, uint64_t tsMs, float value) {
  Measurement m{};
  copyStr(m.nodeId, sizeof(m.nodeId), "CAUCE-001");
  copyStr(m.sensorId, sizeof(m.sensorId), "BME280-1");
  m.sequence = seq;
  m.timestampUtcMs = tsMs;
  m.value = value;
  m.variable = Variable::AirTemperature;
  m.quality = Quality::Valid;
  m.timeUncertain = false;
  return m;
}

void fill(Measurement* out, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    out[i] = makeM(static_cast<uint32_t>(i + 1),
                   1787356800000ULL + i * 60000ULL,
                   20.0f + static_cast<float>(i) * 0.25f);
  }
}

// The real wire size of a frame. LoRaBatchHeader::fragmentCount is the number
// of fragments, not a length, which is an easy mistake to make.
size_t frameSize(const LoRaBatchHeader& h) {
  return kLoRaOverhead + h.payloadBytes;
}

struct Encoded {
  uint8_t bytes[8][256];
  size_t sizes[8];
  size_t count{0};
  uint8_t announced{0};
};

bool encodeAll(Measurement* records, size_t n, uint16_t batchId, size_t budget,
               Encoded& out) {
  LoRaBatchEncoder encoder(records, n, batchId);
  uint8_t frame[256];
  LoRaBatchHeader header{};
  while (!encoder.done() && out.count < 8) {
    if (!encoder.nextFrame(frame, sizeof(frame), budget, header)) return false;
    out.announced = header.fragmentCount;
    std::memcpy(out.bytes[out.count], frame, frameSize(header));
    out.sizes[out.count] = frameSize(header);
    ++out.count;
  }
  return true;
}

}  // namespace

void test_lora_batch_roundtrips_through_every_fragment() {
  Measurement records[5];
  fill(records, 5);
  // 115 B is the SF9 payload ceiling: the tightest spread rate that still fits
  // one 60-byte record.
  Encoded encoded;
  TEST_ASSERT_TRUE(encodeAll(records, 5, 4242, 115, encoded));
  TEST_ASSERT_TRUE(encoded.count > 1);
  TEST_ASSERT_EQUAL(encoded.announced, encoded.count);

  LoRaBatchReassembler reassembler(4242);
  for (size_t i = 0; i < encoded.count; ++i) {
    TEST_ASSERT_EQUAL(static_cast<int>(LoRaBatchReassembler::Status::Ok),
                      static_cast<int>(
                          reassembler.add(encoded.bytes[i], encoded.sizes[i])));
  }
  TEST_ASSERT_TRUE(reassembler.complete());

  Measurement out[8];
  const size_t n = reassembler.copyRecords(out, 8);
  TEST_ASSERT_EQUAL(5, n);
  for (size_t i = 0; i < n; ++i) {
    TEST_ASSERT_EQUAL_UINT32(i + 1, out[i].sequence);
    TEST_ASSERT_EQUAL_UINT64(1787356800000ULL + i * 60000ULL,
                             out[i].timestampUtcMs);
    TEST_ASSERT_EQUAL_STRING("CAUCE-001", out[i].nodeId);
    TEST_ASSERT_EQUAL_STRING("BME280-1", out[i].sensorId);
    TEST_ASSERT_EQUAL_FLOAT(20.0f + static_cast<float>(i) * 0.25f, out[i].value);
    TEST_ASSERT_EQUAL(static_cast<int>(Quality::Valid),
                      static_cast<int>(out[i].quality));
  }
}

void test_wider_budget_packs_more_records_per_frame() {
  Measurement records[4];
  fill(records, 4);
  uint8_t frame[256];
  LoRaBatchHeader header{};

  LoRaBatchEncoder sf7(records, 4, 1);
  TEST_ASSERT_TRUE(sf7.nextFrame(frame, sizeof(frame), 222, header));
  // 222 - 12 = 210 usable, so three 60-byte records ride one uplink.
  TEST_ASSERT_EQUAL(3u, header.payloadBytes / kMeasurementPayloadSize);
  TEST_ASSERT_EQUAL(2, header.fragmentCount);

  LoRaBatchEncoder sf9(records, 4, 1);
  LoRaBatchHeader header9{};
  TEST_ASSERT_TRUE(sf9.nextFrame(frame, sizeof(frame), 115, header9));
  TEST_ASSERT_EQUAL(1u, header9.payloadBytes / kMeasurementPayloadSize);
  TEST_ASSERT_EQUAL(4, header9.fragmentCount);
}

void test_a_budget_that_cannot_hold_one_record_is_refused() {
  Measurement records[2];
  fill(records, 2);
  uint8_t frame[256];
  LoRaBatchHeader header{};
  // SF11 is 28 bytes. No record fits, and pretending otherwise would mean
  // silently dropping data.
  LoRaBatchEncoder encoder(records, 2, 1);
  TEST_ASSERT_FALSE(encoder.nextFrame(frame, sizeof(frame), 28, header));
}

void test_a_lost_fragment_means_the_batch_is_not_complete() {
  Measurement records[4];
  fill(records, 4);
  Encoded encoded;
  TEST_ASSERT_TRUE(encodeAll(records, 4, 77, 115, encoded));
  TEST_ASSERT_EQUAL(4, encoded.count);

  LoRaBatchReassembler reassembler(77);
  TEST_ASSERT_EQUAL(static_cast<int>(LoRaBatchReassembler::Status::Ok),
                    static_cast<int>(reassembler.add(encoded.bytes[0],
                                                     encoded.sizes[0])));
  // Fragment 1 never arrives.
  TEST_ASSERT_EQUAL(static_cast<int>(LoRaBatchReassembler::Status::Ok),
                    static_cast<int>(reassembler.add(encoded.bytes[2],
                                                     encoded.sizes[2])));
  TEST_ASSERT_FALSE(reassembler.complete());

  Measurement out[8];
  TEST_ASSERT_EQUAL(0u, reassembler.copyRecords(out, 8));

  TEST_ASSERT_EQUAL(static_cast<int>(LoRaBatchReassembler::Status::Ok),
                    static_cast<int>(reassembler.add(encoded.bytes[3],
                                                     encoded.sizes[3])));
  TEST_ASSERT_FALSE(reassembler.complete());

  // The late fragment still completes it: a batch is worth waiting for.
  TEST_ASSERT_EQUAL(static_cast<int>(LoRaBatchReassembler::Status::Ok),
                    static_cast<int>(reassembler.add(encoded.bytes[1],
                                                     encoded.sizes[1])));
  TEST_ASSERT_TRUE(reassembler.complete());
  TEST_ASSERT_EQUAL(4u, reassembler.copyRecords(out, 8));
}

void test_corrupt_frames_are_rejected_and_do_not_poison_the_batch() {
  Measurement records[1];
  fill(records, 1);
  uint8_t frame[256];
  LoRaBatchHeader header{};
  LoRaBatchEncoder encoder(records, 1, 5);
  TEST_ASSERT_TRUE(encoder.nextFrame(frame, sizeof(frame), 115, header));
  TEST_ASSERT_EQUAL(1, header.fragmentCount);
  const size_t len = frameSize(header);

  LoRaBatchReassembler reassembler(5);
  {
    uint8_t bad[256];
    std::memcpy(bad, frame, len);
    bad[1] = 0x00;  // magic, high byte little-endian
    TEST_ASSERT_EQUAL(static_cast<int>(LoRaBatchReassembler::Status::BadMagic),
                      static_cast<int>(reassembler.add(bad, len)));
  }
  {
    uint8_t bad[256];
    std::memcpy(bad, frame, len);
    bad[len - 1] ^= 0xFF;
    TEST_ASSERT_EQUAL(static_cast<int>(LoRaBatchReassembler::Status::BadCrc),
                      static_cast<int>(reassembler.add(bad, len)));
  }
  {
    uint8_t bad[256];
    std::memcpy(bad, frame, len);
    bad[0] = 0x09;  // unknown version, low byte
    TEST_ASSERT_EQUAL(
        static_cast<int>(LoRaBatchReassembler::Status::BadVersion),
        static_cast<int>(reassembler.add(bad, len)));
  }
  TEST_ASSERT_EQUAL(static_cast<int>(LoRaBatchReassembler::Status::TooShort),
                    static_cast<int>(reassembler.add(frame, 4)));
  TEST_ASSERT_EQUAL(static_cast<int>(LoRaBatchReassembler::Status::TooShort),
                    static_cast<int>(reassembler.add(nullptr, 0)));
  TEST_ASSERT_FALSE(reassembler.complete());

  // The intact frame must still be accepted: a bad frame is not a bad batch.
  TEST_ASSERT_EQUAL(static_cast<int>(LoRaBatchReassembler::Status::Ok),
                    static_cast<int>(reassembler.add(frame, len)));
  TEST_ASSERT_TRUE(reassembler.complete());
}

void test_a_frame_whose_length_contradicts_its_header_is_rejected() {
  Measurement records[1];
  fill(records, 1);
  uint8_t frame[256];
  LoRaBatchHeader header{};
  LoRaBatchEncoder encoder(records, 1, 6);
  TEST_ASSERT_TRUE(encoder.nextFrame(frame, sizeof(frame), 115, header));

  LoRaBatchReassembler reassembler(6);
  // Long enough to read the header, but not the length the header announces.
  const size_t stretched = frameSize(header) + 1;
  TEST_ASSERT_EQUAL(static_cast<int>(LoRaBatchReassembler::Status::BadCrc),
                    static_cast<int>(reassembler.add(frame, stretched)));
  // Short of a full record is caught even earlier.
  TEST_ASSERT_EQUAL(static_cast<int>(LoRaBatchReassembler::Status::TooShort),
                    static_cast<int>(reassembler.add(frame, frameSize(header) - 4)));
}

void test_a_fragment_from_another_batch_is_refused() {
  Measurement records[2];
  fill(records, 2);
  uint8_t frame[256];
  LoRaBatchHeader header{};
  LoRaBatchEncoder encoder(records, 2, 900);
  TEST_ASSERT_TRUE(encoder.nextFrame(frame, sizeof(frame), 115, header));

  LoRaBatchReassembler reassembler(901);
  TEST_ASSERT_EQUAL(
      static_cast<int>(LoRaBatchReassembler::Status::ForeignBatch),
      static_cast<int>(reassembler.add(frame, frameSize(header))));
  TEST_ASSERT_FALSE(reassembler.complete());
}

void test_a_duplicate_fragment_is_refused() {
  Measurement records[2];
  fill(records, 2);
  uint8_t frame[256];
  LoRaBatchHeader header{};
  LoRaBatchEncoder encoder(records, 2, 11);
  TEST_ASSERT_TRUE(encoder.nextFrame(frame, sizeof(frame), 115, header));

  LoRaBatchReassembler reassembler(11);
  TEST_ASSERT_EQUAL(static_cast<int>(LoRaBatchReassembler::Status::Ok),
                    static_cast<int>(reassembler.add(frame, frameSize(header))));
  TEST_ASSERT_EQUAL(
      static_cast<int>(LoRaBatchReassembler::Status::DuplicateFragment),
      static_cast<int>(reassembler.add(frame, frameSize(header))));
}

void test_out_of_order_fragments_reassemble() {
  Measurement records[3];
  fill(records, 3);
  Encoded encoded;
  TEST_ASSERT_TRUE(encodeAll(records, 3, 31, 115, encoded));
  TEST_ASSERT_EQUAL(3, encoded.count);

  LoRaBatchReassembler reassembler(31);
  // A radio promises nothing about ordering.
  for (int i = 2; i >= 0; --i) {
    TEST_ASSERT_EQUAL(static_cast<int>(LoRaBatchReassembler::Status::Ok),
                      static_cast<int>(reassembler.add(encoded.bytes[i],
                                                       encoded.sizes[i])));
  }
  TEST_ASSERT_TRUE(reassembler.complete());
  Measurement out[8];
  TEST_ASSERT_EQUAL(3u, reassembler.copyRecords(out, 8));
  for (size_t i = 0; i < 3; ++i) TEST_ASSERT_EQUAL_UINT32(i + 1, out[i].sequence);
}

void test_reset_abandons_a_batch_that_cannot_complete() {
  Measurement records[3];
  fill(records, 3);
  uint8_t frame[256];
  LoRaBatchHeader header{};
  LoRaBatchEncoder encoder(records, 3, 55);
  TEST_ASSERT_TRUE(encoder.nextFrame(frame, sizeof(frame), 115, header));

  LoRaBatchReassembler reassembler(55);
  reassembler.add(frame, frameSize(header));
  reassembler.reset();
  TEST_ASSERT_FALSE(reassembler.complete());
  TEST_ASSERT_EQUAL(0u, reassembler.recordCount());
}

void test_an_empty_batch_produces_nothing() {
  LoRaBatchEncoder encoder(nullptr, 0, 1);
  uint8_t frame[256];
  LoRaBatchHeader header{};
  TEST_ASSERT_FALSE(encoder.nextFrame(frame, sizeof(frame), 115, header));
  TEST_ASSERT_TRUE(encoder.done());
}

void registerLoRaBatchTests() {
  RUN_TEST(test_lora_batch_roundtrips_through_every_fragment);
  RUN_TEST(test_wider_budget_packs_more_records_per_frame);
  RUN_TEST(test_a_budget_that_cannot_hold_one_record_is_refused);
  RUN_TEST(test_a_lost_fragment_means_the_batch_is_not_complete);
  RUN_TEST(test_corrupt_frames_are_rejected_and_do_not_poison_the_batch);
  RUN_TEST(test_a_frame_whose_length_contradicts_its_header_is_rejected);
  RUN_TEST(test_a_fragment_from_another_batch_is_refused);
  RUN_TEST(test_a_duplicate_fragment_is_refused);
  RUN_TEST(test_out_of_order_fragments_reassemble);
  RUN_TEST(test_reset_abandons_a_batch_that_cannot_complete);
  RUN_TEST(test_an_empty_batch_produces_nothing);
}
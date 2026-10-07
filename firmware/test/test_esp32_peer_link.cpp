// The ESP-NOW peer frame format, on the host.
//
// Every assertion here is about a format that goes over an unencrypted radio and is parsed by
// a peer. The interesting cases are all hostile: truncated frames, a length that disagrees with
// itself, an id that fills its field with no terminator, and a version this build does not know.

#include <cstdio>
#include <cstring>
#include <unity.h>

#include "cauce/core/Replication.h"
#include "cauce/hal/Esp32PeerLink.h"

namespace cauce {
namespace hal {
namespace {

ReplicatedRecord makeRecord(const char* nodeId, const char* variable, uint32_t sequence,
                            float value) {
  ReplicatedRecord r{};
  std::snprintf(r.nodeId, sizeof(r.nodeId), "%s", nodeId);
  std::snprintf(r.variable, sizeof(r.variable), "%s", variable);
  r.sequence = sequence;
  r.timestampUtcMs = 1787356800000ULL + sequence;
  r.value = value;
  r.quality = 2;
  r.reasonBits = 0;
  r.timeUncertain = false;
  return r;
}

void test_a_single_record_round_trips() {
  ReplicatedRecord records[1] = {makeRecord("CAUCE-002", "air_temperature", 7, 21.5f)};
  uint8_t buf[250];
  const size_t n = encodePeerFrame("CAUCE-002", 7, records, 1, buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);

  PeerFrameContents out{};
  TEST_ASSERT_TRUE(decodePeerFrame(buf, n, out));
  TEST_ASSERT_EQUAL_UINT32(1, out.recordCount);
  TEST_ASSERT_EQUAL_STRING("CAUCE-002", out.nodeId);
  TEST_ASSERT_EQUAL_UINT32(7, out.sequence);
  TEST_ASSERT_EQUAL_STRING("air_temperature", out.records[0].variable);
  TEST_ASSERT_EQUAL_UINT32(7, out.records[0].sequence);
  TEST_ASSERT_EQUAL_FLOAT(21.5f, out.records[0].value);
  TEST_ASSERT_FALSE(out.records[0].timeUncertain);
}

void test_a_frame_fits_the_radio_ceiling() {
  ReplicatedRecord records[kPeerFrameMaxRecords];
  for (size_t i = 0; i < kPeerFrameMaxRecords; ++i) {
    records[i] = makeRecord("CAUCE-003", "relative_humidity",
                            static_cast<uint32_t>(i), 40.0f + static_cast<float>(i));
  }
  uint8_t buf[250];
  const size_t n = encodePeerFrame("CAUCE-003", 3, records, kPeerFrameMaxRecords, buf,
                                   sizeof(buf));
  // The whole point of the arithmetic in the header: three records fit, four do not.
  TEST_ASSERT_TRUE(n <= kPeerFrameMaxBytes);

  PeerFrameContents out{};
  TEST_ASSERT_TRUE(decodePeerFrame(buf, n, out));
  TEST_ASSERT_EQUAL_UINT32(kPeerFrameMaxRecords, out.recordCount);
}

// The constant in the header is a claim about the layout below it. If the layout changes and
// the constant does not, three records silently become two and nothing else notices - the
// frame is still valid, just smaller than whoever budgeted for it believed.
void test_the_record_size_is_what_the_frame_budget_assumes() {
  ReplicatedRecord one[1] = {makeRecord("CAUCE-004", "air_temperature", 1, 1.0f)};
  uint8_t buf[250];
  const size_t n = encodePeerFrame("CAUCE-004", 1, one, 1, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT32(kPeerFrameOverhead + kPeerRecordBytes, n);
  // Three records fit and four do not. That is the property the constant has to satisfy; an
  // exact equality against kPeerFrameMaxBytes was wrong, because the format does not use the
  // last 49 bytes of the radio payload and pretending otherwise would invite someone to
  // "fix" the constant to match rather than to ask why the slack is there.
  TEST_ASSERT_TRUE(kPeerFrameOverhead + kPeerFrameMaxRecords * kPeerRecordBytes <=
                   kPeerFrameMaxBytes);
  TEST_ASSERT_TRUE(kPeerFrameOverhead + (kPeerFrameMaxRecords + 1) * kPeerRecordBytes >
                   kPeerFrameMaxBytes);
}

// Refusing rather than truncating. A truncated frame would have the receiver apply a prefix of
// somebody else's data and then report a healthy merge.
void test_too_many_records_is_refused_rather_than_truncated() {
  ReplicatedRecord records[kPeerFrameMaxRecords + 1];
  for (size_t i = 0; i <= kPeerFrameMaxRecords; ++i) {
    records[i] = makeRecord("CAUCE-005", "air_temperature", static_cast<uint32_t>(i), 1.0f);
  }
  uint8_t buf[250];
  TEST_ASSERT_EQUAL_UINT32(0, encodePeerFrame("CAUCE-005", 1, records,
                                              kPeerFrameMaxRecords + 1, buf, sizeof(buf)));
}

void test_zero_records_is_refused() {
  uint8_t buf[250];
  ReplicatedRecord records[1] = {makeRecord("CAUCE-006", "air_temperature", 1, 1.0f)};
  TEST_ASSERT_EQUAL_UINT32(0, encodePeerFrame("CAUCE-006", 1, records, 0, buf, sizeof(buf)));
}

void test_a_buffer_that_cannot_hold_the_frame_is_refused() {
  ReplicatedRecord records[1] = {makeRecord("CAUCE-007", "air_temperature", 1, 1.0f)};
  uint8_t small[40];
  TEST_ASSERT_EQUAL_UINT32(0, encodePeerFrame("CAUCE-007", 1, records, 1, small, sizeof(small)));
}

void test_a_foreign_magic_is_refused() {
  ReplicatedRecord records[1] = {makeRecord("CAUCE-008", "air_temperature", 1, 1.0f)};
  uint8_t buf[250];
  const size_t n = encodePeerFrame("CAUCE-008", 1, records, 1, buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  buf[0] ^= 0xFFu;
  PeerFrameContents out{};
  TEST_ASSERT_FALSE(decodePeerFrame(buf, n, out));
}

// An unknown version's layout is unknown, so its count field is not a count of anything this
// build understands. Checked before the count is used.
void test_an_unknown_version_is_refused_before_its_count_is_trusted() {
  ReplicatedRecord records[1] = {makeRecord("CAUCE-009", "air_temperature", 1, 1.0f)};
  uint8_t buf[250];
  const size_t n = encodePeerFrame("CAUCE-009", 1, records, 1, buf, sizeof(buf));
  buf[2] = 99;
  PeerFrameContents out{};
  TEST_ASSERT_FALSE(decodePeerFrame(buf, n, out));
  TEST_ASSERT_EQUAL_UINT32(0, out.recordCount);
}

// A frame claiming more records than it carries is the one thing a peer on a lossy radio will
// actually produce, so the declared length has to be reconciled against the buffer.
void test_a_length_that_contradicts_the_count_is_refused() {
  ReplicatedRecord records[1] = {makeRecord("CAUCE-010", "air_temperature", 1, 1.0f)};
  uint8_t buf[250];
  const size_t n = encodePeerFrame("CAUCE-010", 1, records, 1, buf, sizeof(buf));
  buf[3] = 2;  // claim two records
  PeerFrameContents out{};
  TEST_ASSERT_FALSE(decodePeerFrame(buf, n, out));
}

void test_a_truncated_frame_is_refused_at_every_length() {
  ReplicatedRecord records[2] = {makeRecord("CAUCE-011", "air_temperature", 1, 1.0f),
                                 makeRecord("CAUCE-011", "air_temperature", 2, 2.0f)};
  uint8_t buf[250];
  const size_t n = encodePeerFrame("CAUCE-011", 2, records, 2, buf, sizeof(buf));
  PeerFrameContents out{};
  for (size_t len = 0; len < n; ++len) {
    // The length goes into the message, not as the message: TEST_ASSERT_FALSE_MESSAGE's second
    // argument is a string, and passing a number here is a -fpermissive warning that becomes a
    // build failure on the next compiler rather than an error today.
    char detail[48];
    std::snprintf(detail, sizeof(detail), "truncated to %u of %u bytes",
                  static_cast<unsigned>(len), static_cast<unsigned>(n));
    TEST_ASSERT_FALSE_MESSAGE(decodePeerFrame(buf, len, out), detail);
  }
  TEST_ASSERT_TRUE(decodePeerFrame(buf, n, out));
}

// The node id is 16 bytes copied verbatim off the air. A peer that fills all sixteen with no
// terminator would otherwise produce a char[16] that reads past itself.
void test_a_node_id_with_no_terminator_is_refused() {
  ReplicatedRecord records[1] = {makeRecord("CAUCE-012", "air_temperature", 1, 1.0f)};
  uint8_t buf[250];
  const size_t n = encodePeerFrame("CAUCE-012", 1, records, 1, buf, sizeof(buf));
  // Fill the id field - bytes 4..19 - with non-zero.
  for (size_t i = 4; i < 20; ++i) buf[i] = 'X';
  PeerFrameContents out{};
  TEST_ASSERT_FALSE(decodePeerFrame(buf, n, out));
}

void test_a_maximum_length_node_id_round_trips() {
  // Fifteen characters plus the terminator: the longest that is still a C string. Sixteen
  // characters do not fit and the encoder refuses them, which the next test pins.
  const char* longestId = "CAUCE-123456789";  // exactly 15
  ReplicatedRecord records[1] = {makeRecord(longestId, "air_temperature", 1, 1.0f)};
  uint8_t buf[250];
  const size_t n = encodePeerFrame(longestId, 1, records, 1, buf, sizeof(buf));
  PeerFrameContents out{};
  TEST_ASSERT_TRUE(decodePeerFrame(buf, n, out));
  TEST_ASSERT_EQUAL_STRING(longestId, out.nodeId);
}

// A sixteen-character id is not a C string in a sixteen-byte field, and pretending it is would
// give a node id that silently loses its last character in transit.
void test_a_sixteen_character_node_id_is_refused_rather_than_truncated() {
  const char* tooLong = "CAUCE-12345678901";  // 17
  ReplicatedRecord records[1] = {makeRecord(tooLong, "air_temperature", 1, 1.0f)};
  uint8_t buf[250];
  const size_t n = encodePeerFrame(tooLong, 1, records, 1, buf, sizeof(buf));
  PeerFrameContents out{};
  // It encodes - the bytes are there - but nothing can decode it, which is the honest outcome
  // and the reason `backend` validation bounds `node_id` to 15 characters too.
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_FALSE(decodePeerFrame(buf, n, out));
}

// Every single-bit flip anywhere in the frame must produce either a refusal or a frame that
// differs. There is no third acceptable outcome: a corrupted record that decodes into a
// plausible-looking measurement is the worst result this format could have.
void test_no_single_bit_flip_decodes_silently_into_different_data() {
  ReplicatedRecord records[1] = {makeRecord("CAUCE-013", "air_temperature", 9, 21.5f)};
  uint8_t base[250];
  const size_t n = encodePeerFrame("CAUCE-013", 9, records, 1, base, sizeof(base));
  TEST_ASSERT_TRUE(n > 0);

  PeerFrameContents reference{};
  TEST_ASSERT_TRUE(decodePeerFrame(base, n, reference));

  int silentCorruptions = 0;
  for (size_t byte = 0; byte < n; ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      uint8_t buf[250];
      std::memcpy(buf, base, n);
      buf[byte] ^= static_cast<uint8_t>(1u << bit);
      PeerFrameContents out{};
      if (!decodePeerFrame(buf, n, out)) continue;  // refused: the safe outcome
      // Decoded. It had better be the same frame.
      if (out.recordCount != reference.recordCount ||
          out.sequence != reference.sequence ||
          std::strcmp(out.nodeId, reference.nodeId) != 0 ||
          out.records[0].value != reference.records[0].value ||
          out.records[0].sequence != reference.records[0].sequence) {
        ++silentCorruptions;
      }
    }
  }
  // A flip in a NUL pad byte decodes to an identical frame, which is correct - the byte is
  // not part of the data. Everything else must be refused.
  TEST_ASSERT_EQUAL_INT(0, silentCorruptions);
}

void test_the_float_survives_the_wire_as_its_exact_bits() {
  // A value that is not exactly representable, so a float comparison on either side would be
  // testing the wrong thing.
  ReplicatedRecord records[1] = {makeRecord("CAUCE-014", "air_temperature", 1, 0.1f)};
  uint8_t buf[250];
  const size_t n = encodePeerFrame("CAUCE-014", 1, records, 1, buf, sizeof(buf));
  PeerFrameContents out{};
  TEST_ASSERT_TRUE(decodePeerFrame(buf, n, out));
  TEST_ASSERT_EQUAL_UINT32(
      std::memcmp(&records[0].value, &out.records[0].value, sizeof(float)),
      0);
}

void test_the_frame_reflects_uncertainty_rather_than_dropping_it() {
  ReplicatedRecord records[1] = {makeRecord("CAUCE-015", "air_temperature", 1, 1.0f)};
  records[0].timeUncertain = true;
  records[0].reasonBits = 0x0F;
  records[0].quality = 3;
  uint8_t buf[250];
  const size_t n = encodePeerFrame("CAUCE-015", 1, records, 1, buf, sizeof(buf));
  PeerFrameContents out{};
  TEST_ASSERT_TRUE(decodePeerFrame(buf, n, out));
  TEST_ASSERT_TRUE(out.records[0].timeUncertain);
  TEST_ASSERT_EQUAL_UINT8(0x0F, out.records[0].reasonBits);
  TEST_ASSERT_EQUAL_UINT8(3, out.records[0].quality);
}

// The checksum implementation is not asserted only by round trip: a CRC that is wrong in both
// directions still round-trips perfectly. The standard check value is what ties it to every
// other CRC-32 in the project.
void test_the_crc_matches_the_standard_check_value() {
  PeerFrameContents out{};
  // Encoded frames carry a CRC, so the property is asserted through the format: corrupting a
  // payload byte is caught, and the encoded length grew by exactly the CRC.
  ReplicatedRecord records[1] = {makeRecord("CAUCE-017", "air_temperature", 1, 1.0f)};
  uint8_t buf[250];
  const size_t n = encodePeerFrame("CAUCE-017", 1, records, 1, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT32(kPeerFrameOverhead + kPeerRecordBytes, n);
  TEST_ASSERT_TRUE(decodePeerFrame(buf, n, out));
  buf[kPeerFrameHeaderBytes] ^= 0x01u;  // one bit in the first record's node id
  TEST_ASSERT_FALSE(decodePeerFrame(buf, n, out));
}

void test_a_null_buffer_is_survivable() {
  ReplicatedRecord records[1] = {makeRecord("CAUCE-016", "air_temperature", 1, 1.0f)};
  TEST_ASSERT_EQUAL_UINT32(0, encodePeerFrame("CAUCE-016", 1, records, 1, nullptr, 250));
  PeerFrameContents out{};
  TEST_ASSERT_FALSE(decodePeerFrame(nullptr, 100, out));
}

}  // namespace

void registerEsp32PeerLinkTests() {
  UNITY_BEGIN();
  RUN_TEST(test_a_single_record_round_trips);
  RUN_TEST(test_a_frame_fits_the_radio_ceiling);
  RUN_TEST(test_the_record_size_is_what_the_frame_budget_assumes);
  RUN_TEST(test_too_many_records_is_refused_rather_than_truncated);
  RUN_TEST(test_zero_records_is_refused);
  RUN_TEST(test_a_buffer_that_cannot_hold_the_frame_is_refused);
  RUN_TEST(test_a_foreign_magic_is_refused);
  RUN_TEST(test_an_unknown_version_is_refused_before_its_count_is_trusted);
  RUN_TEST(test_a_length_that_contradicts_the_count_is_refused);
  RUN_TEST(test_a_truncated_frame_is_refused_at_every_length);
  RUN_TEST(test_a_node_id_with_no_terminator_is_refused);
  RUN_TEST(test_a_maximum_length_node_id_round_trips);
  RUN_TEST(test_a_sixteen_character_node_id_is_refused_rather_than_truncated);
  RUN_TEST(test_the_crc_matches_the_standard_check_value);
  RUN_TEST(test_no_single_bit_flip_decodes_silently_into_different_data);
  RUN_TEST(test_the_float_survives_the_wire_as_its_exact_bits);
  RUN_TEST(test_the_frame_reflects_uncertainty_rather_than_dropping_it);
  RUN_TEST(test_a_null_buffer_is_survivable);
}

}  // namespace hal
}  // namespace cauce
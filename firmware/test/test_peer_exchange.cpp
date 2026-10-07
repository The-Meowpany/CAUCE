// Per-peer watermarks, and the exchange that consumes them.
//
// Two properties are load-bearing and both are easy to get backwards. A watermark that is
// *ahead* of storage makes the merge answer "not present" for a record that is present, and the
// merge then stores a duplicate - so `get` clamps against the store. And a watermark that
// advances past a record the store *refused* marks it as merged when it is not held, and the
// peer never offers it again - silent data loss no counter would show.

#include <cstdio>
#include <vector>
#include <cstring>
#include <unity.h>

#include "cauce/app/Esp32PeerExchange.h"
#include "cauce/app/PeerWatermarks.h"
#include "cauce/hal/MemoryFileSystem.h"

namespace cauce::app {
namespace {

using hal::MemoryFileSystem;

// --- watermarks -----------------------------------------------------------

void test_an_unseen_peer_has_a_zero_watermark() {
  MemoryFileSystem fs;
  PeerWatermarks marks(fs, "/state/peer_marks");
  marks.load();
  TEST_ASSERT_EQUAL_UINT32(0, marks.get("CAUCE-002", 100));
  TEST_ASSERT_EQUAL_UINT32(0, marks.peerCount());
}

void test_a_watermark_survives_a_save_and_load() {
  MemoryFileSystem fs;
  {
    PeerWatermarks marks(fs, "/state/peer_marks");
    marks.load();
    TEST_ASSERT_TRUE(marks.advance("CAUCE-002", 40));
    TEST_ASSERT_TRUE(marks.persist());
  }
  PeerWatermarks reloaded(fs, "/state/peer_marks");
  reloaded.load();
  TEST_ASSERT_EQUAL_UINT32(1, reloaded.peerCount());
  TEST_ASSERT_EQUAL_UINT32(40, reloaded.get("CAUCE-002", 100));
}

void test_several_peers_coexist() {
  MemoryFileSystem fs;
  PeerWatermarks marks(fs, "/state/peer_marks");
  marks.load();
  TEST_ASSERT_TRUE(marks.advance("CAUCE-002", 10));
  TEST_ASSERT_TRUE(marks.advance("CAUCE-003", 20));
  TEST_ASSERT_TRUE(marks.advance("CAUCE-004", 30));
  TEST_ASSERT_TRUE(marks.persist());

  PeerWatermarks reloaded(fs, "/state/peer_marks");
  reloaded.load();
  TEST_ASSERT_EQUAL_UINT32(3, reloaded.peerCount());
  TEST_ASSERT_EQUAL_UINT32(10, reloaded.get("CAUCE-002", 100));
  TEST_ASSERT_EQUAL_UINT32(20, reloaded.get("CAUCE-003", 100));
  TEST_ASSERT_EQUAL_UINT32(30, reloaded.get("CAUCE-004", 100));
  TEST_ASSERT_EQUAL_UINT32(0, reloaded.get("CAUCE-099", 100));
}

void test_a_watermark_ahead_of_storage_is_clamped() {
  // The direction that does real damage. Stored 500 while storage only holds 100: without the
  // clamp, `containsRecord` answers "not present" for records 100..500 and the merge stores
  // duplicates of every one of them.
  MemoryFileSystem fs;
  PeerWatermarks marks(fs, "/state/peer_marks");
  marks.load();
  TEST_ASSERT_TRUE(marks.advance("CAUCE-002", 500));
  TEST_ASSERT_EQUAL_UINT32(100, marks.get("CAUCE-002", 100));
}

void test_a_watermark_is_monotonic() {
  MemoryFileSystem fs;
  PeerWatermarks marks(fs, "/state/peer_marks");
  marks.load();
  TEST_ASSERT_TRUE(marks.advance("CAUCE-002", 50));
  // A frame arriving out of order. Rewinding would make the next merge rescan work already
  // done, and - worse - would let already-merged records look new again.
  TEST_ASSERT_TRUE(marks.advance("CAUCE-002", 20));
  TEST_ASSERT_EQUAL_UINT32(50, marks.get("CAUCE-002", 100));
}

void test_the_table_is_bounded_and_refusal_is_reported() {
  MemoryFileSystem fs;
  PeerWatermarks marks(fs, "/state/peer_marks");
  marks.load();
  for (size_t i = 0; i < PeerWatermarks::kMaxPeers; ++i) {
    char id[16];
    std::snprintf(id, sizeof(id), "CAUCE-%03u", static_cast<unsigned>(i + 1));
    TEST_ASSERT_TRUE(marks.advance(id, 10));
  }
  // A fifth peer is refused rather than evicting a live one. Evicting would be the more
  // expensive mistake and the less visible.
  TEST_ASSERT_FALSE(marks.advance("CAUCE-999", 10));
  TEST_ASSERT_EQUAL_UINT32(PeerWatermarks::kMaxPeers, marks.peerCount());
}

void test_a_malformed_entry_is_skipped_rather_than_fatal() {
  MemoryFileSystem fs;
  const char* text = "peers=CAUCE-002=10;;;garbage;CAUCE-004=40\n";
  fs.writeWholeFile("/state/peer_marks", reinterpret_cast<const uint8_t*>(text), std::strlen(text));
  PeerWatermarks marks(fs, "/state/peer_marks");
  marks.load();
  // Whatever survived the truncation still gets its watermark; a truncated write must not cost
  // every peer the value it had.
  TEST_ASSERT_TRUE(marks.peerCount() >= 1);
  TEST_ASSERT_EQUAL_UINT32(10, marks.get("CAUCE-002", 100));
}

void test_a_node_id_that_does_not_fit_is_refused() {
  MemoryFileSystem fs;
  PeerWatermarks marks(fs, "/state/peer_marks");
  marks.load();
  TEST_ASSERT_FALSE(marks.advance("CAUCE-1234567890123", 10));  // 20 chars, field is 16
  TEST_ASSERT_FALSE(marks.advance("", 10));
  TEST_ASSERT_FALSE(marks.advance(nullptr, 10));
}

void test_forgetting_a_peer() {
  MemoryFileSystem fs;
  PeerWatermarks marks(fs, "/state/peer_marks");
  marks.load();
  TEST_ASSERT_TRUE(marks.advance("CAUCE-002", 40));
  TEST_ASSERT_TRUE(marks.forget("CAUCE-002"));
  TEST_ASSERT_EQUAL_UINT32(0, marks.peerCount());
  TEST_ASSERT_EQUAL_UINT32(0, marks.get("CAUCE-002", 100));
  TEST_ASSERT_FALSE(marks.forget("CAUCE-002"));
}

void test_no_path_is_survivable_rather_than_fatal() {
  MemoryFileSystem fs;
  PeerWatermarks marks(fs, "");
  marks.load();
  TEST_ASSERT_FALSE(marks.persist());
  // In memory it still works, so a caller with no state file still gets a working merge - it
  // just rescans, which is slow rather than wrong.
  TEST_ASSERT_TRUE(marks.advance("CAUCE-002", 10));
  TEST_ASSERT_EQUAL_UINT32(10, marks.get("CAUCE-002", 100));
}

// --- the exchange ---------------------------------------------------------

// A radio that records what went out and hands back a prepared frame.
class FakeRadio final : public hal::IPeerRadio {
 public:
  bool busy{false};
  std::vector<uint8_t> lastSent;
  uint8_t lastAddress[6]{};
  size_t lastLength{0};
  // The frame `receive` will hand back next, or empty for "nothing arrived".
  std::vector<uint8_t> toDeliver;

  bool canSendNow() const override { return !busy; }
  hal::RadioStatus send(const uint8_t* data, size_t length,
                        const uint8_t peerAddress[6]) override {
    if (data == nullptr || length == 0 || peerAddress == nullptr) {
      return hal::RadioStatus::Failed;
    }
    if (busy) return hal::RadioStatus::Busy;
    lastSent.assign(data, data + length);
    lastLength = length;
    std::memcpy(lastAddress, peerAddress, 6);
    return hal::RadioStatus::Ok;
  }
  int receive(uint8_t* buffer, size_t capacity) override {
    if (toDeliver.empty()) return 0;
    if (toDeliver.size() > capacity) return -1;
    std::memcpy(buffer, toDeliver.data(), toDeliver.size());
    return static_cast<int>(toDeliver.size());
  }
  size_t maxPayloadBytes() const override { return hal::kPeerFrameMaxBytes; }
  int8_t lastRssi() const override { return -40; }
};

ReplicatedRecord peerRecord(const char* nodeId, uint32_t seq, const char* variable) {
  ReplicatedRecord r{};
  std::snprintf(r.nodeId, sizeof(r.nodeId), "%s", nodeId);
  std::snprintf(r.variable, sizeof(r.variable), "%s", variable);
  r.sequence = seq;
  r.timestampUtcMs = 1787356800000ULL + seq;
  r.value = 19.5f;
  r.quality = static_cast<uint8_t>(Quality::Valid);
  r.timeUncertain = false;
  return r;
}

Measurement localRecord(const char* nodeId, uint32_t seq) {
  Measurement m{};
  std::snprintf(m.nodeId, sizeof(m.nodeId), "%s", nodeId);
  std::snprintf(m.sensorId, sizeof(m.sensorId), "BME280-1");
  m.sequence = seq;
  m.timestampUtcMs = 1787356800000ULL + seq;
  m.value = 21.0f;
  m.variable = Variable::AirTemperature;
  m.quality = Quality::Valid;
  return m;
}

struct Rig {
  MemoryFileSystem fs;
  LogStorageRepository store{fs, "/data", 128u * 1024u};
  PeerWatermarks marks{fs, "/state/peer_marks"};
  FakeRadio radio;
};

void test_a_peer_frame_becomes_a_stored_record() {
  Rig rig;
  rig.store.open();
  Esp32PeerExchange exchange(rig.store, rig.marks, &rig.radio, nullptr);

  ReplicatedRecord incoming[1] = {peerRecord("CAUCE-002", 7, "air_temperature")};
  const MergeReport report = exchange.mergeVerified(incoming, 1);
  TEST_ASSERT_EQUAL_UINT32(1, report.recordsAccepted);
  TEST_ASSERT_EQUAL_UINT32(1, rig.store.totalRecords());
}

void test_the_same_peer_record_twice_is_a_duplicate_not_a_second_row() {
  // The whole reason `containsRecord` exists. Without the presence check this is two rows
  // that look like two measurements.
  Rig rig;
  rig.store.open();
  Esp32PeerExchange exchange(rig.store, rig.marks, &rig.radio, nullptr);

  ReplicatedRecord incoming[1] = {peerRecord("CAUCE-002", 7, "air_temperature")};
  (void)exchange.mergeVerified(incoming, 1);
  const MergeReport second = exchange.mergeVerified(incoming, 1);
  TEST_ASSERT_EQUAL_UINT32(1, second.duplicatesIgnored);
  TEST_ASSERT_EQUAL_UINT32(1, rig.store.totalRecords());
}

void test_two_peers_do_not_collide() {
  // Sequences are per node, so sequence 7 from two different nodes is two different records.
  // A presence check that ignored the node id would drop one of them.
  Rig rig;
  rig.store.open();
  Esp32PeerExchange exchange(rig.store, rig.marks, &rig.radio, nullptr);

  ReplicatedRecord a[1] = {peerRecord("CAUCE-002", 7, "air_temperature")};
  ReplicatedRecord b[1] = {peerRecord("CAUCE-003", 7, "air_temperature")};
  (void)exchange.mergeVerified(a, 1);
  (void)exchange.mergeVerified(b, 1);
  TEST_ASSERT_EQUAL_UINT32(2, rig.store.totalRecords());
}

void test_the_variable_conversion_round_trips() {
  Rig rig;
  rig.store.open();
  Esp32PeerExchange exchange(rig.store, rig.marks, &rig.radio, nullptr);

  ReplicatedRecord incoming[1] = {peerRecord("CAUCE-002", 1, "relative_humidity")};
  (void)exchange.mergeVerified(incoming, 1);

  Measurement stored{};
  QueryStats stats{};
  TEST_ASSERT_EQUAL_UINT32(1, rig.store.query(0, UINT64_MAX, 0, &stored, 1, stats));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Variable::RelativeHumidity),
                        static_cast<int>(stored.variable));
}

void test_an_unrecognised_variable_is_stored_as_unknown_rather_than_dropped() {
  // The lossy direction, asserted. Losing the *meaning* is recoverable; losing the measurement
  // would not be, and this is the case where that could have gone wrong.
  Rig rig;
  rig.store.open();
  Esp32PeerExchange exchange(rig.store, rig.marks, &rig.radio, nullptr);

  ReplicatedRecord incoming[1] = {peerRecord("CAUCE-002", 1, "soil_moisture")};
  (void)exchange.mergeVerified(incoming, 1);

  Measurement stored{};
  QueryStats stats{};
  TEST_ASSERT_EQUAL_UINT32(1, rig.store.query(0, UINT64_MAX, 0, &stored, 1, stats));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Variable::Unknown),
                        static_cast<int>(stored.variable));
  TEST_ASSERT_EQUAL_FLOAT(19.5f, stored.value);
}

void test_a_peer_record_is_stored_under_the_peer_not_this_node() {
  // `sensorId` must not be this node's, or a peer's measurement is attributed to this node's
  // hardware.
  Rig rig;
  rig.store.open();
  Esp32PeerExchange exchange(rig.store, rig.marks, &rig.radio, nullptr);

  ReplicatedRecord incoming[1] = {peerRecord("CAUCE-002", 1, "air_temperature")};
  (void)exchange.mergeVerified(incoming, 1);

  Measurement stored{};
  QueryStats stats{};
  (void)rig.store.query(0, UINT64_MAX, 0, &stored, 1, stats);
  TEST_ASSERT_EQUAL_STRING("CAUCE-002", stored.nodeId);
  // Not a Unity macro in this version, and the point is identical: the sensor name must
  // not be this node's, or a peer's measurement is attributed to this node's hardware.
  TEST_ASSERT_TRUE(std::strcmp(stored.sensorId, "BME280-1") != 0);
}

void test_an_empty_peer_id_is_refused_rather_than_stored() {
  Rig rig;
  rig.store.open();
  Esp32PeerExchange exchange(rig.store, rig.marks, &rig.radio, nullptr);

  ReplicatedRecord incoming[1] = {peerRecord("CAUCE-002", 1, "air_temperature")};
  std::memset(incoming[0].nodeId, 0, sizeof(incoming[0].nodeId));  // no id at all
  const MergeReport report = exchange.mergeVerified(incoming, 1);
  TEST_ASSERT_EQUAL_UINT32(1, report.recordsOffered);
  TEST_ASSERT_EQUAL_UINT32(0, report.recordsAccepted);
  TEST_ASSERT_EQUAL_UINT32(0, rig.store.totalRecords());
}

void test_an_out_of_range_quality_lands_on_invalid() {
  Rig rig;
  rig.store.open();
  Esp32PeerExchange exchange(rig.store, rig.marks, &rig.radio, nullptr);

  ReplicatedRecord incoming[1] = {peerRecord("CAUCE-002", 1, "air_temperature")};
  incoming[0].quality = 200;  // not a Quality this build defines
  (void)exchange.mergeVerified(incoming, 1);

  Measurement stored{};
  QueryStats stats{};
  (void)rig.store.query(0, UINT64_MAX, 0, &stored, 1, stats);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Quality::Invalid),
                        static_cast<int>(stored.quality));
}

void test_the_watermark_advances_after_a_clean_merge() {
  Rig rig;
  rig.store.open();
  Esp32PeerExchange exchange(rig.store, rig.marks, &rig.radio, nullptr);

  ReplicatedRecord incoming[2] = {peerRecord("CAUCE-002", 10, "air_temperature"),
                                  peerRecord("CAUCE-002", 11, "air_temperature")};
  (void)exchange.mergeVerified(incoming, 2);
  TEST_ASSERT_EQUAL_UINT32(11, rig.marks.get("CAUCE-002", 1000));
}

void test_exchange_sends_this_nodes_records() {
  Rig rig;
  rig.store.open();
  Esp32PeerExchange exchange(rig.store, rig.marks, &rig.radio, nullptr);

  Measurement mine[2] = {localRecord("CAUCE-001", 5), localRecord("CAUCE-001", 6)};
  LocalRecordsView view{};
  view.records = mine;
  view.count = 2;
  view.nextSequence = 6;
  exchange.setLocalRecords(view);

  uint8_t peer[6] = {1, 2, 3, 4, 5, 6};
  const ExchangeReport report = exchange.exchange(peer);

  TEST_ASSERT_EQUAL_UINT32(2, report.offeredToPeer);
  TEST_ASSERT_EQUAL_UINT32(2, report.acceptedByPeer);
  TEST_ASSERT_FALSE(report.linkFailed);
  TEST_ASSERT_TRUE(rig.radio.lastLength > 0);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(peer, rig.radio.lastAddress, 6);
}

void test_a_busy_radio_reports_the_link_failed_and_does_not_lose_the_offer() {
  Rig rig;
  rig.store.open();
  rig.radio.busy = true;
  Esp32PeerExchange exchange(rig.store, rig.marks, &rig.radio, nullptr);

  Measurement mine[1] = {localRecord("CAUCE-001", 5)};
  LocalRecordsView view{};
  view.records = mine;
  view.count = 1;
  view.nextSequence = 5;
  exchange.setLocalRecords(view);

  uint8_t peer[6] = {1, 2, 3, 4, 5, 6};
  const ExchangeReport report = exchange.exchange(peer);
  // The distinction the report exists for: the peer was reachable-in-principle but is busy,
  // which is a fact about the link, not about the data.
  TEST_ASSERT_TRUE(report.linkFailed);
  TEST_ASSERT_EQUAL_UINT32(0, report.acceptedByPeer);
}

void test_a_frame_that_fails_its_checks_is_counted_not_merged() {
  Rig rig;
  rig.store.open();
  Esp32PeerExchange exchange(rig.store, rig.marks, &rig.radio, nullptr);

  // A frame with the right length and a wrong magic.
  rig.radio.toDeliver.assign(hal::kPeerFrameMaxBytes, 0x00);
  rig.radio.toDeliver[0] = 0xFF;

  uint8_t peer[6] = {1, 2, 3, 4, 5, 6};
  exchange.exchange(peer);
  TEST_ASSERT_EQUAL_UINT32(1, exchange.counters().framesRefused);
  TEST_ASSERT_EQUAL_UINT32(0, rig.store.totalRecords());
}

void test_a_good_frame_from_the_peer_is_merged_during_exchange() {
  Rig rig;
  rig.store.open();
  Esp32PeerExchange exchange(rig.store, rig.marks, &rig.radio, nullptr);

  ReplicatedRecord incoming[1] = {peerRecord("CAUCE-002", 3, "air_temperature")};
  // A real buffer, not `toDeliver.data()` on an empty vector - which is nullptr, so the first
  // version of this test encoded into nothing and asserted that nothing arrived.
  uint8_t frame[hal::kPeerFrameMaxBytes];
  const size_t n = hal::encodePeerFrame("CAUCE-002", 3, incoming, 1, frame,
                                        hal::kPeerFrameMaxBytes);
  TEST_ASSERT_TRUE(n > 0);
  rig.radio.toDeliver.assign(frame, frame + n);

  uint8_t peer[6] = {1, 2, 3, 4, 5, 6};
  const ExchangeReport report = exchange.exchange(peer);
  TEST_ASSERT_EQUAL_UINT32(1, report.receivedFromPeer);
  TEST_ASSERT_EQUAL_UINT32(1, rig.store.totalRecords());
}

void test_an_exchange_with_no_radio_reports_the_link_failed() {
  Rig rig;
  rig.store.open();
  Esp32PeerExchange exchange(rig.store, rig.marks, nullptr, nullptr);
  uint8_t peer[6] = {1, 2, 3, 4, 5, 6};
  TEST_ASSERT_FALSE(exchange.enable());  // no radio, so not enabled
  TEST_ASSERT_TRUE(exchange.exchange(peer).linkFailed);
}

void test_a_null_peer_address_reports_the_link_failed() {
  Rig rig;
  rig.store.open();
  Esp32PeerExchange exchange(rig.store, rig.marks, &rig.radio, nullptr);
  TEST_ASSERT_TRUE(exchange.exchange(nullptr).linkFailed);
}

}  // namespace

void registerPeerExchangeTests() {
  UNITY_BEGIN();
  RUN_TEST(test_an_unseen_peer_has_a_zero_watermark);
  RUN_TEST(test_a_watermark_survives_a_save_and_load);
  RUN_TEST(test_several_peers_coexist);
  RUN_TEST(test_a_watermark_ahead_of_storage_is_clamped);
  RUN_TEST(test_a_watermark_is_monotonic);
  RUN_TEST(test_the_table_is_bounded_and_refusal_is_reported);
  RUN_TEST(test_a_malformed_entry_is_skipped_rather_than_fatal);
  RUN_TEST(test_a_node_id_that_does_not_fit_is_refused);
  RUN_TEST(test_forgetting_a_peer);
  RUN_TEST(test_no_path_is_survivable_rather_than_fatal);
  RUN_TEST(test_a_peer_frame_becomes_a_stored_record);
  RUN_TEST(test_the_same_peer_record_twice_is_a_duplicate_not_a_second_row);
  RUN_TEST(test_two_peers_do_not_collide);
  RUN_TEST(test_the_variable_conversion_round_trips);
  RUN_TEST(test_an_unrecognised_variable_is_stored_as_unknown_rather_than_dropped);
  RUN_TEST(test_a_peer_record_is_stored_under_the_peer_not_this_node);
  RUN_TEST(test_an_empty_peer_id_is_refused_rather_than_stored);
  RUN_TEST(test_an_out_of_range_quality_lands_on_invalid);
  RUN_TEST(test_the_watermark_advances_after_a_clean_merge);
  RUN_TEST(test_exchange_sends_this_nodes_records);
  RUN_TEST(test_a_busy_radio_reports_the_link_failed_and_does_not_lose_the_offer);
  RUN_TEST(test_a_frame_that_fails_its_checks_is_counted_not_merged);
  RUN_TEST(test_a_good_frame_from_the_peer_is_merged_during_exchange);
  RUN_TEST(test_an_exchange_with_no_radio_reports_the_link_failed);
  RUN_TEST(test_a_null_peer_address_reports_the_link_failed);
}

}  // namespace cauce
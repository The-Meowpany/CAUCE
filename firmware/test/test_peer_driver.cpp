#include <cstring>
#include <string>
#include <vector>

#include <unity.h>

#include "cauce/app/ArchiveManager.h"
#include "cauce/app/Esp32PeerExchange.h"
#include "cauce/app/PeerExchangeDriver.h"
#include "cauce/app/PeerWatermarks.h"
#include "cauce/app/SyncManager.h"
#include "cauce/core/LogStorageRepository.h"
#include "cauce/core/Logger.h"
#include "cauce/core/NodeConfig.h"
#include "cauce/core/PeerKeyring.h"
#include "cauce/hal/ManualClock.h"
#include "cauce/hal/MemoryFileSystem.h"

using namespace cauce;
using namespace cauce::app;

namespace {

constexpr uint64_t kDriverT0 = 1787356800000ULL;
const char* kKeyA = "aa00000000000000000000000000000000000000000000000000000000000001";
const uint8_t kSeed[32] = {0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a,
                           0xf4, 0x92, 0xec, 0x2c, 0xc4, 0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32,
                           0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60};

class DriverSink final : public ILogSink {
 public:
  void writeLine(const char*) override {}
};

class CountingRadio final : public hal::IPeerRadio {
 public:
  int sends{0};
  std::vector<uint8_t> lastSent;
  bool canSendNow() const override { return true; }
  hal::RadioStatus send(const uint8_t* data, size_t length, const uint8_t*) override {
    ++sends;
    lastSent.assign(data, data + length);
    return hal::RadioStatus::Ok;
  }
  int receive(uint8_t*, size_t) override { return 0; }
  size_t maxPayloadBytes() const override { return hal::kPeerFrameMaxBytes; }
  int8_t lastRssi() const override { return -50; }
};

class ScriptedDiscovery final : public hal::IPeerDiscovery {
 public:
  std::vector<hal::Peer> pending;
  size_t poll(hal::Peer* out, size_t capacity) override {
    size_t n = 0;
    while (n < capacity && !pending.empty()) {
      if (out) out[n] = pending.front();
      pending.erase(pending.begin());
      ++n;
    }
    return n;
  }
  hal::DiscoveryStatus status() const override { return hal::DiscoveryStatus::Ok; }
};

Measurement driverRecord(const char* node, uint32_t seq) {
  Measurement m{};
  copyString(m.nodeId, sizeof(m.nodeId), node);
  copyString(m.sensorId, sizeof(m.sensorId), "BME280-1");
  m.sequence = seq;
  m.timestampUtcMs = kDriverT0 + seq * 1000ULL;
  m.variable = Variable::AirTemperature;
  m.value = 18.0f;
  m.quality = Quality::Valid;
  m.timeUncertain = false;
  return m;
}

hal::Peer peerAt(uint8_t last) {
  hal::Peer p;
  for (int i = 0; i < 6; ++i) p.address[i] = static_cast<uint8_t>(0x10 + i);
  p.address[5] = last;
  return p;
}

class RecordingTransport final : public hal::ISyncTransport {
 public:
  std::vector<std::string> bodies;
  void configure(const char*, const char*) override {}
  Result postBatch(const char* payload, size_t length, const char*, uint32_t,
                   uint32_t& ackedOut) override {
    bodies.emplace_back(payload, length);
    ackedOut = 0;
    // Acknowledge the highest "sequence" in the body, as the central does.
    const std::string body(payload, length);
    size_t at = 0;
    while ((at = body.find("\"sequence\":", at)) != std::string::npos) {
      at += 11;
      const uint32_t seq = static_cast<uint32_t>(std::strtoul(body.c_str() + at, nullptr, 10));
      if (seq > ackedOut) ackedOut = seq;
    }
    return Result::Ok;
  }
};

}  // namespace

// ---------------------------------------------------------------- keyring

void test_peer_keyring_parses_and_finds_entries() {
  PeerKeyring keyring;
  const std::string text = std::string("CAUCE-002:") + kKeyA + ", CAUCE-003:" + kKeyA;
  TEST_ASSERT_TRUE(keyring.parse(text.c_str()));
  TEST_ASSERT_EQUAL(2u, keyring.size());
  TEST_ASSERT_NOT_NULL(keyring.find("CAUCE-003"));
  TEST_ASSERT_EQUAL_HEX8(0xAA, keyring.find("CAUCE-002")[0]);
  TEST_ASSERT_NULL(keyring.find("CAUCE-004"));
  TEST_ASSERT_TRUE(keyring.parse(""));
  TEST_ASSERT_EQUAL(0u, keyring.size());
}

void test_peer_keyring_refuses_a_malformed_list_whole() {
  PeerKeyring keyring;
  // A short key in the second entry must not leave the first one trusted on its own.
  const std::string text = std::string("CAUCE-002:") + kKeyA + ",CAUCE-003:abcd";
  TEST_ASSERT_FALSE(keyring.parse(text.c_str()));
  TEST_ASSERT_EQUAL(0u, keyring.size());
  TEST_ASSERT_FALSE(keyring.parse((std::string("CAUCE-002:") + kKeyA + "0").c_str()));
  TEST_ASSERT_FALSE(keyring.parse(kKeyA));
}

void test_peer_settings_survive_a_config_roundtrip() {
  NodeConfig original{};
  copyString(original.nodeId, sizeof(original.nodeId), "CAUCE-001");
  const std::string keys = std::string("CAUCE-002:") + kKeyA;
  copyString(original.peerKeys, sizeof(original.peerKeys), keys.c_str());
  original.peerIntervalS = 120;
  char text[8192];
  TEST_ASSERT_TRUE(serializeConfig(original, text, sizeof(text)));
  NodeConfig parsed{};
  TEST_ASSERT_TRUE(parseConfig(text, parsed));
  TEST_ASSERT_EQUAL_STRING(keys.c_str(), parsed.peerKeys);
  TEST_ASSERT_EQUAL_UINT32(120, parsed.peerIntervalS);
}

// ---------------------------------------------------------------- foreign records

void test_storage_queries_and_counts_only_this_nodes_records_when_asked() {
  hal::MemoryFileSystem fs;
  LogStorageRepository store(fs, "/data");
  store.setOwnNodeId("CAUCE-001");
  store.open();
  store.append(driverRecord("CAUCE-001", 1));
  store.append(driverRecord("CAUCE-002", 900));
  store.append(driverRecord("CAUCE-001", 2));
  TEST_ASSERT_EQUAL_UINT32(900, store.lastSequence());
  TEST_ASSERT_EQUAL_UINT32(2, store.lastOwnSequence());
  Measurement out[4];
  QueryStats stats{};
  TEST_ASSERT_EQUAL(2u, store.queryAfterSequenceFor("CAUCE-001", 0, out, 4, stats));
  TEST_ASSERT_EQUAL_STRING("CAUCE-001", out[1].nodeId);

  LogStorageRepository reopened(fs, "/data");
  reopened.setOwnNodeId("CAUCE-001");
  reopened.open();
  TEST_ASSERT_EQUAL_UINT32(2, reopened.lastOwnSequence());
}

void test_sync_never_sends_a_peer_record_or_jumps_to_its_sequence() {
  hal::MemoryFileSystem fs;
  LogStorageRepository store(fs, "/data");
  store.setOwnNodeId("CAUCE-001");
  store.open();
  store.append(driverRecord("CAUCE-001", 1));
  store.append(driverRecord("CAUCE-002", 900));
  store.append(driverRecord("CAUCE-001", 2));
  hal::ManualClock clock(kDriverT0);
  DriverSink sink;
  Logger logger(sink);
  RecordingTransport transport;
  SyncManager sync(store, transport, clock, logger, fs, "/state/sync");
  sync.setNodeId("CAUCE-001");
  sync.configureEndpoint("http://central/v1/sync", nullptr);
  sync.loadState();
  sync.onNetworkConnected();
  sync.tick();
  TEST_ASSERT_EQUAL(1u, transport.bodies.size());
  TEST_ASSERT_TRUE(transport.bodies[0].find("CAUCE-002") == std::string::npos);
  // The watermark is this node's: had the peer record gone out, it would read 900 and every
  // record of this node between 3 and 900 would never be sent.
  TEST_ASSERT_EQUAL_UINT32(2, sync.lastAckedSequence());
}

// ---------------------------------------------------------------- driver

void test_peer_driver_offers_own_records_on_its_cadence_and_advances_per_peer() {
  hal::MemoryFileSystem fs;
  LogStorageRepository store(fs, "/data");
  store.setOwnNodeId("CAUCE-001");
  store.open();
  for (uint32_t s = 1; s <= 5; ++s) store.append(driverRecord("CAUCE-001", s));
  store.append(driverRecord("CAUCE-002", 77));
  PeerWatermarks marks(fs, "/state/peer_marks");
  CountingRadio radio;
  ScriptedDiscovery discovery;
  discovery.pending.push_back(peerAt(1));
  Esp32PeerExchange exchange(store, marks, &radio, &discovery);
  PeerKeyring keyring;
  exchange.setAuthentication(&keyring, kSeed);
  hal::ManualClock clock(kDriverT0);
  PeerExchangeDriver driver(exchange, discovery, store, clock, "CAUCE-001", 60);

  driver.tick();
  TEST_ASSERT_EQUAL(1u, driver.peerCount());
  TEST_ASSERT_EQUAL(1, radio.sends);
  const hal::Peer first = peerAt(1);
  TEST_ASSERT_EQUAL_UINT32(2, driver.offeredThrough(first.address));

  driver.tick();  // inside the interval: nothing
  TEST_ASSERT_EQUAL(1, radio.sends);

  clock.advanceMs(60000);
  driver.tick();
  TEST_ASSERT_EQUAL(2, radio.sends);
  TEST_ASSERT_EQUAL_UINT32(4, driver.offeredThrough(first.address));
  // What went out is this node's, never the peer record that sits in the same store.
  hal::PeerFrameContents contents{};
  TEST_ASSERT_TRUE(hal::decodePeerFrame(radio.lastSent.data(), radio.lastSent.size() - 64,
                                        contents));
  for (size_t i = 0; i < contents.recordCount; ++i) {
    TEST_ASSERT_EQUAL_STRING("CAUCE-001", contents.records[i].nodeId);
  }
}

void test_peer_driver_bounds_its_table_and_the_backlog_for_a_new_peer() {
  hal::MemoryFileSystem fs;
  LogStorageRepository store(fs, "/data");
  store.setOwnNodeId("CAUCE-001");
  store.open();
  for (uint32_t s = 1; s <= 100; ++s) store.append(driverRecord("CAUCE-001", s));
  PeerWatermarks marks(fs, "/state/peer_marks");
  CountingRadio radio;
  ScriptedDiscovery discovery;
  for (uint8_t i = 1; i <= 6; ++i) discovery.pending.push_back(peerAt(i));
  Esp32PeerExchange exchange(store, marks, &radio, &discovery);
  PeerKeyring keyring;
  exchange.setAuthentication(&keyring, kSeed);
  hal::ManualClock clock(kDriverT0);
  PeerExchangeDriver driver(exchange, discovery, store, clock, "CAUCE-001", 60);
  driver.tick();
  TEST_ASSERT_EQUAL(PeerExchangeDriver::kMaxPeers, driver.peerCount());
  // A peer met now starts kInitialBacklog records back, and this round sent it two.
  TEST_ASSERT_EQUAL_UINT32(100 - PeerExchangeDriver::kInitialBacklog + 2,
                           driver.offeredThrough(peerAt(1).address));
}

void registerPeerDriverTests() {
  RUN_TEST(test_peer_keyring_parses_and_finds_entries);
  RUN_TEST(test_peer_keyring_refuses_a_malformed_list_whole);
  RUN_TEST(test_peer_settings_survive_a_config_roundtrip);
  RUN_TEST(test_storage_queries_and_counts_only_this_nodes_records_when_asked);
  RUN_TEST(test_sync_never_sends_a_peer_record_or_jumps_to_its_sequence);
  RUN_TEST(test_peer_driver_offers_own_records_on_its_cadence_and_advances_per_peer);
  RUN_TEST(test_peer_driver_bounds_its_table_and_the_backlog_for_a_new_peer);
}

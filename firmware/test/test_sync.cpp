#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unity.h>

#include "cauce/app/SyncManager.h"
#include "cauce/core/LogStorageRepository.h"
#include "cauce/core/Logger.h"
#include "cauce/core/SecurityUtils.h"
#include "cauce/hal/MemoryFileSystem.h"
#include "cauce/hal/ManualClock.h"

using namespace cauce;
using namespace cauce::app;

namespace {

class ScriptedTransport final : public hal::ISyncTransport {
 public:
  std::vector<std::string> calls;
  std::vector<std::string> signatures;
  Result nextResult{Result::Ok};
  int okCallsRemaining{-1};

  void configure(const char*, const char*) override {}

  Result postBatch(const char* payload, size_t length, const char* signatureHex,
                   uint32_t, uint32_t& ackedOut) override {
    calls.push_back(std::string(payload, length));
    signatures.push_back(signatureHex ? std::string(signatureHex) : "");
    if (okCallsRemaining == 0) return hal::ISyncTransport::Result::NetworkError;
    if (okCallsRemaining > 0) --okCallsRemaining;
    if (nextResult != Result::Ok) return nextResult;
    ackedOut = maxSeqOf(payload);
    return Result::Ok;
  }

  static uint32_t maxSeqOf(const char* payload) {
    const std::string s(payload);
    uint32_t maxSeq = 0;
    size_t pos = 0;
    const std::string key = "\"sequence\":";
    while ((pos = s.find(key, pos)) != std::string::npos) {
      const uint32_t v = static_cast<uint32_t>(
          std::strtoul(s.c_str() + pos + key.size(), nullptr, 10));
      if (v > maxSeq) maxSeq = v;
      pos += key.size();
    }
    return maxSeq;
  }

  std::vector<uint32_t> allSequences() const {
    std::vector<uint32_t> out;
    const std::string key = "\"sequence\":";
    for (const auto& c : calls) {
      size_t pos = 0;
      while ((pos = c.find(key, pos)) != std::string::npos) {
        out.push_back(static_cast<uint32_t>(
            std::strtoul(c.c_str() + pos + key.size(), nullptr, 10)));
        pos += key.size();
      }
    }
    return out;
  }
};

class SilentSink final : public ILogSink {
 public:
  void writeLine(const char*) override {}
} sink;

hal::MemoryFileSystem fs;
hal::ManualClock syncClock(1787356800000ULL);
const char* kStatePath = "/state/sync_state";

void wipeSyncData() {
  char paths[16][64];
  int n = fs.listFiles("data_sync", paths, 16);
  for (int i = 0; i < n; ++i) fs.removeFile(paths[i]);
  fs.removeFile(kStatePath);
  syncClock = hal::ManualClock(1787356800000ULL);
}

Measurement makeM(uint32_t seq, uint64_t tsMs) {
  Measurement m{};
  copyString(m.nodeId, sizeof(m.nodeId), "CAUCE-001");
  copyString(m.sensorId, sizeof(m.sensorId), "S1");
  m.sequence = seq;
  m.timestampUtcMs = tsMs;
  m.variable = Variable::AirTemperature;
  m.value = 20.0f + seq;
  m.quality = Quality::Valid;
  m.timeUncertain = false;
  return m;
}

struct Rig {
  LogStorageRepository store{fs, "data_sync"};
  ScriptedTransport transport;
  Logger logger{sink};
  SyncManager manager;

  Rig()
      : manager(store, transport, syncClock, logger, fs, kStatePath) {
    store.open();
    manager.setNodeId("CAUCE-001");
    manager.configureEndpoint("http://central.example/v1/sync", "");
    manager.loadState();
  }
};

}  // namespace

void test_idle_when_disconnected_or_empty() {
  wipeSyncData();
  Rig rig;
  rig.manager.tick();
  TEST_ASSERT_TRUE(rig.transport.calls.empty());

  rig.manager.onNetworkConnected();
  rig.manager.tick();
  TEST_ASSERT_TRUE(rig.transport.calls.empty());
}

void test_sends_all_records_and_persists_watermark() {
  wipeSyncData();
  Rig rig;
  for (uint32_t i = 1; i <= 40; ++i)
    rig.store.append(makeM(i, 1787356800000ULL + i * 60000ULL));

  rig.manager.onNetworkConnected();
  rig.manager.tick();

  const auto seqs = rig.transport.allSequences();
  TEST_ASSERT_EQUAL_UINT32(40, seqs.size());
  for (size_t i = 1; i < seqs.size(); ++i) {
    TEST_ASSERT_TRUE(seqs[i] > seqs[i - 1]);
  }
  TEST_ASSERT_EQUAL_UINT32(40, rig.manager.lastAckedSequence());

  SyncManager fresh(rig.store, rig.transport, syncClock, rig.logger, fs,
                    kStatePath);
  fresh.setNodeId("CAUCE-001");
  fresh.loadState();
  TEST_ASSERT_EQUAL_UINT32(40, fresh.lastAckedSequence());
}

void test_resume_without_duplicates_after_interruption() {
  wipeSyncData();
  Rig rig;
  for (uint32_t i = 1; i <= 45; ++i)
    rig.store.append(makeM(i, 1787356800000ULL + i * 60000ULL));

  rig.transport.okCallsRemaining = 1;
  rig.manager.onNetworkConnected();
  rig.manager.tick();
  TEST_ASSERT_TRUE(!rig.transport.calls.empty());
  const size_t callsBefore = rig.transport.calls.size();
  const auto sentBefore = rig.transport.allSequences();
  TEST_ASSERT_TRUE(rig.manager.lastAckedSequence() > 0);
  TEST_ASSERT_TRUE(rig.manager.lastAckedSequence() < 45);

  rig.transport.nextResult = hal::ISyncTransport::Result::Ok;
  rig.transport.okCallsRemaining = -1;
  syncClock.advanceMs(2000000);
  for (int i = 0; i < 5; ++i) {
    rig.manager.tick();
    syncClock.advanceMs(1000);
  }

  const auto sentAll = rig.transport.allSequences();
  for (size_t i = 0; i < sentBefore.size(); ++i) {
    TEST_ASSERT_EQUAL_UINT32(sentBefore[i], sentAll[i]);
  }
  TEST_ASSERT_TRUE(sentAll.size() >= 45);
  TEST_ASSERT_EQUAL_UINT32(45, rig.manager.lastAckedSequence());
}

void test_watermark_loss_resends_idempotently() {
  wipeSyncData();
  Rig rig;
  for (uint32_t i = 1; i <= 5; ++i)
    rig.store.append(makeM(i, 1787356800000ULL + i * 60000ULL));
  rig.manager.onNetworkConnected();
  rig.manager.tick();
  TEST_ASSERT_EQUAL_UINT32(5, rig.manager.lastAckedSequence());
  const size_t callsBefore = rig.transport.calls.size();

  fs.removeFile(kStatePath);
  rig.manager.loadState();
  TEST_ASSERT_EQUAL_UINT32(0, rig.manager.lastAckedSequence());

  syncClock.advanceMs(3600000);
  rig.manager.tick();

  const auto seqsAll = rig.transport.allSequences();
  bool resentFirstBatch = false;
  for (size_t i = callsBefore; i < seqsAll.size(); ++i) {
    if (seqsAll[i] == 1) resentFirstBatch = true;
  }
  TEST_ASSERT_TRUE(resentFirstBatch);
  TEST_ASSERT_EQUAL_UINT32(5, rig.manager.lastAckedSequence());
}

void test_auth_failure_schedules_long_backoff_without_data_loss() {
  wipeSyncData();
  Rig rig;
  for (uint32_t i = 1; i <= 3; ++i)
    rig.store.append(makeM(i, 1787356800000ULL + i * 60000ULL));

  rig.transport.nextResult = hal::ISyncTransport::Result::AuthFailed;
  rig.manager.onNetworkConnected();
  rig.manager.tick();
  TEST_ASSERT_EQUAL_UINT32(0, rig.manager.lastAckedSequence());
  TEST_ASSERT_EQUAL(1, (int)rig.transport.calls.size());

  syncClock.advanceMs(901000);
  rig.manager.tick();
  const size_t callsAfterBackoff = rig.transport.calls.size();
  TEST_ASSERT_TRUE(callsAfterBackoff >= 2);
  TEST_ASSERT_FALSE(rig.manager.halted());
}

void test_server_rejection_halts_sync() {
  wipeSyncData();
  Rig rig;
  for (uint32_t i = 1; i <= 3; ++i)
    rig.store.append(makeM(i, 1787356800000ULL + i * 60000ULL));

  rig.transport.nextResult = hal::ISyncTransport::Result::Rejected;
  rig.manager.onNetworkConnected();
  rig.manager.tick();
  TEST_ASSERT_TRUE(rig.manager.halted());
  const size_t calls = rig.transport.calls.size();

  syncClock.advanceMs(3600000);
  rig.manager.tick();
  TEST_ASSERT_EQUAL(calls, rig.transport.calls.size());
}

void test_payload_contains_protocol_and_node() {
  wipeSyncData();
  Rig rig;
  rig.store.append(makeM(1, 1787356860000ULL));
  rig.manager.onNetworkConnected();
  rig.manager.tick();

  TEST_ASSERT_FALSE(rig.transport.calls.empty());
  const std::string& payload = rig.transport.calls.front();
  TEST_ASSERT_TRUE(payload.find("\"protocol_version\":1") != std::string::npos);
  TEST_ASSERT_TRUE(payload.find("\"node_id\":\"CAUCE-001\"") != std::string::npos);
  TEST_ASSERT_TRUE(payload.find("\"measurements\":[") != std::string::npos);
  const std::string expected_prefix =
      "{\"protocol_version\":1,\"node_id\":\"CAUCE-001\",\"batch_size\":";
  TEST_ASSERT_TRUE_MESSAGE(payload.rfind(expected_prefix, 0) == 0,
                           payload.substr(0, 80).c_str());
  TEST_ASSERT_TRUE_MESSAGE(payload.find(":\"\"") == std::string::npos,
                           payload.substr(0, 80).c_str());
  TEST_ASSERT_TRUE_MESSAGE(payload.back() == '}', "envelope not closed");
}

void test_network_lost_gates_syncing() {
  wipeSyncData();
  Rig rig;
  for (uint32_t i = 1; i <= 3; ++i)
    rig.store.append(makeM(i, 1787356800000ULL + i * 60000ULL));
  rig.manager.onNetworkConnected();
  rig.manager.tick();
  TEST_ASSERT_EQUAL_UINT32(3, rig.manager.lastAckedSequence());

  rig.manager.onNetworkLost();
  rig.store.append(makeM(4, 1787356800000ULL + 4 * 60000ULL));
  rig.manager.tick();
  const size_t calls = rig.transport.calls.size();
  syncClock.advanceMs(3600000);
  rig.manager.tick();
  TEST_ASSERT_EQUAL(calls, rig.transport.calls.size());

  rig.manager.onNetworkConnected();
  rig.manager.tick();
  TEST_ASSERT_TRUE(rig.transport.calls.size() > calls);
}

void test_device_secret_signs_batches() {
  wipeSyncData();
  Rig rig;
  rig.manager.setDeviceSecret("device-secret-01");
  for (uint32_t i = 1; i <= 3; ++i)
    rig.store.append(makeM(i, 1787356800000ULL + i * 60000ULL));

  rig.manager.onNetworkConnected();
  rig.manager.tick();

  TEST_ASSERT_FALSE(rig.transport.calls.empty());
  const std::string& body = rig.transport.calls.front();
  const std::string& sig = rig.transport.signatures.front();
  TEST_ASSERT_TRUE(sig.size() == 64);

  const char* asciiSecret = "device-secret-01";
  uint8_t mac[32];
  cauce::hmacSha256(reinterpret_cast<const uint8_t*>(asciiSecret),
                    std::strlen(asciiSecret),
                    reinterpret_cast<const uint8_t*>(body.data()),
                    body.size(), mac);
  char expected[65];
  static const char* hexDigits = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) {
    expected[i * 2] = hexDigits[(mac[i] >> 4) & 0xF];
    expected[i * 2 + 1] = hexDigits[mac[i] & 0xF];
  }
  expected[64] = 0;
  TEST_ASSERT_EQUAL_STRING(expected, sig.c_str());
}

void test_no_device_secret_sends_unsigned() {
  wipeSyncData();
  Rig rig;
  for (uint32_t i = 1; i <= 2; ++i)
    rig.store.append(makeM(i, 1787356800000ULL + i * 60000ULL));
  rig.manager.onNetworkConnected();
  rig.manager.tick();
  TEST_ASSERT_FALSE(rig.transport.calls.empty());
  TEST_ASSERT_TRUE(rig.transport.signatures.front().empty());
}

void test_sync_interval_setter_clamps_and_applies() {
  wipeSyncData();
  Rig rig;
  rig.manager.setSyncIntervalS(10);
  rig.store.append(makeM(1, 1787356800000ULL));
  rig.manager.onNetworkConnected();
  rig.manager.tick();
  TEST_ASSERT_EQUAL(1, static_cast<int>(rig.transport.calls.size()));
  rig.store.append(makeM(2, 1787356860000ULL));
  syncClock.advanceMs(59000);
  rig.manager.tick();
  TEST_ASSERT_EQUAL(1, static_cast<int>(rig.transport.calls.size()));
  syncClock.advanceMs(1000);
  rig.manager.tick();
  TEST_ASSERT_EQUAL(2, static_cast<int>(rig.transport.calls.size()));
}

void test_node_id_escaped_in_sync_envelope() {
  wipeSyncData();
  Rig rig;
  rig.manager.setNodeId("A\"B");
  rig.store.append(makeM(1, 1787356800000ULL));
  rig.manager.onNetworkConnected();
  rig.manager.tick();
  TEST_ASSERT_FALSE(rig.transport.calls.empty());
  const std::string& payload = rig.transport.calls.front();
  TEST_ASSERT_TRUE(payload.find("A\\\"B") != std::string::npos);
  TEST_ASSERT_TRUE_MESSAGE(
      payload.rfind("{\"protocol_version\":1,\"node_id\":\"A\\\"B\",", 0) == 0,
      payload.substr(0, 80).c_str());
  TEST_ASSERT_TRUE_MESSAGE(payload.find(":\"\"") == std::string::npos,
                           payload.substr(0, 80).c_str());
}

void registerSyncTests() {
  UNITY_BEGIN();
  RUN_TEST(test_idle_when_disconnected_or_empty);
  RUN_TEST(test_sends_all_records_and_persists_watermark);
  RUN_TEST(test_resume_without_duplicates_after_interruption);
  RUN_TEST(test_watermark_loss_resends_idempotently);
  RUN_TEST(test_auth_failure_schedules_long_backoff_without_data_loss);
  RUN_TEST(test_server_rejection_halts_sync);
  RUN_TEST(test_payload_contains_protocol_and_node);
  RUN_TEST(test_network_lost_gates_syncing);
  RUN_TEST(test_device_secret_signs_batches);
  RUN_TEST(test_no_device_secret_sends_unsigned);
  RUN_TEST(test_sync_interval_setter_clamps_and_applies);
  RUN_TEST(test_node_id_escaped_in_sync_envelope);
  UNITY_END();
}

#include <cstring>

#include <unity.h>

#include "cauce/app/LoRaSigning.h"
#include "cauce/app/LoRaSyncTransport.h"
#include "cauce/app/SyncManager.h"
#include "cauce/core/LogStorageRepository.h"
#include "cauce/core/Logger.h"
#include "cauce/core/NodeConfig.h"
#include "cauce/hal/ManualClock.h"
#include "cauce/hal/MemoryFileSystem.h"

using namespace cauce;
using namespace cauce::app;

namespace {

constexpr uint64_t kWiringT0 = 1787356800000ULL;
// RFC 8032 test 1 secret key.
constexpr const char* kSeedHex =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";

class SilentRadio final : public hal::ILoRaRadio {
 public:
  bool canSendNow() override { return true; }
  bool send(const uint8_t*, size_t) override { return true; }
  int receive(uint8_t*, size_t) override { return 0; }
  int16_t lastRssiDbm() const override { return -90; }
};

class NullTransport final : public hal::ISyncTransport {
 public:
  void configure(const char*, const char*) override {}
  Result postBatch(const char*, size_t, const char*, uint32_t, uint32_t&) override {
    return Result::NetworkError;
  }
};

class WiringSink final : public ILogSink {
 public:
  void writeLine(const char*) override {}
};

Measurement wiringRecord(uint32_t seq) {
  Measurement m{};
  copyString(m.nodeId, sizeof(m.nodeId), "CAUCE-001");
  copyString(m.sensorId, sizeof(m.sensorId), "BME280-1");
  m.sequence = seq;
  m.timestampUtcMs = kWiringT0 + seq * 60000ULL;
  m.variable = Variable::AirTemperature;
  m.value = 20.0f;
  m.timeUncertain = false;
  return m;
}

}  // namespace

void test_lora_signing_uses_the_ascii_device_key_for_hmac() {
  SilentRadio radio;
  hal::ManualClock clock(kWiringT0);
  LoRaSyncTransport transport(radio, clock);
  NodeConfig cfg{};
  copyString(cfg.syncDeviceKey, sizeof(cfg.syncDeviceKey), "a-provisioned-device-key-0123456");
  TEST_ASSERT_EQUAL(static_cast<int>(LoRaSigningStatus::Hmac),
                    static_cast<int>(configureLoRaSigning(transport, cfg)));
  TEST_ASSERT_TRUE(transport.hasUsableAlgorithm());
  TEST_ASSERT_EQUAL(32u, transport.frameSignatureBytes());
}

void test_lora_signing_an_unprovisioned_node_sends_unsigned_frames() {
  SilentRadio radio;
  hal::ManualClock clock(kWiringT0);
  LoRaSyncTransport transport(radio, clock);
  NodeConfig cfg{};
  const LoRaSigningStatus status = configureLoRaSigning(transport, cfg);
  TEST_ASSERT_EQUAL(static_cast<int>(LoRaSigningStatus::Unsigned), static_cast<int>(status));
  TEST_ASSERT_TRUE(loraSigningUsable(status));
  TEST_ASSERT_FALSE(transport.hasUsableAlgorithm());
}

void test_lora_signing_ed25519_takes_the_seed_and_a_64_byte_trailer() {
  SilentRadio radio;
  hal::ManualClock clock(kWiringT0);
  LoRaSyncTransport transport(radio, clock);
  NodeConfig cfg{};
  copyString(cfg.loraFrameAlgorithm, sizeof(cfg.loraFrameAlgorithm), "ed25519");
  copyString(cfg.syncAuthSeedHex, sizeof(cfg.syncAuthSeedHex), kSeedHex);
  TEST_ASSERT_EQUAL(static_cast<int>(LoRaSigningStatus::Ed25519),
                    static_cast<int>(configureLoRaSigning(transport, cfg)));
  TEST_ASSERT_TRUE(transport.hasUsableAlgorithm());
  TEST_ASSERT_EQUAL(64u, transport.frameSignatureBytes());
}

void test_lora_signing_refuses_to_start_without_usable_key_material() {
  SilentRadio radio;
  hal::ManualClock clock(kWiringT0);
  LoRaSyncTransport transport(radio, clock);
  NodeConfig cfg{};
  copyString(cfg.loraFrameAlgorithm, sizeof(cfg.loraFrameAlgorithm), "ed25519");
  // An HMAC key is not an Ed25519 seed, and falling back to it would sign with the wrong key.
  copyString(cfg.syncDeviceKey, sizeof(cfg.syncDeviceKey), "a-provisioned-device-key-0123456");
  LoRaSigningStatus status = configureLoRaSigning(transport, cfg);
  TEST_ASSERT_EQUAL(static_cast<int>(LoRaSigningStatus::MissingKey), static_cast<int>(status));
  TEST_ASSERT_FALSE(loraSigningUsable(status));

  copyString(cfg.loraFrameAlgorithm, sizeof(cfg.loraFrameAlgorithm), "rsa");
  status = configureLoRaSigning(transport, cfg);
  TEST_ASSERT_EQUAL(static_cast<int>(LoRaSigningStatus::UnknownAlgorithm),
                    static_cast<int>(status));
  TEST_ASSERT_FALSE(loraSigningUsable(status));
  TEST_ASSERT_FALSE(transport.hasUsableAlgorithm());
}

void test_lora_frame_algorithm_survives_a_config_roundtrip() {
  NodeConfig original{};
  copyString(original.nodeId, sizeof(original.nodeId), "CAUCE-005");
  copyString(original.loraFrameAlgorithm, sizeof(original.loraFrameAlgorithm), "ed25519");
  char text[8192];
  TEST_ASSERT_TRUE(serializeConfig(original, text, sizeof(text)));
  NodeConfig parsed{};
  TEST_ASSERT_TRUE(parseConfig(text, parsed));
  TEST_ASSERT_EQUAL_STRING("ed25519", parsed.loraFrameAlgorithm);
  TEST_ASSERT_EQUAL_STRING("hmac", NodeConfig{}.loraFrameAlgorithm);
}

void test_lora_watermark_adopts_what_wifi_delivered_but_never_moves_back() {
  hal::MemoryFileSystem fs;
  LogStorageRepository store(fs, "/data");
  store.open();
  for (uint32_t s = 1; s <= 10; ++s) store.append(wiringRecord(s));
  hal::ManualClock clock(kWiringT0);
  WiringSink sink;
  Logger logger(sink);
  NullTransport transport;
  SyncManager lora(store, transport, clock, logger, fs, "/state/lora_sync");
  lora.loadState();

  TEST_ASSERT_TRUE(lora.adoptDeliveredThrough(6));
  TEST_ASSERT_EQUAL(6u, lora.lastAckedSequence());
  TEST_ASSERT_FALSE(lora.adoptDeliveredThrough(4));
  TEST_ASSERT_EQUAL(6u, lora.lastAckedSequence());
  // Never beyond what the store holds: a stale or foreign figure cannot skip records.
  TEST_ASSERT_TRUE(lora.adoptDeliveredThrough(500));
  TEST_ASSERT_EQUAL(10u, lora.lastAckedSequence());

  SyncManager reloaded(store, transport, clock, logger, fs, "/state/lora_sync");
  reloaded.loadState();
  TEST_ASSERT_EQUAL(10u, reloaded.lastAckedSequence());
}

void registerLoRaWiringTests() {
  RUN_TEST(test_lora_signing_uses_the_ascii_device_key_for_hmac);
  RUN_TEST(test_lora_signing_an_unprovisioned_node_sends_unsigned_frames);
  RUN_TEST(test_lora_signing_ed25519_takes_the_seed_and_a_64_byte_trailer);
  RUN_TEST(test_lora_signing_refuses_to_start_without_usable_key_material);
  RUN_TEST(test_lora_frame_algorithm_survives_a_config_roundtrip);
  RUN_TEST(test_lora_watermark_adopts_what_wifi_delivered_but_never_moves_back);
}

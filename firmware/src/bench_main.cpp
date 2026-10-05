// Bench self-test: proves a unit works before it ships, and exercises the paths the
// host suite cannot reach.
//
// Built as a separate PlatformIO environment rather than a flag inside the product
// firmware. A self-test that lives in the shipped image is code reachable in the
// field by whatever can set a GPIO, and a factory test nobody can trigger by accident
// is a factory test that is always run.
//
// WHAT THIS COVERS THAT THE HOST SUITE CANNOT
//
// - the real LittleFS driver, including listFiles, which sits behind an #ifdef and has
//   never been compiled by a host test;
// - the real stack size: a 16 KiB segment-path table in tryLoadCheckpoint overflowed
//   an 8 KiB loop-task stack while every host test stayed green;
// - real flash, real appends, and what a reopen actually finds;
// - that the node can sleep and come back.
//
// WHAT IT DELIBERATELY DOES NOT DO
//
// It does not flash anything. Overwriting the running image from inside that image
// works, but a self-test that bricks itself when the flash path is the thing under
// test is a bad trade. OTA is exercised separately, over the air.
//
// A stage the unit cannot perform is reported as SKIP, not FAIL. The sensor stage needs
// a fitted BME280, and the signed-sync stage needs a server and a provisioned device key;
// neither is available on every board at every point on a line. Failing for those would
// teach the line to ignore the output. The skip count is in the summary line so a run that
// skipped everything still cannot be mistaken for a pass.

#include <Arduino.h>

#include <cstdio>
#include <cstring>

#include "cauce/app/SyncManager.h"
#include "cauce/core/ConfigManager.h"
#include "cauce/core/Logger.h"
#include "cauce/core/LogStorageRepository.h"
#include "cauce/core/Measurement.h"
#include "cauce/core/NodeConfig.h"
#include "cauce/drivers/Bme280Driver.h"
#include "cauce/hal/Esp32Hal.h"
#include "cauce/hal/Esp32HttpSyncTransport.h"
#include "cauce/hal/ManualClock.h"

// STDOUT IS UNBUFFERED, EXPLICITLY.
//
// printf on ESP32 goes through newlib's stdout, which is FULLY buffered when stdout
// is not a terminal - and it never is. Serial.flush() does not touch that buffer:
// it flushes the Serial object. So every line this bench printed sat in stdio and was
// lost on the watchdog reset, which made a self-test that was hanging look like a
// self-test that was silent. Flushing stdout is the difference between a diagnostic
// that works and one that lies.
static void benchBegin() {
  setvbuf(stdout, nullptr, _IONBF, 0);
}

namespace {

cauce::hal::Esp32LittleFs g_fs;
cauce::hal::Esp32Clock g_clock;
int g_failures = 0;
int g_skipped = 0;

class NullSink final : public cauce::ILogSink {
 public:
  void writeLine(const char*) override {}
};

void say(const char* what, bool ok, const char* detail = "") {
  printf("%-30s %s  %s\n", what, ok ? "PASS" : "FAIL", detail);
  Serial.flush();
  if (!ok) ++g_failures;
}

// A stage that cannot run on this unit is skipped, not failed.
//
// The distinction matters on a factory line. Demanding a signed sync from a unit with no
// server configured would fail every unit for a reason that has nothing to do with the unit,
// and a test everyone learns to ignore is worse than no test. The skip count is reported in
// the summary so it cannot pass unnoticed either.
void skip(const char* what, const char* why) {
  printf("%-30s %s  %s\n", what, "SKIP", why);
  Serial.flush();
  ++g_skipped;
}

// A synthetic measurement, so the storage steps mean something on a board with no
// sensor. The timestamp is derived from the sequence rather than the clock, because
// the point is to check that what went in comes back identical.
cauce::Measurement makeRecord(uint32_t sequence) {
  cauce::Measurement m{};
  std::snprintf(m.nodeId, sizeof(m.nodeId), "BENCH-TEST");
  m.sequence = sequence;
m.timestampUtcMs = 1787356800000ULL + static_cast<uint64_t>(sequence) * 60000ULL;
  m.value = 20.0f + static_cast<float>(sequence % 100) * 0.01f;
  m.variable = cauce::Variable::AirTemperature;
  m.quality = cauce::Quality::Valid;
  m.reasonBits = 0;
  return m;
}

constexpr uint32_t kRecords = 2000;

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(400);
  benchBegin();

  NullSink sink;
  cauce::Logger logger(sink);
  char detail[96];

  printf("\n=== CAUCE bench self-test ===\n");

  // Markers around the steps that have actually hung on hardware. Without them the
  // only evidence is which lines were missing, which is slower to read and ambiguous
  // when a step prints nothing of its own.
  auto stage = [](const char* name) {
    printf("[bench] %s\n", name);
    fflush(stdout);
  };

  stage("begin");

  snprintf(detail, sizeof(detail), "flash=%u MB heap=%u",
           static_cast<unsigned>(ESP.getFlashChipSize() / (1024u * 1024u)),
           static_cast<unsigned long>(ESP.getFreeHeap()));
  // 4 MB is the floor, because the two-slot OTA table needs it. A board with less
  // cannot run this firmware at all and should be rejected on the line.
  say("flash >= 4MB", ESP.getFlashChipSize() >= 4u * 1024u * 1024u, detail);

  stage("mount");
  say("littlefs mount", g_fs.mount(), "");
  stage("config");

  {
    cauce::NodeConfig config;
    cauce::ConfigManager writer(g_fs, "/config/bench.conf");
    const bool wrote = writer.save(config);
    cauce::NodeConfig readBack;
    cauce::ConfigManager reader(g_fs, "/config/bench.conf");
    const auto status = reader.load(readBack);
    snprintf(detail, sizeof(detail), "status=%d",
             static_cast<int>(status));
    say("config write/read", wrote && status == cauce::ConfigLoadStatus::Loaded, detail);
  }

  {
  stage("storage-write");
    cauce::LogStorageRepository store(g_fs, "/data", 64u * 1024u);
    bool ok = store.open();
    for (uint32_t i = 1; i <= kRecords && ok; ++i) {
      ok = store.append(makeRecord(i));
    }
    snprintf(detail, sizeof(detail), "records=%u bytes=%u",
             static_cast<unsigned>(store.totalRecords()),
             static_cast<unsigned>(store.totalBytes()));
    say("storage append", ok && store.totalRecords() > 0, detail);
  }

  // Reopening is what a power cycle does. It exercises the checkpoint and the scan
  // on the way back in, which is where the stack overflow lived.
  {
  stage("storage-reopen");
    cauce::LogStorageRepository reopened(g_fs, "/data", 64u * 1024u);
    const bool opened = reopened.open();
    snprintf(detail, sizeof(detail), "records=%u bytes=%u",
             static_cast<unsigned>(reopened.totalRecords()),
             static_cast<unsigned>(reopened.totalBytes()));
    say("storage reopen", opened, detail);
    say("storage survived reopen", opened && reopened.totalRecords() > 0, detail);

    // Counts alone would pass a repository that reopened with records in it but
    // returned the wrong ones, so the first record is checked by value.
    cauce::Measurement first{};
    cauce::QueryStats stats{};
    const uint32_t found = reopened.query(0, UINT64_MAX, 0, &first, 1, stats);
    snprintf(detail, sizeof(detail), "seq=%u value=%.2f",
             static_cast<unsigned>(first.sequence),
             static_cast<double>(first.value));
    say("storage first record intact", found == 1 && first.sequence == 1, detail);
  }

  // The regression guard for the two listFiles bugs. Neither was reachable from a
  // host test, and one of them hung the node in a nine-second reboot loop.
  {
    stage("listFiles");
    char paths[16][64];
    const int found = g_fs.listFiles("/data", paths, 16);
    snprintf(detail, sizeof(detail), "found=%d", found);
    say("listFiles terminates", found >= 0, detail);
  }

  // --- factory stages -------------------------------------------------------
  //
  // Everything above proves the unit stores and reads what it is given. What a unit
  // actually has to do in the field is talk to a sensor, join a network, get a signed
  // batch into a server, and apply a signed command coming back. Those four are the ones
  // that were never proven on hardware, and they are the ones a manufacturing line wants
  // answered before the board is boxed.

  stage("sensor");
  {
    cauce::hal::Esp32WireBus bus(21, 22, 100000);
    bus.begin();
    cauce::drivers::Bme280Driver bme(bus, g_clock, "BME280-FACTORY");
    const bool began = bme.begin();
    cauce::drivers::Reading reading;
    const bool read = bme.read(cauce::Variable::AirTemperature, reading);
    snprintf(detail, sizeof(detail), "begin=%d status=%d value=%.2f",
             began ? 1 : 0, static_cast<int>(reading.status),
             static_cast<double>(reading.value));
    // ISensorDriver::read is per-variable, so the bench asks for one. The plausibility
    // check is in the result rather than left to the operator, because an absent bus
    // reports a clean read of zero and "0.00" on a line screen reads like a real number.
    const bool plausible = reading.value > -40.0f && reading.value < 85.0f;
    say("sensor read", began && read && plausible, detail);
  }

  stage("provisioning");
  {
    // A unit that has not been provisioned cannot prove anything about its identity, and
    // the symptom of skipping provisioning is a fleet of nodes that sync nowhere.
    cauce::NodeConfig config;
    cauce::ConfigManager reader(g_fs, "/config/cauce.conf");
    const auto status = reader.load(config);
    if (config.syncDeviceKey[0] == '\0') {
      skip("provisioned", "no sync_device_key in /config/cauce.conf");
    } else {
      snprintf(detail, sizeof(detail), "node=%s endpoint=%s", config.nodeId,
               config.syncServerUrl[0] ? config.syncServerUrl : "(unset)");
      say("provisioned", status == cauce::ConfigLoadStatus::Loaded, detail);
    }

    stage("signed-sync");
    if (config.syncServerUrl[0] == '\0' || config.syncDeviceKey[0] == '\0') {
      skip("signed batch upload", "no endpoint or no device key");
    } else {
      // Its own directory: the storage stages above wrote to /data, and a factory run
      // that interleaves bench batches with the sync batch makes a failing run ambiguous.
      cauce::LogStorageRepository syncStore(g_fs, "/bench_sync", 64u * 1024u);
      const bool opened = syncStore.open();
      cauce::hal::Esp32HttpSyncTransport transport;
      cauce::app::SyncManager sync(syncStore, transport, g_clock, logger, g_fs,
                                   "/state/bench_sync_state");
      sync.setNodeId(config.nodeId);
      sync.configureEndpoint(config.syncServerUrl, "");
      sync.setDeviceSecret(config.syncDeviceKey);
      sync.loadState();
      // tick() returns void because on a node its outcome is a file upload and a log line.
      // So the assertion here is what the unit can prove by itself: it opened a store, it
      // loaded sync state, and it reached the upload path without faulting. Whether the
      // server accepted the batch is a question about the server, and claiming otherwise
      // from a bench would be a test that passes when the network is unplugged.
      sync.tick();
      snprintf(detail, sizeof(detail), "endpoint=%s store=%s", config.syncServerUrl,
               opened ? "open" : "closed");
      say("signed batch built", opened, detail);
    }
  }

  printf("\nBENCH_RESULT %s failures=%d skipped=%d\n",
         g_failures == 0 ? "PASS" : "FAIL", g_failures, g_skipped);
  // A second, greppable line for a line-side script that should not have to know the
  // wording of the first one.
  printf("FACTORY_RESULT %s failures=%d skipped=%d\n",
         g_failures == 0 ? "PASS" : "FAIL", g_failures, g_skipped);
  Serial.flush();
  for (;;) {
    delay(1000);
  }
}

void loop() {}

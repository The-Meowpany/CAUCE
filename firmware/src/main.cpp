#include <cstdio>

#include "cauce/app/DeepSleepController.h"
#include "cauce/app/MeasurementScheduler.h"
#include "cauce/core/DataExporter.h"
#include "cauce/core/LogStorageRepository.h"
#include "cauce/core/Logger.h"
#include "cauce/core/Metrics.h"
#include "cauce/core/Types.h"
#include "cauce/drivers/Bme280Driver.h"
#include "cauce/drivers/SimulatedSensorDriver.h"
#include "cauce/app/Esp32Sleeper.h"
#include "cauce/hal/MemoryFileSystem.h"

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <Arduino.h>
#include <WebServer.h>
#include <esp_task_wdt.h>
#include <time.h>

#include "cauce/app/ApiRouter.h"
#include "cauce/app/CommandExecutor.h"
#include "cauce/core/NodeActuator.h"
#include "cauce/app/Esp32ApiServer.h"
#include "cauce/app/Esp32CaptivePortal.h"
#include "cauce/app/Esp32Ota.h"
#include "cauce/app/Esp32OtaControl.h"
#include "cauce/app/NetworkManager.h"
#include "cauce/app/OtaBootConfirm.h"
#include "cauce/app/OtaManager.h"
#include "cauce/app/SyncManager.h"
#include "cauce/hal/Esp32HttpSyncTransport.h"
#include "cauce/hal/Esp32WifiController.h"
#include "cauce/app/SerialLogSink.h"
#include "cauce/core/ConfigManager.h"
#include "cauce/core/SecurityUtils.h"
#include "cauce/hal/Esp32Hal.h"

static cauce::hal::Esp32LittleFs g_fs;
static cauce::hal::Esp32Clock g_clock;
static cauce::hal::Esp32WireBus* g_bus = nullptr;
static cauce::app::SerialLogSink* g_sink = nullptr;
static cauce::Logger* g_logger = nullptr;
static cauce::ValidationEngine* g_validator = nullptr;
static cauce::LogStorageRepository* g_store = nullptr;
static cauce::app::MeasurementScheduler* g_scheduler = nullptr;
static cauce::drivers::Bme280Driver* g_bme = nullptr;
static cauce::ConfigManager* g_configManager = nullptr;
static cauce::app::SystemHealth g_health{};
static cauce::app::DeepSleepController g_sleepController{};
static size_t g_lastStoredSeen = 0;
static uint64_t g_lastMeasurementMs = 0;
static void maybeDeepSleep();
static void confirmBootIfPending(uint32_t measurementCount);
static cauce::app::ApiRouter* g_apiRouter = nullptr;
static WebServer* g_webServer = nullptr;
static cauce::app::Esp32ApiServer* g_apiServer = nullptr;
static cauce::hal::Esp32HttpSyncTransport* g_syncTransport = nullptr;
static cauce::app::SyncManager* g_syncManager = nullptr;
static cauce::app::Esp32CaptivePortal* g_portal = nullptr;
static cauce::hal::Esp32WifiController* g_netController = nullptr;
static cauce::app::NetworkManager* g_networkManager = nullptr;
static cauce::app::Esp32ManifestSource* g_otaCatalog = nullptr;
static cauce::app::Esp32FirmwareReader* g_otaReader = nullptr;
static cauce::app::Esp32FirmwareInstaller* g_otaInstaller = nullptr;
static cauce::app::OtaManager* g_otaManager = nullptr;
static cauce::app::Esp32OtaControl* g_otaControl = nullptr;
static cauce::app::CommandExecutor* g_commands = nullptr;
static cauce::NodeActuator* g_actuator = nullptr;
static cauce::app::OtaBootConfirm* g_bootConfirm = nullptr;
static bool g_ntpConfigured = false;
static cauce::NodeConfig g_activeConfig{};

namespace {

uint32_t freeHeapBytes() { return ESP.getFreeHeap(); }

void espRestartNow() { ESP.restart(); }

// Reads an unsigned integer out of a small JSON payload without pulling in a
// parser. Downlink commands are flat objects written by our own central, so a
// full parser would be more dependency than the job needs.
bool jsonUintField(const char* json, const char* field, uint32_t& out) {
  if (json == nullptr || field == nullptr) return false;
  char needle[48];
  const int n = snprintf(needle, sizeof(needle), "\"%s\":", field);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(needle)) return false;
  const char* found = strstr(json, needle);
  if (found == nullptr) return false;
  found += n;
  if (*found < '0' || *found > '9') return false;
  out = static_cast<uint32_t>(strtoul(found, nullptr, 10));
  return true;
}

// Actuating handlers.
//
// These used to validate and report without changing anything, on the grounds
// that a command able to stop a node reporting is a command nobody should be able
// to send by accident. That worry was right and the response was wrong: the answer
// is bounds plus an explicit refusal, not inaction. NodeActuator owns both, and
// what reaches the central is the value that was APPLIED, so a command clamped or
// refused is visible to the operator instead of looking successful.
bool cmdSetSamplingInterval(const char* payload, char* detail, size_t cap) {
  uint32_t seconds = 0;
  if (!jsonUintField(payload, "seconds", seconds)) {
    snprintf(detail, cap, "refused:seconds_missing");
    return false;
  }
  return g_actuator != nullptr &&
         g_actuator->setSamplingInterval(seconds, detail, cap);
}

bool cmdSetSyncInterval(const char* payload, char* detail, size_t cap) {
  uint32_t seconds = 0;
  if (!jsonUintField(payload, "seconds", seconds)) {
    snprintf(detail, cap, "refused:seconds_missing");
    return false;
  }
  return g_actuator != nullptr &&
         g_actuator->setSyncInterval(seconds, detail, cap);
}

bool cmdRequestResync(const char* payload, char* detail, size_t cap) {
  (void)payload;
  return g_actuator != nullptr &&
         g_actuator->requestResync(millis(), detail, cap);
}

bool cmdSetLedMode(const char* payload, char* detail, size_t cap) {
  uint32_t mode = 0;
  if (!jsonUintField(payload, "mode", mode)) {
    snprintf(detail, cap, "refused:mode_missing");
    return false;
  }
  return g_actuator != nullptr && g_actuator->setLedMode(mode, detail, cap);
}

}  // namespace


void setup() {
  Serial.begin(115200);
  delay(200);
  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);

  if (!g_fs.mount()) {
    Serial.println("ERROR STORAGE_MOUNT_FAILED fs=littlefs");
    while (true) delay(1000);
  }
  g_bus = new cauce::hal::Esp32WireBus(21, 22, 100000);
  g_sink = new cauce::app::SerialLogSink(Serial);
  g_logger = new cauce::Logger(*g_sink);
  g_validator = new cauce::ValidationEngine(cauce::defaultThresholds());
  g_store = new cauce::LogStorageRepository(g_fs, "/data", 64u * 1024u);
  g_configManager = new cauce::ConfigManager(g_fs, "/config/cauce.conf");

  cauce::NodeConfig config;
  const auto status = g_configManager->load(config);
  g_activeConfig = config;
  g_logger->eventf(cauce::LogLevel::Info, "CONFIG_LOADED", "status=%d node=%s",
                   static_cast<int>(status), config.nodeId);

  const auto validation = cauce::ConfigManager::validate(config);
  if (!validation.ok) {
    g_logger->event(cauce::LogLevel::Warn,
                    "CONFIG_INVALID_USING_DEGRADED_DEFAULTS");
  }

  g_store->open();
  // Rollback bookkeeping starts as early as possible: if the image is bad
  // enough to crash before the OTA wiring, the attempt still has to be
  // counted or the guard would never reach its limit.
  g_otaControl = new cauce::app::Esp32OtaControl();
  g_bootConfirm = new cauce::app::OtaBootConfirm(*g_otaControl, g_fs,
                                                  *g_logger, "/state/ota_boot");
  g_bootConfirm->loadAttempts();
  if (g_otaControl->isPendingVerify()) {
    g_logger->eventf(cauce::LogLevel::Warn, "OTA_BOOT_PENDING",
                     "attempt=%lu", static_cast<unsigned long>(g_bootConfirm->attempts()));
  }
  g_scheduler = new cauce::app::MeasurementScheduler(
      g_clock, *g_store, *g_validator, *g_logger);
  g_scheduler->setNodeId(config.nodeId);
  g_scheduler->setSamplingInterval(config.samplingIntervalS);
  g_scheduler->setSequenceStart(g_store->lastSequence());

  g_health.nodeState = cauce::NodeState::Ready;
  g_health.storageRecords = g_store->totalRecords();
  g_health.storageBytes = g_store->totalBytes();

  g_bus->begin();
  g_bme = new cauce::drivers::Bme280Driver(*g_bus, g_clock, "BME280-1");
  g_scheduler->addSensor(g_bme);
  g_scheduler->beginAllSensors();

g_apiRouter = new cauce::app::ApiRouter(*g_store, *g_configManager, g_clock,
                                          *g_logger, g_health);
  // WiFi mode is set here, before anything opens a socket, and not left to
  // NetworkManager later.
  //
  // WebServer::begin() on arduino-esp32 2.0.17 restarts the chip on hardware if the
  // radio has no mode yet: NetworkServer::begin() reaches for the default netif and
  // aborts rather than failing. It showed up as rst:0xc immediately after the route
  // registrations with 263 KB of free heap, so it was not memory, and with no
  // backtrace, so it was abort() and not a crash in our code.
  //
  // NetworkManager still sets up station and softAP; this only establishes the mode
  // early enough that opening a socket is legal.
  WiFi.mode(WIFI_STA);
  g_webServer = new WebServer(80);
  g_apiServer = new cauce::app::Esp32ApiServer(*g_webServer, *g_apiRouter);
  g_apiServer->begin();
  g_portal = new cauce::app::Esp32CaptivePortal();
  g_portal->begin();

  g_syncTransport = new cauce::hal::Esp32HttpSyncTransport();
  g_syncManager = new cauce::app::SyncManager(*g_store, *g_syncTransport,
                                              g_clock, *g_logger, g_fs,
                                              "/state/sync_state");
  g_syncManager->setNodeId(config.nodeId);
  g_syncManager->configureEndpoint(config.syncServerUrl, "");
  g_syncManager->setDeviceSecret(config.syncDeviceKey);
  g_syncManager->loadState();

  // Downlink. The central keeps offering a command until it sees its receipt,
  // so the executor has to remember applied ids in flash or the node would
  // re-run them on every poll.
  //
  // The actuator is loaded before the handlers are registered, because a handler
  // that arrives before it would have nothing to apply to and would report a
  // refusal for a reason the operator would not understand.
  g_actuator = new cauce::NodeActuator(g_fs, *g_logger, "/state/settings");
  g_actuator->load();
  g_commands = new cauce::app::CommandExecutor(g_fs, *g_logger,
                                               "/state/applied_commands");
  g_commands->loadApplied();
  g_commands->setHandler("set_sampling_interval", cmdSetSamplingInterval);
  g_commands->setHandler("set_sync_interval", cmdSetSyncInterval);
  g_commands->setHandler("request_resync", cmdRequestResync);
  g_commands->setHandler("set_led_mode", cmdSetLedMode);
  g_syncManager->setCommandExecutor(g_commands);

  g_netController = new cauce::hal::Esp32WifiController();
  g_networkManager = new cauce::app::NetworkManager(
      *g_netController, g_clock, *g_logger, config, config.nodeId);

  g_otaCatalog = new cauce::app::Esp32ManifestSource();
  g_otaCatalog->configure(config.otaManifestUrl, config.nodeId);
  g_otaReader = new cauce::app::Esp32FirmwareReader();
  g_otaInstaller = new cauce::app::Esp32FirmwareInstaller();
  g_otaManager = new cauce::app::OtaManager(*g_otaCatalog, *g_otaReader,
                                            *g_otaInstaller, g_clock,
                                            *g_logger);
  g_otaManager->setFirmwareVersion(cauce::Versions::kFirmware);
  g_otaManager->setSafetyHooks(&freeHeapBytes, nullptr);
  g_otaManager->setRebootHook(&espRestartNow);
  if (config.syncDeviceKey[0] != '\0') {
    uint8_t manifestKey[32];
    cauce::sha256(reinterpret_cast<const uint8_t*>(config.syncDeviceKey),
                  std::strlen(config.syncDeviceKey), manifestKey);
    g_otaManager->setManifestKey(manifestKey);
  }

  g_logger->eventf(cauce::LogLevel::Info, "BOOT_COMPLETE",
                   "firmware=%s node=%s api=80", cauce::Versions::kFirmware,
                   config.nodeId);
}

void loop() {
  esp_task_wdt_reset();
  // Downlink actuation, applied at the top of the loop rather than inside the
  // handler. The command usually arrives in the same batch as the sync response,
  // so a change made during ingestion would be racing the code that reads it.
  if (g_actuator != nullptr) {
    // A resync request is honoured by resetting the retry backoff rather than by
    // poking the scheduler: the command usually arrives with a sync response that
    // already failed, and the reason it is still failing is a backoff that has not
    // expired.
    if (g_actuator->consumeResyncRequest()) {
      g_syncManager->requestSyncNow();
      g_logger->event(cauce::LogLevel::Info, "resync_requested_by_command");
    }
    if (g_actuator->isDirty()) {
      // Written once per change rather than once per loop: flash does not care
      // that the setting did not change again ten milliseconds later.
      if (g_actuator->persist()) {
        const auto& s = g_actuator->settings();
        g_scheduler->setSamplingInterval(s.samplingIntervalSeconds);
        g_syncManager->setSyncIntervalS(s.syncIntervalSeconds);
        g_logger->eventf(cauce::LogLevel::Info, "settings_applied",
                         "sampling_s=%u sync_s=%u led=%u",
                         static_cast<unsigned>(s.samplingIntervalSeconds),
                         static_cast<unsigned>(s.syncIntervalSeconds),
                         static_cast<unsigned>(s.ledMode));
      }
    }
  }
  g_networkManager->tick();
  g_health.netState = g_networkManager->state();
  g_health.rssiDbm = g_netController->rssiDbm();
  g_scheduler->tick();
  const auto& counters = g_scheduler->counters();
  g_health.uptimeMs = g_clock.monotonicMs();
  g_health.utcTimeValid = g_clock.utcTimeValid();
  g_health.measurementCount = counters.measurementCount;
  g_health.storedCount = counters.storedCount;
  g_health.invalidCount = counters.invalidCount;
  g_health.suspectCount = counters.suspectCount;
  g_health.readFailures = counters.readFailures;
  g_health.storageFailures = counters.storageFailures;
  g_health.lastSuccessUtcMs = counters.lastSuccessUtcMs;
  g_health.storageRecords = g_store->totalRecords();
  g_health.storageBytes = g_store->totalBytes();
  const bool linkUp =
      (g_health.netState == cauce::app::NetState::Connected ||
       g_health.netState == cauce::app::NetState::Degraded);
  if (linkUp) {
    if (!g_ntpConfigured) {
      configTime(static_cast<long>(g_activeConfig.timezoneOffsetMin) * 60L, 0,
                 g_activeConfig.ntpServer);
      g_ntpConfigured = true;
    }
    g_syncManager->onNetworkConnected();
    g_syncManager->tick();
  } else {
    g_syncManager->onNetworkLost();
  }
  g_otaManager->tick();
  confirmBootIfPending(counters.measurementCount);
  g_portal->begin();
  g_portal->processNextRequest();
  g_apiServer->handleClient();
  maybeDeepSleep();
}

// A freshly flashed image stays PENDING_VERIFY until the node proves it can
// work. Without this an image that crashes on the first scheduler tick would
// stay installed forever, because nothing would ever mark it valid or roll it
// back. Evidence is deliberately weak-but-real: storage is writable, nothing
// failed writing, and either a measurement landed or the grace window passed.
void confirmBootIfPending(uint32_t measurementCount) {
  if (g_bootConfirm == nullptr) return;
  if (!g_otaControl->isPendingVerify()) return;
  cauce::app::BootSelfTest selfTest;
  selfTest.storageWritable = true;
  selfTest.measurementsStored = measurementCount;
  selfTest.storageFailures = g_health.storageFailures;
  selfTest.uptimeMs = static_cast<uint32_t>(g_clock.monotonicMs());
  if (!g_bootConfirm->tick(selfTest)) {
    g_health.storageFailures += 1;
  }
}

void maybeDeepSleep() {
  if (!g_activeConfig.deepSleepEnabled) return;
  const cauce::app::OtaState ota = g_otaManager->state();
  if (ota == cauce::app::OtaState::Downloading ||
      ota == cauce::app::OtaState::Checking ||
      ota == cauce::app::OtaState::RebootPending) {
    return;
  }
  const bool linkUp =
      (g_health.netState == cauce::app::NetState::Connected ||
       g_health.netState == cauce::app::NetState::Degraded);
  const size_t stored = g_store->totalRecords();
  // Stamp the last *newly stored* measurement, not "a loop where data was
  // pending". Otherwise msSinceLastMeasurement would reset on every
  // iteration while records await ack and the node would never sleep.
  if (stored != g_lastStoredSeen) {
    g_lastStoredSeen = stored;
    g_lastMeasurementMs = g_clock.monotonicMs();
  }
  cauce::app::DeepSleepInputs in;
  in.enabled = true;
  in.samplingIntervalS = g_activeConfig.samplingIntervalS;
  in.syncIntervalS = g_activeConfig.syncIntervalS;
  in.batteryV = g_health.batteryVoltageV;
  in.networkConnected = linkUp;
  in.portalActive = g_portal->started();
  in.storageHasPendingSync = stored > g_syncManager->lastAckedSequence();
  in.msSinceLastMeasurement = g_clock.monotonicMs() - g_lastMeasurementMs;
  in.msSinceBoot = g_clock.monotonicMs();
  const cauce::app::SleepPlan plan = g_sleepController.evaluate(in);
  if (plan.decision != cauce::app::SleepDecision::Sleep) return;
  g_logger->eventf(cauce::LogLevel::Info, "DEEP_SLEEP_ENTER",
                   "seconds=%u reason=%s", plan.sleepS, plan.reason);
  cauce::app::Esp32Sleeper::enterDeepSleep(plan.sleepS);
}

#else
#error "CAUCE firmware targets ESP32 or host simulation"
#endif

#else

#include "cauce/app/StdoutLogSink.h"
#include "cauce/hal/ManualClock.h"

using namespace cauce;

namespace {

uint64_t g_sharedUtcMs = 1787356800000ULL;

int runHostSimulation() {
  hal::MemoryFileSystem fs;
  LogStorageRepository store(fs, "data", 4096);
  hal::ManualClock clock(g_sharedUtcMs);
  app::StdoutLogSink sink;
  Logger logger(sink);
  ValidationEngine validator(defaultThresholds());

  logger.eventf(LogLevel::Info, "BOOT", "firmware=%s protocol=%u hw=%s",
                Versions::kFirmware, Versions::kProtocol,
                Versions::kHardwareRevision);

  app::MeasurementScheduler scheduler(clock, store, validator, logger);
  scheduler.setNodeId("CAUCE-SIM");
  scheduler.setSamplingInterval(60);

  drivers::SimulationProfile profile;
  drivers::SimulatedSensorDriver sensor("SIM-A", profile, &g_sharedUtcMs);
  scheduler.addSensor(&sensor);

  store.open();
  scheduler.setSequenceStart(store.lastSequence());
  scheduler.beginAllSensors();

  for (int cycle = 1; cycle <= 48; ++cycle) {
    if (cycle == 20) {
      sensor.injectFault(drivers::SimulatedSensorDriver::Fault::Frozen);
      logger.eventf(LogLevel::Warn, "SCENARIO", "event=FROZEN_SENSOR cycle=%d",
                    cycle);
    }
    if (cycle == 32) {
      sensor.injectFault(drivers::SimulatedSensorDriver::Fault::OutOfRange);
      logger.eventf(LogLevel::Warn, "SCENARIO", "event=OUT_OF_RANGE cycle=%d",
                    cycle);
    }
    if (cycle == 34) {
      sensor.injectFault(drivers::SimulatedSensorDriver::Fault::Disconnect);
      logger.eventf(LogLevel::Warn, "SCENARIO", "event=DISCONNECTED cycle=%d",
                    cycle);
    }
    clock.advanceMs(60000);
    g_sharedUtcMs = clock.utcMs();
    sensor.setUtcMs(g_sharedUtcMs);
    scheduler.tick();
  }
  sensor.injectFault(drivers::SimulatedSensorDriver::Fault::None);
  clock.advanceMs(60000);
  g_sharedUtcMs = clock.utcMs();
  sensor.setUtcMs(g_sharedUtcMs);
  scheduler.tick();

  Measurement latest{};
  if (store.latest(latest)) {
    logger.eventf(LogLevel::Info, "LATEST_RECORD",
                  "seq=%lu var=%s value=%.2f q=%s",
                  static_cast<unsigned long>(latest.sequence),
                  variableName(latest.variable),
                  static_cast<double>(latest.value),
                  qualityName(latest.quality));
  }

  Measurement page[16];
  QueryStats stats{};
  const size_t n = store.query(0, UINT64_MAX, 0, page, 16, stats);
  logger.eventf(LogLevel::Info, "QUERY_SAMPLE", "matched=%lu returned=%lu",
                static_cast<unsigned long>(stats.matched),
                static_cast<unsigned long>(n));

  const uint32_t corrupted = store.integrityCheck();
  logger.eventf(LogLevel::Info, "INTEGRITY_CHECK", "corrupted_frames=%lu total_records=%lu bytes=%lu",
                static_cast<unsigned long>(corrupted),
                static_cast<unsigned long>(store.totalRecords()),
                static_cast<unsigned long>(store.totalBytes()));

  {
    Measurement window[64];
    QueryStats wstats{};
    const size_t gotRecords =
        store.query(0, UINT64_MAX, 0, window, 64, wstats);

    float temps[64];
    const uint32_t nTemps =
        extractVariableValues(window, static_cast<uint32_t>(gotRecords),
                              Variable::AirTemperature, temps, 64);
    SampleStats stats;
    float scratch[64];
    if (computeSampleStats(temps, nTemps, stats, scratch, 64)) {
      logger.eventf(LogLevel::Info, "STATS_AIR_TEMPERATURE",
                    "n=%lu mean=%.2f min=%.2f max=%.2f std=%.2f p95=%.2f",
                    static_cast<unsigned long>(stats.count),
                    static_cast<double>(stats.mean),
                    static_cast<double>(stats.minValue),
                    static_cast<double>(stats.maxValue),
                    static_cast<double>(stats.stdDev),
                    static_cast<double>(stats.p95));
    }

    AggregateBucket buckets[8];
    const uint32_t nBuckets = aggregateBuckets(
        window, static_cast<uint32_t>(gotRecords),
        Variable::AirTemperature, 300, buckets, 8);
    logger.eventf(LogLevel::Info, "AGGREGATION_5MIN", "buckets=%lu",
                  static_cast<unsigned long>(nBuckets));

    const double exposureH =
        exposureHoursAbove(window, static_cast<uint32_t>(gotRecords), 30.0f);
    logger.eventf(LogLevel::Info, "EXPOSURE_HOURS_ABOVE_30C", "hours=%.3f",
                  exposureH);
  }

  {
    ChunkedExporter exporter(store, ChunkedExporter::Format::Csv, 0,
                             UINT64_MAX);
    char csvChunk[384];
    size_t printed = 0;
    while (!exporter.done() && printed < 2) {
      const size_t n = exporter.next(csvChunk, sizeof(csvChunk));
      if (n == 0) break;
      char firstLine[192] = {0};
      size_t lineLen = 0;
      while (lineLen + 1 < n && csvChunk[lineLen] != '\n') ++lineLen;
      std::memcpy(firstLine, csvChunk, lineLen < sizeof(firstLine) - 1
                                           ? lineLen
                                           : sizeof(firstLine) - 1);
      logger.eventf(LogLevel::Info, "CSV_PREVIEW", "%s", firstLine);
      ++printed;
    }
  }

  store.applyRetentionPolicy(2048);
  logger.eventf(LogLevel::Info, "RETENTION_APPLIED",
                "bytes_after=%lu segments=%lu",
                static_cast<unsigned long>(store.totalBytes()),
                static_cast<unsigned long>(store.segments().size()));

  const auto& c = scheduler.counters();
  logger.eventf(LogLevel::Info, "SUMMARY",
                "measured=%lu stored=%lu invalid=%lu suspect=%lu read_failures=%lu",
                static_cast<unsigned long>(c.measurementCount),
                static_cast<unsigned long>(c.storedCount),
                static_cast<unsigned long>(c.invalidCount),
                static_cast<unsigned long>(c.suspectCount),
                static_cast<unsigned long>(c.readFailures));
  return 0;
}

}  // namespace

int main() { return runHostSimulation(); }

#endif

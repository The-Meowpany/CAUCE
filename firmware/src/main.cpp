#include <cstdio>

#include "cauce/app/MeasurementScheduler.h"
#include "cauce/core/DataExporter.h"
#include "cauce/core/LogStorageRepository.h"
#include "cauce/core/Logger.h"
#include "cauce/core/Metrics.h"
#include "cauce/core/Types.h"
#include "cauce/drivers/Bme280Driver.h"
#include "cauce/drivers/SimulatedSensorDriver.h"
#include "cauce/hal/MemoryFileSystem.h"

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <Arduino.h>
#include <WebServer.h>

#include "cauce/app/ApiRouter.h"
#include "cauce/app/Esp32ApiServer.h"
#include "cauce/app/Esp32CaptivePortal.h"
#include "cauce/app/SyncManager.h"
#include "cauce/hal/Esp32HttpSyncTransport.h"
#include "cauce/app/SerialLogSink.h"
#include "cauce/core/ConfigManager.h"
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
static cauce::app::ApiRouter* g_apiRouter = nullptr;
static WebServer* g_webServer = nullptr;
static cauce::app::Esp32ApiServer* g_apiServer = nullptr;
static cauce::hal::Esp32HttpSyncTransport* g_syncTransport = nullptr;
static cauce::app::SyncManager* g_syncManager = nullptr;
static cauce::app::Esp32CaptivePortal* g_portal = nullptr;

void setup() {
  Serial.begin(115200);
  delay(200);

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
  g_logger->eventf(cauce::LogLevel::Info, "CONFIG_LOADED", "status=%d node=%s",
                   static_cast<int>(status), config.nodeId);

  const auto validation = cauce::ConfigManager::validate(config);
  if (!validation.ok) {
    g_logger->event(cauce::LogLevel::Warn,
                    "CONFIG_INVALID_USING_DEGRADED_DEFAULTS");
  }

  g_store->open();
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
  g_syncManager->loadState();

  g_logger->eventf(cauce::LogLevel::Info, "BOOT_COMPLETE",
                   "firmware=%s node=%s api=80", cauce::Versions::kFirmware,
                   config.nodeId);
}

void loop() {
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
  if (g_health.netState == cauce::app::NetState::Connected) {
    g_syncManager->onNetworkConnected();
    g_syncManager->tick();
  } else {
    g_syncManager->onNetworkLost();
  }
  g_portal->processNextRequest();
  g_apiServer->handleClient();
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

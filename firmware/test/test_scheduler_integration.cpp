#include <cstring>
#include <vector>

#include <unity.h>

#include "cauce/app/MeasurementScheduler.h"
#include "cauce/core/LogStorageRepository.h"
#include "cauce/core/Logger.h"
#include "cauce/core/RecordCodec.h"
#include "cauce/drivers/SimulatedSensorDriver.h"
#include "cauce/hal/ManualClock.h"
#include "cauce/hal/MemoryFileSystem.h"

using namespace cauce;
using namespace cauce::app;

namespace {

hal::MemoryFileSystem fs;
hal::ManualClock clock_(1787356800000ULL);
uint64_t sharedUtc = 1787356800000ULL;

class SilentLogSink final : public cauce::ILogSink {
 public:
  void writeLine(const char*) override {}
};

class FailingFileSystem final : public hal::IFileSystem {
 public:
  explicit FailingFileSystem(hal::IFileSystem& inner) : inner_(inner) {}

  bool failAppends{false};

  bool exists(const char* p) override { return inner_.exists(p); }
  bool appendBytes(const char* p, const uint8_t* d, size_t l) override {
    return failAppends ? false : inner_.appendBytes(p, d, l);
  }
  bool readRange(const char* p, size_t o, uint8_t* b, size_t l) override {
    return inner_.readRange(p, o, b, l);
  }
  bool writeWholeFile(const char* p, const uint8_t* d, size_t l) override {
    return inner_.writeWholeFile(p, d, l);
  }
  size_t fileSize(const char* p) override { return inner_.fileSize(p); }
  bool removeFile(const char* p) override { return inner_.removeFile(p); }
  int listFiles(const char* d, char (*o)[64], int m) override {
    return inner_.listFiles(d, o, m);
  }

 private:
  hal::IFileSystem& inner_;
};

SilentLogSink silentSink;

struct Rig {
  LogStorageRepository store;
  Logger logger;
  ValidationEngine validator;
  drivers::SimulatedSensorDriver sensor;
  MeasurementScheduler scheduler;

  Rig(uint32_t segmentBytes)
      : store(fs, "data_sched", segmentBytes),
        logger(silentSink),
        validator(defaultThresholds()),
        sensor("SIM-A", drivers::SimulationProfile{}, &sharedUtc),
        scheduler(clock_, store, validator, logger) {
    store.open();
    scheduler.setNodeId("CAUCE-T");
    scheduler.setSamplingInterval(60);
    scheduler.addSensor(&sensor);
    scheduler.setSequenceStart(store.lastSequence());
    scheduler.beginAllSensors();
  }
};
}  // namespace

void setUp() {
  char paths[16][64];
  const int nData = fs.listFiles("data", paths, 16);
  for (int i = 0; i < nData; ++i) fs.removeFile(paths[i]);
  const int nSched = fs.listFiles("data_sched", paths, 16);
  for (int i = 0; i < nSched; ++i) fs.removeFile(paths[i]);
  clock_ = hal::ManualClock(1787356800000ULL);
  sharedUtc = 1787356800000ULL;
}

void tearDown() {}

void test_scheduler_stores_validated_measurements_each_cycle() {
  Rig rig(kFrameSize * 64);
  for (int c = 0; c < 10; ++c) {
    clock_.advanceMs(60000);
    sharedUtc = clock_.utcMs();
    rig.sensor.setUtcMs(sharedUtc);
    rig.scheduler.tick();
  }
  TEST_ASSERT_EQUAL_UINT32(20, rig.scheduler.counters().measurementCount);
  TEST_ASSERT_EQUAL_UINT32(20, rig.scheduler.counters().storedCount);
  TEST_ASSERT_EQUAL_UINT32(0, rig.scheduler.counters().readFailures);

  Measurement page[32];
  QueryStats stats{};
  rig.store.query(0, UINT64_MAX, 0, page, 32, stats);
  TEST_ASSERT_EQUAL_UINT32(20, stats.matched);
  bool sawTemp = false;
  bool sawHumidity = false;
  for (size_t i = 0; i < stats.returned; ++i) {
    if (page[i].variable == Variable::AirTemperature) sawTemp = true;
    if (page[i].variable == Variable::RelativeHumidity) sawHumidity = true;
  }
  TEST_ASSERT_TRUE(sawTemp);
  TEST_ASSERT_TRUE(sawHumidity);
}

void test_sequence_continues_across_reboot_without_duplicates() {
  {
    Rig rig(kFrameSize * 64);
    for (int c = 0; c < 5; ++c) {
      clock_.advanceMs(60000);
      sharedUtc = clock_.utcMs();
      rig.sensor.setUtcMs(sharedUtc);
      rig.scheduler.tick();
    }
    TEST_ASSERT_EQUAL_UINT32(10, rig.store.lastSequence());
  }
  {
    Rig rig2(kFrameSize * 64);
    clock_.advanceMs(60000);
    sharedUtc = clock_.utcMs();
    rig2.sensor.setUtcMs(sharedUtc);
    rig2.scheduler.tick();
    TEST_ASSERT_EQUAL_UINT32(12, rig2.store.lastSequence());

    Measurement page[32];
    QueryStats stats{};
    rig2.store.query(0, UINT64_MAX, 0, page, 32, stats);
    TEST_ASSERT_EQUAL_UINT32(12, stats.matched);
    for (size_t i = 1; i < stats.returned; ++i) {
      TEST_ASSERT_TRUE(page[i].sequence != page[i - 1].sequence);
    }
  }
}

void test_sensor_disconnect_produces_missing_record_and_recovery() {
  Rig rig(kFrameSize * 64);
  for (int c = 0; c < 3; ++c) {
    clock_.advanceMs(60000);
    sharedUtc = clock_.utcMs();
    rig.sensor.setUtcMs(sharedUtc);
    rig.scheduler.tick();
  }
  rig.sensor.injectFault(drivers::SimulatedSensorDriver::Fault::Disconnect);
  for (int c = 0; c < 3; ++c) {
    clock_.advanceMs(60000);
    sharedUtc = clock_.utcMs();
    rig.scheduler.tick();
  }
  TEST_ASSERT_TRUE(rig.scheduler.counters().readFailures >= 3);

  Measurement page[64];
  QueryStats stats{};
  rig.store.query(0, UINT64_MAX, 0, page, 64, stats);
  int missingCount = 0;
  for (size_t i = 0; i < stats.returned; ++i) {
    if (page[i].quality == Quality::Missing) missingCount++;
  }
  TEST_ASSERT_TRUE(missingCount >= 1);

  rig.sensor.injectFault(drivers::SimulatedSensorDriver::Fault::None);
  clock_.advanceMs(60000);
  sharedUtc = clock_.utcMs();
  rig.sensor.setUtcMs(sharedUtc);
  rig.scheduler.tick();

  Measurement latest{};
  TEST_ASSERT_TRUE(rig.store.latest(latest));
  TEST_ASSERT_NOT_EQUAL(Quality::Missing, latest.quality);
}

void test_storage_failure_does_not_crash_node() {
  FailingFileSystem failingFs(fs);
  failingFs.failAppends = true;
  LogStorageRepository store(failingFs, "data_sched", kFrameSize * 64);
  Logger logger(silentSink);
  ValidationEngine validator(defaultThresholds());
  drivers::SimulatedSensorDriver sensor("SIM-A", drivers::SimulationProfile{},
                                        &sharedUtc);
  MeasurementScheduler scheduler(clock_, store, validator, logger);
  scheduler.setNodeId("CAUCE-T");
  scheduler.setSamplingInterval(60);
  scheduler.addSensor(&sensor);
  store.open();
  scheduler.beginAllSensors();

  for (int c = 0; c < 3; ++c) {
    clock_.advanceMs(60000);
    sharedUtc = clock_.utcMs();
    sensor.setUtcMs(sharedUtc);
    scheduler.tick();
  }
  TEST_ASSERT_TRUE(scheduler.counters().storageFailures >= 3);
  TEST_ASSERT_EQUAL(NodeState::Ready, scheduler.currentState());
}

void test_time_uncertainty_is_recorded_during_outage() {
  Rig rig(kFrameSize * 64);
  clock_.invalidateTime();
  sharedUtc = 0;
  clock_.advanceMs(60000);
  rig.sensor.setUtcMs(clock_.utcMs());
  rig.scheduler.tick();

  Measurement latest{};
  TEST_ASSERT_TRUE(rig.store.latest(latest));
  TEST_ASSERT_TRUE(latest.timeUncertain);
  TEST_ASSERT_TRUE(latest.quality != Quality::Missing);
}

void registerSchedulerTests() {

  RUN_TEST(test_scheduler_stores_validated_measurements_each_cycle);
  RUN_TEST(test_sequence_continues_across_reboot_without_duplicates);
  RUN_TEST(test_sensor_disconnect_produces_missing_record_and_recovery);
  RUN_TEST(test_storage_failure_does_not_crash_node);
  RUN_TEST(test_time_uncertainty_is_recorded_during_outage);
}

#include <cstdio>
#include <cstring>

#include "cauce/app/HostHttpTransport.h"
#include "cauce/app/SyncManager.h"
#include "cauce/core/LogStorageRepository.h"
#include "cauce/core/Logger.h"
#include "cauce/hal/MemoryFileSystem.h"
#include "cauce/hal/ManualClock.h"

using namespace cauce;
using namespace cauce::app;

namespace {

class Sink final : public ILogSink {
 public:
  void writeLine(const char* line) override { std::printf("%s\n", line); }
};

Measurement makeM(uint32_t seq, uint64_t tsMs) {
  Measurement m{};
  copyString(m.nodeId, sizeof(m.nodeId), "CAUCE-E2E");
  copyString(m.sensorId, sizeof(m.sensorId), "BME280-1");
  m.sequence = seq;
  m.timestampUtcMs = tsMs;
  m.variable = seq % 2 ? Variable::AirTemperature : Variable::RelativeHumidity;
  m.value = 18.0f + static_cast<float>(seq % 10) * 0.5f;
  m.quality = Quality::Valid;
  m.timeUncertain = false;
  return m;
}

}  // namespace

int main(int argc, char** argv) {
  const char* url = argc > 1 ? argv[1] : "http://127.0.0.1:8766/v1/sync";
  const int initialCount = argc > 2 ? std::atoi(argv[2]) : 50;

  hal::MemoryFileSystem fs;
  LogStorageRepository store(fs, "data_e2e");
  Sink sink;
  Logger logger(sink);
  hal::ManualClock clock(1787356800000ULL);

  HostHttpTransport transport;
  SyncManager manager(store, transport, clock, logger, fs,
                      "/state/sync_state");
  manager.setNodeId("CAUCE-E2E");
  manager.configureEndpoint(url, "");
  manager.loadState();

  store.open();
  const uint64_t baseTs = 1787356800000ULL;
  for (int i = 1; i <= initialCount; ++i) {
    store.append(makeM(static_cast<uint32_t>(i),
                       baseTs + static_cast<uint64_t>(i) * 60000ULL));
  }

  manager.onNetworkConnected();
  manager.tick();
  if (manager.lastAckedSequence() != static_cast<uint32_t>(initialCount)) {
    std::printf("E2E_FAIL stage=initial acked=%lu expected=%d\n",
                static_cast<unsigned long>(manager.lastAckedSequence()),
                initialCount);
    return 1;
  }
  std::printf("E2E_OK stage=initial acked=%lu\n",
              static_cast<unsigned long>(manager.lastAckedSequence()));

  for (int i = initialCount + 1; i <= initialCount + 5; ++i) {
    store.append(makeM(static_cast<uint32_t>(i),
                       baseTs + static_cast<uint64_t>(i) * 60000ULL));
  }
  clock.advanceMs(400000);
  manager.tick();
  const uint32_t expectedTotal = static_cast<uint32_t>(initialCount + 5);
  if (manager.lastAckedSequence() != expectedTotal) {
    std::printf("E2E_FAIL stage=incremental acked=%lu expected=%u\n",
                static_cast<unsigned long>(manager.lastAckedSequence()),
                expectedTotal);
    return 1;
  }
  std::printf("E2E_OK stage=incremental acked=%lu\n",
              static_cast<unsigned long>(manager.lastAckedSequence()));

  fs.removeFile("/state/sync_state");
  manager.loadState();
  clock.advanceMs(400000);
  manager.tick();
  if (manager.lastAckedSequence() != expectedTotal) {
    std::printf("E2E_FAIL stage=idempotent_replay acked=%lu\n",
                static_cast<unsigned long>(manager.lastAckedSequence()));
    return 1;
  }
  std::printf("E2E_OK stage=idempotent_replay acked=%lu total_sent_twice=yes\n",
              static_cast<unsigned long>(manager.lastAckedSequence()));
  return 0;
}

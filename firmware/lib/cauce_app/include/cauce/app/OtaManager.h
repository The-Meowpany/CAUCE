#pragma once

#include <cstddef>
#include <cstdint>

#include "cauce/app/OtaInterfaces.h"
#include "cauce/core/Logger.h"
#include "cauce/core/Sha256Stream.h"
#include "cauce/hal/IClock.h"

namespace cauce::app {

enum class OtaState : uint8_t {
  Idle = 0,
  Checking = 1,
  UpToDate = 2,
  Downloading = 3,
  RebootPending = 4,
  CheckFailed = 5,
  VerifyFailed = 6,
  InstallFailed = 7,
};

const char* otaStateName(OtaState state);

int compareSemver(const char* a, const char* b);

class OtaManager {
 public:
  struct Tuning {
    uint32_t checkIntervalS{21600};
    size_t chunkSize{512};
    uint32_t minFreeHeapBytes{40960};
    float minBatteryV{0.0f};
    uint32_t maxStallTicks{600};
  };

  using FreeHeapFn = uint32_t (*)();
  using BatteryFn = float (*)();
  using RebootFn = void (*)();

  // Called from inside `tick()`, at the points where it may block.
  //
  // WHY THIS EXISTS
  //
  // Downloading is mostly non-blocking: `read` returns NoDataYet and the manager waits for
  // the next tick. But two calls are not. `IFirmwareReader::open` performs a whole HTTP GET
  // - headers, and up to the socket read timeout - and `IManifestSource::fetchLatest` does
  // the same for the manifest. Both happen inside `tick()`, which runs inside `loop()`.
  //
  // The task watchdog is 30 s and the socket timeout is 15 s, so one blocking call fits.
  // What is not guaranteed is that only one happens per tick: a tick can open a download and
  // then, in the same pass, fetch a manifest for the next release, and 15 + 15 is already
  // past half the budget with the rest of loop() still to run. The margin was arithmetic
  // rather than a design, and the failure mode is a board that reboots mid-update with no
  // log line explaining it - the same class of symptom that made the original nine-second
  // reboot loop hard to read.
  //
  // Feeding at each blocking point makes the budget an invariant rather than a hope. A
  // default of nullptr keeps the host tests unchanged and honest: they are not pretending
  // to model a watchdog.
  using FeedFn = void (*)();

  OtaManager(IManifestSource& catalog, IFirmwareReader& reader,
             IFirmwareInstaller& installer, hal::IClock& clock, Logger& logger);

  void setFirmwareVersion(const char* version);
  void setSafetyHooks(FreeHeapFn freeHeap, BatteryFn battery);
  void setMinFreeHeapBytes(uint32_t minBytes);
  void setMinBatteryV(float minVolts);
  void setManifestKey(const uint8_t key[32]);
  // Symmetric with setManifestKey, and it exists because the manager now refuses to update
  // without a key. A test that wants to exercise the download path needs a key; a test that
  // wants to prove the refusal needs to be able to take it away again, and there was no way
  // to do that before.
  void clearManifestKey() { hasManifestKey_ = false; }
  void setRebootHook(RebootFn reboot);
  // See `FeedFn`. Passing nullptr, or never calling this, is supported.
  void setFeedHook(FeedFn feed) { feed_ = feed; }
  void setInterval(uint32_t checkIntervalS);
  void setMaxStallTicks(uint32_t ticks);
  void tick();

  OtaState state() const { return state_; }
  const char* pendingVersion() const { return pendingVersion_; }
  size_t downloadProgress() const { return downloadReceived_; }
  uint32_t stallTicks() const { return stallTicks_; }

 private:
  bool safetyOk() const;
  void runCheck();
  bool startDownload();
  bool pumpChunk();
  void feedWatchdog();
  bool finishDownload();
  void abortDownload(const char* event, OtaState failState, LogLevel level);
  void scheduleFailure(OtaState failureState, const char* event, LogLevel level);
  void resetDownload();

  IManifestSource& catalog_;
  IFirmwareReader& reader_;
  IFirmwareInstaller& installer_;
  hal::IClock& clock_;
  Logger& logger_;

  char currentVersion_[16]{"0.0.0"};
  char pendingVersion_[16]{};
  Tuning tuning_;
  OtaState state_{OtaState::Idle};
  uint64_t nextCheckMonotonicMs_{0};
  FreeHeapFn freeHeap_{nullptr};
  BatteryFn battery_{nullptr};
  RebootFn reboot_{nullptr};
  FeedFn feed_{nullptr};
  uint8_t manifestKey_[32];
  bool hasManifestKey_{false};

  Sha256Ctx shaCtx_{};
  size_t downloadReceived_{0};
  uint32_t stallTicks_{0};
  bool readerOpened_{false};
  OtaRelease pendingRelease_{};
};

}  // namespace cauce::app

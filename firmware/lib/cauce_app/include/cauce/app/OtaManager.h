#pragma once

#include <cstddef>
#include <cstdint>

#include "cauce/app/OtaInterfaces.h"
#include "cauce/core/Logger.h"
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
    size_t chunkSize{1024};
    uint32_t minFreeHeapBytes{40960};
    float minBatteryV{0.0f};
  };

  using FreeHeapFn = uint32_t (*)();
  using BatteryFn = float (*)();
  using RebootFn = void (*)();

  OtaManager(IManifestSource& catalog, IFirmwareReader& reader,
             IFirmwareInstaller& installer, hal::IClock& clock, Logger& logger);

  void setFirmwareVersion(const char* version);
  void setSafetyHooks(FreeHeapFn freeHeap, BatteryFn battery);
  void setMinFreeHeapBytes(uint32_t minBytes);
  void setMinBatteryV(float minVolts);
  void setRebootHook(RebootFn reboot);
  void setInterval(uint32_t checkIntervalS);
  void tick();

  OtaState state() const { return state_; }
  const char* pendingVersion() const { return pendingVersion_; }

 private:
  bool safetyOk() const;
  void runCheck();
  bool runDownload(const OtaRelease& release);
  void scheduleFailure(OtaState failureState, const char* event, LogLevel level);

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
};

}  // namespace cauce::app

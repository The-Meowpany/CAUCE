#pragma once

#include <cstdint>

#include "cauce/app/OtaRollback.h"
#include "cauce/core/Logger.h"
#include "cauce/hal/IFileSystem.h"

namespace cauce::app {

struct BootSelfTest {
  bool storageWritable{false};
  uint32_t measurementsStored{0};
  uint32_t storageFailures{0};
  uint32_t uptimeMs{0};
  uint32_t settleGraceMs{300000};
};

// Decides whether a freshly flashed image proved itself, and keeps the
// boot-attempt counter that survives the reboots a rollback causes.
//
// The counter has to live outside RAM: a rollback reboots the node, so a
// volatile counter would reset on every attempt and the guard would never
// reach its limit.
class OtaBootConfirm {
 public:
  OtaBootConfirm(IOtaControl& control, hal::IFileSystem& fs, Logger& logger,
                 const char* statePath = "/state/ota_boot",
                 uint8_t maxAttempts = OtaRollbackGuard::kDefaultMaxAttempts);

  void loadAttempts();

  // Verdict for the current boot. Pure decision, no side effects, so the
  // policy is testable without an ESP32 partition table.
  BootVerdict evaluate(const BootSelfTest& selfTest) const;

  // Evaluates and applies. Returns false only when a required flash
  // operation failed, which is worth surfacing as a boot fault.
  bool tick(const BootSelfTest& selfTest);

  bool selfTestPassed(const BootSelfTest& selfTest) const;

  uint32_t attempts() const { return attempts_; }
  bool settled() const { return settled_; }

  // True once the guard had something to say, i.e. the running image was
  // pending verification. Logged once so a healthy boot stays quiet.
  bool pendingVerifyObserved() const { return pendingObserved_; }

 private:
  void persistAttempts() const;

  IOtaControl& control_;
  hal::IFileSystem& fs_;
  Logger& logger_;
  char statePath_[64];
  uint8_t maxAttempts_;
  uint32_t attempts_{0};
  bool settled_{false};
  bool pendingObserved_{false};
};

}  // namespace cauce::app

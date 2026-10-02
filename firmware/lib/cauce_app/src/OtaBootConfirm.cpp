#include "cauce/app/OtaBootConfirm.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cauce::app {

namespace {

void copyString(char* dst, size_t cap, const char* src) {
  if (cap == 0) return;
  size_t i = 0;
  for (; src != nullptr && src[i] != '\0' && i + 1 < cap; ++i) dst[i] = src[i];
  dst[i] = '\0';
}

// Feeds the boot self-test result into the guard's no-argument
// selfTestPassed(), so OtaRollbackGuard stays the single place where the
// rollback policy lives and this class only supplies evidence.
class SelfTestAdapter final : public IOtaControl {
 public:
  SelfTestAdapter(IOtaControl& real, bool passed)
      : real_(real), passed_(passed) {}

  bool isPendingVerify() const override { return real_.isPendingVerify(); }
  bool markAppValid() override { return real_.markAppValid(); }
  bool requestRollback() override { return real_.requestRollback(); }
  bool selfTestPassed() const override { return passed_; }

 private:
  IOtaControl& real_;
  bool passed_;
};

}  // namespace

OtaBootConfirm::OtaBootConfirm(IOtaControl& control, hal::IFileSystem& fs,
                               Logger& logger, const char* statePath,
                               uint8_t maxAttempts)
    : control_(control),
      fs_(fs),
      logger_(logger),
      maxAttempts_(maxAttempts == 0 ? 1 : maxAttempts) {
  copyString(statePath_, sizeof(statePath_), statePath);
}

void OtaBootConfirm::loadAttempts() {
  attempts_ = 0;
  if (statePath_[0] == '\0') return;
  if (!fs_.exists(statePath_)) return;
  const size_t size = fs_.fileSize(statePath_);
  if (size == 0 || size > 48) return;
  char buf[49];
  if (!fs_.readRange(statePath_, 0, reinterpret_cast<uint8_t*>(buf), size)) return;
  buf[size] = '\0';
  const char* key = "boot_attempts=";
  const char* found = std::strstr(buf, key);
  if (found == nullptr) return;
  attempts_ = static_cast<uint32_t>(std::strtoul(found + std::strlen(key), nullptr, 10));
}

void OtaBootConfirm::persistAttempts() const {
  if (statePath_[0] == '\0') return;
  char buf[48];
  std::snprintf(buf, sizeof(buf), "boot_attempts=%lu\n",
                static_cast<unsigned long>(attempts_));
  fs_.writeWholeFile(statePath_, reinterpret_cast<const uint8_t*>(buf),
                     std::strlen(buf));
}

bool OtaBootConfirm::selfTestPassed(const BootSelfTest& selfTest) const {
  // Storing a measurement is the strongest evidence the image works, but a
  // node with a dead sensor would then never confirm and would be rolled
  // back forever. Storage writability plus a grace window is therefore
  // enough: the fallback exists precisely so a good image is not punished
  // for an unrelated sensor failure.
  if (!selfTest.storageWritable) return false;
  if (selfTest.storageFailures > 0) return false;
  if (selfTest.measurementsStored > 0) return true;
  return selfTest.uptimeMs >= selfTest.settleGraceMs;
}

BootVerdict OtaBootConfirm::evaluate(const BootSelfTest& selfTest) const {
  // Const evaluation must not touch flash, so only reason about it.
  if (!control_.isPendingVerify()) return BootVerdict::NotPending;
  if (selfTestPassed(selfTest)) return BootVerdict::MarkValid;
  if (attempts_ >= maxAttempts_) return BootVerdict::Rollback;
  return BootVerdict::StayPending;
}

bool OtaBootConfirm::tick(const BootSelfTest& selfTest) {
  if (!control_.isPendingVerify()) {
    // Nothing pending: a confirmed image must leave no counter behind, so a
    // later unrelated crash does not inherit a stale attempt count.
    if (pendingObserved_ && !settled_) {
      attempts_ = 0;
      settled_ = true;
      persistAttempts();
      logger_.event(LogLevel::Info, "OTA_BOOT_CONFIRMED");
    }
    return true;
  }

  if (!pendingObserved_) {
    pendingObserved_ = true;
    if (attempts_ < 0xFFFFFFFFu) attempts_ += 1;
    persistAttempts();
    logger_.eventf(LogLevel::Warn, "OTA_BOOT_ATTEMPT", "attempt=%lu max=%u",
                   static_cast<unsigned long>(attempts_),
                   static_cast<unsigned>(maxAttempts_));
  }

  SelfTestAdapter adapter(control_, selfTestPassed(selfTest));
  OtaRollbackGuard guard(adapter, maxAttempts_);
  const BootVerdict verdict =
      guard.evaluate(attempts_ > 0 ? attempts_ - 1 : 0);

  if (verdict == BootVerdict::NotPending || verdict == BootVerdict::StayPending)
    return true;

  if (!guard.apply(verdict)) {
    logger_.eventf(LogLevel::Error, "OTA_BOOT_APPLY_FAILED", "verdict=%u",
                   static_cast<unsigned>(verdict));
    return false;
  }

  if (verdict == BootVerdict::Rollback) {
    logger_.eventf(LogLevel::Warn, "OTA_BOOT_ROLLBACK", "attempt=%lu",
                   static_cast<unsigned long>(attempts_));
  } else {
    // Confirmed: the counter has no meaning for a good image, and leaving it
    // would let a later unrelated crash inherit the old count.
    settled_ = true;
    attempts_ = 0;
    persistAttempts();
    logger_.event(LogLevel::Info, "OTA_BOOT_CONFIRMED");
  }
  return true;
}

}  // namespace cauce::app
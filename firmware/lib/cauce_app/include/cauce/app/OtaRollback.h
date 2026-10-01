#pragma once

#include <cstdint>

namespace cauce::app {

enum class BootVerdict : uint8_t { NotPending, MarkValid, StayPending, Rollback };

class IOtaControl {
 public:
  virtual ~IOtaControl() = default;
  virtual bool isPendingVerify() const = 0;
  virtual bool markAppValid() = 0;
  virtual bool requestRollback() = 0;
  virtual bool selfTestPassed() const = 0;
};

class OtaRollbackGuard {
 public:
  static constexpr uint8_t kDefaultMaxAttempts = 3;

  explicit OtaRollbackGuard(IOtaControl& control,
                            uint8_t maxAttempts = kDefaultMaxAttempts)
      : control_(control),
        maxAttempts_(maxAttempts == 0 ? 1 : maxAttempts) {}

  BootVerdict evaluate(uint32_t attempt) const;

  bool apply(BootVerdict verdict) const;

  uint8_t maxAttempts() const { return maxAttempts_; }

 private:
  IOtaControl& control_;
  uint8_t maxAttempts_;
};

}  // namespace cauce::app

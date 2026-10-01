#pragma once

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include "cauce/app/OtaRollback.h"

namespace cauce::app {

class Esp32OtaControl final : public IOtaControl {
 public:
  Esp32OtaControl();
  bool isPendingVerify() const override;
  bool markAppValid() override;
  bool requestRollback() override;
  bool selfTestPassed() const override;
  void setSelfTestPassed(bool passed) { selfTestPassed_ = passed; }
  const char* runningPartitionLabel() const;
  uint32_t runningPartitionSize() const;

 private:
  bool selfTestPassed_{false};
};

}  // namespace cauce::app

#endif
#endif

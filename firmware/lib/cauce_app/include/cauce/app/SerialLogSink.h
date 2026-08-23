#pragma once

#ifdef ARDUINO
#include <Arduino.h>

#include "cauce/core/Logger.h"

namespace cauce::app {

class SerialLogSink final : public ILogSink {
 public:
  explicit SerialLogSink(HardwareSerial& serial) : serial_(serial) {}
  void writeLine(const char* line) override { serial_.println(line); }

 private:
  HardwareSerial& serial_;
};

}  // namespace cauce::app

#endif

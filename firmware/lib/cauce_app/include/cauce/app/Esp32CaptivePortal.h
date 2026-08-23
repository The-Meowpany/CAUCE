#pragma once

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <DNSServer.h>

namespace cauce::app {

class Esp32CaptivePortal {
 public:
  void begin();
  void processNextRequest();
  void stop();

 private:
  DNSServer dns_;
  bool started_{false};
};

}  // namespace cauce::app

#endif
#endif

#pragma once

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <cstdint>

namespace cauce::app {

class Esp32Sleeper {
 public:
  // Does not return on success: the chip resets and the loop restarts.
  static void enterDeepSleep(uint32_t seconds);
};

}  // namespace cauce::app

#endif
#endif

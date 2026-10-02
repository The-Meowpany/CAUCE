#include "cauce/hal/Esp32Sleeper.h"

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <esp_sleep.h>

namespace cauce::app {

void Esp32Sleeper::enterDeepSleep(uint32_t seconds) {
  if (seconds == 0) return;
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(seconds) * 1000000ULL);
  esp_deep_sleep_start();
}

}  // namespace cauce::app

#endif
#endif

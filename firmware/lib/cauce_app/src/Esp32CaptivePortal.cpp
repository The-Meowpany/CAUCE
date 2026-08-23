#include "cauce/app/Esp32CaptivePortal.h"

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <WiFi.h>

namespace cauce::app {

void Esp32CaptivePortal::begin() {
  if (started_) return;
  const IPAddress apIp = WiFi.softAPIP();
  if (apIp == IPAddress(0, 0, 0, 0)) return;
  dns_.start(53, "*", apIp);
  started_ = true;
}

void Esp32CaptivePortal::processNextRequest() {
  if (started_) dns_.processNextRequest();
}

void Esp32CaptivePortal::stop() {
  if (!started_) return;
  dns_.stop();
  started_ = false;
}

}  // namespace cauce::app

#endif
#endif

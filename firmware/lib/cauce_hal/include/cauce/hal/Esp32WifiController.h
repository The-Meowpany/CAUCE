#pragma once

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include "cauce/hal/INetworkController.h"

namespace cauce::hal {

class Esp32WifiController final : public INetworkController {
 public:
  Esp32WifiController();
  bool startAp(const char* ssid) override;
  void stopAp() override;
  bool connectSta(const char* ssid, const char* password) override;
  void disconnectSta() override;
  NetEvent pollEvent() override;
  int8_t rssiDbm() override;

 private:
  bool connecting_{false};
  bool wasConnected_{false};
};

}  // namespace cauce::hal

#endif
#endif

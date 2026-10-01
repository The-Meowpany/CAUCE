#include "cauce/hal/Esp32WifiController.h"

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <WiFi.h>

namespace cauce::hal {

Esp32WifiController::Esp32WifiController() {}

bool Esp32WifiController::startAp(const char* ssid) {
  WiFi.mode(WIFI_AP_STA);
  return WiFi.softAP(ssid ? ssid : "CAUCE-AP");
}

void Esp32WifiController::stopAp() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
}

bool Esp32WifiController::connectSta(const char* ssid, const char* password) {
  if (!ssid || !ssid[0]) return false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password ? password : "");
  connecting_ = true;
  return true;
}

void Esp32WifiController::disconnectSta() {
  connecting_ = false;
  wasConnected_ = false;
  WiFi.disconnect(true);
}

NetEvent Esp32WifiController::pollEvent() {
  const wl_status_t st = WiFi.status();
  const bool connected = (st == WL_CONNECTED);
  if (connected && !wasConnected_) {
    wasConnected_ = true;
    connecting_ = false;
    return NetEvent::GotIp;
  }
  if (!connected && wasConnected_) {
    wasConnected_ = false;
    connecting_ = false;
    return NetEvent::LinkLost;
  }
  if (!connected && connecting_ && st == WL_CONNECT_FAILED) {
    connecting_ = false;
    return NetEvent::ConnectFailed;
  }
  return NetEvent::None;
}

int8_t Esp32WifiController::rssiDbm() {
  if (WiFi.status() != WL_CONNECTED) return -100;
  const long rssi = WiFi.RSSI();
  if (rssi < -127) return -100;
  if (rssi > 0) return 0;
  return static_cast<int8_t>(rssi);
}

}  // namespace cauce::hal

#endif
#endif

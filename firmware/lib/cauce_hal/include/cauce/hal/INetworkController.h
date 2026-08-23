#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce::hal {

enum class NetEvent : uint8_t {
  None = 0,
  GotIp = 1,
  ConnectFailed = 2,
  LinkLost = 3,
};

class INetworkController {
 public:
  virtual ~INetworkController() = default;
  virtual bool startAp(const char* ssid) = 0;
  virtual void stopAp() = 0;
  virtual bool connectSta(const char* ssid, const char* password) = 0;
  virtual void disconnectSta() = 0;
  virtual NetEvent pollEvent() = 0;
  virtual int8_t rssiDbm() = 0;
};

}  // namespace cauce::hal

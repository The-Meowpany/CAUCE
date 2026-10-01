#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce::hal {

class ILoRaRadio {
 public:
  virtual ~ILoRaRadio() = default;
  virtual bool canSendNow() = 0;
  virtual bool send(const uint8_t* data, size_t length) = 0;
  virtual int16_t lastRssiDbm() const = 0;
};

}  // namespace cauce::hal

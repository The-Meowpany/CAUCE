#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce::hal {

class ILoRaRadio {
 public:
  virtual ~ILoRaRadio() = default;
  virtual bool canSendNow() = 0;
  virtual bool send(const uint8_t* data, size_t length) = 0;

  // Copies one received frame into `buffer` and returns its length, or 0 when
  // nothing is pending. A negative value means the radio failed to read.
  //
  // Receive exists so a delivery can be confirmed. A send-only radio cannot
  // tell the node whether a frame arrived, which turns every LoRa uplink into
  // an at-most-once gamble: the node has to advance its watermark on faith and
  // a lost frame becomes permanent data loss.
  virtual int receive(uint8_t* buffer, size_t capacity) = 0;

  virtual int16_t lastRssiDbm() const = 0;
};

}  // namespace cauce::hal
#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce::hal {

// A byte-stream serial port. The MH-Z19C speaks a 9-byte request/response protocol over one.
class IUart {
 public:
  virtual ~IUart() = default;
  virtual bool begin(uint32_t baud) = 0;
  virtual size_t write(const uint8_t* data, size_t length) = 0;
  // Reads up to `length` bytes, waiting at most `timeoutMs` for them. Returns how many arrived.
  virtual size_t read(uint8_t* buffer, size_t length, uint32_t timeoutMs) = 0;
  // Drops anything already buffered, so a late reply to an earlier request is never taken as
  // the answer to the current one.
  virtual void discardInput() = 0;
};

// Analogue input in millivolts at the pin. Millivolts rather than raw counts because the
// ESP32's ADC is non-linear and its calibrated conversion belongs below this line, not in
// every driver.
class IAnalogInput {
 public:
  virtual ~IAnalogInput() = default;
  virtual bool readMillivolts(uint8_t channel, float& outMillivolts) = 0;
};

// Dallas 1-Wire bus primitives; the DS18B20 protocol is built on these three.
class IOneWire {
 public:
  virtual ~IOneWire() = default;
  // Reset pulse. Returns true when at least one device answered with a presence pulse.
  virtual bool reset() = 0;
  virtual void writeByte(uint8_t value) = 0;
  virtual uint8_t readByte() = 0;
};

}  // namespace cauce::hal

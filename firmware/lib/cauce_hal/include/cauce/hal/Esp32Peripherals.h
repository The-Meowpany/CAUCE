#pragma once

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <HardwareSerial.h>

#include "cauce/hal/IPeripherals.h"

namespace cauce::hal {

class Esp32Uart final : public IUart {
 public:
  Esp32Uart(HardwareSerial& port, int rxPin, int txPin);
  bool begin(uint32_t baud) override;
  size_t write(const uint8_t* data, size_t length) override;
  size_t read(uint8_t* buffer, size_t length, uint32_t timeoutMs) override;
  void discardInput() override;

 private:
  HardwareSerial& port_;
  int rxPin_;
  int txPin_;
};

// The channel is the GPIO number. Only ADC1 pins are usable: ADC2 is shared with the Wi-Fi
// radio and returns errors whenever Wi-Fi is up, which on this node is most of the time.
class Esp32AnalogInput final : public IAnalogInput {
 public:
  // Readings are averaged over `samples` conversions; the ESP32 ADC has several LSB of noise.
  explicit Esp32AnalogInput(uint8_t samples = 16);
  bool readMillivolts(uint8_t channel, float& outMillivolts) override;

 private:
  uint8_t samples_;
};

// Bit-banged 1-Wire on one GPIO with the pull-up fitted externally (4.7k to 3V3). Timing is
// the standard-speed slot from the Maxim application note; interrupts are masked only for the
// few microseconds of each slot that are timing-critical.
class Esp32OneWire final : public IOneWire {
 public:
  explicit Esp32OneWire(uint8_t pin);
  bool reset() override;
  void writeByte(uint8_t value) override;
  uint8_t readByte() override;

 private:
  void writeBit(bool bit);
  bool readBit();

  uint8_t pin_;
};

}  // namespace cauce::hal

#endif
#endif

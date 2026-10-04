#pragma once

// The two buses a radio driver needs, behind interfaces.
//
// This exists so a register-level driver can be tested without hardware. A driver
// written directly against `SPIClass` can only be tested by plugging in a board and
// reading a register, which means the register sequence is never asserted by a test
// and only by whatever the radio happens to do. Behind these two interfaces the
// whole protocol - opmode transitions, FIFO addressing, interrupt masking, the RSSI
// conversion - is exercised on the host against a scripted bus.
//
// Nothing here knows about ESP32.

#include <cstddef>
#include <cstdint>

namespace cauce::hal {

// Full-duplex SPI. One call transfers one byte in each direction, which is what the
// SX1276 expects: the register address is sent on MOSI while the previous byte
// arrives on MISO, so a read is a transfer whose output is discarded.
class ISpiBus {
 public:
  virtual ~ISpiBus() = default;

  // Chip select low.
  virtual void beginTransaction() = 0;
  // Chip select high.
  virtual void endTransaction() = 0;

  // Transfers `length` bytes, ignoring MOSI and returning MISO.
  virtual uint8_t transfer(uint8_t out) = 0;

  // Blocking read into `buffer`. Provided as a default over transfer() so a test
  // double only has to implement the primitive.
  virtual void read(uint8_t* buffer, size_t length) {
    for (size_t i = 0; i < length; ++i) buffer[i] = transfer(0x00);
  }

  // Blocking write from `buffer`, discarding MISO.
  virtual void write(const uint8_t* buffer, size_t length) {
    for (size_t i = 0; i < length; ++i) (void)transfer(buffer[i]);
  }
};

// Pins and a monotonic millisecond clock, so the driver never calls Arduino.
class IRadioControl {
 public:
  virtual ~IRadioControl() = default;

  virtual void pinMode(uint8_t pin, bool output) = 0;
  virtual void digitalWrite(uint8_t pin, bool high) = 0;
  virtual bool digitalRead(uint8_t pin) const = 0;

  // Monotonic milliseconds. Must not wrap within a frame's airtime.
  virtual uint32_t millis() const = 0;
  // Blocking, but bounded: never longer than the longest plausible instruction.
  virtual void delay(uint32_t milliseconds) = 0;
};

}  // namespace cauce::hal

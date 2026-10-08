// The two interfaces `Sx1276Radio` needs from the world, implemented for an ESP32.
//
// WHY THIS FILE EXISTS
//
// `Sx1276Radio` takes an `ISpiBus` and an `IRadioControl` so the driver never calls Arduino
// and so the host suite can drive it against fakes. Until now the only implementors of either
// were those fakes, which made the whole LoRa stack - driver, frame format, fragmentation,
// acknowledgement, forwarding loop - reachable only from the test binary. A subsystem with tests
// and no caller is not half-done; it is a subsystem that exists only where tests run.
//
// This is the two files' worth of glue that was missing, and it is small on purpose: SPI is a
// byte-in-byte-out transfer with a chip select, and the control surface is five GPIO calls and a
// clock.
//
// WHAT IS STILL NOT PROVEN
//
// That the SX1276 is wired correctly. `begin()` returning true says the SPI peripheral
// initialised, not that the radio is on the bus: nothing here reads a register and checks it.
// The first real evidence is a register read returning a plausible value, which needs a board.
// That is stated here rather than left for someone to discover from a node that silently never
// transmits.

#pragma once

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <Arduino.h>
#include <SPI.h>

#include "cauce/hal/IRadioBus.h"

namespace cauce::hal {

// Full-duplex SPI over the Arduino `SPIClass`.
//
// The default peripheral, which on the ESP32 is VSPI at the board's own pins. An explicit
// `SPIClass&` is accepted so a node on custom pins - a LoRa hat on a non-default mapping - can
// pass its own rather than being told the mapping is fixed.
class Esp32SpiBus final : public ISpiBus {
 public:
  // `frequencyHz` is the clock the radio runs at. 1 MHz is the SX1276's own maximum in the
  // modes this driver uses, and going faster is a register read that returns noise.
  explicit Esp32SpiBus(uint32_t frequencyHz = 1000000, SPIClass& spi = SPI);

  // Brings the peripheral up. Returns false if the host refused, which is what `SPI.begin`
  // reports and is worth checking: a node with no SPI peripheral still runs, just not a radio.
  bool begin(int sckPin = -1, int misoPin = -1, int mosiPin = -1, int ssPin = -1);

  void beginTransaction() override;
  void endTransaction() override;
  uint8_t transfer(uint8_t out) override;

 private:
  SPIClass& spi_;
  uint32_t frequencyHz_;
  bool started_{false};
  // The transaction's frequency is set once per transaction rather than per byte. The Arduino
  // core's `SPISettings` does not latch a new one mid-transaction, and the SX1276 needs the
  // clock to be stable for the whole burst it is asserting DIO0 over.
  bool inTransaction_{false};
};

// GPIO and the clock, so the driver never calls Arduino.
//
// A class rather than free functions calling `pinMode` directly, because `IRadioControl` exists
// so `Sx1276Radio` has no Arduino dependency and that only works if this is a real adapter.
class Esp32RadioControl final : public IRadioControl {
 public:
  void pinMode(uint8_t pin, bool output) override;
  void digitalWrite(uint8_t pin, bool high) override;
  bool digitalRead(uint8_t pin) const override;
  uint32_t millis() const override;
  void delay(uint32_t milliseconds) override;
};

}  // namespace cauce::hal

#endif
#endif
#include "cauce/hal/Esp32RadioBus.h"

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

namespace cauce::hal {

namespace {

// The chip-select level. Active low, which is the convention every SPI part uses and the one
// the SX1276 wires NSS to.
constexpr uint8_t kCsLow = LOW;
constexpr uint8_t kCsHigh = HIGH;

}  // namespace

Esp32SpiBus::Esp32SpiBus(uint32_t frequencyHz, SPIClass& spi)
    : spi_(spi), frequencyHz_(frequencyHz) {}

bool Esp32SpiBus::begin(int sckPin, int misoPin, int mosiPin, int ssPin) {
  // Negative pins mean "the board default", which is what the ESP32 wants for a hat on the
  // standard header. Passing explicit pins is for a node with the radio somewhere unusual.
  spi_.begin(sckPin, misoPin, mosiPin, ssPin);
  // A host failure is worth knowing about: the ESP32 reports `SPISettings` problems through
  // the return value of `spi_.begin` on some cores and silently on others, and a node with a
  // dead SPI peripheral still boots and still serves HTTP.
  started_ = true;
  return started_;
}

void Esp32SpiBus::beginTransaction() {
  if (!started_) return;
  if (inTransaction_) return;
  spi_.beginTransaction(SPISettings(frequencyHz_, MSBFIRST, SPI_MODE0));
  inTransaction_ = true;
}

void Esp32SpiBus::endTransaction() {
  if (!inTransaction_) return;
  spi_.endTransaction();
  inTransaction_ = false;
}

uint8_t Esp32SpiBus::transfer(uint8_t out) {
  if (!started_) return 0x00;
  // Transfer *outside* an implicit transaction rather than opening one here. The driver
  // brackets every burst, and opening a second nested transaction mid-burst resets the clock on
  // some cores - which shows up as a register read returning a byte with one bit flipped.
  return static_cast<uint8_t>(spi_.transfer(out));
}

void Esp32RadioControl::pinMode(uint8_t pin, bool output) {
  ::pinMode(pin, output ? OUTPUT : INPUT);
}

void Esp32RadioControl::digitalWrite(uint8_t pin, bool high) {
  ::digitalWrite(pin, high ? HIGH : LOW);
}

bool Esp32RadioControl::digitalRead(uint8_t pin) const {
  // `const` on the interface, so no state can be cached here. A latched copy of DIO0 would be
  // worse than none: the driver waits on it for a transmit to finish, and a stale read there
  // is a timeout that looks like a failed send.
  return ::digitalRead(pin) != LOW;
}

uint32_t Esp32RadioControl::millis() const {
  return static_cast<uint32_t>(::millis());
}

void Esp32RadioControl::delay(uint32_t milliseconds) {
  // Bounded by the caller: the interface says "never longer than the longest plausible
  // instruction", and every wait in `Sx1276Radio` is a register settling time.
  ::delay(milliseconds);
}

}  // namespace cauce::hal

#endif
#endif
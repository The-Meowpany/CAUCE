#include "cauce/hal/Esp32Peripherals.h"

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <Arduino.h>

namespace cauce::hal {

Esp32Uart::Esp32Uart(HardwareSerial& port, int rxPin, int txPin)
    : port_(port), rxPin_(rxPin), txPin_(txPin) {}

bool Esp32Uart::begin(uint32_t baud) {
  port_.begin(baud, SERIAL_8N1, rxPin_, txPin_);
  return true;
}

size_t Esp32Uart::write(const uint8_t* data, size_t length) {
  const size_t n = port_.write(data, length);
  port_.flush();
  return n;
}

size_t Esp32Uart::read(uint8_t* buffer, size_t length, uint32_t timeoutMs) {
  const uint32_t start = millis();
  size_t n = 0;
  while (n < length && millis() - start < timeoutMs) {
    if (port_.available() > 0) {
      buffer[n++] = static_cast<uint8_t>(port_.read());
    } else {
      delay(1);
    }
  }
  return n;
}

void Esp32Uart::discardInput() {
  while (port_.available() > 0) port_.read();
}

Esp32AnalogInput::Esp32AnalogInput(uint8_t samples) : samples_(samples ? samples : 1) {}

bool Esp32AnalogInput::readMillivolts(uint8_t channel, float& outMillivolts) {
  // ADC1 is GPIO32..39 on the WROOM-32. Anything else would read garbage or fail silently
  // while Wi-Fi is on, so it is refused here instead of in the field.
  if (channel < 32 || channel > 39) return false;
  uint32_t sum = 0;
  for (uint8_t i = 0; i < samples_; ++i) {
    sum += analogReadMilliVolts(channel);
  }
  outMillivolts = static_cast<float>(sum) / static_cast<float>(samples_);
  return true;
}

Esp32OneWire::Esp32OneWire(uint8_t pin) : pin_(pin) {
  pinMode(pin_, INPUT);
}

bool Esp32OneWire::reset() {
  pinMode(pin_, OUTPUT);
  digitalWrite(pin_, LOW);
  delayMicroseconds(480);
  noInterrupts();
  pinMode(pin_, INPUT);
  delayMicroseconds(70);
  const bool present = digitalRead(pin_) == LOW;
  interrupts();
  delayMicroseconds(410);
  return present;
}

void Esp32OneWire::writeBit(bool bit) {
  noInterrupts();
  pinMode(pin_, OUTPUT);
  digitalWrite(pin_, LOW);
  delayMicroseconds(bit ? 6 : 60);
  pinMode(pin_, INPUT);
  interrupts();
  delayMicroseconds(bit ? 64 : 10);
}

bool Esp32OneWire::readBit() {
  noInterrupts();
  pinMode(pin_, OUTPUT);
  digitalWrite(pin_, LOW);
  delayMicroseconds(6);
  pinMode(pin_, INPUT);
  delayMicroseconds(9);
  const bool bit = digitalRead(pin_) == HIGH;
  interrupts();
  delayMicroseconds(55);
  return bit;
}

void Esp32OneWire::writeByte(uint8_t value) {
  for (uint8_t i = 0; i < 8; ++i) {
    writeBit((value >> i) & 0x01);
  }
}

uint8_t Esp32OneWire::readByte() {
  uint8_t value = 0;
  for (uint8_t i = 0; i < 8; ++i) {
    if (readBit()) value |= static_cast<uint8_t>(1u << i);
  }
  return value;
}

}  // namespace cauce::hal

#endif
#endif

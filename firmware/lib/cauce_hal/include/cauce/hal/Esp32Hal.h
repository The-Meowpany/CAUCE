#pragma once

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include "cauce/hal/IClock.h"
#include "cauce/hal/IFileSystem.h"
#include "cauce/hal/II2cBus.h"

#include <Wire.h>

namespace cauce::hal {

class Esp32Clock final : public IClock {
 public:
  Esp32Clock();
  uint32_t monotonicMs() const override;
  uint64_t utcMs() const override;
  bool utcTimeValid() const override;
  void setUtcMs(uint64_t epochMs) override;
  void sleepMs(uint32_t durationMs) override;

 private:
  mutable uint64_t lastUtcMs_;
  mutable uint32_t anchorMonotonicMs_;
};

class Esp32WireBus final : public II2cBus {
 public:
  Esp32WireBus(int sdaPin, int sclPin, uint32_t frequencyHz);
  bool begin() override;
  bool writeRegisters(uint8_t deviceAddress, uint8_t startRegister,
                      const uint8_t* data, size_t length) override;
  bool readRegisters(uint8_t deviceAddress, uint8_t startRegister,
                     uint8_t* buffer, size_t length) override;
  bool isPresent(uint8_t deviceAddress) override;

 private:
  int sdaPin_;
  int sclPin_;
  uint32_t frequencyHz_;
};

class Esp32LittleFs final : public IFileSystem {
 public:
  bool mount();
  bool exists(const char* path) override;
  bool appendBytes(const char* path, const uint8_t* data, size_t length) override;
  bool readRange(const char* path, size_t offset, uint8_t* buffer,
                 size_t length) override;
  bool writeWholeFile(const char* path, const uint8_t* data, size_t length) override;
  size_t fileSize(const char* path) override;
  bool removeFile(const char* path) override;
  int listFiles(const char* directory, char (*outPaths)[64], int maxItems) override;
};

}  // namespace cauce::hal

#endif
#endif

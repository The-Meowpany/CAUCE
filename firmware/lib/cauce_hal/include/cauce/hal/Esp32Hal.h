#pragma once

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include "cauce/hal/IClock.h"
#include "cauce/hal/IFileSystem.h"
#include "cauce/hal/II2cBus.h"

#include <FS.h>
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
  bool writeBytes(uint8_t deviceAddress, const uint8_t* data, size_t length) override;
  bool readBytes(uint8_t deviceAddress, uint8_t* buffer, size_t length) override;

 private:
  int sdaPin_;
  int sclPin_;
  uint32_t frequencyHz_;
};

// IFileSystem over any Arduino `fs::FS`. LittleFS and the SD card share it, so the
// directory-iteration fix in listFiles() covers both.
class Esp32ArduinoFs : public IFileSystem {
 public:
  explicit Esp32ArduinoFs(fs::FS& fs);
  bool exists(const char* path) override;
  bool appendBytes(const char* path, const uint8_t* data, size_t length) override;
  bool readRange(const char* path, size_t offset, uint8_t* buffer,
                 size_t length) override;
  bool writeWholeFile(const char* path, const uint8_t* data, size_t length) override;
  size_t fileSize(const char* path) override;
  bool removeFile(const char* path) override;
  int listFiles(const char* directory, char (*outPaths)[64], int maxItems) override;

 protected:
  fs::FS& fs_;
};

class Esp32LittleFs final : public Esp32ArduinoFs {
 public:
  Esp32LittleFs();
  bool mount();
};

// microSD on the shared VSPI bus. A card pulled out and pushed back in needs a fresh mount,
// so any failed operation drops the mount and the next one tries again; the archive's own
// backoff decides how often that is.
class Esp32SdCard final : public Esp32ArduinoFs {
 public:
  Esp32SdCard(uint8_t csPin, uint32_t frequencyHz);
  bool mount();
  bool mounted() const { return mounted_; }

  bool exists(const char* path) override;
  bool appendBytes(const char* path, const uint8_t* data, size_t length) override;
  bool readRange(const char* path, size_t offset, uint8_t* buffer,
                 size_t length) override;
  bool writeWholeFile(const char* path, const uint8_t* data, size_t length) override;
  size_t fileSize(const char* path) override;

 private:
  bool ensureMounted();
  bool track(bool ok);

  uint8_t csPin_;
  uint32_t frequencyHz_;
  bool mounted_{false};
};

}  // namespace cauce::hal

#endif
#endif

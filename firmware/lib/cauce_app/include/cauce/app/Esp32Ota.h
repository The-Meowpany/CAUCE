#pragma once

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <HTTPClient.h>

#include <cstring>

#include "cauce/app/OtaInterfaces.h"

namespace cauce::app {

class Esp32ManifestSource final : public IManifestSource {
 public:
  Esp32ManifestSource();
  void configure(const char* baseUrl, const char* nodeId);
  bool fetchLatest(const char* currentVersion, OtaRelease& out) override;

 private:
  char url_[160]{};
  char nodeId_[16]{};
};

class Esp32FirmwareReader final : public IFirmwareReader {
 public:
  Esp32FirmwareReader();
  bool open(const char* url) override;
  size_t read(uint8_t* buffer, size_t capacity) override;
  void close() override;

 private:
  HTTPClient http_;
  Stream* stream_{nullptr};
};

class Esp32FirmwareInstaller final : public IFirmwareInstaller {
 public:
  bool beginInstall(uint32_t totalSize) override;
  bool writeChunk(const uint8_t* data, size_t length) override;
  InstallDecision finishInstall() override;
  void abortInstall() override;
};

}  // namespace cauce::app

#endif
#endif

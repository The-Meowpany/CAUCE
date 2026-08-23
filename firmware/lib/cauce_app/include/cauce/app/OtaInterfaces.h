#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce::app {

struct OtaRelease {
  char version[16];
  char sha256Hex[65];
  char url[128];
  char manifestHmacHex[65];
  uint32_t totalSize;
};

class IManifestSource {
 public:
  virtual ~IManifestSource() = default;
  virtual bool fetchLatest(const char* currentVersion, OtaRelease& out) = 0;
};

class IFirmwareReader {
 public:
  virtual ~IFirmwareReader() = default;
  virtual bool open(const char* url) = 0;
  virtual size_t read(uint8_t* buffer, size_t capacity) = 0;
  virtual void close() = 0;
};

enum class InstallDecision : uint8_t { Proceed, Abort };

class IFirmwareInstaller {
 public:
  virtual ~IFirmwareInstaller() = default;
  virtual bool beginInstall(uint32_t totalSize) = 0;
  virtual bool writeChunk(const uint8_t* data, size_t length) = 0;
  virtual InstallDecision finishInstall() = 0;
  virtual void abortInstall() = 0;
};

}  // namespace cauce::app

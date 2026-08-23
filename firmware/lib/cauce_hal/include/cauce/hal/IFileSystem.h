#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce::hal {

class IFileSystem {
 public:
  virtual ~IFileSystem() = default;
  virtual bool exists(const char* path) = 0;
  virtual bool appendBytes(const char* path, const uint8_t* data, size_t length) = 0;
  virtual bool readRange(const char* path, size_t offset, uint8_t* buffer,
                         size_t length) = 0;
  virtual bool writeWholeFile(const char* path, const uint8_t* data, size_t length) = 0;
  virtual size_t fileSize(const char* path) = 0;
  virtual bool removeFile(const char* path) = 0;
  virtual int listFiles(const char* directory, char (*outPaths)[64], int maxItems) = 0;
};

}  // namespace cauce::hal

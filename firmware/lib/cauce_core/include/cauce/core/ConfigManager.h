#pragma once

#include "cauce/core/NodeConfig.h"
#include "cauce/hal/IFileSystem.h"

namespace cauce {

enum class ConfigLoadStatus : uint8_t {
  Loaded = 0,
  CreatedDefaults = 1,
  RestoredFromBackup = 2,
};

struct ConfigValidation {
  bool ok{true};
  uint8_t errorCount{0};
  char errors[4][64]{};

  void addError(const char* message) {
    ok = false;
    if (errorCount < 4) {
      copyString(errors[errorCount], sizeof(errors[0]), message);
    }
    ++errorCount;
  }
};

class ConfigManager {
 public:
  ConfigManager(hal::IFileSystem& fileSystem, const char* configPath);

  ConfigLoadStatus load(NodeConfig& outConfig);
  bool save(const NodeConfig& config);
  static ConfigValidation validate(const NodeConfig& config);
  const NodeConfig& current() const { return current_; }

 private:
  bool readTextFile(const char* path, char* buffer, size_t capacity);

  static constexpr size_t kScratchSize = 8192;

  hal::IFileSystem& fs_;
  char path_[64];
  char backupPath_[68];
  NodeConfig current_{};
  char scratch_[kScratchSize];
  uint8_t backupScratch_[kScratchSize];
};

}  // namespace cauce

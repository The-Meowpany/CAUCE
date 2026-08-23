#pragma once

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cauce::hal {

class MemoryFileSystem final : public IFileSystem {
 public:
  bool exists(const char* path) override {
    return files_.count(normalize(path)) > 0;
  }

  bool appendBytes(const char* path, const uint8_t* data, size_t length) override {
    std::string key = normalize(path);
    ensureParent(key);
    auto it = files_.find(key);
    if (it == files_.end()) it = files_.emplace(key, std::vector<uint8_t>{}).first;
    it->second.insert(it->second.end(), data, data + length);
    return true;
  }

  bool readRange(const char* path, size_t offset, uint8_t* buffer,
                 size_t length) override {
    auto it = files_.find(normalize(path));
    if (it == files_.end() || offset + length > it->second.size()) return false;
    std::memcpy(buffer, it->second.data() + offset, length);
    return true;
  }

  bool writeWholeFile(const char* path, const uint8_t* data, size_t length) override {
    std::string key = normalize(path);
    ensureParent(key);
    files_[key] = std::vector<uint8_t>(data, data + length);
    return true;
  }

  size_t fileSize(const char* path) override {
    auto it = files_.find(normalize(path));
    return it == files_.end() ? 0 : it->second.size();
  }

  bool removeFile(const char* path) override {
    return files_.erase(normalize(path)) > 0;
  }

  int listFiles(const char* directory, char (*outPaths)[64], int maxItems) override {
    std::string prefix = normalize(directory);
    int count = 0;
    for (const auto& entry : files_) {
      if (entry.first.rfind(prefix, 0) == 0 && count < maxItems) {
        std::snprintf(outPaths[count], 64, "%s", entry.first.c_str());
        count++;
      }
    }
    return count;
  }

 private:
  static std::string normalize(const char* path) {
    std::string p(path);
    if (!p.empty() && p.front() != '/') p.insert(p.begin(), '/');
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
  }

  void ensureParent(const std::string& key) {
    size_t pos = key.find_last_of('/');
    if (pos != std::string::npos && pos > 0) dirs_.insert(key.substr(0, pos));
  }

  std::map<std::string, std::vector<uint8_t>> files_;
  std::set<std::string> dirs_;
};

}  // namespace cauce::hal

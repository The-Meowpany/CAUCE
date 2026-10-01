#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace cauce {

class JsonWriter {
 public:
  JsonWriter(char* buffer, size_t capacity) : buffer_(buffer), capacity_(capacity), used_(0), truncated_(false) {
    if (capacity_ > 0) buffer_[0] = '\0';
  }

  bool append(const char* text) {
    if (truncated_ || !text) return false;
    const size_t len = std::strlen(text);
    if (used_ + len >= capacity_) {
      truncated_ = true;
      return false;
    }
    std::memcpy(buffer_ + used_, text, len);
    used_ += len;
    buffer_[used_] = '\0';
    return true;
  }

  bool appendf(const char* fmt, ...) {
    if (truncated_) return false;
    va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(buffer_ + used_, capacity_ - used_, fmt, args);
    va_end(args);
    if (written < 0 || static_cast<size_t>(written) >= capacity_ - used_) {
      truncated_ = true;
      return false;
    }
    used_ += static_cast<size_t>(written);
    return true;
  }

  bool truncated() const { return truncated_; }
  size_t size() const { return used_; }
  const char* c_str() const { return buffer_; }

 private:
  char* buffer_;
  size_t capacity_;
  size_t used_;
  bool truncated_;
};

}  // namespace cauce

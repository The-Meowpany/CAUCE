#pragma once

#include <cstddef>
#include <cstdarg>
#include <cstdint>

namespace cauce {

enum class LogLevel : uint8_t { Debug = 0, Info = 1, Warn = 2, Error = 3 };

class ILogSink {
 public:
  virtual ~ILogSink() = default;
  virtual void writeLine(const char* line) = 0;
};

class Logger {
 public:
  explicit Logger(ILogSink& sink) : sink_(sink) {}

  bool eventf(LogLevel level, const char* event, const char* detailsFormat, ...);
  void event(LogLevel level, const char* event);

  static constexpr size_t kMaxLineLength = 512;

 private:
  ILogSink& sink_;
};

}  // namespace cauce

#include "cauce/core/Logger.h"

#include <cstdio>
#include <cstring>

namespace cauce {

namespace {
const char* levelName(LogLevel level) {
  switch (level) {
    case LogLevel::Debug:
      return "DEBUG";
    case LogLevel::Info:
      return "INFO";
    case LogLevel::Warn:
      return "WARN";
    case LogLevel::Error:
      return "ERROR";
  }
  return "?";
}
}  // namespace

void Logger::event(LogLevel level, const char* event) {
  char line[Logger::kMaxLineLength];
  std::snprintf(line, sizeof(line), "%s %s", levelName(level), event);
  sink_.writeLine(line);
}

bool Logger::eventf(LogLevel level, const char* event, const char* detailsFormat,
                    ...) {
  char line[Logger::kMaxLineLength];
  int used = std::snprintf(line, sizeof(line), "%s %s ", levelName(level), event);
  if (used < 0 || static_cast<size_t>(used) >= sizeof(line)) {
    sink_.writeLine(levelName(level));
    return true;
  }
  va_list args;
  va_start(args, detailsFormat);
  const int vs = std::vsnprintf(line + used, sizeof(line) - static_cast<size_t>(used),
                                detailsFormat, args);
  va_end(args);
  const bool truncated = vs < 0 || static_cast<size_t>(vs) >= sizeof(line) - static_cast<size_t>(used);
  sink_.writeLine(line);
  return truncated;
}

}  // namespace cauce

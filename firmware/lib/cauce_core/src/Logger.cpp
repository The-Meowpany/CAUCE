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
  char line[160];
  std::snprintf(line, sizeof(line), "%s %s", levelName(level), event);
  sink_.writeLine(line);
}

void Logger::eventf(LogLevel level, const char* event, const char* detailsFormat,
                    ...) {
  char line[224];
  int used = std::snprintf(line, sizeof(line), "%s %s ", levelName(level), event);
  if (used < 0 || static_cast<size_t>(used) >= sizeof(line)) {
    sink_.writeLine(levelName(level));
    return;
  }
  va_list args;
  va_start(args, detailsFormat);
  std::vsnprintf(line + used, sizeof(line) - static_cast<size_t>(used),
                 detailsFormat, args);
  va_end(args);
  sink_.writeLine(line);
}

}  // namespace cauce

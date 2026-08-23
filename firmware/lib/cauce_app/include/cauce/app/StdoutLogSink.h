#pragma once

#include <cstdio>
#include <cstring>

#include "cauce/core/Logger.h"

namespace cauce::app {

class StdoutLogSink final : public ILogSink {
 public:
  void writeLine(const char* line) override {
    std::printf("%s\n", line);
    std::fflush(stdout);
  }
};

}  // namespace cauce::app

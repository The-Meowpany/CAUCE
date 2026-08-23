#pragma once

#ifndef ARDUINO

#include <string>

#include "cauce/hal/ISyncTransport.h"

namespace cauce::app {

class HostHttpTransport final : public hal::ISyncTransport {
 public:
  void configure(const char* serverUrl, const char* bearerToken) override;
  hal::ISyncTransport::Result postBatch(const char* jsonPayload, size_t length,
                                        uint32_t timeoutMs,
                                        uint32_t& ackedSequenceOut) override;

 private:
  std::string url_;
  std::string token_;
};

}  // namespace cauce::app

#endif

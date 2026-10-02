#pragma once

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "cauce/hal/ISyncTransport.h"

namespace cauce::hal {

class Esp32HttpSyncTransport final : public ISyncTransport {
 public:
  void configure(const char* serverUrl, const char* bearerToken) override;
  Result postBatch(const char* jsonPayload, size_t length,
                   const char* signatureHex, uint32_t timeoutMs,
                   uint32_t& ackedSequenceOut) override;

 Result fetchCommands(CommandBatch& out) override;

 private:
  CommandBatch lastCommands_;
  String url_;
  String token_;
};

}  // namespace cauce::hal

#endif
#endif

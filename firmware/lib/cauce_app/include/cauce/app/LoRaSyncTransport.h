#pragma once

#include <cstddef>
#include <cstdint>

#include "cauce/hal/IClock.h"
#include "cauce/hal/ILoRaRadio.h"
#include "cauce/hal/ISyncTransport.h"

namespace cauce::app {

class LoRaSyncTransport final : public hal::ISyncTransport {
 public:
  LoRaSyncTransport(hal::ILoRaRadio& radio, hal::IClock& clock);

  void setMaxPayloadBytes(size_t maxBytes);
  void setMinIntervalMs(uint32_t intervalMs);

  void configure(const char* serverUrl, const char* bearerToken) override;
  Result postBatch(const char* jsonPayload, size_t length,
                   const char* signatureHex, uint32_t timeoutMs,
                   uint32_t& ackedSequenceOut) override;

 private:
  hal::ILoRaRadio& radio_;
  hal::IClock& clock_;
  size_t maxPayloadBytes_{200};
  uint32_t minIntervalMs_{600000};
  uint32_t lastSendMonotonicMs_{0};
  bool everSent_{false};
};

}  // namespace cauce::app

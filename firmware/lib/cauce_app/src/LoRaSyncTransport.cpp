#include "cauce/app/LoRaSyncTransport.h"

#include <cstring>

namespace cauce::app {

LoRaSyncTransport::LoRaSyncTransport(hal::ILoRaRadio& radio, hal::IClock& clock)
    : radio_(radio), clock_(clock) {}

void LoRaSyncTransport::setMaxPayloadBytes(size_t maxBytes) {
  maxPayloadBytes_ = maxBytes;
}

void LoRaSyncTransport::setMinIntervalMs(uint32_t intervalMs) {
  minIntervalMs_ = intervalMs;
}

void LoRaSyncTransport::configure(const char*, const char*) {}

hal::ISyncTransport::Result LoRaSyncTransport::postBatch(
    const char* jsonPayload, size_t length, const char*, uint32_t,
    uint32_t& ackedSequenceOut) {
  if (!jsonPayload || length == 0 || length > maxPayloadBytes_) {
    return Result::NetworkError;
  }
  const uint32_t now = clock_.monotonicMs();
  if (everSent_ && now - lastSendMonotonicMs_ < minIntervalMs_) {
    return Result::NetworkError;
  }
  if (!radio_.canSendNow()) return Result::NetworkError;
  if (!radio_.send(reinterpret_cast<const uint8_t*>(jsonPayload), length)) {
    return Result::NetworkError;
  }
  lastSendMonotonicMs_ = now;
  everSent_ = true;
  ackedSequenceOut = 0xFFFFFFFFu;
  return Result::Ok;
}

}  // namespace cauce::app

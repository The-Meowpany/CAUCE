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

void LoRaSyncTransport::setAckTimeoutMs(uint32_t timeoutMs) {
  ackTimeoutMs_ = timeoutMs;
}

void LoRaSyncTransport::setAckPollIntervalMs(uint32_t pollIntervalMs) {
  ackPollIntervalMs_ = pollIntervalMs == 0 ? 1 : pollIntervalMs;
}

void LoRaSyncTransport::encodeAck(uint8_t out[kAckFrameSize], uint32_t sequence) {
  out[0] = kAckMagic;
  out[1] = static_cast<uint8_t>((sequence >> 24) & 0xFF);
  out[2] = static_cast<uint8_t>((sequence >> 16) & 0xFF);
  out[3] = static_cast<uint8_t>((sequence >> 8) & 0xFF);
  out[4] = static_cast<uint8_t>(sequence & 0xFF);
  out[5] = static_cast<uint8_t>(out[0] ^ out[1] ^ out[2] ^ out[3] ^ out[4]);
}

bool LoRaSyncTransport::parseAck(const uint8_t* frame, size_t length,
                                 uint32_t& sequenceOut) {
  if (frame == nullptr || length != kAckFrameSize) return false;
  if (frame[0] != kAckMagic) return false;
  const uint8_t expected = static_cast<uint8_t>(
      frame[0] ^ frame[1] ^ frame[2] ^ frame[3] ^ frame[4]);
  if (frame[5] != expected) return false;
  sequenceOut = (static_cast<uint32_t>(frame[1]) << 24) |
                (static_cast<uint32_t>(frame[2]) << 16) |
                (static_cast<uint32_t>(frame[3]) << 8) |
                static_cast<uint32_t>(frame[4]);
  return true;
}

void LoRaSyncTransport::drainAcks() {
  uint8_t buffer[kAckFrameSize];
  // Bounded: a chatty peer must not be able to hold the scheduler here.
  for (int i = 0; i < 8; ++i) {
    const int got = radio_.receive(buffer, sizeof(buffer));
    if (got <= 0) return;
  }
}

void LoRaSyncTransport::configure(const char*, const char*) {}

hal::ISyncTransport::Result LoRaSyncTransport::postBatch(
    const char* jsonPayload, size_t length, const char*, uint32_t,
    uint32_t& ackedSequenceOut) {
  confirmed_ = false;

  if (!jsonPayload || length == 0 || length > maxPayloadBytes_) {
    return Result::NetworkError;
  }
  const uint32_t now = clock_.monotonicMs();
  if (everSent_ && now - lastSendMonotonicMs_ < minIntervalMs_) {
    return Result::NetworkError;
  }
  if (!radio_.canSendNow()) return Result::NetworkError;

  // Clear stale answers before transmitting, otherwise an acknowledgement
  // delayed from a previous attempt would be read as this one's.
  drainAcks();

  if (!radio_.send(reinterpret_cast<const uint8_t*>(jsonPayload), length)) {
    return Result::NetworkError;
  }
  lastSendMonotonicMs_ = now;
  everSent_ = true;

  const uint64_t deadline = now + ackTimeoutMs_;
  uint8_t buffer[kAckFrameSize];
  uint32_t acked = 0;
  while (clock_.monotonicMs() < deadline) {
    const int got = radio_.receive(buffer, sizeof(buffer));
    if (got < 0) return Result::NetworkError;
    if (got > 0 && parseAck(buffer, static_cast<size_t>(got), acked)) {
      ackedSequenceOut = acked;
      confirmed_ = true;
      return Result::Ok;
    }
    const uint32_t elapsed = static_cast<uint32_t>(clock_.monotonicMs() - now);
    if (ackTimeoutMs_ - elapsed < ackPollIntervalMs_) break;
    clock_.sleepMs(ackPollIntervalMs_);
  }

  // No confirmation. Report a failure so the batch is retried instead of
  // being silently dropped.
  ackedSequenceOut = 0;
  return Result::NetworkError;
}

}  // namespace cauce::app
#pragma once

#include <cstddef>
#include <cstdint>

#include "cauce/hal/IClock.h"
#include "cauce/hal/ILoRaRadio.h"
#include "cauce/hal/ISyncTransport.h"

namespace cauce::app {

// Sends a batch over LoRa and waits for the gateway to confirm how far it got.
//
// Delivery is at-least-once: an unconfirmed batch is reported as a network
// error so SyncManager retries it, and the server dedupes by
// (node_id, sequence). The previous optimistic behaviour advanced the
// watermark on faith, which turned every lost uplink into permanent loss.
class LoRaSyncTransport final : public hal::ISyncTransport {
 public:
  // Gateway confirmation frame: magic, 4-byte big-endian sequence, XOR.
  static constexpr size_t kAckFrameSize = 6;
  static constexpr uint8_t kAckMagic = 0xA5;

  LoRaSyncTransport(hal::ILoRaRadio& radio, hal::IClock& clock);

  void setMaxPayloadBytes(size_t maxBytes);
  void setMinIntervalMs(uint32_t intervalMs);

  // How long to wait for the gateway acknowledgement, and how often to poll
  // the radio while waiting.
  void setAckTimeoutMs(uint32_t timeoutMs);
  void setAckPollIntervalMs(uint32_t pollIntervalMs);

  // True when the gateway answered, i.e. the last postBatch returned Ok.
  bool lastDeliveryConfirmed() const { return confirmed_; }

  void configure(const char* serverUrl, const char* bearerToken) override;
  Result postBatch(const char* jsonPayload, size_t length,
                   const char* signatureHex, uint32_t timeoutMs,
                   uint32_t& ackedSequenceOut) override;

  // Parses one ack frame. Exposed for the host tests and for a gateway-side
  // unit test; returns false for a bad magic, a wrong length or a bad XOR.
  static bool parseAck(const uint8_t* frame, size_t length, uint32_t& sequenceOut);
  static void encodeAck(uint8_t out[kAckFrameSize], uint32_t sequence);

 private:
  // Drops acknowledgements that arrived late, so one cannot be mistaken for
  // the answer to the frame sent after it.
  void drainAcks();

  hal::ILoRaRadio& radio_;
  hal::IClock& clock_;
  size_t maxPayloadBytes_{200};
  uint32_t minIntervalMs_{600000};
  uint32_t ackTimeoutMs_{5000};
  uint32_t ackPollIntervalMs_{50};
  uint32_t lastSendMonotonicMs_{0};
  bool everSent_{false};
  bool confirmed_{false};
};

}  // namespace cauce::app
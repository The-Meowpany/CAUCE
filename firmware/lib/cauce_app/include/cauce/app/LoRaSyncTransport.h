#pragma once

#include <cstddef>
#include <cstdint>

#include "cauce/core/LoRaBatchCodec.h"
#include "cauce/core/Measurement.h"
#include "cauce/hal/IClock.h"
#include "cauce/hal/ILoRaRadio.h"
#include "cauce/hal/ISyncTransport.h"

namespace cauce::app {

// Sends a batch over LoRa and waits for the gateway to confirm how far it got.
//
// The wire format is the compact 68-byte frame, not JSON: a JSON batch does not
// fit a LoRa payload and the roadmap already called for the frame. Records are
// packed with LoRaBatchEncoder and the gateway reassembles them.
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

  // Radio payload budget. 115 B is the SF9 ceiling and the tightest spreading
  // rate that still fits one 60-byte record; SF10 and below are refused
  // outright rather than silently truncated.
  static constexpr size_t kDefaultRadioPayloadBytes = 115;
  static constexpr size_t kMaxRecordsPerBatch = 16;
  static constexpr size_t LoRaBatchHeaderSize = kLoRaHeaderSize;
  static constexpr size_t LoRaTrailerSize = kLoRaTrailerSize;
  static constexpr uint32_t kDefaultFragmentGapMs = 50;

  LoRaSyncTransport(hal::ILoRaRadio& radio, hal::IClock& clock);

  void setMaxPayloadBytes(size_t maxBytes);
  void setMinIntervalMs(uint32_t intervalMs);
  void setRadioPayloadBytes(size_t bytes) { radioPayloadBytes_ = bytes; }
  size_t radioPayloadBytes() const { return radioPayloadBytes_; }
  void setFragmentGapMs(uint32_t gapMs) { fragmentGapMs_ = gapMs; }

  // Device key used to sign each transmitted frame. Copied, because the
  // caller's config buffer outlives this call but the key should not be
  // reachable from here afterwards.
  //
  // Leaving this unset is not a downgrade the transport decides to make: the
  // frames simply go out unsigned and the central refuses them if the node is
  // provisioned. Silently falling back to unsigned for a provisioned node would
  // be the dangerous option, so it is not offered.
  void setDeviceKey(const uint8_t* key, size_t length);
  bool hasDeviceKey() const { return deviceKeyLength_ > 0; }

  // Selects the frame algorithm, and nothing else. The central provisions one
  // algorithm per node and dispatches on the trailer length, so this must agree
  // with `device_key_algorithm` at the central or every frame is refused.
  //
  // Defaults to HMAC because that is what a node provisioned before Ed25519
  // signing existed has. A node that wants Ed25519 sets the algorithm AND
  // supplies a 32-byte seed via setDeviceKey; a node that sets the algorithm
  // without the right key material transmits nothing rather than something the
  // central will reject.
  void setFrameAlgorithm(cauce::FrameAlgorithm algorithm) {
    frameAlgorithm_ = algorithm;
  }
  cauce::FrameAlgorithm frameAlgorithm() const { return frameAlgorithm_; }

  // True when the configured algorithm has the key material it needs.
  // Checkable before the first transmission, so a misconfigured node is caught by
  // a test or a health check rather than by a central that silently drops every
  // frame.
  bool hasUsableAlgorithm() const;

  // Trailer bytes the current algorithm appends: 0, 32 or 64.
  size_t frameSignatureBytes() const {
    return cauce::frameSignatureSize(frameAlgorithm_);
  }

  // Parses the JSON batch SyncManager produces into records for the compact
  // encoder. Only the fields the frame format carries are read; anything else
  // in the envelope is dropped because the frame has no room for it.
  // Returns the record count written, or 0 when the payload is unusable.
  static size_t parseBatchJson(const char* jsonPayload, size_t length,
                               Measurement* out, size_t capacity);

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

  static size_t frameSizeOf(const LoRaBatchHeader& header);

  hal::ILoRaRadio& radio_;
  hal::IClock& clock_;
  size_t maxPayloadBytes_{200};
  size_t radioPayloadBytes_{kDefaultRadioPayloadBytes};
  uint8_t deviceKey_[64];
  size_t deviceKeyLength_{0};
  cauce::FrameAlgorithm frameAlgorithm_{cauce::FrameAlgorithm::kHmacSha256};
  uint32_t minIntervalMs_{600000};
  uint32_t ackTimeoutMs_{5000};
  uint32_t ackPollIntervalMs_{50};
  uint32_t fragmentGapMs_{kDefaultFragmentGapMs};
  uint32_t lastSendMonotonicMs_{0};
  uint32_t nextBatchId_{1};
  bool everSent_{false};
  bool confirmed_{false};
};

}  // namespace cauce::app
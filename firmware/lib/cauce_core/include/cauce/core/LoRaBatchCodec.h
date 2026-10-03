#pragma once

#include <cstddef>
#include <cstdint>

#include "cauce/core/Measurement.h"
#include "cauce/core/RecordCodec.h"

namespace cauce {

// Packs a batch of measurements into radio-sized frames.
//
// A measurement frame is 68 bytes, and a LoRa payload at SF9 is around 51
// bytes, so a batch never fits in one uplink. Every frame therefore carries a
// slice of the batch plus enough header for the receiver to know what it is
// looking at: batch id, fragment index, fragment count and the record count it
// should end up with. That is what lets the gateway reassemble, and what stops
// a fragment from being mistaken for a complete batch.
//
// The header is deliberately redundant rather than minimal. Radio time is
// expensive and a lost fragment costs a whole batch, so the receiver is given
// everything it needs to reject a bad frame without asking for a retransmit.
struct LoRaBatchHeader {
  uint16_t batchId{0};
  uint8_t fragmentIndex{0};
  uint8_t fragmentCount{0};
  uint16_t recordCount{0};   // records in the whole batch, not this fragment
  uint16_t payloadBytes{0};  // record bytes carried by this frame
};

static constexpr size_t kLoRaHeaderSize = 10;
static constexpr size_t kLoRaTrailerSize = 2;  // CRC16 over header + payload
static constexpr size_t kLoRaOverhead = kLoRaHeaderSize + kLoRaTrailerSize;
static constexpr size_t kLoRaSignatureSize = 32;  // HMAC-SHA256 over the frame
static constexpr uint8_t kLoRaMagic = 0xCA;
static constexpr uint8_t kLoRaVersion = 1;
static constexpr size_t kLoRaMaxFragments = 255;

// A signed frame is the framed bytes followed by a 32-byte HMAC over exactly
// those bytes, signature last and outside the CRC.
//
// The point of signing per frame rather than per batch is who has to hold the
// key. A gateway that signs a batch on a node's behalf must be given that
// node's device_key, and from then on it can forge measurements as that node
// forever. Signing here means the gateway only ever copies bytes: it needs no
// secret to relay, and the central verifies against the key it issued.
//
// The signature covers the CRC as well as the header and payload, so a frame
// cannot be altered, re-CRCed and replayed.

// Frames one frame at a time so the caller controls pacing and duty cycle
// rather than buffering a whole batch on the radio path.
class LoRaBatchEncoder {
 public:
  explicit LoRaBatchEncoder(const Measurement* records, size_t count,
                            uint16_t batchId);

  // Bytes this frame needs, given a radio payload budget.
  size_t nextFrameSize(size_t maxFrameBytes) const;

  // Writes one frame. Returns false when `outCapacity` is too small or the
  // budget cannot hold even a single record.
  bool nextFrame(uint8_t* out, size_t outCapacity, size_t maxFrameBytes,
                 LoRaBatchHeader& headerOut);

  size_t fragmentCount() const { return fragmentCount_; }
  // fragmentCount_ is only known once a budget has been seen, so `started_`
  // keeps an untouched encoder from claiming to be finished.
  bool done() const {
    return count_ == 0 || (started_ && emitted_ >= fragmentCount_);
  }

 private:
  const Measurement* records_;
  size_t count_;
  uint16_t batchId_;
  size_t fragmentCount_{0};
  bool started_{false};
  size_t emitted_{0};
  size_t nextRecord_{0};
};

// Bytes a signed frame occupies: the frame plus its trailing HMAC.
inline size_t signedFrameSize(size_t frameLength) {
  return frameLength + kLoRaSignatureSize;
}

// Copies `frame` into `out` and appends HMAC-SHA256(deviceKey, frame).
//
// Returns the signed length, or 0 when there is no key or no room. A node
// without a configured key transmits unsigned frames, which is exactly what an
// unprovisioned deployment looks like; the central decides whether that is
// acceptable, and it must never be this function's decision to make.
size_t signFrame(const uint8_t* frame, size_t frameLength,
                 const uint8_t* deviceKey, size_t deviceKeyLength,
                 uint8_t* out, size_t outCapacity);

// Reassembles fragments on the receiving side.
//
// A fragment from a different batch, or a duplicate of one already stored,
// must be rejected rather than merged: merging them would corrupt the batch
// silently, which is the failure mode a custody system cannot have.
class LoRaBatchReassembler {
 public:
  static constexpr size_t kMaxFragments = 16;
  static constexpr size_t kMaxRecords = 64;

  enum class Status : uint8_t {
    Ok = 0,
    TooShort = 1,
    BadMagic = 2,
    BadVersion = 3,
    BadCrc = 4,
    ForeignBatch = 5,
    DuplicateFragment = 6,
    NoRoom = 7,
  };

  explicit LoRaBatchReassembler(uint16_t batchId);

  Status add(const uint8_t* frame, size_t length);

  // True once every announced fragment has arrived.
  bool complete() const { return complete_; }

  size_t recordCount() const { return recordCount_; }
  uint16_t batchId() const { return batchId_; }

  // Copies the reassembled records out. Returns how many were written.
  size_t copyRecords(Measurement* out, size_t capacity) const;

  // A batch that cannot be completed within the fragment budget should not
  // hold memory forever; the gateway drops it and reports the loss.
  void reset();

 private:
  struct Fragment {
    bool present{false};
    uint16_t bytes{0};
    uint8_t buffer[kMeasurementPayloadSize * kMaxFragments]{};
  };

  uint16_t batchId_;
  Fragment fragments_[kMaxFragments];
  uint8_t fragmentCount_{0};
  size_t fragmentSeen_{0};
  uint16_t recordCount_{0};
  bool complete_{false};
};

}  // namespace cauce
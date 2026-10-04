#include "cauce/app/LoRaSyncTransport.h"

#include <cstdlib>
#include <cstring>

#include "cauce/core/Ed25519Points.h"

namespace cauce::app {

namespace {

// Minimal reader for the flat record objects SyncManager emits. A full JSON
// parser is not justified here: the producer is our own SyncManager and the
// frame format only has room for a handful of fields anyway.
//
// Every lookup is bounded by [begin, end) so a field can only ever be read
// from the object being parsed. A cursor that scanned the whole document would
// silently return the first record's value for every record.
class JsonCursor {
 public:
  JsonCursor(const char* data, size_t begin, size_t end)
      : data_(data), begin_(begin), end_(end), cursor_(begin) {}

  size_t position() const { return cursor_; }
  bool atEnd() const { return cursor_ >= end_; }
  char peek() const { return cursor_ < end_ ? data_[cursor_] : '\0'; }
  void seek(size_t position) { cursor_ = position; }

  // Locates the next '{' at or after the cursor and returns its index.
  bool nextObject(size_t& startOut) {
    while (cursor_ < end_ && data_[cursor_] != '{') ++cursor_;
    if (cursor_ >= end_) return false;
    startOut = cursor_;
    return true;
  }

  // Steps past the closing '}' of the object that starts at `objectStart`.
  size_t endOfObject(size_t objectStart) {
    int depth = 0;
    bool inString = false;
    for (size_t i = objectStart; i < end_; ++i) {
      const char c = data_[i];
      if (inString) {
        if (c == '\\') {
          ++i;
          continue;
        }
        if (c == '"') inString = false;
        continue;
      }
      if (c == '"') {
        inString = true;
        continue;
      }
      if (c == '{') ++depth;
      if (c == '}') {
        --depth;
        if (depth == 0) return i + 1;
      }
    }
    return end_;
  }

  // Seeks to the value of `key` inside this object. Returns false when absent.
  //
  // The scan starts at the beginning of the object, not at the cursor: fields
  // are looked up in whatever order the caller asks for, and the JSON order is
  // not that order. A forward-only scan silently returns "absent" for any field
  // that happens to appear earlier than the previous lookup, which is how a
  // sequence number turns into a silent zero.
  bool seekField(const char* key) {
    const size_t keyLen = std::strlen(key);
    for (size_t i = begin_; i + keyLen + 1 < end_; ++i) {
      if (data_[i] != '"') continue;
      if (std::strncmp(data_ + i + 1, key, keyLen) != 0) continue;
      if (data_[i + 1 + keyLen] != '"') continue;
      size_t j = i + 2 + keyLen;
      while (j < end_ && (data_[j] == ' ' || data_[j] == ':')) ++j;
      if (j >= end_) return false;
      cursor_ = j;
      return true;
    }
    return false;
  }

  uint32_t uintField(const char* key, uint32_t fallback) {
    if (!seekField(key)) return fallback;
    if (data_[cursor_] < '0' || data_[cursor_] > '9') return fallback;
    return static_cast<uint32_t>(std::strtoul(data_ + cursor_, nullptr, 10));
  }

  int64_t intField(const char* key, int64_t fallback) {
    if (!seekField(key)) return fallback;
    return std::strtoll(data_ + cursor_, nullptr, 10);
  }

  float floatField(const char* key, float fallback) {
    if (!seekField(key)) return fallback;
    if (data_[cursor_] == 'n') return fallback;  // null
    return static_cast<float>(std::strtod(data_ + cursor_, nullptr));
  }

  // Reads a quoted string, stopping at the object end so a hostile payload
  // cannot walk off the buffer.
  void stringField(const char* key, char* out, size_t capacity) {
    out[0] = '\0';
    if (!seekField(key)) return;
    if (data_[cursor_] != '"') return;
    ++cursor_;
    size_t i = 0;
    while (cursor_ < end_ && data_[cursor_] != '"' && i + 1 < capacity) {
      out[i++] = data_[cursor_++];
    }
    out[i] = '\0';
  }

 private:
  const char* data_;
  size_t begin_;
  size_t end_;
  size_t cursor_;
};

Variable variableFromName(const char* name) { return parseVariable(name); }

Quality qualityFromName(const char* name) {
  for (uint8_t i = 0; i <= static_cast<uint8_t>(Quality::Missing); ++i) {
    const Quality candidate = static_cast<Quality>(i);
    const char* text = qualityName(candidate);
    if (text != nullptr && std::strcmp(text, name) == 0) return candidate;
  }
  return Quality::Suspect;
}

}  // namespace

size_t LoRaSyncTransport::parseBatchJson(const char* jsonPayload, size_t length,
                                         Measurement* out, size_t capacity) {
  if (jsonPayload == nullptr || out == nullptr || capacity == 0) return 0;
  JsonCursor scanner(jsonPayload, 0, length);
  if (!scanner.seekField("measurements")) return 0;

  size_t written = 0;
  size_t arrayStart = scanner.position();
  // The array itself is the only place records live, so the scan is bounded to
  // the matching bracket. Without that bound a `{` outside the array would be
  // parsed as a record.
  size_t arrayEnd = length;
  int depth = 0;
  bool inString = false;
  for (size_t i = arrayStart; i < length; ++i) {
    const char c = jsonPayload[i];
    if (inString) {
      if (c == '\\') {
        ++i;
        continue;
      }
      if (c == '"') inString = false;
      continue;
    }
    if (c == '"') {
      inString = true;
      continue;
    }
    if (c == '[') ++depth;
    if (c == ']') {
      --depth;
      if (depth == 0) {
        arrayEnd = i;
        break;
      }
    }
  }

  JsonCursor cursor(jsonPayload, arrayStart, arrayEnd);
  for (;;) {
    size_t objectStart = 0;
    if (!cursor.nextObject(objectStart)) break;
    const size_t objectEnd = cursor.endOfObject(objectStart);
    // Re-anchor on this object so no lookup can escape it.
    JsonCursor record(jsonPayload, objectStart, objectEnd);
    record.seek(objectStart);

    Measurement m{};
    record.stringField("node_id", m.nodeId, sizeof(m.nodeId));
    record.stringField("sensor_id", m.sensorId, sizeof(m.sensorId));
    char variable[16] = {0};
    record.stringField("variable", variable, sizeof(variable));
    char quality[16] = {0};
    record.stringField("quality", quality, sizeof(quality));
    m.sequence = record.uintField("sequence", 0);
    m.timestampUtcMs =
        static_cast<uint64_t>(record.intField("timestamp_utc_ms", 0));
    m.value = record.floatField("value", 0.0f);
    m.reasonBits = static_cast<uint8_t>(record.uintField("reason_bits", 0));
    m.timeUncertain = record.uintField("time_uncertain", 0) != 0;
    m.variable = variableFromName(variable);
    m.quality = qualityFromName(quality);

    // A record without a sequence or with a variable the frame cannot name is
    // not something the compact frame can carry honestly, so it is dropped
    // rather than framed as a zero.
    if (m.sequence != 0 && m.variable != Variable::Unknown &&
        written < capacity) {
      out[written++] = m;
    }

    cursor.seek(objectEnd);
  }
  return written;
}

LoRaSyncTransport::LoRaSyncTransport(hal::ILoRaRadio& radio, hal::IClock& clock)
    : radio_(radio), clock_(clock) {}

void LoRaSyncTransport::setMaxPayloadBytes(size_t maxBytes) {
  maxPayloadBytes_ = maxBytes;
}

void LoRaSyncTransport::setDeviceKey(const uint8_t* key, size_t length) {
  deviceKeyLength_ = 0;
  if (key && length > 0) {
    const size_t copy = length > sizeof(deviceKey_) ? sizeof(deviceKey_)
                                                    : length;
    std::memcpy(deviceKey_, key, copy);
    deviceKeyLength_ = copy;
  }
}

bool LoRaSyncTransport::hasUsableAlgorithm() const {
  if (frameAlgorithm_ == cauce::FrameAlgorithm::kNone) return false;
  if (deviceKeyLength_ == 0) return false;
  // Exactly the seed, for the asymmetric case. A 16- or 64-byte key with
  // Ed25519 selected is a provisioning mistake, and refusing it here means the
  // node transmits nothing rather than something no central will accept.
  if (cauce::frameAlgorithmIsAsymmetric(frameAlgorithm_) &&
      deviceKeyLength_ != cauce::kEd25519SeedBytes) {
    return false;
  }
  return true;
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
  // Every failure path returns without touching this again, so it must not
  // arrive holding the caller's previous value: SyncManager would read that as
  // progress the radio never made.
  ackedSequenceOut = 0;

  if (!jsonPayload || length == 0 || length > maxPayloadBytes_) {
    return Result::NetworkError;
  }
  const uint32_t now = clock_.monotonicMs();
  if (everSent_ && now - lastSendMonotonicMs_ < minIntervalMs_) {
    return Result::NetworkError;
  }
  if (!radio_.canSendNow()) return Result::NetworkError;

  Measurement records[kMaxRecordsPerBatch];
  const size_t count = parseBatchJson(jsonPayload, length, records,
                                      kMaxRecordsPerBatch);
  // A batch that did not survive the trip into the compact frame is not sent:
  // reporting success would make the node drop rows nobody ever received.
  if (count == 0) return Result::NetworkError;

  // Clear stale answers before transmitting, otherwise an acknowledgement
  // delayed from a previous attempt would be read as this one's.
  drainAcks();

  // The signature is part of the air interface, so when signing, the radio
  // budget has to leave room for it. Spending the whole budget on records
  // would produce a frame that could not be signed without overflowing.
  // Unsigned nodes keep the full budget: they are not paying for a signature
  // they are not sending, and silently re-fragmenting them would change the
  // air interface for an already-deployed node.
  const size_t signatureRoom = deviceKeyLength_ > 0 ? kLoRaSignatureSize : 0;
  const size_t payloadBudget =
      radioPayloadBytes_ > signatureRoom ? radioPayloadBytes_ - signatureRoom
                                         : 0;
  if (payloadBudget < kLoRaOverhead) return Result::NetworkError;

  const uint16_t batchId = static_cast<uint16_t>(nextBatchId_++);
  LoRaBatchEncoder encoder(records, count, batchId);
  uint8_t frame[256];
  uint8_t signedFrame[256];
  LoRaBatchHeader header{};
  bool sentAny = false;
  while (!encoder.done()) {
    if (!encoder.nextFrame(frame, sizeof(frame), payloadBudget, header)) {
      return Result::NetworkError;
    }
    const size_t frameLength = frameSizeOf(header);

    // Sign with the configured algorithm, otherwise transmit the frame as it is.
    // Either way what goes on the air is what a relay will copy byte for byte.
    const uint8_t* toSend = frame;
    size_t toSendLength = frameLength;
    if (hasUsableAlgorithm()) {
      const size_t signedLength =
          signFrame(frame, frameLength, deviceKey_, deviceKeyLength_,
                    frameAlgorithm_, signedFrame, sizeof(signedFrame));
      if (signedLength == 0) return Result::NetworkError;
      toSend = signedFrame;
      toSendLength = signedLength;
    }

    if (!radio_.send(toSend, toSendLength)) {
      return Result::NetworkError;
    }
    sentAny = true;
    if (!encoder.done()) {
      // Pacing between fragments keeps the duty cycle inside the limit and
      // gives the gateway a window to receive what it already has.
      clock_.sleepMs(fragmentGapMs_);
    }
  }
  if (!sentAny) return Result::NetworkError;

  lastSendMonotonicMs_ = clock_.monotonicMs();
  everSent_ = true;

  const uint64_t deadline = lastSendMonotonicMs_ + ackTimeoutMs_;
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
    const uint32_t elapsed =
        static_cast<uint32_t>(clock_.monotonicMs() - lastSendMonotonicMs_);
    if (ackTimeoutMs_ - elapsed < ackPollIntervalMs_) break;
    clock_.sleepMs(ackPollIntervalMs_);
  }

  // No confirmation. Report a failure so the batch is retried instead of
  // being silently dropped.
  ackedSequenceOut = 0;
  return Result::NetworkError;
}

size_t LoRaSyncTransport::frameSizeOf(const LoRaBatchHeader& header) {
  // The trailer carries the CRC the gateway checks. Leaving it out of the
  // length would send a frame the receiver must reject.
  return LoRaBatchHeaderSize + LoRaTrailerSize + header.payloadBytes;
}

}  // namespace cauce::app

#include "cauce/core/LoRaBatchCodec.h"

#include <cstring>

#include "cauce/core/RecordCodec.h"

namespace cauce {

namespace {

// CRC16-CCITT over the header and payload. Small on purpose: a LoRa frame has
// no room for a CRC32, and this only has to catch radio noise.
uint16_t crc16(const uint8_t* data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

// Must agree with the packing loop in nextFrame(), or the announced fragment
// count would not match the frames actually produced.
constexpr size_t kMaxRecordsPerFrame = 4;

size_t recordsPerFrame(size_t budget) {
  const size_t usable = budget > kLoRaOverhead ? budget - kLoRaOverhead : 0;
  const size_t cap = kMeasurementPayloadSize * kMaxRecordsPerFrame;
  const size_t perFrame = usable >= cap ? cap : usable;
  if (perFrame < kMeasurementPayloadSize) return 0;
  return perFrame / kMeasurementPayloadSize;
}

size_t framesNeeded(size_t recordCount, size_t budget) {
  const size_t perFrame = recordsPerFrame(budget);
  if (perFrame == 0 || recordCount == 0) return 0;
  return (recordCount + perFrame - 1) / perFrame;
}

}  // namespace

LoRaBatchEncoder::LoRaBatchEncoder(const Measurement* records, size_t count,
                                   uint16_t batchId)
    : records_(records), count_(count), batchId_(batchId) {}

size_t LoRaBatchEncoder::nextFrameSize(size_t maxFrameBytes) const {
  if (count_ == 0) return kLoRaOverhead;
  const size_t frames = framesNeeded(count_, maxFrameBytes);
  if (frames == 0) return kLoRaOverhead;
  const size_t perFrame = recordsPerFrame(maxFrameBytes);
  if (perFrame == 0) return kLoRaOverhead;
  const size_t lastFragmentRecords = count_ % perFrame;
  const size_t records = lastFragmentRecords == 0 ? perFrame : lastFragmentRecords;
  return kLoRaOverhead + records * kMeasurementPayloadSize;
}

bool LoRaBatchEncoder::nextFrame(uint8_t* out, size_t outCapacity,
                                 size_t maxFrameBytes,
                                 LoRaBatchHeader& headerOut) {
  if (out == nullptr) return false;
  if (count_ == 0 || done()) return false;

  if (fragmentCount_ == 0) {
    fragmentCount_ = framesNeeded(count_, maxFrameBytes);
    if (fragmentCount_ == 0 || fragmentCount_ > kLoRaMaxFragments) return false;
    started_ = true;
  }

  const size_t usable = maxFrameBytes > kLoRaOverhead
                            ? maxFrameBytes - kLoRaOverhead
                            : 0;
  if (usable < kMeasurementPayloadSize) return false;
  // Bounded so the scratch buffer is a compile-time size. A wider budget than
  // this simply costs more fragments instead of a bigger stack frame.
  constexpr size_t kMaxRecords = kMaxRecordsPerFrame;
  const size_t maxPerFrame = kMeasurementPayloadSize * kMaxRecords;
  const size_t perFrame = usable >= maxPerFrame ? maxPerFrame : usable;
  // A frame that fits fewer bytes than one record would never terminate.
  if (perFrame < kMeasurementPayloadSize) return false;

  size_t produced = 0;
  uint8_t payload[kMeasurementPayloadSize * kMaxRecords];
  while (nextRecord_ < count_ && produced + kMeasurementPayloadSize <= perFrame) {
    encodePayload(records_[nextRecord_],
                  *reinterpret_cast<uint8_t(*)[kMeasurementPayloadSize]>(
                      payload + produced));
    ++nextRecord_;
    produced += kMeasurementPayloadSize;
  }
  if (produced == 0) return false;

  const size_t total = kLoRaOverhead + produced;
  if (outCapacity < total) return false;

  headerOut.batchId = batchId_;
  headerOut.fragmentIndex = static_cast<uint8_t>(emitted_);
  headerOut.fragmentCount = static_cast<uint8_t>(fragmentCount_);
  headerOut.recordCount = static_cast<uint16_t>(count_);
  headerOut.payloadBytes = static_cast<uint16_t>(produced);

  uint8_t* cursor = out;
  putU16(cursor, static_cast<uint16_t>((kLoRaMagic << 8) | kLoRaVersion));
  putU16(cursor, batchId_);
  *cursor++ = static_cast<uint8_t>(emitted_);
  *cursor++ = static_cast<uint8_t>(fragmentCount_);
  putU16(cursor, static_cast<uint16_t>(count_));
  putU16(cursor, static_cast<uint16_t>(produced));
  std::memcpy(cursor, payload, produced);
  cursor += produced;
  const uint16_t check = crc16(out, total - kLoRaTrailerSize);
  putU16(cursor, check);

  ++emitted_;
  return true;
}

LoRaBatchReassembler::LoRaBatchReassembler(uint16_t batchId)
    : batchId_(batchId) {}

void LoRaBatchReassembler::reset() {
  for (size_t i = 0; i < kMaxFragments; ++i) {
    fragments_[i].present = false;
    fragments_[i].bytes = 0;
  }
  fragmentCount_ = 0;
  fragmentSeen_ = 0;
  recordCount_ = 0;
  complete_ = false;
}

LoRaBatchReassembler::Status LoRaBatchReassembler::add(const uint8_t* frame,
                                                       size_t length) {
  if (frame == nullptr || length < kLoRaOverhead + kMeasurementPayloadSize)
    return Status::TooShort;
  if (length > kLoRaOverhead + kMeasurementPayloadSize * kMaxFragments)
    return Status::NoRoom;

  const uint8_t* cursor = frame;
  const uint16_t tag = getU16(cursor);
  if ((tag >> 8) != kLoRaMagic) return Status::BadMagic;
  if ((tag & 0xFF) != kLoRaVersion) return Status::BadVersion;

  const uint16_t batch = getU16(cursor);
  const uint8_t index = *cursor++;
  const uint8_t fragments = *cursor++;
  const uint16_t records = getU16(cursor);
  const uint16_t payloadBytes = getU16(cursor);

  const size_t headerBytes = kLoRaHeaderSize;
  const size_t expected = headerBytes + payloadBytes + kLoRaTrailerSize;
  if (payloadBytes == 0 || payloadBytes % kMeasurementPayloadSize != 0)
    return Status::BadCrc;
  if (length != expected) return Status::BadCrc;
  if (fragments == 0 || fragments > kMaxFragments || index >= fragments)
    return Status::BadCrc;
  if (payloadBytes / kMeasurementPayloadSize > fragments)
    return Status::BadCrc;
  if (records == 0 || records > kMaxRecords) return Status::NoRoom;

  if (crc16(frame, length - kLoRaTrailerSize) !=
      static_cast<uint16_t>((frame[length - 1] << 8) | frame[length - 2])) {
    return Status::BadCrc;
  }

  if (batch != batchId_) return Status::ForeignBatch;
  if (complete_) return Status::DuplicateFragment;

  Fragment& slot = fragments_[index];
  if (slot.present) return Status::DuplicateFragment;

  if (fragmentCount_ == 0) {
    fragmentCount_ = fragments;
    recordCount_ = records;
  } else if (fragmentCount_ != fragments || recordCount_ != records) {
    // A second batch reusing the id, or a corrupted count. Either way this
    // reassembly is abandoned rather than merged.
    reset();
    return Status::ForeignBatch;
  }

  slot.bytes = payloadBytes;
  std::memcpy(slot.buffer, cursor, payloadBytes);
  slot.present = true;
  ++fragmentSeen_;

  if (fragmentSeen_ == fragmentCount_) complete_ = true;
  return Status::Ok;
}

size_t LoRaBatchReassembler::copyRecords(Measurement* out,
                                         size_t capacity) const {
  if (out == nullptr || !complete_) return 0;
  size_t written = 0;
  for (size_t i = 0; i < fragmentCount_; ++i) {
    const Fragment& slot = fragments_[i];
    size_t offset = 0;
    while (offset + kMeasurementPayloadSize <= slot.bytes &&
           written < capacity) {
      uint8_t raw[kMeasurementPayloadSize];
      std::memcpy(raw, slot.buffer + offset, kMeasurementPayloadSize);
      if (decodePayload(*reinterpret_cast<const uint8_t(*)[
                                   kMeasurementPayloadSize]>(raw),
                        out[written]) == DecodeStatus::Ok) {
        ++written;
      }
      offset += kMeasurementPayloadSize;
    }
  }
  return written;
}

}  // namespace cauce
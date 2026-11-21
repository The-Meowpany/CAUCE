#include "cauce/core/SerialBridge.h"

#include <cstring>

namespace cauce::bridge {

namespace {

uint16_t crcUpdate(uint16_t crc, const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    crc = static_cast<uint16_t>(crc ^ (static_cast<uint16_t>(data[i]) << 8));
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

}  // namespace

uint16_t crc16(const uint8_t* data, size_t length) { return crcUpdate(0xFFFF, data, length); }

size_t encode(FrameType type, const uint8_t* payload, size_t length, uint8_t* out,
              size_t capacity) {
  if (length > kMaxPayloadBytes || capacity < length + kOverheadBytes) return 0;
  if (length > 0 && payload == nullptr) return 0;
  out[0] = kMagic0;
  out[1] = kMagic1;
  out[2] = static_cast<uint8_t>(type);
  out[3] = static_cast<uint8_t>(length >> 8);
  out[4] = static_cast<uint8_t>(length & 0xFF);
  if (length > 0) std::memcpy(out + kHeaderBytes, payload, length);
  // Type, length and payload: everything after the magic.
  const uint16_t crc = crc16(out + 2, length + 3);
  out[kHeaderBytes + length] = static_cast<uint8_t>(crc >> 8);
  out[kHeaderBytes + length + 1] = static_cast<uint8_t>(crc & 0xFF);
  return length + kOverheadBytes;
}

bool Decoder::push(uint8_t byte) {
  switch (state_) {
    case State::Magic0:
      if (byte == kMagic0) state_ = State::Magic1;
      return false;
    case State::Magic1:
      // A repeated first magic byte keeps us one step in rather than losing the frame.
      state_ = byte == kMagic1 ? State::Type : (byte == kMagic0 ? State::Magic1 : State::Magic0);
      return false;
    case State::Type:
      rawType_ = byte;
      state_ = State::Length0;
      return false;
    case State::Length0:
      length_ = static_cast<size_t>(byte) << 8;
      state_ = State::Length1;
      return false;
    case State::Length1:
      length_ |= byte;
      if (length_ > kMaxPayloadBytes) {
        ++errors_;
        reset();
        return false;
      }
      received_ = 0;
      state_ = length_ == 0 ? State::Crc0 : State::Payload;
      return false;
    case State::Payload:
      payload_[received_++] = byte;
      if (received_ == length_) state_ = State::Crc0;
      return false;
    case State::Crc0:
      crc_ = static_cast<uint16_t>(byte) << 8;
      state_ = State::Crc1;
      return false;
    case State::Crc1: {
      crc_ = static_cast<uint16_t>(crc_ | byte);
      reset();
      uint8_t header[3] = {rawType_, static_cast<uint8_t>(length_ >> 8),
                           static_cast<uint8_t>(length_ & 0xFF)};
      // Header then payload, as one CRC continued across the two buffers.
      const uint16_t expected = crcUpdate(crcUpdate(0xFFFF, header, sizeof(header)), payload_,
                                          length_);
      if (expected != crc_) {
        ++errors_;
        return false;
      }
      type_ = static_cast<FrameType>(rawType_);
      return true;
    }
  }
  reset();
  return false;
}

}  // namespace cauce::bridge

#include "cauce/core/RecordCodec.h"

#include <cstring>

namespace cauce {

namespace {
constexpr uint8_t kFrameMagic = 0xCA;
constexpr uint8_t kFrameVersion = 0x01;
}  // namespace

uint32_t crc32(const uint8_t* data, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = static_cast<uint32_t>(-(crc & 1u));
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

void putU16(uint8_t*& cursor, uint16_t v) {
  cursor[0] = static_cast<uint8_t>(v & 0xFF);
  cursor[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  cursor += 2;
}

void putU32(uint8_t*& cursor, uint32_t v) {
  for (int i = 0; i < 4; ++i) cursor[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
  cursor += 4;
}

void putU64(uint8_t*& cursor, uint64_t v) {
  for (int i = 0; i < 8; ++i) cursor[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
  cursor += 8;
}

void putF32(uint8_t*& cursor, float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  putU32(cursor, bits);
}

uint16_t getU16(const uint8_t*& cursor) {
  const uint16_t v = static_cast<uint16_t>(cursor[0]) |
                     static_cast<uint16_t>(cursor[1]) << 8;
  cursor += 2;
  return v;
}

uint32_t getU32(const uint8_t*& cursor) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(cursor[i]) << (8 * i);
  cursor += 4;
  return v;
}

uint64_t getU64(const uint8_t*& cursor) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(cursor[i]) << (8 * i);
  cursor += 8;
  return v;
}

float getF32(const uint8_t*& cursor) {
  const uint32_t bits = getU32(cursor);
  float v = 0.0f;
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

size_t encodePayload(const Measurement& m,
                     uint8_t (&out)[kMeasurementPayloadSize]) {
  uint8_t* c = out;
  putU32(c, m.sequence);
  putU64(c, m.timestampUtcMs);
  putF32(c, m.value);
  *c++ = static_cast<uint8_t>(m.variable);
  *c++ = static_cast<uint8_t>(m.quality);
  *c++ = m.reasonBits;
  *c++ = m.timeUncertain ? 1 : 0;
  std::memcpy(c, m.nodeId, sizeof(m.nodeId));
  c += sizeof(m.nodeId);
  std::memcpy(c, m.sensorId, sizeof(m.sensorId));
  c += sizeof(m.sensorId);
  return static_cast<size_t>(c - out);
}

DecodeStatus decodePayload(const uint8_t (&data)[kMeasurementPayloadSize],
                           Measurement& m) {
  const uint8_t* c = data;
  m.sequence = getU32(c);
  m.timestampUtcMs = getU64(c);
  m.value = getF32(c);
  m.variable = static_cast<Variable>(*c++);
  m.quality = static_cast<Quality>(*c++);
  m.reasonBits = *c++;
  m.timeUncertain = *c++ != 0;
  std::memcpy(m.nodeId, c, sizeof(m.nodeId));
  c += sizeof(m.nodeId);
  m.sensorId[23] = '\0';
  std::memcpy(m.sensorId, c, sizeof(m.sensorId) - 1);
  c += sizeof(m.sensorId) - 1;
  return DecodeStatus::Ok;
}

size_t encodeFrame(const Measurement& measurement, uint8_t* out) {
  uint8_t payload[kMeasurementPayloadSize];
  const size_t payloadLen = encodePayload(measurement, payload);
  uint8_t* c = out;
  *c++ = kFrameMagic;
  *c++ = kFrameVersion;
  putU16(c, static_cast<uint16_t>(payloadLen));
  std::memcpy(c, payload, payloadLen);
  c += payloadLen;
  const size_t covered = static_cast<size_t>(c - out);
  const uint32_t crc = crc32(out, covered);
  putU32(c, crc);
  return covered + kFrameCrcSize;
}

DecodeStatus decodeFrame(const uint8_t* data, size_t length, Measurement& out) {
  if (length < kFrameSize) return DecodeStatus::BadLength;
  if (data[0] != kFrameMagic) return DecodeStatus::BadMagic;
  if (data[1] != kFrameVersion) return DecodeStatus::BadVersion;

  const uint8_t* lenCursor = data + 2;
  const uint16_t payloadLen = getU16(lenCursor);
  if (payloadLen != kMeasurementPayloadSize) return DecodeStatus::BadLength;

  const size_t covered = kFrameHeaderSize + payloadLen;
  const uint8_t* crcCursor = data + covered;
  uint32_t storedCrc = getU32(crcCursor);
  if (crc32(data, covered) != storedCrc) return DecodeStatus::BadCrc;

  uint8_t payload[kMeasurementPayloadSize];
  std::memcpy(payload, data + kFrameHeaderSize, kMeasurementPayloadSize);
  return decodePayload(payload, out);
}

}  // namespace cauce

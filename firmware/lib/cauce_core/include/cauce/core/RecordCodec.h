#pragma once

#include <cstddef>
#include <cstdint>

#include "cauce/core/Measurement.h"

namespace cauce {

static constexpr size_t kMeasurementPayloadSize = 60;
static constexpr size_t kFrameHeaderSize = 4;
static constexpr size_t kFrameCrcSize = 4;
static constexpr size_t kFrameSize =
    kFrameHeaderSize + kMeasurementPayloadSize + kFrameCrcSize;

enum class DecodeStatus : uint8_t {
  Ok = 0,
  BadMagic = 1,
  BadVersion = 2,
  BadLength = 3,
  BadCrc = 4,
};

uint32_t crc32(const uint8_t* data, size_t length);

size_t encodeFrame(const Measurement& measurement, uint8_t* out);
DecodeStatus decodeFrame(const uint8_t* data, size_t length, Measurement& out);

size_t encodePayload(const Measurement& measurement,
                     uint8_t (&out)[kMeasurementPayloadSize]);
DecodeStatus decodePayload(const uint8_t (&data)[kMeasurementPayloadSize],
                           Measurement& out);

void putU16(uint8_t*& cursor, uint16_t v);
void putU32(uint8_t*& cursor, uint32_t v);
void putU64(uint8_t*& cursor, uint64_t v);
void putF32(uint8_t*& cursor, float v);
uint16_t getU16(const uint8_t*& cursor);
uint32_t getU32(const uint8_t*& cursor);
uint64_t getU64(const uint8_t*& cursor);
float getF32(const uint8_t*& cursor);

}  // namespace cauce

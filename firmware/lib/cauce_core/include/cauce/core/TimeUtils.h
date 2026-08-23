#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce {

void formatIso8601Utc(uint64_t epochMs, char* out, size_t capacity);
bool parseIso8601Utc(const char* text, uint64_t& outEpochMs);

}  // namespace cauce

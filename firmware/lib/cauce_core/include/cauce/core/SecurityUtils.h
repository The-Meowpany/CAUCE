#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce {

void sha256(const uint8_t* data, size_t length, uint8_t outDigest[32]);
void sha256Hex(const char* input, char outHex[65]);
bool secureEquals(const char* a, const char* b);

}  // namespace cauce

#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce {

void sha256(const uint8_t* data, size_t length, uint8_t outDigest[32]);
void sha256Hex(const char* input, char outHex[65]);
bool secureEquals(const char* a, const char* b);

// HMAC-SHA-256 (RFC 2104) built on the internal streaming SHA-256.
// Keys of any length are accepted (hashed when longer than 64 bytes).
void hmacSha256(const uint8_t* key, size_t keyLen,
                const uint8_t* data, size_t dataLen,
                uint8_t outDigest[32]);

}  // namespace cauce

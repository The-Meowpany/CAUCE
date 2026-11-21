#include "cauce/core/PeerKeyring.h"

#include <cstring>

namespace cauce {

namespace {

int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

bool PeerKeyring::parse(const char* text) {
  count_ = 0;
  if (text == nullptr) return true;
  const char* p = text;
  while (*p != '\0') {
    while (*p == ' ' || *p == ',') ++p;
    if (*p == '\0') break;
    if (count_ >= kMaxPeers) {
      count_ = 0;
      return false;
    }
    const char* colon = std::strchr(p, ':');
    const size_t idLength = colon ? static_cast<size_t>(colon - p) : 0;
    Entry& entry = entries_[count_];
    if (colon == nullptr || idLength == 0 || idLength >= sizeof(entry.nodeId)) {
      count_ = 0;
      return false;
    }
    std::memcpy(entry.nodeId, p, idLength);
    entry.nodeId[idLength] = '\0';
    const char* hex = colon + 1;
    for (size_t i = 0; i < kPublicKeyBytes; ++i) {
      const int hi = nibble(hex[2 * i]);
      const int lo = hi < 0 ? -1 : nibble(hex[2 * i + 1]);
      if (hi < 0 || lo < 0) {
        count_ = 0;
        return false;
      }
      entry.key[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    p = hex + 2 * kPublicKeyBytes;
    if (*p != '\0' && *p != ',' && *p != ' ') {
      count_ = 0;  // a 65th hex digit is a typo, not a key
      return false;
    }
    ++count_;
  }
  return true;
}

const uint8_t* PeerKeyring::find(const char* nodeId) const {
  if (nodeId == nullptr) return nullptr;
  for (size_t i = 0; i < count_; ++i) {
    if (std::strcmp(entries_[i].nodeId, nodeId) == 0) return entries_[i].key;
  }
  return nullptr;
}

}  // namespace cauce

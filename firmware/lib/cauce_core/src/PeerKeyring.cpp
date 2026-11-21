#include "cauce/core/PeerKeyring.h"

#include <cstdio>
#include <cstdlib>
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
  dropRevoked();
  return true;
}

bool PeerKeyring::addVouched(const char* nodeId, const uint8_t key[kPublicKeyBytes],
                             uint64_t notAfterUtcMs) {
  if (nodeId == nullptr || nodeId[0] == '\0' || std::strlen(nodeId) >= 16) return false;
  if (isRevoked(nodeId, notAfterUtcMs)) return false;
  for (size_t i = 0; i < count_; ++i) {
    if (std::strcmp(entries_[i].nodeId, nodeId) != 0) continue;
    if (!entries_[i].vouched) return true;  // listed by the operator: keep that key
    std::memcpy(entries_[i].key, key, kPublicKeyBytes);
    entries_[i].notAfterUtcMs = notAfterUtcMs;
    return true;
  }
  if (count_ >= kMaxPeers) return false;
  Entry& entry = entries_[count_++];
  entry = Entry{};
  std::memcpy(entry.nodeId, nodeId, std::strlen(nodeId));
  std::memcpy(entry.key, key, kPublicKeyBytes);
  entry.vouched = true;
  entry.notAfterUtcMs = notAfterUtcMs;
  return true;
}

void PeerKeyring::expire(uint64_t nowUtcMs) {
  if (nowUtcMs == 0) return;
  size_t kept = 0;
  for (size_t i = 0; i < count_; ++i) {
    if (entries_[i].vouched && nowUtcMs > entries_[i].notAfterUtcMs) continue;
    if (kept != i) entries_[kept] = entries_[i];
    ++kept;
  }
  count_ = kept;
  // Past its floor a revocation matches nothing a retired identity could still present.
  size_t keptRevoked = 0;
  for (size_t i = 0; i < revokedCount_; ++i) {
    if (nowUtcMs > revoked_[i].notAfterFloorUtcMs) continue;
    if (keptRevoked != i) revoked_[keptRevoked] = revoked_[i];
    ++keptRevoked;
  }
  revokedCount_ = keptRevoked;
}

bool PeerKeyring::revokedId(const char* nodeId) const {
  for (size_t i = 0; i < revokedCount_; ++i) {
    if (std::strcmp(revoked_[i].nodeId, nodeId) == 0) return true;
  }
  return false;
}

bool PeerKeyring::isRevoked(const char* nodeId, uint64_t notAfterUtcMs) const {
  if (nodeId == nullptr) return false;
  for (size_t i = 0; i < revokedCount_; ++i) {
    if (std::strcmp(revoked_[i].nodeId, nodeId) == 0 &&
        notAfterUtcMs <= revoked_[i].notAfterFloorUtcMs) {
      return true;
    }
  }
  return false;
}

bool PeerKeyring::revoke(const char* nodeId, uint64_t notAfterFloorUtcMs) {
  if (nodeId == nullptr || nodeId[0] == '\0' ||
      std::strlen(nodeId) >= sizeof(revoked_[0].nodeId)) {
    return false;
  }
  Revocation* slot = nullptr;
  for (size_t i = 0; i < revokedCount_; ++i) {
    if (std::strcmp(revoked_[i].nodeId, nodeId) == 0) slot = &revoked_[i];
  }
  if (slot == nullptr) {
    if (revokedCount_ >= kMaxRevoked) return false;
    slot = &revoked_[revokedCount_++];
    *slot = Revocation{};
    std::memcpy(slot->nodeId, nodeId, std::strlen(nodeId) + 1);
  }
  // A later floor only widens what is refused; an earlier one never narrows it.
  if (notAfterFloorUtcMs > slot->notAfterFloorUtcMs) slot->notAfterFloorUtcMs = notAfterFloorUtcMs;
  dropRevoked();
  return true;
}

void PeerKeyring::dropRevoked() {
  size_t kept = 0;
  for (size_t i = 0; i < count_; ++i) {
    const Entry& e = entries_[i];
    const bool drop = e.vouched ? isRevoked(e.nodeId, e.notAfterUtcMs) : revokedId(e.nodeId);
    if (drop) continue;
    if (kept != i) entries_[kept] = entries_[i];
    ++kept;
  }
  count_ = kept;
}

bool PeerKeyring::serializeRevoked(char* out, size_t capacity) const {
  if (out == nullptr || capacity == 0) return false;
  size_t used = 0;
  out[0] = '\0';
  for (size_t i = 0; i < revokedCount_; ++i) {
    const int n = std::snprintf(out + used, capacity - used, "%s%s:%llu", i > 0 ? "," : "",
                                revoked_[i].nodeId,
                                static_cast<unsigned long long>(revoked_[i].notAfterFloorUtcMs));
    if (n <= 0 || static_cast<size_t>(n) >= capacity - used) return false;
    used += static_cast<size_t>(n);
  }
  return true;
}

bool PeerKeyring::parseRevoked(const char* text) {
  revokedCount_ = 0;
  if (text == nullptr) return true;
  const char* p = text;
  while (*p != '\0') {
    while (*p == ',' || *p == ' ' || *p == '\n') ++p;
    if (*p == '\0') break;
    const char* colon = std::strchr(p, ':');
    const size_t idLength = colon ? static_cast<size_t>(colon - p) : 0;
    if (colon == nullptr || idLength == 0 || idLength >= sizeof(revoked_[0].nodeId) ||
        revokedCount_ >= kMaxRevoked) {
      revokedCount_ = 0;
      return false;
    }
    char* end = nullptr;
    const unsigned long long floor = std::strtoull(colon + 1, &end, 10);
    if (end == colon + 1 || (*end != '\0' && *end != ',' && *end != '\n')) {
      revokedCount_ = 0;
      return false;
    }
    Revocation& r = revoked_[revokedCount_++];
    r = Revocation{};
    std::memcpy(r.nodeId, p, idLength);
    r.notAfterFloorUtcMs = floor;
    p = end;
  }
  dropRevoked();
  return true;
}

size_t PeerKeyring::vouchedCount() const {
  size_t n = 0;
  for (size_t i = 0; i < count_; ++i) n += entries_[i].vouched ? 1 : 0;
  return n;
}

const uint8_t* PeerKeyring::find(const char* nodeId) const {
  if (nodeId == nullptr) return nullptr;
  for (size_t i = 0; i < count_; ++i) {
    if (std::strcmp(entries_[i].nodeId, nodeId) == 0) return entries_[i].key;
  }
  return nullptr;
}

}  // namespace cauce

#include "cauce/app/PeerWatermarks.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cauce::app {

namespace {

// Small on purpose. Four peers at 16 bytes of id and up to 10 digits of sequence is well under
// 256 bytes, which is the bound `CommandExecutor` uses for the same kind of file.
constexpr size_t kMaxFileBytes = 256;
constexpr const char* kKey = "peers=";

}  // namespace

uint32_t PeerWatermarks::get(const char* nodeId, uint32_t storageLastSequence) const {
  if (nodeId == nullptr || nodeId[0] == '\0') return 0;
  for (size_t i = 0; i < kMaxPeers; ++i) {
    if (!entries_[i].inUse) continue;
    if (std::strcmp(entries_[i].nodeId, nodeId) != 0) continue;
    // Clamped to what storage actually holds. A stored watermark ahead of storage would make
    // `containsRecord` answer "not present" for a record that *is* present, and the merge would
    // store a duplicate of it. This is the one direction a corrupt state file can do real damage
    // in, so it is clamped rather than trusted.
    return entries_[i].sequence > storageLastSequence ? storageLastSequence
                                                      : entries_[i].sequence;
  }
  return 0;
}

bool PeerWatermarks::advance(const char* nodeId, uint32_t sequence) {
  if (nodeId == nullptr || nodeId[0] == '\0') return false;
  if (std::strlen(nodeId) >= sizeof(entries_[0].nodeId)) return false;

  for (size_t i = 0; i < kMaxPeers; ++i) {
    if (entries_[i].inUse && std::strcmp(entries_[i].nodeId, nodeId) == 0) {
      // Monotonic. A lower sequence means a frame arrived out of order, and rewinding the
      // watermark would make the next merge rescan work already done.
      if (sequence > entries_[i].sequence) entries_[i].sequence = sequence;
      return true;
    }
  }

  for (size_t i = 0; i < kMaxPeers; ++i) {
    if (entries_[i].inUse) continue;
    std::memset(&entries_[i], 0, sizeof(entries_[i]));
    std::memcpy(entries_[i].nodeId, nodeId, std::strlen(nodeId));
    entries_[i].sequence = sequence;
    entries_[i].inUse = true;
    ++count_;
    return true;
  }
  // Full. Rescanning from zero is slow, not wrong, so this is a refusal rather than an eviction:
  // evicting a live peer's watermark because a fifth neighbour appeared would be the more
  // expensive mistake, and less visible.
  return false;
}

bool PeerWatermarks::forget(const char* nodeId) {
  if (nodeId == nullptr) return false;
  for (size_t i = 0; i < kMaxPeers; ++i) {
    if (entries_[i].inUse && std::strcmp(entries_[i].nodeId, nodeId) == 0) {
      entries_[i] = Entry{};
      if (count_ > 0) --count_;
      return true;
    }
  }
  return false;
}

bool PeerWatermarks::persist() const {
  if (path_ == nullptr || path_[0] == '\0') return false;

  char buf[kMaxFileBytes];
  int n = std::snprintf(buf, sizeof(buf), "%s", kKey);
  if (n <= 0) return false;
  for (size_t i = 0; i < kMaxPeers && n > 0; ++i) {
    if (!entries_[i].inUse) continue;
    const int written = std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n),
                                      "%s%s=%lu", n > static_cast<int>(std::strlen(kKey)) ? ";" : "",
                                      entries_[i].nodeId,
                                      static_cast<unsigned long>(entries_[i].sequence));
    if (written <= 0 || static_cast<size_t>(n) + static_cast<size_t>(written) >= sizeof(buf)) {
      return false;  // would truncate: a half-written watermark file is worse than none
    }
    n += written;
  }
  return fs_.writeWholeFile(path_, reinterpret_cast<const uint8_t*>(buf),
                            static_cast<size_t>(n));
}

void PeerWatermarks::load() {
  for (size_t i = 0; i < kMaxPeers; ++i) entries_[i] = Entry{};
  count_ = 0;
  if (path_ == nullptr || path_[0] == '\0') return;
  if (!fs_.exists(path_)) return;

  const size_t size = fs_.fileSize(path_);
  if (size == 0 || size >= kMaxFileBytes) return;

  char buf[kMaxFileBytes];
  if (!fs_.readRange(path_, 0, reinterpret_cast<uint8_t*>(buf), size)) return;
  buf[size] = '\0';

  const char* found = std::strstr(buf, kKey);
  if (found == nullptr) return;
  const char* cursor = found + std::strlen(kKey);

  // Skips malformed entries rather than stopping. A truncated write must not cost every peer
  // its watermark; the ones that survived the truncation still get theirs.
  while (*cursor != '\0' && count_ < kMaxPeers) {
    if (*cursor == ';' || *cursor == ' ' || *cursor == '\n' || *cursor == '\r') {
      ++cursor;
      continue;
    }
    char id[16];
    size_t idLen = 0;
    while (idLen + 1 < sizeof(id) && *cursor != '\0' && *cursor != '=' && *cursor != ';') {
      id[idLen++] = *cursor++;
    }
    id[idLen] = '\0';
    if (idLen == 0) break;  // no id: this is not a pair, and guessing would corrupt the table
    if (*cursor != '=') break;
    ++cursor;

    char* end = nullptr;
    const unsigned long value = std::strtoul(cursor, &end, 10);
    if (end == cursor) break;  // no number
    cursor = end;

    std::memset(&entries_[count_], 0, sizeof(entries_[count_]));
    std::memcpy(entries_[count_].nodeId, id, idLen);
    entries_[count_].sequence = static_cast<uint32_t>(value);
    entries_[count_].inUse = true;
    ++count_;
  }
}

}  // namespace cauce::app
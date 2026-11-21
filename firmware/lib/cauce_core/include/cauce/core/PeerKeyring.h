#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce {

// The public keys of the neighbours a node accepts records from.
//
// ESP-NOW gives no authenticity: anyone with an ESP32 on the channel can transmit a well-formed
// peer frame. So a frame is merged only when it is signed by the node it names, with a key the
// operator put here. That is a pre-shared list rather than a certificate chain because a frame
// has 250 bytes and a signature already costs 64 of them; for an eight-node pilot the list is
// eight lines of config.
//
// Format, as written in `peer_keys`: comma-separated `NODE_ID:64-hex-public-key` entries.
class PeerKeyring {
 public:
  static constexpr size_t kMaxPeers = 8;
  static constexpr size_t kPublicKeyBytes = 32;

  // Replaces the table. Returns false, and leaves it empty, when any entry is malformed: a
  // half-parsed keyring would silently drop a neighbour, which then reads as a dead radio.
  bool parse(const char* text);

  // The key for `nodeId`, or null when the node is not trusted.
  const uint8_t* find(const char* nodeId) const;

  size_t size() const { return count_; }

 private:
  struct Entry {
    char nodeId[16]{};
    uint8_t key[kPublicKeyBytes]{};
  };
  Entry entries_[kMaxPeers]{};
  size_t count_{0};
};

}  // namespace cauce

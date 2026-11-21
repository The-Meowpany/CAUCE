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
  // Larger than the table: a site that replaces hardware retires more ids within one
  // credential lifetime than it has neighbours at any one time.
  static constexpr size_t kMaxRevoked = 16;
  // Longest serialised revocation list: id (15) + ':' + 20 digits + ',' per entry.
  static constexpr size_t kRevokedTextBytes = kMaxRevoked * 37 + 1;
  static constexpr size_t kPublicKeyBytes = 32;

  // Replaces the table. Returns false, and leaves it empty, when any entry is malformed: a
  // half-parsed keyring would silently drop a neighbour, which then reads as a dead radio.
  bool parse(const char* text);

  // The key for `nodeId`, or null when the node is not trusted.
  const uint8_t* find(const char* nodeId) const;

  // Adds or refreshes a neighbour the CA vouched for (see PeerCredential.h). Entries from
  // `peer_keys` are never replaced by one: an operator's explicit line outranks a credential.
  // False when the table is full.
  bool addVouched(const char* nodeId, const uint8_t key[kPublicKeyBytes],
                  uint64_t notAfterUtcMs);
  // Drops vouched entries whose credential has expired, and revocations that no credential
  // can match any more. Listed entries do not expire.
  void expire(uint64_t nowUtcMs);

  // Stops trusting `nodeId` for every credential that expires at or before `notAfterFloorUtcMs`
  // - the expiry of the last certificate the central issued it before retiring it. That is
  // exactly the set of credentials the retired identity can still present: a credential is
  // signature-valid until it expires, so dropping the key alone would let the next
  // announcement put it straight back. A node re-provisioned later gets a credential that
  // expires after the floor and is trusted again without anyone editing this node. A
  // `peer_keys` line for the id is dropped as well while the revocation stands. False when the
  // id is malformed or the list is full.
  bool revoke(const char* nodeId, uint64_t notAfterFloorUtcMs);
  // Whether a credential for `nodeId` expiring at `notAfterUtcMs` is revoked.
  bool isRevoked(const char* nodeId, uint64_t notAfterUtcMs) const;
  size_t revokedCount() const { return revokedCount_; }
  // `NODE_ID:floor` pairs, comma-separated, for persisting across a reboot, and back.
  bool serializeRevoked(char* out, size_t capacity) const;
  bool parseRevoked(const char* text);

  size_t size() const { return count_; }
  size_t vouchedCount() const;

 private:
  struct Entry {
    char nodeId[16]{};
    uint8_t key[kPublicKeyBytes]{};
    bool vouched{false};
    uint64_t notAfterUtcMs{0};
  };
  struct Revocation {
    char nodeId[16]{};
    uint64_t notAfterFloorUtcMs{0};
  };
  bool revokedId(const char* nodeId) const;
  void dropRevoked();

  Entry entries_[kMaxPeers]{};
  size_t count_{0};
  Revocation revoked_[kMaxRevoked]{};
  size_t revokedCount_{0};
};

}  // namespace cauce

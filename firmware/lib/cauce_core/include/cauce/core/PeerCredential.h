#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce {

// A neighbour's identity, vouched for by the central's CA, in a form small enough to ride inside
// an ESP-NOW discovery announcement:
//
//   node_id (16, NUL padded) | Ed25519 public key (32) | not_after_utc_ms (u64 LE) | signature (64)
//
// The CA signs "cauce-peer-v1" followed by the first 56 bytes; the prefix keeps a peer credential
// from ever being mistaken for anything else the same CA signs. It is issued with the node's
// certificate (POST /v1/nodes/{id}/certificate returns `peer_credential`).
constexpr size_t kPeerCredentialBytes = 120;

struct PeerCredential {
  char nodeId[16]{};
  uint8_t publicKey[32]{};
  uint64_t notAfterUtcMs{0};
};

enum class PeerCredentialStatus : uint8_t {
  Ok = 0,
  BadFormat = 1,
  BadSignature = 2,
  Expired = 3,
};

// Checks the CA signature and, when `nowUtcMs` is non-zero, the expiry. A node without a
// trusted clock passes 0 and gets the signature check alone: refusing every neighbour until
// NTP arrives would leave a site with no network unable to replicate at all.
PeerCredentialStatus verifyPeerCredential(const uint8_t raw[kPeerCredentialBytes],
                                          const uint8_t caPublicKey[32], uint64_t nowUtcMs,
                                          PeerCredential& out);

}  // namespace cauce

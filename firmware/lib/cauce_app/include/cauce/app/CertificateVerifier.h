// Verifying a certificate on the node, before it is used.
//
// THE GAP THIS CLOSES
//
// `NodeAuthenticator` presents whatever certificate JSON it was handed and never checks it. A
// node therefore cannot tell a certificate the CA signed from one an attacker wrote, and the
// only thing standing between that and impersonation is that the central verifies the
// signature server-side - which stops an *attacker* using a forged certificate, and does
// nothing about a node presenting its own.
//
// The concrete failure this enables: a certificate issued to CAUCE-002 is a valid, correctly
// signed document naming a different node. A node configured with it would present a genuine
// certificate that says nothing about who it is, and the only defence is the central noticing.
// That defence exists. It is the wrong place for it to be the only one.
//
// WHAT IS CHECKED, AND WHY NOT MORE
//
// 1. The CA signature over the canonical body, against a pinned CA public key. Without this
//    nothing else means anything, so it is checked first.
// 2. The node id, against the one this node believes it is. A certificate that names another
//    node is refused even though its signature is perfect.
// 3. The validity window, against the node's clock. Reported separately from the signature
//    because "expired" and "forged" are different problems with different fixes, and a node
//    whose clock is wrong needs to hear about the clock.
//
// NOT checked: revocation. The central is the revocation point and a node has no way to learn
// about it without a channel, so a revoked certificate still verifies here. That is stated in
// the header rather than left for someone to discover, and it is why revocation still protects
// the system: the central refuses it on the next request regardless of what the node believes.
//
// The canonical body is the backend's, byte for byte: `json.dumps` with `sort_keys=True`,
// `separators=(",", ":")` and `ensure_ascii=False`, over every field except `signature` and
// `ca_public_key`. Reimplementing that is a second encoder, which is the exact class of
// mistake this project has already made once - so it is pinned by a cross-language test rather
// than trusted.

#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce::app {

enum class CertVerifyStatus : uint8_t {
  Ok = 0,
  NoCertificate = 1,
  Malformed = 2,       // not parseable as the expected JSON shape
  BadCaSignature = 3,  // the CA did not sign this
  WrongNode = 4,       // validly signed, but for a different node
  NotYetValid = 5,
  Expired = 6,
  NoCaKey = 7,         // no pinned CA key, so nothing can be verified
};

const char* describeCertVerify(CertVerifyStatus status);

class CertificateVerifier {
 public:
  // Pins the CA public key. Hex, 32 bytes.
  //
  // Pinned rather than taken from the certificate, because `ca_public_key` travels *inside* the
  // document it describes. Trusting it would mean any certificate names the key that verifies
  // it, which verifies nothing at all.
  void pinCaPublicKey(const char* hex) { copyHex(hex, caPublicKey_); }
  bool hasCaKey() const { return caPublicKey_[0] != '\0'; }

  // Verifies `certificateJson` for `nodeId` at `nowUtcMs`.
  //
  // Returns Ok only when all three checks pass. On every failure the output fields are cleared,
  // so a caller that ignores the result cannot go on to trust the parsed values.
  CertVerifyStatus verify(const char* certificateJson, const char* nodeId,
                          uint64_t nowUtcMs);

  // The public key the certificate binds to this node, valid only after Ok.
  //
  // Read through an accessor rather than published as a field so "verified" cannot be skipped
  // by reading the buffer directly - which is the only way this can be made to matter.
  bool boundPublicKey(uint8_t* out, size_t capacity) const;

  // The certificate serial, for logging which certificate a node is using. Also verified-only.
  const char* serial() const { return verified_ ? serial_ : ""; }

 private:
  void copyHex(const char* hex, char* out) const;

  char caPublicKey_[65]{};
  char nodeId_[16]{};
  char serial_[40]{};
  uint8_t publicKey_[32]{};
  uint64_t notBeforeMs_{0};
  uint64_t notAfterMs_{0};
  bool verified_{false};
};

}  // namespace cauce::app
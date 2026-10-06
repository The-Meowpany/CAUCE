#pragma once

// Certificate authentication for a node, on the device side.
//
// WHY THIS EXISTS
//
// The central can already authenticate a node by certificate: it issues a single-use
// challenge and checks a signature made with the private key it has only ever seen the
// public half of. Nothing on a node implemented the other half of that exchange, so a node
// had no way to use it and the scheme reached no hardware.
//
// WHAT IT DOES NOT DO
//
// It does not do mutual TLS. The credential here is an application-level header over a
// channel the node authenticated by DNS name, which is a weaker statement than the node
// proving who it is *and* proving who it is talking to. Saying so is the point; the
// alternative is a comment implying more than the code delivers.
//
// THE ENCODING IS THE CONTRACT
//
// Both sides must produce byte-identical `sign_this` strings, or every authentication
// fails with a signature error that looks like a key problem. The canonical form is
//
//     cauce-sync-challenge\n<node_id>\n<nonce>\n<expires_utc_ms>
//
// and it is pinned on both sides: `test_node_auth.cpp` here, and
// `test_the_canonical_challenge_is_stable` in the backend. If one changes and the other
// does not, one of the two tests fails - which is the only reason to change either.

#include <cstddef>
#include <cstdint>

#include "cauce/core/Ed25519Points.h"
#include "cauce/hal/ISyncTransport.h"

namespace cauce::app {

// One challenge as the central sent it.
//
// `signThis` is stored verbatim rather than rebuilt from the other two fields, because the
// central returns the exact string it will verify against. Reconstructing it here would
// duplicate the encoding on a second side that could drift, which is precisely the failure
// this type exists to prevent.
struct NodeChallenge {
  char nodeId[16]{};
  char nonce[64]{};
  char signThis[256]{};
  uint64_t expiresUtcMs{0};
};

// Fetches a challenge. A separate interface so the host suite can drive the authenticator
// without a radio, and so a deployment could put a cache or a proxy in front of it.
class INodeChallengeSource {
 public:
  virtual ~INodeChallengeSource() = default;

  // Returns Ok and fills `out`, or returns the reason. A transport that cannot reach the
  // central must say so rather than leaving `out` zeroed, because a zeroed nonce would
  // otherwise sign successfully and be rejected by the central with an error that points
  // at the signature instead of at the network.
  virtual hal::ISyncTransport::Result fetchChallenge(const char* baseUrl, const char* nodeId,
                                NodeChallenge& out) = 0;
};

enum class NodeAuthStatus : uint8_t {
  Ok = 0,
  NotConfigured = 1,   // no seed or no certificate
  Unreachable = 2,     // could not ask for a challenge
  NoChallenge = 3,     // the central answered with something unusable
  SignFailed = 4,      // ed25519Sign refused
  EncodeFailed = 5,    // the headers did not fit
};

class NodeAuthenticator {
 public:
  // The seed is copied rather than referenced. A caller that later reuses or wipes its
  // configuration buffer must not be able to change the credential under an authenticator
  // that has already been handed it, and the cost is 32 bytes.
  //
  // The certificate is stored as raw JSON text, not parsed. Nothing here reads it: it is
  // presented to the central, which is the only party that should decide what it means.
  // Parsing it would mean a second implementation of a format this module does not own.
  void setCredential(const uint8_t seed[kEd25519SeedBytes], const char* certificateJson);
  void clearCredential();
  bool isConfigured() const { return hasSeed_ && certificate_[0] != '\0'; }

  void setChallengeSource(INodeChallengeSource* source) { source_ = source; }

  // The central address and this node's id, kept here because the challenge request needs
  // both and the caller should not have to remember to pass them alongside the four output
  // buffers.
  void setIdentity(const char* baseUrl, const char* nodeId);

  // Asks for a challenge and signs it, writing the four headers the central expects.
  //
  //   X-Cauce-Node
  //   X-Cause-Certificate   base64 of the certificate JSON
  //   X-Cause-Nonce         the nonce, verbatim
  //   X-Cause-Nonce-Signature   base64 of the Ed25519 signature over sign_this
  //
  // Every output buffer is cleared on failure. A partially written header set is worse than
  // an empty one: it would be sent, and the central would reject a signature the node
  // believed it had produced.
  NodeAuthStatus authenticate(uint32_t timeoutMs, char* nodeIdOut, size_t nodeIdCap,
                              char* certificateOut, size_t certificateCap,
                              char* nonceOut, size_t nonceCap,
                              char* signatureOut, size_t signatureCap);

  // How many times a challenge has been obtained and answered. Reported on the node's
  // health page, because "certificate authentication is failing" is otherwise invisible
  // until the data stops arriving.
  uint32_t successCount() const { return successes_; }
  uint32_t failureCount() const { return failures_; }

  // The canonical bytes, exposed so the host suite can pin them against the backend's.
  static size_t canonicalChallengeBytes(const char* nodeId, const char* nonce,
                                        uint64_t expiresUtcMs, char* out,
                                        size_t capacity);

  // Base64, standard alphabet with `=` padding. Written here rather than pulled from a
  // library because it has to be host-testable, because the encoding is a wire contract
  // with Python's, and because a round-trip test is cheaper than an argument about which
  // library is available on the target.
  static size_t base64Encode(const uint8_t* data, size_t length, char* out,
                             size_t capacity);

  // Extracts a string field from a JSON object without a parser.
  //
  // ArduinoJson is available on the target and is the right tool for real documents. It is
  // not available to the host suite, and pulling a parser into a core module to read three
  // flat string fields would be worse than the `strstr` scan that `LoRaSyncTransport`
  // already uses for the same reason. This handles only `"key": "value"` with no escapes,
  // which is what the central emits and what a malformed answer fails on.
  static bool jsonStringField(const char* json, const char* key, char* out,
                              size_t capacity);

  static bool jsonUintField(const char* json, const char* key, uint64_t& out);

 private:
  uint8_t seed_[kEd25519SeedBytes]{};
  char certificate_[1024]{};
  bool hasSeed_{false};
  uint32_t successes_{0};
  uint32_t failures_{0};
  INodeChallengeSource* source_{nullptr};
  char baseUrl_[128]{};
  char nodeId_[16]{};
};

}  // namespace cauce::app
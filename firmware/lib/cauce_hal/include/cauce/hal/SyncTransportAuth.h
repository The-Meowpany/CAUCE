// The seam between a sync transport and whatever authenticates it.
//
// WHY THIS LIVES IN `cauce_hal` AND NAMES NO CREDENTIAL
//
// `NodeAuthenticator` lives in `cauce_app`, and `cauce_app` already depends on `cauce_hal`
// for `ISyncTransport`. Having the ESP32 transport hold an authenticator directly closes that
// into a cycle, and the first build failed on exactly that: `cauce_hal` could not see
// `cauce/core/Ed25519Points.h` because it was reaching it through a header two libraries up.
//
// So this direction of dependency is inverted instead. `cauce_hal` knows *that* it needs some
// headers added before a POST and *what they are called* - which is wire contract it owns -
// and knows nothing about seeds, certificates, nonces or signatures. The credential lives in
// `cauce_app`, which already depends on this library and can therefore implement the
// interface below. One direction, no cycle, and the ESP32 transport holds a pointer to an
// interface rather than to a component.
//
// It also means the header list is host-testable. `HTTPClient::addHeader` is not, but
// `IHeaderSink` is, and the names below are asserted against the spelling the central reads.

#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce::hal {

// Somewhere to put one header.
//
// Deliberately *not* satisfied by `HTTPClient` directly. `HTTPClient::addHeader` takes
// `(String, String)`, and passing `http` where a sink is expected got as far as overload
// resolution before failing to compile: HTTPClient is not a sink, it is a caller of one.
//
// So `postBatch` wraps it, about five lines. The alternative - matching `(const String&,
// const String&)` - would drag `Arduino.h` into `cauce_hal` for every consumer including the
// host suite, which has no Arduino at all. The signature here is the one the host suite can
// implement with a vector, and that is what these tests exist to exercise.
class IHeaderSink {
 public:
  virtual ~IHeaderSink() = default;
  virtual void addHeader(const char* name, const char* value) = 0;
};

// The wire names the central reads as FastAPI's `x_cauce_*` parameters. A typo here is a 401
// that reports "certificate auth with incomplete headers" and nothing more, so they are
// asserted against the central's spelling rather than left to review.
namespace headers {
inline constexpr const char* kNode = "X-Cauce-Node";
inline constexpr const char* kCertificate = "X-Cauce-Certificate";
inline constexpr const char* kNonce = "X-Cauce-Nonce";
inline constexpr const char* kNonceSignature = "X-Cauce-Nonce-Signature";
}  // namespace headers

// Adds authentication headers to a sync POST.
//
// Returns false and emits *nothing* on any failure. That is the whole contract, and it is
// not a detail: the central commits to certificate authentication the moment it sees any
// certificate header and will not fall back to the shared secret, so a partial set is worse
// than an empty one - it would be sent, and the central would reject a signature the node
// believes it produced. Emitting nothing is what lets the caller fall back to HMAC without
// that being a silent downgrade.
class IAuthHeaderSource {
 public:
  virtual ~IAuthHeaderSource() = default;

  // `baseUrl` and `nodeId` are supplied by the transport rather than remembered by the
  // implementation, so the caller cannot end up signing an identity it did not mean to.
  virtual bool addAuthHeaders(const char* baseUrl, const char* nodeId, uint32_t timeoutMs,
                              IHeaderSink& sink) = 0;
};

}  // namespace cauce::hal
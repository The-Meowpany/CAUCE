// The ESP32 side of the certificate exchange: fetch a challenge over HTTP.
//
// WHY IT IS IN `cauce_app` AND NOT IN `cauce_hal`
//
// Because it needs `NodeAuthenticator`'s parser, and `NodeAuthenticator` is in `cauce_app`.
// Putting this in `cauce_hal` and including `cauce/app/NodeAuthenticator.h` from there closes a
// dependency cycle - `cauce_app` already depends on `cauce_hal` - and the ESP32 build fails on
// it: `cauce_hal` can no longer reach `cauce/core/Ed25519Points.h` through a header two
// libraries up. That cycle has already been inverted once, for `NodeCertificateAuth`; this file
// is on the correct side of it from the start.
//
// WHY IT PARSES WITH `NodeAuthenticator`'s OWN FUNCTIONS
//
// Because a second parser for this exchange is a second thing to get wrong, and there is
// already a recorded instance of the format being wrong: `json.dumps` escapes the canonical
// string's three newlines as three `\n` pairs, and the first parser in this project refused
// every escape. A parser here would either repeat that bug or diverge from the fix, and a test
// exercising only this file would not notice either.
//
// WHAT IS DELIBERATELY NOT HERE
//
// mDNS, ESP-NOW and every other form of node discovery. This is an HTTP client for one
// endpoint on an address the operator configured, which is a different problem from finding a
// node you were not told about.

#pragma once

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <Arduino.h>

#include "cauce/app/NodeAuthenticator.h"
#include "cauce/hal/ISyncTransport.h"

namespace cauce::app {

// Fetches a challenge from `/v1/sync/challenge`.
//
// The result enum is finer than `ISyncTransport::Result` on purpose. The four failures a
// deployment actually produces - wrong address, no answer, refused, nonsense body - have
// different causes and different fixes, and collapsing them into `NetworkError` means an
// operator with a DNS problem and an operator with a proxy problem read the same log line.
class Esp32ChallengeSource final : public INodeChallengeSource {
 public:
  enum class Result : uint8_t {
    Ok = 0,
    NotConfigured = 1,  // no base URL or no node id
    NetworkError = 2,   // DNS, TCP, or no answer at all
    ServerError = 3,    // the central answered with a 4xx or 5xx that is not auth
    AuthFailed = 4,     // 401 or 403: the central rejected this node's credentials
    Unusable = 5,       // 200 with a body that is not a challenge
  };

  explicit Esp32ChallengeSource(uint32_t timeoutMs = 5000) : timeoutMs_(timeoutMs) {}

  // Returns false on every failure and leaves `out` untouched. An untouched nonce matters more
  // than a zeroed one: a zeroed nonce signs successfully and is rejected as an invalid
  // signature, and a *previous* call''s nonce is worse still - it is real, and it is spent.
  bool fetch(const char* baseUrl, const char* nodeId, NodeChallenge& out);

  // Why the last attempt failed, in words an operator can act on. The four causes a
  // deployment produces - wrong address, no answer, refused, nonsense body - have different
  // fixes, and `NetworkError` for all of them sends everyone to the same wrong place.
  Result lastResult() const { return lastResult_; }
  static const char* describe(Result result);

  hal::ISyncTransport::Result fetchChallenge(const char* baseUrl, const char* nodeId,
                                       NodeChallenge& out) override;

 private:
  uint32_t timeoutMs_;
  Result lastResult_{Result::NotConfigured};
};

}  // namespace cauce::app

#endif
#endif
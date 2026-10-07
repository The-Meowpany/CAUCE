#include "cauce/app/Esp32ChallengeSource.h"

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <Arduino.h>
#include <HTTPClient.h>

namespace cauce::app {

const char* Esp32ChallengeSource::describe(Result result) {
  switch (result) {
    case Result::Ok: return "ok";
    case Result::NotConfigured: return "no server url or node id";
    case Result::NetworkError: return "no answer from the central";
    case Result::ServerError: return "the central refused the request";
    case Result::AuthFailed: return "the central rejected this node's credentials";
    case Result::Unusable: return "the answer was not a challenge";
  }
  return "unknown";
}

// The interface needs `ISyncTransport::Result`, and `Result` is finer-grained on purpose - so
// the two are mapped here rather than collapsed. Collapsing them in `fetch` would throw away
// the distinction between "no answer" and "the answer was nonsense", which are the two a
// deployment misdiagnoses most often.
hal::ISyncTransport::Result Esp32ChallengeSource::fetchChallenge(
    const char* baseUrl, const char* nodeId, NodeChallenge& out) {
  if (!fetch(baseUrl, nodeId, out)) {
    switch (lastResult_) {
      case Result::AuthFailed: return hal::ISyncTransport::Result::AuthFailed;
      case Result::NotConfigured: return hal::ISyncTransport::Result::Rejected;
      // `Unusable` and `ServerError` both mean the central was reached and did not produce a
      // challenge. `ServerError` is the honest mapping for both: the answer was not usable.
      default: return hal::ISyncTransport::Result::ServerError;
    }
  }
  return hal::ISyncTransport::Result::Ok;
}

bool Esp32ChallengeSource::fetch(const char* baseUrl, const char* nodeId,
                                 NodeChallenge& out) {
  if (baseUrl == nullptr || baseUrl[0] == '\0' || nodeId == nullptr || nodeId[0] == '\0') {
    lastResult_ = Result::NotConfigured;
    return false;
  }

  // Zeroed before the request, not after it fails. A partial fill from a previous successful
  // call is worse than a zeroed struct: it is a real nonce that has already been spent.
  NodeChallenge parsed{};
  bool ok = false;

  String url(baseUrl);
  // A trailing slash on the configured base would produce `//v1/...`, which some proxies
  // normalise and some do not. Trimmed here rather than documented as a requirement, because a
  // requirement about slashes is one nobody reads and the failure is a 404 in a deployment.
  while (url.length() > 0 && url[url.length() - 1] == '/') {
    url.remove(url.length() - 1);
  }
  url += "/v1/sync/challenge";

  HTTPClient http;
  http.setTimeout(timeoutMs_);
  if (!http.begin(url)) {
    lastResult_ = Result::NetworkError;
    return false;
  }
  http.addHeader("Content-Type", "application/json");

  const String body = String("{\"node_id\":\"") + nodeId + "\"}";
  const int code = http.POST(body);

  if (code == 200) {
    const String answer = http.getString();
    // `NodeAuthenticator`'s own parser. See the header for why there is not a second one.
    ok = NodeAuthenticator::jsonStringField(answer.c_str(), "node_id", parsed.nodeId,
                                            sizeof(parsed.nodeId)) &&
         NodeAuthenticator::jsonStringField(answer.c_str(), "nonce", parsed.nonce,
                                            sizeof(parsed.nonce)) &&
         NodeAuthenticator::jsonStringField(answer.c_str(), "sign_this", parsed.signThis,
                                            sizeof(parsed.signThis)) &&
         NodeAuthenticator::jsonUintField(answer.c_str(), "expires_utc_ms",
                                         parsed.expiresUtcMs);
    // A 200 with an unusable body is `Unusable`, not `NetworkError`. The central answered;
    // what came back is not a challenge. Reporting a network fault here would send an operator
    // to the radio to debug a JSON field.
    lastResult_ = ok ? Result::Ok : Result::Unusable;
  } else if (code == 401 || code == 403) {
    lastResult_ = Result::AuthFailed;
  } else if (code >= 400) {
    lastResult_ = Result::ServerError;
  } else {
    lastResult_ = Result::NetworkError;
  }
  http.end();

  if (ok) out = parsed;
  return ok;
}

}  // namespace cauce::app

#endif
#endif
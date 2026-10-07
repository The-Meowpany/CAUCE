// The ESP32 sync transport, and the authentication seam it calls into.
//
// WHY THE TRANSPORT DOES NOT KNOW WHAT A CERTIFICATE IS
//
// The exchange needs a POST to `/v1/sync/challenge`, a signature over the answer, and four
// headers on the *next* POST - all inside one `postBatch`, because the caller has nowhere to
// put a round trip. So `postBatch` calls an `IAuthHeaderSource` and gets either the four
// headers or nothing.
//
// `IAuthHeaderSource` rather than a `NodeAuthenticator`, and that is the whole design. The
// credential lives in `cauce_app`, which already depends on `cauce_hal` for `ISyncTransport`;
// naming the authenticator here would close a cycle, and the first attempt did exactly that
// and failed to compile because `cauce_hal` could no longer reach `cauce/core/Ed25519Points.h`
// through a header two libraries up. The dependency is inverted rather than allowed to close.
//
// The trade is that this class no longer says which headers it emits. `SyncTransportAuth.h`
// does, and the host suite asserts those names against the central's spelling, which is a
// stronger guarantee than a comment here would have been.

#pragma once

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <Arduino.h>
#include <ArduinoJson.h>

#include "cauce/hal/ISyncTransport.h"
#include "cauce/hal/SyncTransportAuth.h"

namespace cauce::hal {

class Esp32HttpSyncTransport final : public ISyncTransport {
 public:
  void configure(const char* serverUrl, const char* bearerToken) override;
  Result postBatch(const char* jsonPayload, size_t length,
                   const char* signatureHex, uint32_t timeoutMs,
                   uint32_t& ackedSequenceOut) override;

  Result fetchCommands(CommandBatch& out) override;

  // Optional. When set, every POST asks it for authentication headers first. When unset, or
  // when it declines, the node uses HMAC - so a fleet provisioned before certificate auth
  // existed is untouched.
  //
  // Declining is a normal outcome, not an error: the central being unreachable must not stop a
  // node that has a working shared secret from syncing.
  void setAuthHeaderSource(IAuthHeaderSource* source) { auth_ = source; }

  // Who this node is, for the authenticator to sign as. Set once at setup from `NodeConfig`.
  // Passed per call rather than remembered by the authenticator, so the two cannot drift: the
  // transport is the thing that knows who it is right now.
  //
  // Returns false for a null or empty argument and clears the field. A stale node id here
  // would sign challenges as the wrong node, which fails at the central as an invalid
  // signature rather than as an identity mismatch - the worse of the two diagnoses.
  bool setNodeId(const char* nodeId);

 private:
  CommandBatch lastCommands_;
  String url_;
  String token_;
  String nodeId_;
  IAuthHeaderSource* auth_{nullptr};
};

}  // namespace cauce::hal

#endif
#endif
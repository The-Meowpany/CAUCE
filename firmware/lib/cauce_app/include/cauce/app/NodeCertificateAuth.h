// Certificate authentication as something a transport can call.
//
// This is the bridge the ESP32 transport needs and cannot itself provide. `cauce_hal` owns the
// header names - they are wire contract, and it is what puts them on the wire - but the
// credential lives in `cauce_app`, and `cauce_app` already depends on `cauce_hal`. Holding an
// `INodeChallengeSource` inside `cauce_hal` would close the cycle; the first attempt did, and
// failed to compile because `cauce_hal` could no longer reach `cauce/core/Ed25519Points.h`
// through a header two libraries up.
//
// So the dependency is inverted instead: this adapter sits in `cauce_app`, implements
// `hal::IAuthHeaderSource`, and is the only thing that knows both halves.

#pragma once

#include <cstddef>
#include <cstdint>

#include "cauce/app/NodeAuthenticator.h"
#include "cauce/hal/SyncTransportAuth.h"

namespace cauce::app {

// Bridges an authenticator to a transport.
//
// Holds pointers rather than references because `main.cpp` builds these with `new` during
// setup and nothing guarantees the authenticator outlives the transport's first POST in the
// host-simulation build. A null authenticator or source is `NotConfigured`, not a crash:
// nodes provisioned before certificate auth existed must keep syncing.
class NodeCertificateAuth final : public hal::IAuthHeaderSource {
 public:
  void setAuthenticator(NodeAuthenticator* authenticator) { auth_ = authenticator; }
  void setChallengeSource(INodeChallengeSource* source);

  bool addAuthHeaders(const char* baseUrl, const char* nodeId, uint32_t timeoutMs,
                      hal::IHeaderSink& sink) override;

  // Buffers live here rather than on the stack of a caller that has nowhere to put them: the
  // largest is the base64'd certificate, and a stack frame that big inside a POST path is a
  // bad trade on a device with this little RAM.
  struct Buffers {
    char nodeId[16]{};
    char certificate[1536]{};
    char nonce[64]{};
    char signature[128]{};
  };

 private:
  NodeAuthenticator* auth_{nullptr};
  INodeChallengeSource* source_{nullptr};
  Buffers buffers_{};
};

}  // namespace cauce::app
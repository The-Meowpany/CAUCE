#include "cauce/app/NodeCertificateAuth.h"

#include <cstring>

namespace cauce::app {

void NodeCertificateAuth::setChallengeSource(INodeChallengeSource* source) {
  source_ = source;
  if (auth_ != nullptr) auth_->setChallengeSource(source);
}

bool NodeCertificateAuth::addAuthHeaders(const char* baseUrl, const char* nodeId,
                                         uint32_t timeoutMs, hal::IHeaderSink& sink) {
  // Cleared before anything else, so a caller that ignores the return value still has empty
  // buffers rather than a previous call's signature. A stale signature here would be re-sent
  // for a challenge the central has already spent.
  std::memset(&buffers_, 0, sizeof(buffers_));

  if (auth_ == nullptr || source_ == nullptr || !auth_->isConfigured()) {
    return false;
  }

  // The identity comes from the caller, not from whatever `setIdentity` last saw. The
  // transport knows who it is talking to and what it is calling itself right now; the
  // authenticator's stored identity is a default that must not silently win over it.
  auth_->setIdentity(baseUrl, nodeId);
  auth_->setChallengeSource(source_);

  const NodeAuthStatus status =
      auth_->authenticate(timeoutMs, buffers_.nodeId, sizeof(buffers_.nodeId),
                          buffers_.certificate, sizeof(buffers_.certificate),
                          buffers_.nonce, sizeof(buffers_.nonce), buffers_.signature,
                          sizeof(buffers_.signature));
  if (status != NodeAuthStatus::Ok) {
    std::memset(&buffers_, 0, sizeof(buffers_));
    return false;
  }

  // Fixed order, asserted by the host suite. The central does not care about order; the test
  // does, because a reordered list means somebody edited this by accident and nothing else
  // would have noticed.
  sink.addHeader(hal::headers::kNode, buffers_.nodeId);
  sink.addHeader(hal::headers::kCertificate, buffers_.certificate);
  sink.addHeader(hal::headers::kNonce, buffers_.nonce);
  sink.addHeader(hal::headers::kNonceSignature, buffers_.signature);
  return true;
}

}  // namespace cauce::app
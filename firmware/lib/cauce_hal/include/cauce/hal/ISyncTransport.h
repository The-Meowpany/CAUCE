#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce::hal {

class ISyncTransport {
 public:
  enum class Result : uint8_t {
    Ok = 0,
    AuthFailed = 1,
    NetworkError = 2,
    ServerError = 3,
    Rejected = 4,
  };

  virtual ~ISyncTransport() = default;
  virtual void configure(const char* serverUrl, const char* bearerToken) = 0;
  virtual Result postBatch(const char* jsonPayload, size_t length,
                           const char* signatureHex,
                           uint32_t timeoutMs,
                           uint32_t& ackedSequenceOut) = 0;
};

}  // namespace cauce::hal

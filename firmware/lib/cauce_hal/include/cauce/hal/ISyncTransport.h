#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce::hal {

// A batch of downlink commands handed over by the central in the same HTTP
// response that acknowledges a batch.
struct CommandBatch {
  static constexpr size_t kMaxCommands = 8;
  static constexpr size_t kMaxKind = 32;
  static constexpr size_t kMaxPayload = 256;

  uint32_t commandId[kMaxCommands]{};
  char kind[kMaxCommands][kMaxKind]{};
  char payload[kMaxCommands][kMaxPayload]{};
  size_t count{0};

  void clear() { count = 0; }
  bool add(uint32_t id, const char* kindText, const char* payloadText);
};

class ISyncTransport {
 public:
  enum class Result : uint8_t {
    Ok = 0,
    AuthFailed = 1,
    NetworkError = 2,
    ServerError = 3,
    Rejected = 4,
    NotSupported = 5,
  };

  virtual ~ISyncTransport() = default;
  virtual void configure(const char* serverUrl, const char* bearerToken) = 0;
  virtual Result postBatch(const char* jsonPayload, size_t length,
                           const char* signatureHex,
                           uint32_t timeoutMs,
                           uint32_t& ackedSequenceOut) = 0;

  // Commands the last postBatch carried back. Non-pure with a "nothing
  // pending" default so a transport that cannot do downlink, such as the LoRa
  // one, stays valid instead of pretending to.
  virtual Result fetchCommands(CommandBatch& out) {
    out.clear();
    return Result::NotSupported;
  }
};

}  // namespace cauce::hal

#include "cauce/hal/Esp32HttpSyncTransport.h"

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

namespace cauce::hal {

namespace {

// The five lines `IHeaderSink` exists to avoid dragging `Arduino.h` into `cauce_hal` for.
// `HTTPClient::addHeader` wants `String`s; the sink interface wants `const char*` because the
// host suite implements it with a `std::vector` and has no Arduino.
class HttpClientSink final : public IHeaderSink {
 public:
  explicit HttpClientSink(HTTPClient& client) : client_(client) {}
  void addHeader(const char* name, const char* value) override {
    client_.addHeader(String(name), String(value));
  }

 private:
  HTTPClient& client_;
};

}  // namespace

void Esp32HttpSyncTransport::configure(const char* serverUrl,
                                       const char* bearerToken) {
  url_ = String(serverUrl);
  token_ = String(bearerToken);
}

bool Esp32HttpSyncTransport::setNodeId(const char* nodeId) {
  nodeId_ = (nodeId != nullptr && nodeId[0] != '\0') ? String(nodeId) : String();
  return nodeId_.length() > 0;
}

ISyncTransport::Result Esp32HttpSyncTransport::postBatch(
    const char* jsonPayload, size_t length, const char* signatureHex,
    uint32_t timeoutMs, uint32_t& ackedSequenceOut) {
  HTTPClient http;
  http.setTimeout(timeoutMs);
  if (!http.begin(url_)) return Result::NetworkError;
  http.addHeader("Content-Type", "application/json");
  if (token_.length() > 0) {
    http.addHeader("Authorization", "Bearer " + token_);
  }

  // Certificate auth, when something is there to do it.
  //
  // The return value is deliberately ignored. `addAuthHeaders` emits nothing on any failure,
  // and that is what makes this safe: the central commits to certificate authentication the
  // instant it sees `X-Cauce-Certificate` and will not fall back to the shared secret, so a
  // partial header set would earn a rejection where HMAC would have worked. Declining is the
  // normal path when the central is unreachable, and the node still syncs.
  if (auth_ != nullptr && nodeId_.length() > 0) {
    HttpClientSink sink(http);
    (void)auth_->addAuthHeaders(url_.c_str(), nodeId_.c_str(), timeoutMs, sink);
  }
  if (signatureHex && signatureHex[0]) {
    http.addHeader("X-CAUCE-Signature", String(signatureHex));
  }

  const int code = http.POST(const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(jsonPayload)),
                             static_cast<size_t>(length));
  Result result = Result::ServerError;
  lastCommands_.clear();
  if (code == 200) {
    const String body = http.getString();
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, body);
    if (err) {
      result = Result::ServerError;
    } else {
      const uint32_t acked =
          doc["acknowledged_sequence"] | static_cast<uint32_t>(0);
      if (acked > 0) {
        ackedSequenceOut = acked;
        result = Result::Ok;
      } else {
        result = Result::Rejected;
      }
      // Downlink travels in the acknowledgement response: no extra round trip
      // and no extra radio wakeup on the node.
      JsonArray commands = doc["commands"];
      if (commands) {
        for (JsonVariant item : commands) {
          const uint32_t id = item["command_id"] | 0u;
          const char* kindText = item["kind"] | "";
          const char* payloadText = item["payload"] | "";
          if (id == 0 || kindText[0] == '\0') continue;
          lastCommands_.add(id, kindText, payloadText);
        }
      }
    }
  } else if (code == 401 || code == 403) {
    result = Result::AuthFailed;
  } else if (code < 0) {
    result = Result::NetworkError;
  } else if (code == 422 || code == 409) {
    result = Result::Rejected;
  }
  http.end();
  return result;
}

ISyncTransport::Result Esp32HttpSyncTransport::fetchCommands(CommandBatch& out) {
  out = lastCommands_;
  return Result::Ok;
}

}  // namespace cauce::hal

#endif
#endif
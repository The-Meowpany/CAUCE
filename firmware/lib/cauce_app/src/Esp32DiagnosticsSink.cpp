#include "cauce/app/Esp32DiagnosticsSink.h"

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <HTTPClient.h>

#include <cstdio>
#include <cstring>

namespace cauce::app {

Esp32DiagnosticsSink::Esp32DiagnosticsSink() = default;

bool Esp32DiagnosticsSink::upload(const char* baseUrl, const char* nodeId,
                                  const char* deviceKey, const char* body,
                                  size_t bodyLen) {
  if (baseUrl == nullptr || nodeId == nullptr || body == nullptr) return false;
  if (bodyLen == 0) return false;
  const int n = std::snprintf(url_, sizeof(url_), "%s/v1/nodes/%s/diagnostics",
                              baseUrl, nodeId);
  if (n < 0 || static_cast<size_t>(n) >= sizeof(url_)) return false;

  char signature[65] = {0};
  if (deviceKey != nullptr && deviceKey[0] != '\0') {
    diagnosticsSignatureHex(deviceKey, body, bodyLen, signature);
  }

  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(url_)) return false;
  http.addHeader("Content-Type", "application/json");
  if (signature[0] != '\0') {
    char nodeHeader[32];
    std::snprintf(nodeHeader, sizeof(nodeHeader), "%s", nodeId);
    http.addHeader("X-CAUCE-Node", nodeHeader);
    http.addHeader("X-CAUCE-Signature", signature);
  }
  const int code = http.POST(String(body));
  http.end();
  return code >= 200 && code < 300;
}

}  // namespace cauce::app

#endif
#endif

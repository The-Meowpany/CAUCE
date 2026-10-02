#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce::app {

struct DiagnosticsInput {
  const char* nodeId{"CAUCE-001"};
  const char* siteId{nullptr};
  const char* firmwareVersion{"0.0.0"};
  const char* hardwareRevision{nullptr};
  const char* nodeState{"Boot"};
  const char* netState{"Disabled"};
  uint64_t uptimeMs{0};
  bool clockValid{false};
  int8_t rssiDbm{0};
  float batteryVoltageV{0.0f};
  uint32_t measurementCount{0};
  uint32_t storedCount{0};
  uint32_t invalidCount{0};
  uint32_t suspectCount{0};
  uint32_t readFailures{0};
  uint32_t storageFailures{0};
  uint32_t corruptedFrames{0};
  uint32_t storageRecords{0};
  uint32_t storageBytes{0};
  uint64_t lastSuccessUtcMs{0};
  uint32_t sampleIntervalS{60};
  uint32_t syncAttempts{0};
  uint32_t syncFailures{0};
  int8_t loraRssiDbm{0};
  int8_t loraSnrDb{0};
  int8_t loraSf{0};
  bool loraEnabled{false};
  const char* lastError{nullptr};
  uint32_t resetReason{0};
  uint32_t bootCount{0};
};

size_t buildDiagnosticsJson(char* out, size_t capacity,
                            const DiagnosticsInput& in);

class IDiagnosticsSink {
 public:
  virtual ~IDiagnosticsSink() = default;
  virtual bool upload(const char* baseUrl, const char* nodeId,
                      const char* deviceKey, const char* body,
                      size_t bodyLen) = 0;
};

size_t diagnosticsSignatureHex(const char* deviceKey, size_t keyLen,
                               const char* body, size_t bodyLen,
                               char outHex[65]);

}  // namespace cauce::app

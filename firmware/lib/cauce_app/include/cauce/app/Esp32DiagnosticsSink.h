#pragma once

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include "cauce/app/FieldDiagnostics.h"

namespace cauce::app {

class Esp32DiagnosticsSink final : public IDiagnosticsSink {
 public:
  Esp32DiagnosticsSink();
  bool upload(const char* baseUrl, const char* nodeId, const char* deviceKey,
              const char* body, size_t bodyLen) override;

 private:
  char url_[192]{};
};

}  // namespace cauce::app

#endif
#endif

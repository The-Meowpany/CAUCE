#pragma once

#include <cstddef>
#include <cstdint>

#include "cauce/app/SystemHealth.h"
#include "cauce/core/DataExporter.h"
#include "cauce/core/ConfigManager.h"
#include "cauce/core/IStorageRepository.h"
#include "cauce/core/Logger.h"
#include "cauce/hal/IClock.h"

namespace cauce::app {

class ApiRouter {
 public:
  static constexpr size_t kMinBodyCapacity = 256;

  struct Request {
    const char* method{"GET"};
    const char* target{"/"};
    const char* body{nullptr};
    size_t bodyLen{0};
    const char* authorization{nullptr};
  };

  struct Response {
    uint16_t statusCode{200};
    const char* contentType{"application/json"};
    bool streamActive{false};
    bool streamDone{true};
    size_t bytesWritten{0};
  };

  ApiRouter(IStorageRepository& store, ConfigManager& config,
            hal::IClock& clock, Logger& logger, SystemHealth& health);

  Response handle(const Request& request, char* out, size_t capacity);
  Response continueStream(char* out, size_t capacity);

 private:
  enum class StreamKind : uint8_t { None = 0, Csv = 1, JsonArray = 2, Html = 3 };

  Response respond(uint16_t code, const char* body, size_t len,
                   char* out, size_t capacity);
  Response respondError(uint16_t code, const char* body, char* out, size_t capacity);
  Response routeGet(const char* path, const char* query, char* out,
                    size_t capacity);
  Response routePostConfig(const Request& req, char* out, size_t capacity);
  bool authorized(const Request& req) const;
  bool startMeasurementStream(StreamKind kind, uint64_t fromMs, uint64_t toMs);
  void startHtmlStream();
  Response pumpStream(char* out, size_t capacity);

  IStorageRepository& store_;
  ConfigManager& config_;
  hal::IClock& clock_;
  Logger& logger_;
  SystemHealth& health_;

  alignas(alignof(ChunkedExporter)) uint8_t exporterStorage_[sizeof(ChunkedExporter)];
  ChunkedExporter* exporter_{nullptr};
  StreamKind streamKind_{StreamKind::None};
  const char* htmlData_{nullptr};
  size_t htmlLen_{0};
  size_t htmlPos_{0};
};

}  // namespace cauce::app

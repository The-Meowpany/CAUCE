#pragma once

#include <cstddef>
#include <cstdint>

#include "cauce/core/IStorageRepository.h"

namespace cauce {

class ChunkedExporter {
 public:
  enum class Format : uint8_t { Csv = 0, JsonArray = 1 };

  static constexpr size_t kMinChunkCapacity = 384;

  ChunkedExporter(IStorageRepository& store, Format format, uint64_t fromUtcMs,
                  uint64_t toUtcMs);

  size_t next(char* out, size_t capacity);
  bool done() const { return done_; }

 private:
  static constexpr uint32_t kBatchSize = 16;

  bool fillBatch();
  size_t encodeOne(const Measurement& m, char* out, size_t capacity);
  size_t encodeCsv(const Measurement& m, char* out, size_t capacity);
  size_t encodeJson(const Measurement& m, char* out, size_t capacity);
  size_t writeCsvHeaderIfNeeded(char* out, size_t capacity);

  IStorageRepository& store_;
  Format format_;
  uint64_t fromMs_;
  uint64_t toMs_;

  Measurement batch_[kBatchSize];
  QueryStats stats_{};
  uint32_t batchCount_{0};
  uint32_t batchIndex_{0};
  size_t skipMatches_{0};
  bool headerWritten_{false};
  bool elementEmitted_{false};
  bool done_{false};
};

size_t escapeCsvField(const char* field, char* out, size_t capacity);
size_t escapeJsonString(const char* text, char* out, size_t capacity);
size_t measurementToJson(const Measurement& m, char* out, size_t capacity);

}  // namespace cauce

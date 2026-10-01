#pragma once

#include <cstddef>
#include <cstdint>

#include "cauce/core/IStorageRepository.h"
#include "cauce/core/Logger.h"
#include "cauce/hal/IClock.h"
#include "cauce/hal/IFileSystem.h"
#include "cauce/hal/ISyncTransport.h"

namespace cauce::app {

class SyncManager {
 public:
  static constexpr uint32_t kRecordsPerBatch = 32;

  struct Tuning {
    uint32_t intervalS{300};
    uint32_t requestTimeoutMs{15000};
    uint32_t backoffBaseS{10};
    uint32_t backoffMaxS{1800};
    uint32_t authBackoffS{900};
  };

  SyncManager(IStorageRepository& store, hal::ISyncTransport& transport,
              hal::IClock& clock, Logger& logger, hal::IFileSystem& fs,
              const char* statePath);

  void setNodeId(const char* nodeId);
  void configureEndpoint(const char* serverUrl, const char* bearerToken);
  // Per-device secret used to HMAC-sign every batch (X-CAUCE-Signature).
  // Stored verbatim (up to 64 bytes) to match the server, which signs with
  // the raw provisioned key: HMAC_SHA256(device_key, raw_request_body).
  void setDeviceSecret(const char* asciiSecret);
  void setSyncIntervalS(uint32_t intervalS);
  void loadState();
  void onNetworkConnected();
  void onNetworkLost();
  void tick();

  bool connected() const { return connected_; }
  bool halted() const { return halted_; }
  uint32_t lastAckedSequence() const { return lastAckedSeq_; }
  uint64_t lastSyncUtcMs() const { return lastSyncUtcMs_; }
  uint32_t consecutiveFailures() const { return failures_; }

 private:
  bool syncOneBatch();
  void scheduleRetry(bool authFailure);
  bool saveWatermark();
  void loadWatermarkLocked();

  IStorageRepository& store_;
  hal::ISyncTransport& transport_;
  hal::IClock& clock_;
  Logger& logger_;
  hal::IFileSystem& fs_;
  char statePath_[48];

  char nodeId_[16]{"CAUCE-001"};
  char serverUrl_[128];
  char bearerToken_[65];
  uint8_t deviceKey_[64];
  size_t deviceKeyLen_{0};
  bool hasDeviceKey_{false};
  // Batch serialization buffer lives here (not on the caller's stack):
  // tick() may run on tasks with small stacks.
  char batchPayload_[4096];
  Tuning tuning_;

  bool connected_{false};
  bool halted_{false};
  bool endpointConfigured_{false};
  uint32_t lastAckedSeq_{0};
  uint64_t lastSyncUtcMs_{0};
  uint32_t failures_{0};
  uint64_t nextAttemptMonotonicMs_{0};
};

}  // namespace cauce::app

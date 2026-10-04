#include "cauce/app/SyncManager.h"

#include "cauce/core/TextBuffer.h"

#include "cauce/app/CommandExecutor.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cauce/core/DataExporter.h"
#include "cauce/core/SecurityUtils.h"
#include "cauce/core/Types.h"

namespace cauce::app {

SyncManager::SyncManager(IStorageRepository& store,
                         hal::ISyncTransport& transport, hal::IClock& clock,
                         Logger& logger, hal::IFileSystem& fs,
                         const char* statePath)
    : store_(store),
      transport_(transport),
      clock_(clock),
      logger_(logger),
      fs_(fs) {
  copyString(statePath_, sizeof(statePath_), statePath);
}

void SyncManager::setNodeId(const char* nodeId) {
  copyString(nodeId_, sizeof(nodeId_), nodeId);
}

void SyncManager::configureEndpoint(const char* serverUrl,
                                    const char* bearerToken) {
  copyString(serverUrl_, sizeof(serverUrl_), serverUrl ? serverUrl : "");
  copyString(bearerToken_, sizeof(bearerToken_), bearerToken ? bearerToken : "");
  endpointConfigured_ = serverUrl_[0] != '\0';
  if (endpointConfigured_) transport_.configure(serverUrl_, bearerToken_);
}

void SyncManager::setDeviceSecret(const char* asciiSecret) {
  if (!asciiSecret || !asciiSecret[0]) {
    hasDeviceKey_ = false;
    deviceKeyLen_ = 0;
    return;
  }
  deviceKeyLen_ = std::strlen(asciiSecret);
  if (deviceKeyLen_ > sizeof(deviceKey_)) deviceKeyLen_ = sizeof(deviceKey_);
  std::memcpy(deviceKey_, asciiSecret, deviceKeyLen_);
  hasDeviceKey_ = true;
}

void SyncManager::setSyncIntervalS(uint32_t intervalS) {
  tuning_.intervalS = intervalS < 60 ? 60 : intervalS;
}

void SyncManager::loadState() {
  loadWatermarkLocked();
  logger_.eventf(LogLevel::Info, "SYNC_STATE_LOADED", "node=%s last_acked=%lu",
                 nodeId_, static_cast<unsigned long>(lastAckedSeq_));
}

void SyncManager::loadWatermarkLocked() {
  lastAckedSeq_ = 0;
  if (!fs_.exists(statePath_)) return;
  const size_t size = fs_.fileSize(statePath_);
  if (size == 0 || size > 64) return;
  char buf[65];
  if (!fs_.readRange(statePath_, 0, reinterpret_cast<uint8_t*>(buf), size))
    return;
  buf[size] = '\0';
  const char* key = "last_acked_seq=";
  const char* found = std::strstr(buf, key);
  if (!found) return;
  lastAckedSeq_ = static_cast<uint32_t>(std::strtoul(found + std::strlen(key),
                                                     nullptr, 10));
}

bool SyncManager::saveWatermark() {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "last_acked_seq=%lu\n",
                static_cast<unsigned long>(lastAckedSeq_));
  return fs_.writeWholeFile(statePath_,
                            reinterpret_cast<const uint8_t*>(buf),
                            std::strlen(buf));
}

void SyncManager::onNetworkConnected() {
  if (!connected_) {
    connected_ = true;
    failures_ = 0;
    nextAttemptMonotonicMs_ = clock_.monotonicMs();
    logger_.event(LogLevel::Info, "SYNC_LINK_UP");
  }
}

void SyncManager::onNetworkLost() {
  if (connected_) {
    connected_ = false;
    logger_.event(LogLevel::Warn, "SYNC_LINK_DOWN");
  }
}

void SyncManager::requestSyncNow() {
  // Zeroing the backoff is the whole point: an operator asking for a resync is
  // telling us the previous attempts did not happen yet, not that they failed
  // again. Leaving the counter alone means the next failure still backs off from
  // where it was, which is the correct behaviour for a failure and the wrong one
  // here.
  failures_ = 0;
  nextAttemptMonotonicMs_ = 0;
}

void SyncManager::scheduleRetry(bool authFailure) {
  ++failures_;
  uint32_t backoffS = authFailure
                          ? tuning_.authBackoffS
                          : tuning_.backoffBaseS << (failures_ - 1 > 4 ? 4
                                                                        : failures_ - 1);
  if (backoffS > tuning_.backoffMaxS) backoffS = tuning_.backoffMaxS;
  nextAttemptMonotonicMs_ =
      clock_.monotonicMs() + static_cast<uint64_t>(backoffS) * 1000ULL;
  logger_.eventf(LogLevel::Warn, "SYNC_RETRY_SCHEDULED",
                 "failures=%lu backoff_s=%lu",
                 static_cast<unsigned long>(failures_),
                 static_cast<unsigned long>(backoffS));
}

// Runs whatever commands the central sent back. Kept separate from
// syncOneBatch so a transport that cannot do downlink needs no special case.
void SyncManager::runPendingCommands() {
  if (executor_ == nullptr) return;
  hal::CommandBatch batch;
  if (transport_.fetchCommands(batch) != hal::ISyncTransport::Result::Ok) {
    return;
  }
  if (batch.count == 0) return;
  CommandReceipt receipts[CommandExecutor::kMaxReceipts];
  const size_t n = executor_->ingest(batch, receipts, CommandExecutor::kMaxReceipts);
  if (n == 0) return;
  logger_.eventf(LogLevel::Info, "DOWNLINK_COMMANDS", "count=%u",
                 static_cast<unsigned>(n));
}

bool SyncManager::syncOneBatch() {
  Measurement batch[kRecordsPerBatch];
  QueryStats stats{};
  store_.queryAfterSequence(lastAckedSeq_, batch, kRecordsPerBatch, stats);
  if (stats.returned == 0) {
    nextAttemptMonotonicMs_ =
        clock_.monotonicMs() + static_cast<uint64_t>(tuning_.intervalS) * 1000ULL;
    return true;
  }

  char* payload = batchPayload_;
  const size_t payloadCapacity = sizeof(batchPayload_);
  size_t used = 0;
  receiptsJson_[0] = '\0';
  if (executor_ != nullptr) {
    executor_->writeReceiptsJson(receiptsJson_, sizeof(receiptsJson_));
  }
  char nodeIdJson[64];
  escapeJsonString(nodeId_, nodeIdJson, sizeof(nodeIdJson));
  // Bounded appends throughout: `used` can never pass `payloadCapacity`, so the
  // remaining-capacity arithmetic below cannot underflow. See core/TextBuffer.h.
  TextBuffer payloadText(batchPayload_, payloadCapacity);
  appendText(payloadText, "{\"protocol_version\":%u,\"node_id\":%s,\"batch_size\":",
             Versions::kProtocol, nodeIdJson);
  const size_t batchSizePos = payloadText.used;
  appendRaw(payloadText, "00000");
  used = payloadText.used;
  // Receipts for commands run on the previous round trip go out with this
  // batch, so acknowledgement and delivery share one request.
  if (receiptsJson_[0] != '\0') {
    const size_t receiptsLen = std::strlen(receiptsJson_);
    if (used + receiptsLen < payloadCapacity - 1) {
      std::memcpy(payload + used, ",", 1);
      ++used;
      std::memcpy(payload + used, receiptsJson_, receiptsLen);
      used += receiptsLen;
      batchPayload_[used] = '\0';
    }
  }
  payloadText.used = used;
  appendText(payloadText, ",\"measurements\":[");
  used = payloadText.used;

  uint32_t emitted = 0;
  uint32_t maxSentSeq = lastAckedSeq_;
  for (size_t i = 0; i < stats.returned; ++i) {
    char record[384];
    const size_t recordLen =
        measurementToJson(batch[i], record, sizeof(record));
    const size_t needed = recordLen + (emitted ? 1 : 0) + 3;
    if (recordLen == 0 || used + needed > payloadCapacity - 1) break;
    if (emitted > 0) payload[used++] = ',';
    std::memcpy(payload + used, record, recordLen);
    used += recordLen;
    if (batch[i].sequence > maxSentSeq) maxSentSeq = batch[i].sequence;
    ++emitted;
  }
  used += static_cast<size_t>(
      std::snprintf(payload + used, payloadCapacity - used, "]}"));

  char countText[8];
  std::snprintf(countText, sizeof(countText), "%5u",
                static_cast<unsigned>(emitted));
  std::memcpy(payload + batchSizePos, countText, 5);

  logger_.eventf(LogLevel::Info, "SYNC_BATCH_SENDING",
                 "records=%u after_seq=%lu bytes=%zu", emitted,
                 static_cast<unsigned long>(lastAckedSeq_), used);

  char signatureHex[65] = {0};
  if (hasDeviceKey_) {
    uint8_t mac[32];
    hmacSha256(deviceKey_, deviceKeyLen_,
               reinterpret_cast<const uint8_t*>(payload), used, mac);
    static const char* hexDigits = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
      signatureHex[i * 2] = hexDigits[(mac[i] >> 4) & 0xF];
      signatureHex[i * 2 + 1] = hexDigits[mac[i] & 0xF];
    }
    signatureHex[64] = '\0';
  }

  uint32_t ackedSeq = lastAckedSeq_;
  const auto result = transport_.postBatch(
      payload, used,
      hasDeviceKey_ ? signatureHex : nullptr,
      tuning_.requestTimeoutMs, ackedSeq);

  switch (result) {
    case hal::ISyncTransport::Result::Ok: {
      runPendingCommands();
      // Clamp down to what was actually sent, then never move backwards. A
      // late acknowledgement from a previous attempt must not rewind the
      // watermark: the server dedupes on (node_id, sequence) anyway, so the
      // only cost of rewinding would be resending rows we already stored.
      const uint32_t bounded = ackedSeq > maxSentSeq ? maxSentSeq : ackedSeq;
      lastAckedSeq_ = bounded > lastAckedSeq_ ? bounded : lastAckedSeq_;
      saveWatermark();
      failures_ = 0;
      lastSyncUtcMs_ = clock_.utcMs();
      nextAttemptMonotonicMs_ =
          clock_.monotonicMs() +
          (stats.matched > stats.returned
               ? 0ULL
               : static_cast<uint64_t>(tuning_.intervalS) * 1000ULL);
      logger_.eventf(LogLevel::Info, "SYNC_BATCH_ACKED",
                     "acked_seq=%lu pending_more=%s",
                     static_cast<unsigned long>(lastAckedSeq_),
                     stats.matched > stats.returned ? "yes" : "no");
      return true;
    }
    case hal::ISyncTransport::Result::AuthFailed:
      logger_.event(LogLevel::Error, "SYNC_AUTH_FAILED");
      scheduleRetry(true);
      return false;
    case hal::ISyncTransport::Result::Rejected:
      halted_ = true;
      logger_.eventf(LogLevel::Error, "SYNC_REJECTED_HALTED",
                     "after_seq=%lu", static_cast<unsigned long>(lastAckedSeq_));
      return false;
    default:
      scheduleRetry(false);
      return false;
  }
}

void SyncManager::tick() {
  if (!connected_ || halted_) return;
  if (!endpointConfigured_) return;
  const uint32_t now = clock_.monotonicMs();
  if (now < nextAttemptMonotonicMs_) return;

  for (uint32_t drain = 0; drain < kRecordsPerBatch; ++drain) {
    if (!syncOneBatch()) return;
    Measurement probe[1];
    QueryStats probeStats{};
    store_.queryAfterSequence(lastAckedSeq_, probe, 1, probeStats);
    if (probeStats.matched == 0) break;
  }
}

}  // namespace cauce::app

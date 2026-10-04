#include "cauce/app/CommandExecutor.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cauce::app {

namespace {

void copyString(char* dst, size_t cap, const char* src) {
  if (cap == 0) return;
  size_t i = 0;
  for (; src != nullptr && src[i] != '\0' && i + 1 < cap; ++i) dst[i] = src[i];
  dst[i] = '\0';
}


}  // namespace

CommandExecutor::CommandExecutor(hal::IFileSystem& fs, Logger& logger,
                                 const char* statePath, uint8_t nodeIdSeed)
    : fs_(fs), logger_(logger), nodeIdSeed_(nodeIdSeed) {
  copyString(statePath_, sizeof(statePath_), statePath);
}

bool CommandExecutor::setHandler(const char* kind, Handler handler) {
  if (kind == nullptr || handler == nullptr) return false;
  for (size_t i = 0; i < kMaxHandlers; ++i) {
    if (slots_[i].handler != nullptr &&
        std::strcmp(slots_[i].kind, kind) == 0) {
      slots_[i].handler = handler;
      return true;
    }
  }
  for (size_t i = 0; i < kMaxHandlers; ++i) {
    if (slots_[i].handler == nullptr) {
      copyString(slots_[i].kind, sizeof(slots_[i].kind), kind);
      slots_[i].handler = handler;
      return true;
    }
  }
  return false;
}

void CommandExecutor::loadApplied() {
  historyCount_ = 0;
  historyNext_ = 0;
  if (statePath_[0] == '\0') return;
  if (!fs_.exists(statePath_)) return;
  const size_t size = fs_.fileSize(statePath_);
  if (size == 0 || size > 256) return;
  char buf[257];
  if (!fs_.readRange(statePath_, 0, reinterpret_cast<uint8_t*>(buf), size))
    return;
  buf[size] = '\0';

  const char* key = "applied=";
  const char* found = std::strstr(buf, key);
  if (found == nullptr) return;
  const char* cursor = found + std::strlen(key);
  while (*cursor != '\0' && *cursor != '\n') {
    if (*cursor == ';' || *cursor == ' ') {
      ++cursor;
      continue;
    }
    char* end = nullptr;
    const unsigned long value = std::strtoul(cursor, &end, 10);
    if (end == cursor) break;
    if (historyCount_ < kHistoryDepth) {
      history_[historyCount_++] = static_cast<uint32_t>(value);
      historyNext_ = historyCount_ % kHistoryDepth;
    }
    cursor = end;
  }
}

void CommandExecutor::persist() const {
  if (statePath_[0] == '\0') return;
  char buf[256];
  size_t used = 0;
  used += static_cast<size_t>(
      std::snprintf(buf + used, sizeof(buf) - used, "applied="));
  for (size_t i = 0; i < historyCount_; ++i) {
    if (used + 12 >= sizeof(buf)) break;
    used += static_cast<size_t>(std::snprintf(
        buf + used, sizeof(buf) - used, "%s%lu", i == 0 ? "" : ";",
        static_cast<unsigned long>(history_[i])));
  }
  fs_.writeWholeFile(statePath_, reinterpret_cast<const uint8_t*>(buf), used);
}

void CommandExecutor::remember(uint32_t commandId) {
  if (historyCount_ < kHistoryDepth) {
    history_[historyCount_++] = commandId;
    historyNext_ = historyCount_ % kHistoryDepth;
  } else {
    history_[historyNext_] = commandId;
    historyNext_ = (historyNext_ + 1) % kHistoryDepth;
  }
}

bool CommandExecutor::wasApplied(uint32_t commandId) const {
  for (size_t i = 0; i < historyCount_; ++i) {
    if (history_[i] == commandId) return true;
  }
  return false;
}

size_t CommandExecutor::ingest(const hal::CommandBatch& batch,
                               CommandReceipt* receiptsOut, size_t capacity) {
  pendingCount_ = 0;
  if (receiptsOut == nullptr || capacity == 0) return 0;

  for (size_t i = 0; i < batch.count && pendingCount_ < kMaxReceipts; ++i) {
    const uint32_t id = batch.commandId[i];
    if (id == 0) continue;

    CommandReceipt receipt;
    receipt.commandId = id;

    if (wasApplied(id)) {
      // Re-offered by the central. Say so instead of doing it twice.
      receipt.acked = true;
      copyString(receipt.detail, sizeof(receipt.detail), "already_applied");
      receiptsOut[pendingCount_++] = receipt;
      continue;
    }

    Handler handler = nullptr;
    for (size_t s = 0; s < kMaxHandlers; ++s) {
      if (slots_[s].handler != nullptr &&
          std::strcmp(slots_[s].kind, batch.kind[i]) == 0) {
        handler = slots_[s].handler;
        break;
      }
    }

    if (handler == nullptr) {
      ++unknownKinds_;
      receipt.acked = true;
      copyString(receipt.detail, sizeof(receipt.detail), "unsupported_kind");
      // Deliberately not remembered: a firmware update that adds the handler
      // should be able to pick this command up rather than see it dismissed.
    } else {
      char detail[96] = {0};
      const bool ok = handler(batch.payload[i], detail, sizeof(detail));
      copyString(receipt.detail, sizeof(receipt.detail), ok ? detail : "handler_failed");
      receipt.acked = true;
      // Only a command that actually ran is remembered, so a failed one can be
      // retried instead of being silently swallowed forever.
      if (ok) remember(id);
    }

    receiptsOut[pendingCount_++] = receipt;
  }

  if (pendingCount_ > 0) {
    persist();
    for (size_t i = 0; i < pendingCount_; ++i) pending_[i] = receiptsOut[i];
  }
  return pendingCount_;
}

size_t CommandExecutor::writeReceiptsJson(char* out, size_t capacity) const {
  if (out == nullptr || capacity == 0 || pendingCount_ == 0) return 0;
  size_t used = 0;
  used += static_cast<size_t>(std::snprintf(
      out, capacity, "\"command_receipts\":["));
  for (size_t i = 0; i < pendingCount_; ++i) {
    if (used + 48 >= capacity) break;
    used += static_cast<size_t>(std::snprintf(
        out + used, capacity - used,
        "%s{\"command_id\":%lu,\"state\":\"%s\",\"detail\":\"%s\"}",
        i == 0 ? "" : ",", static_cast<unsigned long>(pending_[i].commandId),
        pending_[i].acked ? "acked" : "delivered", pending_[i].detail));
  }
  used += static_cast<size_t>(std::snprintf(out + used, capacity - used, "]"));
  return used;
}

}  // namespace cauce::app

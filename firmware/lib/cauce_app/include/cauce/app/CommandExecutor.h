#pragma once

#include <cstddef>
#include <cstdint>

#include "cauce/core/Logger.h"
#include "cauce/hal/IFileSystem.h"
#include "cauce/hal/ISyncTransport.h"

namespace cauce::app {

// What the node tells the central about a command it was given.
struct CommandReceipt {
  uint32_t commandId{0};
  bool acked{false};
  char detail[96]{};
};

// Executes downlink commands exactly once.
//
// The central re-offers every command until it is acknowledged, so a command
// arrives again by design. Re-running one would be a bug, not a feature: the
// whole point of an offline-first node is that a retry is always safe. Applied
// ids are therefore remembered in flash, which also covers the reboot case -
// without persistence, a node that acknowledged nothing and rebooted would
// re-apply on the next poll.
class CommandExecutor {
 public:
  static constexpr size_t kMaxHandlers = 8;
  static constexpr size_t kHistoryDepth = 32;
  static constexpr size_t kMaxReceipts = hal::CommandBatch::kMaxCommands;

  // Returns true when the command was understood and carried out. A false
  // return is reported to the central as an explicit refusal rather than a
  // silent no-op.
  using Handler = bool (*)(const char* payloadJson, char* detailOut,
                           size_t detailCapacity);

  CommandExecutor(hal::IFileSystem& fs, Logger& logger,
                  const char* statePath = "/state/applied_commands",
                  uint8_t nodeIdSeed = 0);

  bool setHandler(const char* kind, Handler handler);

  void loadApplied();

  // Executes anything new in `batch` and fills `receiptsOut`. Returns the
  // number of receipts written.
  size_t ingest(const hal::CommandBatch& batch, CommandReceipt* receiptsOut,
                size_t capacity);

  bool wasApplied(uint32_t commandId) const;

  size_t appliedCount() const { return historyCount_; }
  size_t unknownKindCount() const { return unknownKinds_; }

  // Serialises pending receipts for the next /v1/sync payload. Returns the
  // number of bytes written, 0 when there is nothing to report.
  size_t writeReceiptsJson(char* out, size_t capacity) const;

 private:
  struct Slot {
    char kind[hal::CommandBatch::kMaxKind]{};
    Handler handler{nullptr};
  };

  void remember(uint32_t commandId);
  void persist() const;

  hal::IFileSystem& fs_;
  Logger& logger_;
  char statePath_[64];
  Slot slots_[kMaxHandlers];
  uint32_t history_[kHistoryDepth]{};
  size_t historyCount_{0};
  size_t historyNext_{0};
  CommandReceipt pending_[kMaxReceipts];
  size_t pendingCount_{0};
  uint8_t nodeIdSeed_{0};
  size_t unknownKinds_{0};
};

}  // namespace cauce::app
#pragma once

#include <cstddef>
#include <cstdint>

#include "cauce/app/PeerWatermarks.h"
#include "cauce/core/IStorageRepository.h"
#include "cauce/core/Logger.h"
#include "cauce/core/Measurement.h"
#include "cauce/hal/IClock.h"
#include "cauce/hal/IFileSystem.h"

namespace cauce::app {

enum class ArchiveState : uint8_t {
  Disabled = 0,
  Ok = 1,
  // The card refused a write or is missing. Measurement continues on LittleFS; the archive
  // retries with backoff and catches up from LittleFS when the card comes back.
  Degraded = 2,
};

const char* archiveStateName(ArchiveState state);

struct ArchiveCounters {
  uint32_t archived{0};
  uint32_t writeFailures{0};
  // Records LittleFS retention removed before they reached the card. Counted, never hidden:
  // a hole in the archive that nobody can see is the worst kind.
  uint32_t gapRecords{0};
  uint32_t watermark{0};
  // Records merged from neighbours and copied to the card (see setPeerSource).
  uint32_t peerArchived{0};
};

// Copies records from the operational store (LittleFS) to the archive card (microSD) as daily
// CSV files. It reads the store rather than sitting in the measurement path, so a card that
// fails, fills or is pulled out can never stop or slow a measurement - the rule the pilot spec
// sets for the storage tier. The archive's progress is a watermark kept on the card itself:
// a fresh card starts from whatever LittleFS still holds, and a card that comes back after an
// outage resumes from where it stopped.
//
// Layout on the card:
//   /archive/YYYY-MM-DD.csv   one file per UTC day
//   /archive/untimed.csv      records taken before any trusted clock
//   /archive/watermark        last sequence written, as decimal text
//   /archive/peers-YYYY-MM-DD.csv, /archive/peers-untimed.csv
//                             records merged from neighbours, same columns; node_id says whose
//   /archive/peer_marks       last sequence written per neighbour
class ArchiveManager {
 public:
  static constexpr size_t kBatch = 16;
  static constexpr uint32_t kFirstRetryMs = 5000;
  static constexpr uint32_t kMaxRetryMs = 600000;

  ArchiveManager(IStorageRepository& store, hal::IFileSystem& card, hal::IClock& clock,
                 Logger& logger, const char* nodeId);

  // Reads the watermark from the card. Returns false (and starts Degraded) when the card
  // cannot be read; tick() keeps retrying.
  bool begin();

  // Archives at most one batch. Cheap when there is nothing new, and backs off after a
  // failure so a dead card does not cost a write attempt on every loop.
  void tick();

  ArchiveState state() const { return state_; }
  const ArchiveCounters& counters() const { return counters_; }
  // Records stored but not yet on the card.
  uint32_t lag() const;

  // Also archives the records this node holds for its neighbours, in files of their own. A
  // node that dies takes its own card with it; the copies its neighbours kept in LittleFS
  // would otherwise be the only ones left, and LittleFS retention removes them eventually.
  // `merged` names the neighbours and how far their records reach. Own records always go
  // first: a neighbour's backlog never delays this node's archive.
  void setPeerSource(const PeerWatermarks* merged) { peerSource_ = merged; }

  // One CSV line, without the newline. Exposed for tests.
  static size_t formatLine(const Measurement& m, char* out, size_t capacity);
  static void dailyPath(const Measurement& m, char* out, size_t capacity);

 private:
  bool loadWatermark();
  bool saveWatermark();
  bool appendRecord(const Measurement& m, bool peer = false);
  // One batch for one neighbour, round-robin. False only on a card failure.
  bool archivePeerBatch();
  void fail(const char* what);

  IStorageRepository& store_;
  hal::IFileSystem& card_;
  hal::IClock& clock_;
  Logger& logger_;
  ArchiveState state_{ArchiveState::Disabled};
  ArchiveCounters counters_{};
  bool watermarkLoaded_{false};
  uint32_t retryDelayMs_{kFirstRetryMs};
  uint32_t nextAttemptMs_{0};
  char nodeId_[16]{};
  const PeerWatermarks* peerSource_{nullptr};
  PeerWatermarks cardPeerMarks_;
  bool peerMarksLoaded_{false};
  size_t nextPeer_{0};
  // A member rather than a local: sixteen records are over a kilobyte, which the loop task's
  // stack should not have to find on every pass.
  Measurement batch_[kBatch]{};
};

}  // namespace cauce::app

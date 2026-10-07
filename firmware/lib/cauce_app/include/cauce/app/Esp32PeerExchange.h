// Peer-to-peer exchange over ESP-NOW: poll, receive, merge, send.
//
// WHAT CLOSES HERE
//
// The driver and the frame format existed with tests and no caller. This is the loop: poll
// discovery, drain received frames into the merge, offer this node's own records to peers on a
// budget. With `containsRecord` and `PeerWatermarks` underneath it, `mergeRecords` can finally
// answer "already held?" for a record from another node, which is the one thing it could not do.
//
// THE CONVERSION, WHICH IS LOSSY AND SAID SO
//
// `ReplicatedRecord.variable` is a 24-character string; `Measurement.variable` is an enum. So a
// peer's record goes through `parseVariable`, and a name this build does not recognise becomes
// `Unknown` rather than being stored as text no query can interpret. The peer node id is
// preserved verbatim - it is the key, and losing it would collapse two nodes' measurements into
// one series. That asymmetry is deliberate: an unknown *variable* loses meaning but keeps its
// measurement, while an unknown *node* would corrupt another node's history.
//
// THE PRECONDITION, AND WHY IT IS ON THE FUNCTION
//
// `mergeVerified` takes records a caller has already verified. A violated precondition writes
// forged measurements into local storage, and "the caller was supposed to check first" is not
// discoverable after the fact. There is no unverified entry point here, deliberately - a peer
// radio gives no authenticity on its own, and the frame's CRC is corruption protection, not a
// signature. A peer whose batch is verified by signature before it reaches this class is the
// only correct caller.

#pragma once

#include <cstddef>
#include <cstdint>

#include "cauce/app/PeerWatermarks.h"
#include "cauce/core/LogStorageRepository.h"
#include "cauce/core/Measurement.h"
#include "cauce/core/Replication.h"
#include "cauce/hal/Esp32PeerLink.h"
#include "cauce/hal/IPeerLink.h"

namespace cauce::app {

struct PeerExchangeCounters {
  uint32_t framesSent{0};
  uint32_t framesReceived{0};
  uint32_t framesRefused{0};  // failed the CRC, the format, or a size limit
  uint32_t recordsOffered{0};
  uint32_t recordsAccepted{0};
  uint32_t duplicatesIgnored{0};
  uint32_t conflictsObserved{0};
  uint32_t peersSeen{0};
  uint32_t mergesBlocked{0};   // refused because storage said no; not an error
  uint32_t sendBudgetExhausted{0};
};

// A view over this node's own records. Not an owning container: the scheduler already has them,
// and copying them into the peer path every tick would allocate on a device with a few hundred
// KB of heap.
struct LocalRecordsView {
  const Measurement* records{nullptr};
  size_t count{0};
  uint32_t nextSequence{0};
};

class Esp32PeerExchange final : public PeerExchange {
 public:
  // One frame's worth, and it is arithmetic rather than taste: an ESP-NOW payload is 250 bytes
  // and a record costs 59, so a frame carries three. Offering more per tick than the link holds
  // means either truncation or a burst that costs the scheduler its deadline, and the second
  // shows up as a node that stopped sampling.
  static constexpr size_t kMaxRecordsPerFrame = hal::kPeerFrameMaxRecords;

  Esp32PeerExchange(LogStorageRepository& store, PeerWatermarks& watermarks,
               hal::IPeerRadio* radio, hal::IPeerDiscovery* discovery);

  // A null radio or discovery leaves it disabled, which is the correct outcome for a node
  // configured before this existed rather than a failure.
  bool enable();
  void disable();
  bool isEnabled() const { return enabled_; }

  // What this node offers. Set by the caller before `exchange`, because the existing
  // interface is per-peer and there is nowhere to pass it in.
  void setLocalRecords(const LocalRecordsView& local) { local_ = local; }

  // Merges records whose batch signature the caller has already verified. Returns an empty
  // report when there is nothing to merge, so a caller cannot distinguish "disabled" from
  // "nothing arrived" by accident - the counters are for that.
  MergeReport mergeVerified(const ReplicatedRecord* records, size_t count);

  const PeerExchangeCounters& counters() const { return counters_; }

  // Not persisted, and deliberately so: a node that rebooted and reported the counters of a
  // previous life would be describing a radio it has not touched yet.
  void resetCounters() { counters_ = PeerExchangeCounters{}; }


 private:
  // The merge's `apply` callback takes a bare `void*`, so the context is how the exchange
  // reaches its own members. A struct rather than a cast from `PeerExchange*`, so if the
  // exchange's members move, this is the one place that has to move with them - and the
  // compiler says so, which is the whole point of a struct over a cast.
  struct ApplyContext;

  // Converts a peer's record to this node's own type. False when the conversion cannot be done
  // honestly - an empty or unterminated node id - because storing it anyway would create a key
  // that reads past itself.
  static bool toMeasurement(const ReplicatedRecord& in, Measurement& out);

  static MergeOutcome applyToStore(const ReplicatedRecord& record, void* context);

 public:
  ExchangeReport exchange(const uint8_t peerAddress[6]) override;


  LogStorageRepository* store_;
  PeerWatermarks* watermarks_;
  hal::IPeerRadio* radio_;
  hal::IPeerDiscovery* discovery_;
  bool enabled_{false};
  PeerExchangeCounters counters_{};
  // Scratch, allocated once. A 250-byte buffer on the stack of something called every loop
  // iteration is a fragmentation source on a device with this little heap.
  uint8_t scratch_[hal::kPeerFrameMaxBytes]{};
  LocalRecordsView local_{};
};

}  // namespace cauce::app
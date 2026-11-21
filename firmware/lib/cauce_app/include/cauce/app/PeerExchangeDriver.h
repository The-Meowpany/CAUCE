#pragma once

#include <cstddef>
#include <cstdint>

#include "cauce/app/Esp32PeerExchange.h"
#include "cauce/core/IStorageRepository.h"
#include "cauce/hal/IClock.h"
#include "cauce/hal/IPeerLink.h"

namespace cauce::app {

// Drives the peer exchange from loop(): the two decisions it was waiting on.
//
// Cadence. One round per `peer_interval_s` (a minute by default), and one frame per known peer
// per round. A round signs a frame per peer, which costs real time on this CPU, and running it
// as fast as the radio allows would compete with sampling and sync for the loop.
//
// What is offered. This node's own records only, read from the store after a per-peer cursor,
// two at a time. The cursor advances when the radio reports the frame delivered; a peer that
// missed a frame gets the same records again next round, and the merge on its side drops the
// duplicate. Cursors live in RAM: after a reboot a peer is re-offered a bounded backlog rather
// than everything, because its own watermark already makes the old records duplicates.
class PeerExchangeDriver {
 public:
  static constexpr size_t kMaxPeers = 4;
  // How far back a peer met for the first time (or after a reboot) is offered records.
  static constexpr uint32_t kInitialBacklog = 60;

  PeerExchangeDriver(Esp32PeerExchange& exchange, hal::IPeerDiscovery& discovery,
                     IStorageRepository& store, hal::IClock& clock, const char* nodeId,
                     uint32_t intervalS);

  void tick();

  size_t peerCount() const;
  uint32_t rounds() const { return rounds_; }
  // The highest own sequence a peer is known to have been sent, or 0 for an unknown address.
  uint32_t offeredThrough(const uint8_t address[6]) const;

 private:
  struct Slot {
    bool used{false};
    uint8_t address[6]{};
    uint32_t offeredThrough{0};
  };

  void learnPeers();

  Esp32PeerExchange& exchange_;
  hal::IPeerDiscovery& discovery_;
  IStorageRepository& store_;
  hal::IClock& clock_;
  char nodeId_[16]{};
  uint32_t intervalMs_;
  bool started_{false};
  uint32_t lastRunMs_{0};
  uint32_t rounds_{0};
  Slot slots_[kMaxPeers]{};
  Measurement batch_[Esp32PeerExchange::kMaxSignedRecordsPerFrame]{};
};

}  // namespace cauce::app

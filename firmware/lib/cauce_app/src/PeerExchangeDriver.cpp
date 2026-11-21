#include "cauce/app/PeerExchangeDriver.h"

#include <cstring>

#include "cauce/core/Types.h"

namespace cauce::app {

PeerExchangeDriver::PeerExchangeDriver(Esp32PeerExchange& exchange,
                                       hal::IPeerDiscovery& discovery,
                                       IStorageRepository& store, hal::IClock& clock,
                                       const char* nodeId, uint32_t intervalS)
    : exchange_(exchange),
      discovery_(discovery),
      store_(store),
      clock_(clock),
      intervalMs_((intervalS < 10 ? 10u : intervalS) * 1000u) {
  copyString(nodeId_, sizeof(nodeId_), nodeId);
}

size_t PeerExchangeDriver::peerCount() const {
  size_t n = 0;
  for (const Slot& s : slots_) n += s.used ? 1 : 0;
  return n;
}

uint32_t PeerExchangeDriver::offeredThrough(const uint8_t address[6]) const {
  for (const Slot& s : slots_) {
    if (s.used && std::memcmp(s.address, address, 6) == 0) return s.offeredThrough;
  }
  return 0;
}

void PeerExchangeDriver::setAuthority(const uint8_t caPublicKey[32], PeerKeyring* keyring) {
  std::memcpy(caPublicKey_, caPublicKey, sizeof(caPublicKey_));
  keyring_ = keyring;
}

void PeerExchangeDriver::considerCredential(Slot& slot, const hal::Peer& peer) {
  if (keyring_ == nullptr || !peer.hasCredential) return;
  if (slot.credentialVerified &&
      std::memcmp(slot.verifiedCredential, peer.credential, kPeerCredentialBytes) == 0) {
    return;
  }
  const uint64_t now = clock_.utcTimeValid() ? clock_.utcMs() : 0;
  PeerCredential credential;
  if (verifyPeerCredential(peer.credential, caPublicKey_, now, credential) !=
          PeerCredentialStatus::Ok ||
      !keyring_->addVouched(credential.nodeId, credential.publicKey,
                            credential.notAfterUtcMs)) {
    ++credentialsRejected_;
    return;
  }
  std::memcpy(slot.verifiedCredential, peer.credential, kPeerCredentialBytes);
  slot.credentialVerified = true;
  ++credentialsAccepted_;
}

void PeerExchangeDriver::learnPeers() {
  hal::Peer found[kMaxPeers];
  const size_t n = discovery_.poll(found, kMaxPeers);
  for (size_t i = 0; i < n; ++i) {
    Slot* known = nullptr;
    Slot* free = nullptr;
    for (Slot& s : slots_) {
      if (s.used && std::memcmp(s.address, found[i].address, 6) == 0) known = &s;
      if (!s.used && free == nullptr) free = &s;
    }
    if (known != nullptr) {
      considerCredential(*known, found[i]);
      continue;
    }
    // A full table ignores newcomers rather than evicting: a neighbour that stays is worth
    // more than one passing by, and the bound is what keeps a busy channel from growing it.
    if (free == nullptr) continue;
    free->used = true;
    std::memcpy(free->address, found[i].address, 6);
    const uint32_t last = store_.lastOwnSequence();
    free->offeredThrough = last > kInitialBacklog ? last - kInitialBacklog : 0;
    considerCredential(*free, found[i]);
  }
}

void PeerExchangeDriver::tick() {
  const uint32_t now = clock_.monotonicMs();
  if (started_ && now - lastRunMs_ < intervalMs_) return;
  started_ = true;
  lastRunMs_ = now;
  ++rounds_;

  if (keyring_ != nullptr && clock_.utcTimeValid()) keyring_->expire(clock_.utcMs());
  learnPeers();
  for (Slot& slot : slots_) {
    if (!slot.used) continue;
    QueryStats stats{};
    const size_t count = store_.queryAfterSequenceFor(
        nodeId_, slot.offeredThrough, batch_, Esp32PeerExchange::kMaxSignedRecordsPerFrame,
        stats);
    LocalRecordsView view;
    view.records = count > 0 ? batch_ : nullptr;
    view.count = count;
    view.nextSequence = count > 0 ? batch_[count - 1].sequence : 0;
    exchange_.setLocalRecords(view);
    // Called even with nothing to offer: the exchange also receives whatever this peer sent.
    const ExchangeReport report = exchange_.exchange(slot.address);
    if (!report.linkFailed && report.acceptedByPeer > 0 &&
        report.acceptedByPeer <= count) {
      slot.offeredThrough = batch_[report.acceptedByPeer - 1].sequence;
    }
  }
}

}  // namespace cauce::app

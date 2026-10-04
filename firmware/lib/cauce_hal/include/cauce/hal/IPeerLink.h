#pragma once

// Interfaces for peer-to-peer exchange: discovery and the peer radio.
//
// Modelled on `ILoRaRadio` rather than invented fresh, because the same three
// properties have to hold for a peer link: sending must be bounded so a
// scheduler is never held, receiving must distinguish "nothing yet" from "link
// broken", and every call must be testable through a fake on the host.
//
// `Replication.h` holds the merge semantics, which is the part that is a
// correctness problem. What follows is plumbing, and plumbing is only worth
// shipping once the thing it carries is right.
//
// The ESP-NOW driver and mDNS responder are **not** implemented. They need the
// radio and a network stack, and an untested driver behind a tested merge would
// make the merge look proven in a way it is not. The interfaces exist so the
// merge has a shape to be driven through once hardware is available; see
// `docs/en/BENCH_PLAN.md`.

#include <cstddef>
#include <cstdint>

#include "cauce/core/Replication.h"

namespace cauce {
namespace hal {

// A peer discovered on the local link.
struct Peer {
  // Link-layer address. Opaque here on purpose: ESP-NOW and a hypothetical
  // wired peer use different ones, and nothing above this layer should care.
  uint8_t address[6]{};
  // True when the address was learned from the peer itself rather than from a
  // discovery response. A self-declared peer is weaker evidence, and a
 // deployment that lets unverified peers join is trusting the local radio
  // segment, so callers are told which they have.
  bool selfDeclared{true};
};

enum class DiscoveryStatus : uint8_t {
  Ok,
  Unsupported,  // no responder compiled in, or disabled by configuration
  Failed,
};

// Finds peers on the local link. Called on a budget: a gateway must not spend
// its whole scheduler cycle listening.
class IPeerDiscovery {
 public:
  virtual ~IPeerDiscovery() = default;

  // Writes at most `capacity` peers and returns how many were written.
  virtual size_t poll(Peer* out, size_t capacity) = 0;

  virtual DiscoveryStatus status() const = 0;
};

enum class RadioStatus : uint8_t {
  Ok,
  Busy,        // another send is in flight; retry rather than block
  NoRoute,     // the peer is not reachable right now
  Failed,
};

// The peer radio: the same shape as ILoRaRadio, different radio.
class IPeerRadio {
 public:
  virtual ~IPeerRadio() = default;

  // Whether a send may start now. A transport asks this rather than sleeping,
  // so a busy peer link costs the caller a retry instead of a stall.
  virtual bool canSendNow() const = 0;

  // Sends one serialized batch. `length` must be within `maxPayloadBytes()`.
  virtual RadioStatus send(const uint8_t* data, size_t length,
                           const uint8_t peerAddress[6]) = 0;

  // Copies one received batch into `buffer` and returns its length.
  // Returns 0 when nothing has arrived, and a negative value when the link has
  // failed, so "no news" is never mistaken for "broken".
  virtual int receive(uint8_t* buffer, size_t capacity) = 0;

  virtual size_t maxPayloadBytes() const = 0;

  // Signal quality of the last successful send, in dBm, or 0 when unknown.
  // A replication decision does not depend on it; it is reported so an operator
  // can see a peer link degrading before merges start failing.
  virtual int8_t lastRssi() const = 0;
};

}  // namespace hal

namespace app {

// What one exchange with a peer produced.
struct ExchangeReport {
  uint32_t offeredToPeer{0};
  uint32_t acceptedByPeer{0};
  uint32_t receivedFromPeer{0};
  uint32_t conflictsObserved{0};
  // True when the exchange ended because the peer could not be reached, as
  // opposed to completing with nothing new. A caller retrying needs to tell
  // those apart: one is a fact about the data, the other a fact about the link.
  bool linkFailed{false};
};

class PeerExchange {
 public:
  virtual ~PeerExchange() = default;

  // Exchanges with one peer, in both directions, within whatever budget the
  // caller allows. Returns what happened.
  virtual ExchangeReport exchange(const uint8_t peerAddress[6]) = 0;
};

}  // namespace app
}  // namespace cauce

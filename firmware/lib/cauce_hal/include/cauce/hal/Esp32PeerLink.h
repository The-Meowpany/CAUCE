// ESP-NOW peer discovery and the peer radio, for the local link.
//
// WHAT THIS IS AND IS NOT
//
// The *plumbing* half of peer-to-peer exchange: discovery, addressing, bounded sends and a
// receive path. The merge semantics live in `cauce/core/Replication.h` and were already
// tested on the host. `IPeerLink.h` said the ESP-NOW driver was not implemented, and named
// the reason: an untested driver behind a tested merge would make the merge look proven in a
// way it is not.
//
// That reason is still true of the hardware, so this does not claim otherwise. What it does is
// make the driver *compile*, put the frame layout and the address handling under host test, and
// leave exactly one untestable seam - the radio - rather than the whole stack.
//
// THE ARITHMETIC, WHICH DECIDES WHAT THIS LINK CAN ACTUALLY DO
//
// An ESP-NOW payload is at most 250 bytes. A `ReplicatedRecord` serializes to 59 bytes with
// the node id and variable names spelled out in full, so one frame carries **three records**,
// not a batch. At the default 60 s sampling interval that is one minute of one node's data in
// one frame, and a 250-byte frame every 60 s is roughly 2.8 kbit/s of link time.
//
// That is a link for a handful of nodes on one site, not a mesh, and the interface says so
// rather than leaving `maxPayloadBytes()` to imply it. It is also why the sender's node id is
// in the frame header instead of being repeated per record: it is the same string in every
// record of a frame, and moving it out is what buys the third record.
//
// WHY THE FRAME LAYOUT LIVES HERE AND NOT IN THE DRIVER
//
// Every byte of this format is a wire contract with a peer's parser, over an unencrypted
// radio. `encodePeerFrame` / `decodePeerFrame` are free functions so the format can be
// round-tripped on the host against every truncation and corruption case. A `#pragma pack`ed
// struct would be shorter and would put the byte order wherever the compiler decided - the
// same class of mistake the Ed25519 frame format already made once.
//
// THE SECURITY POSITION, STATED PLAINLY
//
// ESP-NOW sends the same records the LoRa gateway path sends, and a peer verifies a batch the
// same way it does there. What it cannot do is authenticate the *link*: with the default
// channel and no pairing, anyone in radio range can inject frames, and those frames fail
// signature verification rather than being accepted. Failing verification is the safe outcome,
// which is why this is the default - but it is not encryption, and nothing here pretends
// otherwise. `IPeerDiscovery` marks every discovered peer `selfDeclared` for the same reason.
//
// mDNS remains uncompiled; this file's discovery answers an announcement broadcast instead.
// See `docs/en/BENCH_PLAN.md`.

#pragma once

#include <cstddef>
#include <cstdint>

#include "cauce/core/Replication.h"
#include "cauce/hal/IPeerLink.h"

namespace cauce {
namespace hal {

constexpr size_t kPeerFrameMaxBytes = 250;  // the ESP-NOW ceiling
// magic 2 + version 1 + count 1 + nodeId 16 + sequence 4. This was written as 22, which is
// short by the two magic bytes and one version byte, and the effect was that *every* frame
// failed to decode: encode computed 81 bytes of budget for a frame it then wrote 83 bytes of,
// and decode rejected 83 as "not the length this format implies". Five tests failed with the
// same symptom and the constant was the whole cause.
// magic 2 + version 1 + count 1 + nodeId 16 + sequence 4, then a 4-byte CRC at the end of the
// frame. The CRC is the fourth defect this format had: without it, flipping one bit anywhere in
// the payload produced a frame that decoded cleanly into a *plausible but wrong* measurement -
// a temperature of 21.5 becomes 21.5 ± an ulp and merges as real data. A test that required
// every single-bit flip to be either refused or identical failed with 176 silent corruptions
// out of 664, which is the format telling the truth about itself.
//
// The CRC is not the signature: it protects against radio noise and a flipped bit, not against
// a peer that chooses what to send. Those are different problems and the frame needs both -
// see the security note below.
constexpr size_t kPeerFrameHeaderBytes = 24;
constexpr size_t kPeerFrameCrcBytes = 4;
constexpr size_t kPeerFrameOverhead = kPeerFrameHeaderBytes + kPeerFrameCrcBytes;
constexpr size_t kPeerFrameMaxRecords = 3;  // (250 - 28) / 59, and asserted by a test

constexpr uint8_t kPeerFrameMagic0 = 0xC3;
constexpr uint8_t kPeerFrameMagic1 = 0x5A;
constexpr uint8_t kPeerFrameVersion = 1;

// Bytes one record costs on the wire, computed rather than assumed: the test
// `test_the_record_size_is_what_the_frame_budget_assumes` compares this against a real
// round trip, because a constant that has drifted from the layout it describes is how three
// records silently become two.
constexpr size_t kPeerRecordBytes = 59;

// What one frame carried, after parsing.
struct PeerFrameContents {
  char nodeId[16]{};
  uint32_t sequence{0};
  size_t recordCount{0};
  ReplicatedRecord records[kPeerFrameMaxRecords]{};
};

// Serializes up to `kPeerFrameMaxRecords` records. Returns the byte count, or 0 when zero
// records were offered or the caller cannot hold the result.
//
// Refusing an over-full batch rather than truncating: the receiving side would otherwise apply
// a prefix of somebody else's data and report a healthy merge.
size_t encodePeerFrame(const char* nodeId, uint32_t sequence,
                       const ReplicatedRecord* records, size_t count,
                       uint8_t* out, size_t capacity);

// Parses one frame. Returns false for anything this build does not fully understand - wrong
// magic, unknown version, a length that does not agree with the buffer, a count above the
// ceiling, or a `nodeId` that is not NUL-terminated.
//
// The last of those is the one worth singling out: the id is a fixed 16-byte field copied
// verbatim from the air, so a peer that fills all sixteen bytes with no terminator would
// otherwise produce a `char[16]` that reads past itself on every later access.
bool decodePeerFrame(const uint8_t* data, size_t length, PeerFrameContents& out);

// Finds peers by answering an announcement broadcast.
//
// `poll()` drains a fixed ring the radio callback fills, so a caller spending its whole
// scheduler cycle listening is impossible by construction rather than by convention.
class Esp32PeerDiscovery final : public IPeerDiscovery {
 public:
  // Explicit channel: peers only find each other on the same one, and a deployment that
  // forgets to set it on both ends gets silence that looks like a broken driver.
  explicit Esp32PeerDiscovery(uint8_t channel = 0);
  ~Esp32PeerDiscovery() override;

  bool begin();
  size_t poll(Peer* out, size_t capacity) override;
  DiscoveryStatus status() const override { return status_; }

  // The radio callback. Copies the address into a fixed table; it must not reference a
  // pointer into the callback's own frame, which has already returned by the time anything
  // reads it.
  static void onPeerFound(const uint8_t* address, const uint8_t* payload, int length);

  static constexpr size_t kMaxKnownPeers = 8;

 private:
  struct KnownPeer {
    uint8_t address[6]{};
    uint32_t lastSeenMs{0};
    bool inUse{false};
  };

  DiscoveryStatus status_{DiscoveryStatus::Unsupported};
  uint8_t channel_{0};
  static KnownPeer s_peers[kMaxKnownPeers];
  // Written from the WiFi task, read from `poll`. A `uint8_t` count: single-word writes are
  // atomic on Xtensa, and a torn read here would be a count one too high at worst.
  static volatile uint8_t s_foundCount;
};

// The peer radio. `maxPayloadBytes()` is the frame ceiling, so a caller cannot construct a
// batch that fits the radio and not the format.
class Esp32PeerRadio final : public IPeerRadio {
 public:
  bool begin(uint8_t channel);
  bool canSendNow() const override;
  RadioStatus send(const uint8_t* data, size_t length,
                   const uint8_t peerAddress[6]) override;
  int receive(uint8_t* buffer, size_t capacity) override;
  size_t maxPayloadBytes() const override { return kPeerFrameMaxBytes; }
  int8_t lastRssi() const override { return lastRssi_; }

 private:
  int8_t lastRssi_{0};
  bool started_{false};
};

}  // namespace hal
}  // namespace cauce
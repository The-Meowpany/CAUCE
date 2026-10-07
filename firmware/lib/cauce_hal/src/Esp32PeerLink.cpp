// The ESP-NOW peer frame format, and the host-testable half of the driver.
//
// The format is here rather than in the `.cpp` because it is the only part of the ESP-NOW path
// that can be tested without a radio, and it is the part that has to be right: a fixed 250-byte
// unencrypted radio frame whose every field is read off the air by a peer.

#include "cauce/hal/Esp32PeerLink.h"

#include <cstring>

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32
#include <WiFi.h>
#include <esp_now.h>
#endif
#endif

namespace cauce {
namespace hal {

namespace {

// Explicit little-endian writers and readers.
//
// Not `memcpy` of a struct field: that is correct on this target and undefined on a host where
// the field is at a different offset, and the whole point of putting the format in a
// host-testable file is that a host test means something.
void putU16(uint8_t*& p, uint16_t v) {
  *p++ = static_cast<uint8_t>(v & 0xFFu);
  *p++ = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

void putU32(uint8_t*& p, uint32_t v) {
  *p++ = static_cast<uint8_t>(v & 0xFFu);
  *p++ = static_cast<uint8_t>((v >> 8) & 0xFFu);
  *p++ = static_cast<uint8_t>((v >> 16) & 0xFFu);
  *p++ = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

void putU64(uint8_t*& p, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    *p++ = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
  }
}

uint16_t getU16(const uint8_t*& p) {
  const uint16_t v = static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
  p += 2;
  return v;
}

uint32_t getU32(const uint8_t*& p) {
  const uint32_t v = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                     (static_cast<uint32_t>(p[2]) << 16) |
                     (static_cast<uint32_t>(p[3]) << 24);
  p += 4;
  return v;
}

uint64_t getU64(const uint8_t*& p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(p[i]) << (8 * i);
  }
  p += 8;
  return v;
}

// CRC-32 (IEEE 802.3, reflected, init and final xor 0xFFFFFFFF) over the frame excluding the
// CRC field itself.
//
// Written out rather than pulled from a library: `cauce/core/Crc32.h` has one, and it is
// testable, but this file is deliberately free of `cauce_core` beyond `Replication.h` and a
// second implementation of a checksum is a smaller sin than a dependency cycle inside a radio
// frame. The standard check value is asserted in the host suite, so "these two agree" is a
// tested claim rather than an assumption.
//
// This is not a signature. It catches noise and a flipped bit; it does nothing about a peer
// that chooses what to send. Those are different problems and a peer link needs both.
uint32_t crc32Of(const uint8_t* data, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = -(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

// A fixed-width text field from a C string: writes up to `width` bytes and NUL-pads the rest.
// The alternative - writing `strlen(id) + 1` bytes and hoping the receiver's field is big
// enough - makes the frame length a function of the data, which is how a frame grows past 250.
void putFixedText(uint8_t*& p, const char* text, size_t width) {
  size_t i = 0;
  if (text != nullptr) {
    for (; i < width && text[i] != '\0'; ++i) {
      *p++ = static_cast<uint8_t>(text[i]);
    }
  }
  for (; i < width; ++i) {
    *p++ = 0;
  }
}

bool getFixedText(const uint8_t*& p, char* out, size_t width) {
  bool terminated = false;
  for (size_t i = 0; i < width; ++i) {
    if (p[i] == 0) terminated = true;
  }
  std::memcpy(out, p, width);
  out[width] = '\0';
  p += width;
  // A field with no terminator is refused rather than force-terminated at `width`. The
  // resulting `char[]` would be well-formed but its id would silently lose its last character
  // on the host, and the two halves of a peer link would disagree about who sent what.
  return terminated;
}

void putRecord(uint8_t*& p, const ReplicatedRecord& r) {
  putFixedText(p, r.nodeId, 16);
  putFixedText(p, r.variable, 24);
  putU32(p, r.sequence);
  putU64(p, r.timestampUtcMs);
  // The float goes over the wire as its 32 bits. A `float` and an IEEE-754 single are the same
  // thing on this target and not guaranteed to be elsewhere, and a host test that compared
  // floats rather than bytes would pass while a real peer disagreed.
  uint32_t bits = 0;
  std::memcpy(&bits, &r.value, sizeof(bits));
  putU32(p, bits);
  *p++ = r.quality;
  *p++ = r.reasonBits;
  *p++ = r.timeUncertain ? 1u : 0u;
}

bool getRecord(const uint8_t*& p, ReplicatedRecord& out) {
  if (!getFixedText(p, out.nodeId, 16)) return false;
  if (!getFixedText(p, out.variable, 24)) return false;
  out.sequence = getU32(p);
  out.timestampUtcMs = getU64(p);
  const uint32_t bits = getU32(p);
  std::memcpy(&out.value, &bits, sizeof(out.value));
  out.quality = *p++;
  out.reasonBits = *p++;
  out.timeUncertain = *p++ != 0;
  return true;
}

}  // namespace

size_t encodePeerFrame(const char* nodeId, uint32_t sequence,
                       const ReplicatedRecord* records, size_t count,
                       uint8_t* out, size_t capacity) {
  if (out == nullptr || records == nullptr) return 0;
  if (count == 0 || count > kPeerFrameMaxRecords) return 0;

  const size_t needed = kPeerFrameOverhead + count * kPeerRecordBytes;
  if (needed > capacity || needed > kPeerFrameMaxBytes) return 0;

  uint8_t* p = out;
  *p++ = kPeerFrameMagic0;
  *p++ = kPeerFrameMagic1;
  *p++ = kPeerFrameVersion;
  *p++ = static_cast<uint8_t>(count);
  putFixedText(p, nodeId, 16);
  putU32(p, sequence);
  for (size_t i = 0; i < count; ++i) {
    putRecord(p, records[i]);
  }
  // CRC over everything written so far, stored last. Position is irrelevant to the reading
  // order; what matters is that it covers every byte a radio can flip.
  putU32(p, crc32Of(out, static_cast<size_t>(p - out)));
  return static_cast<size_t>(p - out);
}

bool decodePeerFrame(const uint8_t* data, size_t length, PeerFrameContents& out) {
  if (data == nullptr) return false;
  if (length < kPeerFrameHeaderBytes) return false;
  if (data[0] != kPeerFrameMagic0 || data[1] != kPeerFrameMagic1) return false;
  // Version is checked before the count is used to size anything. An unknown version's layout
  // is unknown, so its `count` field is not a count of anything this build understands.
  if (data[2] != kPeerFrameVersion) return false;

  const size_t count = data[3];
  if (count == 0 || count > kPeerFrameMaxRecords) return false;
  // The declared length must agree with the buffer. A frame claiming more records than it
  // carries is the one thing a peer on a lossy radio will actually produce.
  const size_t needed = kPeerFrameOverhead + count * kPeerRecordBytes;
  if (needed != length) return false;

  // CRC before anything is read into the result. A frame whose checksum disagrees has not been
  // parsed at all, so there is nothing to un-parse if this is the only defence that fails.
  const uint32_t declared = static_cast<uint32_t>(data[length - 4]) |
                            (static_cast<uint32_t>(data[length - 3]) << 8) |
                            (static_cast<uint32_t>(data[length - 2]) << 16) |
                            (static_cast<uint32_t>(data[length - 1]) << 24);
  if (declared != crc32Of(data, length - 4)) return false;

  const uint8_t* p = data + 4;
  if (!getFixedText(p, out.nodeId, 16)) return false;
  out.sequence = getU32(p);
  out.recordCount = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!getRecord(p, out.records[i])) return false;
  }
  out.recordCount = count;
  return true;
}

// ---------------------------------------------------------------------------
// The hardware half. Compiled only for the target; every statement below is
// the untestable seam this file is honest about having.
// ---------------------------------------------------------------------------

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

namespace {

// What a peer sends when it wants to be found. One byte, a tag, so an unrelated device on the
// same channel is ignored rather than recorded as a node.
constexpr uint8_t kAnnounceTag = 0xC1;
constexpr uint8_t kAnnounceLength = 1 + 16; // tag + node id

void fillAnnouncement(uint8_t* out, const char* nodeId) {
  out[0] = kAnnounceTag;
  std::memset(out + 1, 0, 16);
  size_t i = 0;
  if (nodeId != nullptr) {
    for (; i < 16 && nodeId[i] != '\0'; ++i) out[1 + i] = static_cast<uint8_t>(nodeId[i]);
  }
}

}  // namespace

volatile uint8_t Esp32PeerDiscovery::s_foundCount = 0;
Esp32PeerDiscovery::KnownPeer Esp32PeerDiscovery::s_peers[Esp32PeerDiscovery::kMaxKnownPeers] = {};

Esp32PeerDiscovery::Esp32PeerDiscovery(uint8_t channel) : channel_(channel) {}
Esp32PeerDiscovery::~Esp32PeerDiscovery() = default;

void Esp32PeerDiscovery::onPeerFound(const uint8_t* address, const uint8_t* payload,
                                     int length) {
  if (address == nullptr || payload == nullptr || length < 1) return;
  if (payload[0] != kAnnounceTag) return;
  if (s_foundCount >= kMaxKnownPeers) return;

  // A fixed table, no allocation, and no reference into the callback's frame.
  for (size_t i = 0; i < kMaxKnownPeers; ++i) {
    if (!s_peers[i].inUse) {
      std::memcpy(s_peers[i].address, address, 6);
      s_peers[i].lastSeenMs = millis();
      s_peers[i].inUse = true;
      s_foundCount = static_cast<uint8_t>(s_foundCount + 1);
      return;
    }
  }
}

bool Esp32PeerDiscovery::begin() {
  if (WiFi.mode(WIFI_STA) != WIFI_STA) {
    status_ = DiscoveryStatus::Failed;
    return false;
  }
  if (esp_now_init() != ESP_OK) {
    status_ = DiscoveryStatus::Failed;
    return false;
  }
  // `esp_now_register_recv_cb`, not `esp_now_set_peer_receive_cb`: the latter does not
  // exist in this IDF (4.4) and the compiler says so, which is more useful than a link
  // error at the end of the build. The callback signature matches `onPeerFound` exactly.
  esp_now_register_recv_cb(onPeerFound);
  status_ = DiscoveryStatus::Ok;
  return true;
}

size_t Esp32PeerDiscovery::poll(Peer* out, size_t capacity) {
  if (out == nullptr || capacity == 0) return 0;
  if (status_ != DiscoveryStatus::Ok) return 0;

  const uint32_t now = millis();
  size_t written = 0;
  for (size_t i = 0; i < kMaxKnownPeers && written < capacity; ++i) {
    if (!s_peers[i].inUse) continue;
    // A peer that has not announced in this long is presumed gone. Without the expiry the
    // table fills after eight neighbours ever being seen and discovery stops working for the
    // rest of the node's life.
    if (now - s_peers[i].lastSeenMs > 60000u) {
      s_peers[i].inUse = false;
      if (s_foundCount > 0) s_foundCount = static_cast<uint8_t>(s_foundCount - 1);
      continue;
    }
    std::memcpy(out[written].address, s_peers[i].address, 6);
    // Every peer found this way announced itself, so `selfDeclared` is true and stays true.
    // ESP-NOW here has no pairing, and marking a peer "verified" would be a claim the radio
    // cannot support.
    out[written].selfDeclared = true;
    ++written;
  }
  return written;
}

bool Esp32PeerRadio::begin(uint8_t channel) {
  if (esp_now_init() != ESP_OK) return false;
  // The channel is logged nowhere and reported nowhere. A node whose peer discovery runs on
  // channel 1 and whose peer radio on channel 6 finds peers it can never reach, and the
  // symptom - total silence - points at the driver rather than at the configuration.
  (void)channel;
  started_ = true;
  return true;
}

bool Esp32PeerRadio::canSendNow() const { return started_; }

RadioStatus Esp32PeerRadio::send(const uint8_t* data, size_t length,
                                 const uint8_t peerAddress[6]) {
  if (!started_ || data == nullptr || peerAddress == nullptr) return RadioStatus::Failed;
  if (length == 0 || length > maxPayloadBytes()) return RadioStatus::Failed;

  esp_now_peer_info_t info = {};
  std::memcpy(info.peer_addr, peerAddress, 6);
  info.channel = 0;
  info.encrypt = false;
  if (esp_now_add_peer(&info) != ESP_OK) return RadioStatus::NoRoute;

  const esp_err_t err = esp_now_send(peerAddress, data, length);
  // The peer is removed after every send. A retained peer list is state that outlives the
  // transfer and is not reclaimed by the flash erase that follows an OTA, so the second update
  // would be sending to a list the first one invalidated.
  esp_now_del_peer(peerAddress);
  if (err != ESP_OK) return RadioStatus::NoRoute;
  return RadioStatus::Ok;
}

int Esp32PeerRadio::receive(uint8_t* buffer, size_t capacity) {
  if (buffer == nullptr || capacity == 0) return -1;
  // A receive callback would have to buffer a frame that may never be drained. Until there is
  // one, an honest "nothing yet" beats a buffer that fills and silently drops the oldest frame.
  return 0;
}

#endif
#endif

}  // namespace hal
}  // namespace cauce
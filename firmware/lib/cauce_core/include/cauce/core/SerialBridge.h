#pragma once

#include <cstddef>
#include <cstdint>

namespace cauce::bridge {

// Framing for the USB serial link between the LoRa gateway bridge (an ESP32 with an SX1276)
// and the relay process on the central's host. The bridge holds no keys and makes no
// decisions: it hands up every frame the radio received and transmits whatever comes down.
//
//   magic 0xC5 0xA0 | type (1) | length (2, big-endian) | payload | CRC-16/CCITT-FALSE (2)
//
// The CRC covers type, length and payload. A USB serial line is reliable in practice, but a
// bridge that resets mid-write leaves half a frame in the host's buffer, and the decoder has to
// resynchronise on the next magic instead of reading garbage as a LoRa frame.
//
// backend/cauce_server/serial_bridge.py is the other end and must match byte for byte; both
// test suites pin the same known-answer frame.

constexpr uint8_t kMagic0 = 0xC5;
constexpr uint8_t kMagic1 = 0xA0;
constexpr size_t kHeaderBytes = 5;
constexpr size_t kCrcBytes = 2;
constexpr size_t kOverheadBytes = kHeaderBytes + kCrcBytes;
// A LoRa frame is at most 255 bytes; received frames carry two bytes of RSSI in front.
constexpr size_t kMaxPayloadBytes = 300;

enum class FrameType : uint8_t {
  // Bridge -> host: RSSI (int16, big-endian, dBm) followed by the received LoRa frame.
  RadioReceived = 0x01,
  // Host -> bridge: bytes to transmit, in practice a 6-byte acknowledgement.
  RadioTransmit = 0x02,
  // Either way: a short ASCII status line, for the operator's log.
  Status = 0x03,
};

uint16_t crc16(const uint8_t* data, size_t length);

// Returns the encoded size, or 0 when the payload is too long or `capacity` too small.
size_t encode(FrameType type, const uint8_t* payload, size_t length, uint8_t* out,
              size_t capacity);

class Decoder {
 public:
  // Feeds one byte. Returns true when it completed a valid frame, which stays readable until
  // the next push.
  bool push(uint8_t byte);

  FrameType type() const { return type_; }
  const uint8_t* payload() const { return payload_; }
  size_t length() const { return length_; }
  // Frames dropped for a bad CRC or an impossible length.
  uint32_t errors() const { return errors_; }

 private:
  enum class State : uint8_t { Magic0, Magic1, Type, Length0, Length1, Payload, Crc0, Crc1 };

  void reset() { state_ = State::Magic0; }

  State state_{State::Magic0};
  FrameType type_{FrameType::Status};
  uint8_t rawType_{0};
  size_t length_{0};
  size_t received_{0};
  uint16_t crc_{0};
  uint32_t errors_{0};
  uint8_t payload_[kMaxPayloadBytes]{};
};

}  // namespace cauce::bridge

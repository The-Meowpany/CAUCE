// LoRa gateway bridge: an ESP32 with an SX1276, plugged by USB into the central's host.
//
// It is deliberately dumb. It listens, hands every received frame up the serial line with
// its RSSI, and transmits whatever the host sends down - in practice the acknowledgement for a
// batch the central has durably stored. It holds no node key and verifies nothing, so it can
// relay a signature it could not have produced; `cauce_server.gateway_runner` on the host does
// the reassembly and the forwarding, and the central decides what is genuine.
//
// Built as its own PlatformIO environment (`pio run -e gateway`), the same way the bench
// self-test is, so none of this is reachable in node firmware.
//
// Region: AU915 unless the build sets CAUCE_GATEWAY_REGION. It has to match the nodes'
// `lora_region`; a gateway on the wrong band hears nothing and reports nothing wrong, which is
// why the region is announced on the serial line at boot.

#include <Arduino.h>

#include <cstdarg>

#include "cauce/core/SerialBridge.h"
#include "cauce/hal/Esp32RadioBus.h"
#include "cauce/hal/LoRaRegion.h"
#include "cauce/hal/Sx1276Radio.h"

#ifndef CAUCE_GATEWAY_REGION
#define CAUCE_GATEWAY_REGION "AU915"
#endif

namespace {

constexpr uint32_t kSerialBaud = 115200;
// A radio that came up and then stopped hearing anything is indistinguishable from a quiet
// network, so the bridge says it is alive at this interval.
constexpr uint32_t kHeartbeatMs = 60000;

cauce::hal::Esp32SpiBus g_spi(1000000);
cauce::hal::Esp32RadioControl g_control;
// Same pins as the node: SCK 18, MISO 19, MOSI 23, NSS 5, RESET 14, DIO0 26.
cauce::hal::Sx1276Radio g_radio(g_spi, g_control, /*resetPin=*/14, /*dio0Pin=*/26,
                                cauce::hal::Sx1276Radio::kPinNotWired,
                                cauce::hal::Sx1276Radio::kPinNotWired);
cauce::bridge::Decoder g_decoder;
bool g_radioUp = false;
uint32_t g_lastHeartbeatMs = 0;
uint32_t g_framesUp = 0;
uint32_t g_framesDown = 0;
uint32_t g_crcErrors = 0;

void sendFrame(cauce::bridge::FrameType type, const uint8_t* payload, size_t length) {
  uint8_t out[cauce::bridge::kMaxPayloadBytes + cauce::bridge::kOverheadBytes];
  const size_t n = cauce::bridge::encode(type, payload, length, out, sizeof(out));
  if (n > 0) Serial.write(out, n);
}

void status(const char* text) {
  sendFrame(cauce::bridge::FrameType::Status, reinterpret_cast<const uint8_t*>(text),
            strlen(text));
}

void statusf(const char* format, ...) {
  char text[160];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  status(text);
}

void pumpRadio() {
  uint8_t frame[255];
  const int got = g_radio.receive(frame, sizeof(frame));
  if (got < 0) {
    ++g_crcErrors;
    return;
  }
  if (got == 0) return;
  uint8_t payload[2 + sizeof(frame)];
  const int16_t rssi = g_radio.lastRssiDbm();
  payload[0] = static_cast<uint8_t>((static_cast<uint16_t>(rssi) >> 8) & 0xFF);
  payload[1] = static_cast<uint8_t>(static_cast<uint16_t>(rssi) & 0xFF);
  memcpy(payload + 2, frame, static_cast<size_t>(got));
  sendFrame(cauce::bridge::FrameType::RadioReceived, payload, static_cast<size_t>(got) + 2);
  ++g_framesUp;
}

void pumpSerial() {
  while (Serial.available() > 0) {
    if (!g_decoder.push(static_cast<uint8_t>(Serial.read()))) continue;
    if (g_decoder.type() != cauce::bridge::FrameType::RadioTransmit) continue;
    // send() returns the radio to continuous receive on its own, so the next uplink is
    // heard without another call.
    if (g_radio.send(g_decoder.payload(), g_decoder.length())) {
      ++g_framesDown;
    } else {
      statusf("TX_FAILED length=%u", static_cast<unsigned>(g_decoder.length()));
      g_radio.startReceive();
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(kSerialBaud);
  delay(200);
  cauce::hal::Sx1276Config config{};
  const bool regionKnown = cauce::hal::loraDefaultsFor(CAUCE_GATEWAY_REGION, config);
  const bool spiUp = g_spi.begin();
  g_radioUp = regionKnown && spiUp && g_radio.begin(config) && g_radio.startReceive();
  statusf("CAUCE_GATEWAY region=%s freq=%lu sf=%u radio=%s", CAUCE_GATEWAY_REGION,
          static_cast<unsigned long>(config.frequencyHz),
          static_cast<unsigned>(config.spreadingFactor), g_radioUp ? "up" : "down");
}

void loop() {
  if (g_radioUp) {
    pumpRadio();
    pumpSerial();
  }
  const uint32_t now = millis();
  if (now - g_lastHeartbeatMs >= kHeartbeatMs) {
    g_lastHeartbeatMs = now;
    statusf("HEARTBEAT radio=%s up=%lu down=%lu crc_errors=%lu serial_errors=%lu",
            g_radioUp ? "up" : "down", static_cast<unsigned long>(g_framesUp),
            static_cast<unsigned long>(g_framesDown), static_cast<unsigned long>(g_crcErrors),
            static_cast<unsigned long>(g_decoder.errors()));
  }
  delay(1);
}

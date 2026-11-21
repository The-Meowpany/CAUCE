#pragma once

#include <cmath>
#include <cstdint>

#include "cauce/core/Types.h"
#include "cauce/core/ValidationEngine.h"

namespace cauce {

struct NodeConfig {
  NodeConfig() { thresholds = defaultThresholds(); }

  uint8_t schemaVersion{Versions::kConfigSchema};
  char nodeId[16]{"CAUCE-001"};
  char siteId[24]{};
  float latitude{NAN};
  float longitude{NAN};
  float elevationM{NAN};
  char landCover[16]{};
  char shadeCondition[16]{};
  uint32_t samplingIntervalS{60};
  uint32_t syncIntervalS{900};
  int16_t timezoneOffsetMin{-180};
  bool wifiEnabled{false};
  char wifiSsid[33]{};
  char wifiPassword[65]{};
  char ntpServer[48]{"pool.ntp.org"};
  char syncServerUrl[128]{};
  char syncDeviceKey[65]{};
  // --- certificate authentication ---------------------------------------
  //
  // The Ed25519 seed, hex-encoded: 64 characters, which is what makes it
  // round-trippable through a line-oriented text config and printable without a
  // binary blob. Binary would be smaller, and text is the right trade for a
  // credential that an operator has to be able to read, type and compare.
  char syncAuthSeedHex[65]{};
  // The certificate as issued by the central, stored verbatim as JSON and never
  // parsed here. Nothing in the firmware decides what a certificate *means*; the
  // central is the only party that should, and a second parser would be a second
  // implementation of a format this module does not own. 1024 bytes is generous
  // for the current document, which is around 300.
  char syncCertificate[1024]{};
  // The CA public key, hex. Pinned in the node's own configuration rather than taken
  // from the certificate, because `ca_public_key` travels *inside* the document it
  // describes: a verifier that trusted that field would let any certificate name the
  // key that verifies it, which verifies nothing.
  char caPublicKeyHex[65]{};
  // The key that authorises firmware updates, kept separate from `syncDeviceKey`.
  //
  // They were the same secret, which meant the credential that could authorise a firmware
  // image was the credential every measurement arrived under - and an HMAC key is symmetric,
  // so it authenticates *as* that node as well. Setting this removes that coupling; leaving it
  // empty keeps the old behaviour, because a node provisioned before this existed has no
  // other choice. `main.cpp` reports that once at boot rather than failing closed, because
  // failing closed would strand every deployed node on its next update.
  char otaManifestKey[65]{};
  char otaManifestUrl[160]{};
  bool loraEnabled{false};
  // Optional environmental sensors. All off by default, for the same reason peerEnabled is: a
  // node should not announce a channel it was not asked to publish, and a sensor that is wired
  // in but disabled leaves the node measuring exactly what it measured before. Each is enabled
  // per node because the hardware differs - a rooftop node wants wind and light, a sheltered
  // one wants VOC, and a bench node wants none of them.
  bool lightEnabled{false};
  bool vocEnabled{false};
  bool windEnabled{false};
  // Sweep diameter of the anemometer rotor in metres, and pulses per revolution. The default is
  // the common three-cup hobby rotor; a node with a different rotor must say so, because the
  // speed conversion is wrong by the ratio between the two diameters otherwise.
  float windDiameterMetres{0.15f};
  uint8_t windPulsesPerRevolution{3};
  // Pilot station suite. Same rule as above: off until the node says the part is fitted.
  bool pmEnabled{false};         // Sensirion SPS30, I2C
  bool co2Enabled{false};        // Winsen MH-Z19C, UART
  bool uvEnabled{false};         // Vishay VEML6075, I2C
  bool lightningEnabled{false};  // AS3935, I2C
  // Indoor/outdoor gain of the AS3935's analogue front end. Outdoors is the pilot case; the
  // indoor setting is far more sensitive and turns mains noise into false strikes outside.
  bool lightningIndoor{false};
  bool soilTemperatureEnabled{false};  // DS18B20, 1-Wire
  bool rtcEnabled{false};              // DS3231, I2C
  // microSD archive on the shared SPI bus. Off by default like every other fitted part; a
  // node without a card keeps LittleFS as its only store, which is how it ran before.
  bool sdArchiveEnabled{false};
  bool rainEnabled{false};             // tipping-bucket gauge, pulse input
  // Depth of rain per bucket tip. 0.2794 mm is the common 0.011 in bucket; a different gauge
  // must say so, because every total is wrong by the ratio otherwise.
  float rainMmPerTip{0.2794f};
  bool vaneEnabled{false};  // resistor-ladder wind vane, ADC
  // Degrees between the vane's north mark and true north, added to every reading. A vane is
  // mounted by hand, and an unrecorded offset rotates the whole wind field.
  float vaneOffsetDegrees{0.0f};
  // Analogue channels. Each is a two-point calibration in millivolts at the ADC pin, taken
  // from the procedure in docs: dry/wet for moisture probes, and a linear scale for the rest.
  bool soilMoistureEnabled{false};
  float soilMoistureDryMv{2600.0f};
  float soilMoistureWetMv{1100.0f};
  bool leafWetnessEnabled{false};
  float leafWetnessDryMv{3000.0f};
  float leafWetnessWetMv{800.0f};
  bool irradianceEnabled{false};
  // BPW34 behind a transimpedance stage: W/m2 per mV. Uncalibrated until compared against a
  // pyranometer, which is why the default is a datasheet-derived starting point.
  float irradianceWm2PerMv{0.5f};
  bool soilPotentialEnabled{false};
  // Gypsum block suction, kPa = scale * mV + offset, fitted per soil type.
  float soilPotentialKpaPerMv{0.6f};
  float soilPotentialOffsetKpa{0.0f};
  bool batteryMonitorEnabled{false};
  // Ratio of the resistor divider feeding the ADC: 12 V through 100k/22k is ~5.545.
  float batteryDividerRatio{5.545f};
  // Peer-to-peer over ESP-NOW. Off by default, like deep sleep, because a node that
  // announces itself onto a radio segment it was not asked to join is a deployment
  // decision rather than a default. The channel is explicit for the same reason the
  // driver's is: two ends on different channels find each other and cannot reach each
  // other, which presents as total silence and reads as a broken driver.
  bool peerEnabled{false};
  uint8_t peerChannel{0};
  char peerStatePath[64]{"/state/peer_marks"};
  uint32_t loraSyncIntervalS{3600};
  // AU915 because the pilot is in Uruguay, which uses the 915-928 MHz band. A node elsewhere
  // must set lora_region explicitly; the name is validated at boot, not guessed.
  char loraRegion[16]{"AU915"};
  // How LoRa frames are signed: "hmac" with sync_device_key, or "ed25519" with the seed in
  // sync_auth_seed. Must match device_key_algorithm at the central, which refuses the other.
  char loraFrameAlgorithm[12]{"hmac"};
  uint32_t storageMaxBytes{512u * 1024u};
  uint32_t segmentMaxBytes{64u * 1024u};
  bool deepSleepEnabled{false};
  char adminTokenSha256[65]{};
  Thresholds thresholds;
};

bool serializeConfig(const NodeConfig& config, char* out, size_t capacity);
bool parseConfig(const char* text, NodeConfig& out);

// Decodes 64 hex characters into a 32-byte Ed25519 seed.
//
// A free function rather than a `NodeConfig` method because the caller that needs it -
// `main.cpp`, handing the seed to `NodeAuthenticator` - has the hex string and not the
// config object. Returns false for anything that is not exactly 64 hex characters, and
// writes nothing when it does, so a caller cannot authenticate with half a seed.
bool decodeSeedHex(const char* hex, uint8_t* out, size_t capacity);

}  // namespace cauce

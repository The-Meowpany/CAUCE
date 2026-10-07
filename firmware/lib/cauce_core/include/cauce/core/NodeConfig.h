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
  uint32_t loraSyncIntervalS{3600};
  char loraRegion[16]{"EU868"};
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

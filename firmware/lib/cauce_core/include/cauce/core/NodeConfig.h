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
  uint32_t storageMaxBytes{512u * 1024u};
  uint32_t segmentMaxBytes{64u * 1024u};
  char adminTokenSha256[65]{};
  Thresholds thresholds;
};

bool serializeConfig(const NodeConfig& config, char* out, size_t capacity);
bool parseConfig(const char* text, NodeConfig& out);

}  // namespace cauce

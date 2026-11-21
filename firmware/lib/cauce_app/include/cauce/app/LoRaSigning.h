#pragma once

#include <cstdint>

#include "cauce/app/LoRaSyncTransport.h"
#include "cauce/core/NodeConfig.h"

namespace cauce::app {

enum class LoRaSigningStatus : uint8_t {
  // No device key: the node was never provisioned, and the central accepts unsigned frames
  // only from such a node.
  Unsigned = 0,
  Hmac = 1,
  Ed25519 = 2,
  // `lora_frame_algorithm` names something this build does not implement.
  UnknownAlgorithm = 3,
  // The algorithm is set but its key material is missing or malformed. Transmitting would
  // only produce frames the central refuses, so the caller should not start the link.
  MissingKey = 4,
};

const char* loraSigningStatusName(LoRaSigningStatus status);

// True for the statuses under which the LoRa link may run.
inline bool loraSigningUsable(LoRaSigningStatus status) {
  return status == LoRaSigningStatus::Unsigned || status == LoRaSigningStatus::Hmac ||
         status == LoRaSigningStatus::Ed25519;
}

// Gives the transport its key and frame algorithm from the node configuration. HMAC keys are
// the ASCII `sync_device_key`, exactly the bytes the central keys its HMAC with; the Ed25519
// key is the 32-byte seed decoded from `sync_auth_seed`.
LoRaSigningStatus configureLoRaSigning(LoRaSyncTransport& transport, const NodeConfig& config);

}  // namespace cauce::app

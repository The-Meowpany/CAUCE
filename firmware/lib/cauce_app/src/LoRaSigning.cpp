#include "cauce/app/LoRaSigning.h"

#include <cstring>

#include "cauce/core/Ed25519Points.h"

namespace cauce::app {

const char* loraSigningStatusName(LoRaSigningStatus status) {
  switch (status) {
    case LoRaSigningStatus::Unsigned:
      return "unsigned";
    case LoRaSigningStatus::Hmac:
      return "hmac";
    case LoRaSigningStatus::Ed25519:
      return "ed25519";
    case LoRaSigningStatus::UnknownAlgorithm:
      return "unknown_algorithm";
    default:
      return "missing_key";
  }
}

LoRaSigningStatus configureLoRaSigning(LoRaSyncTransport& transport, const NodeConfig& config) {
  transport.setDeviceKey(nullptr, 0);
  transport.setFrameAlgorithm(cauce::FrameAlgorithm::kNone);

  if (std::strcmp(config.loraFrameAlgorithm, "hmac") == 0) {
    const size_t length = std::strlen(config.syncDeviceKey);
    if (length == 0) return LoRaSigningStatus::Unsigned;
    transport.setDeviceKey(reinterpret_cast<const uint8_t*>(config.syncDeviceKey), length);
    transport.setFrameAlgorithm(cauce::FrameAlgorithm::kHmacSha256);
    return LoRaSigningStatus::Hmac;
  }

  if (std::strcmp(config.loraFrameAlgorithm, "ed25519") == 0) {
    uint8_t seed[kEd25519SeedBytes];
    if (!decodeSeedHex(config.syncAuthSeedHex, seed, sizeof(seed))) {
      return LoRaSigningStatus::MissingKey;
    }
    transport.setDeviceKey(seed, sizeof(seed));
    transport.setFrameAlgorithm(cauce::FrameAlgorithm::kEd25519);
    // The transport keeps its own copy; this one should not outlive the call on the stack.
    volatile uint8_t* wipe = seed;
    for (size_t i = 0; i < sizeof(seed); ++i) wipe[i] = 0;
    return transport.hasUsableAlgorithm() ? LoRaSigningStatus::Ed25519
                                          : LoRaSigningStatus::MissingKey;
  }

  return LoRaSigningStatus::UnknownAlgorithm;
}

}  // namespace cauce::app

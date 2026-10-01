#include "cauce/app/OtaManager.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "cauce/core/SecurityUtils.h"
#include "cauce/core/Sha256Stream.h"
#include "cauce/core/Types.h"

namespace cauce::app {

namespace {

bool parseSemver(const char* text, int parts[3]) {
  if (!text) return false;
  int idx = 0;
  int value = 0;
  bool digitSeen = false;
  for (const char* p = text;; ++p) {
    if (*p >= '0' && *p <= '9') {
      value = value * 10 + (*p - '0');
      digitSeen = true;
    } else if (*p == '.' || *p == '\0') {
      if (!digitSeen || idx >= 3) return false;
      parts[idx++] = value;
      value = 0;
      digitSeen = false;
      if (*p == '\0') break;
    } else {
      return false;
    }
  }
  return idx == 3;
}

void sha256ToHex(const uint8_t digest[32], char out[65]) {
  static const char* hexDigits = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) {
    out[i * 2] = hexDigits[(digest[i] >> 4) & 0xF];
    out[i * 2 + 1] = hexDigits[digest[i] & 0xF];
  }
  out[64] = '\0';
}

}  // namespace

const char* otaStateName(OtaState state) {
  switch (state) {
    case OtaState::Idle: return "IDLE";
    case OtaState::Checking: return "CHECKING";
    case OtaState::UpToDate: return "UP_TO_DATE";
    case OtaState::Downloading: return "DOWNLOADING";
    case OtaState::RebootPending: return "REBOOT_PENDING";
    case OtaState::CheckFailed: return "CHECK_FAILED";
    case OtaState::VerifyFailed: return "VERIFY_FAILED";
    case OtaState::InstallFailed: return "INSTALL_FAILED";
  }
  return "UNKNOWN";
}

int compareSemver(const char* a, const char* b) {
  int pa[3] = {0, 0, 0};
  int pb[3] = {0, 0, 0};
  const bool okA = parseSemver(a, pa);
  const bool okB = parseSemver(b, pb);
  if (!okA && !okB) return 0;
  if (!okA) return -1;
  if (!okB) return 1;
  for (int i = 0; i < 3; ++i) {
    if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
  }
  return 0;
}

OtaManager::OtaManager(IManifestSource& catalog, IFirmwareReader& reader,
                       IFirmwareInstaller& installer, hal::IClock& clock,
                       Logger& logger)
    : catalog_(catalog),
      reader_(reader),
      installer_(installer),
      clock_(clock),
      logger_(logger) {}

void OtaManager::setFirmwareVersion(const char* version) {
  copyString(currentVersion_, sizeof(currentVersion_), version);
}

void OtaManager::setSafetyHooks(FreeHeapFn freeHeap, BatteryFn battery) {
  freeHeap_ = freeHeap;
  battery_ = battery;
}

void OtaManager::setMinFreeHeapBytes(uint32_t minBytes) {
  tuning_.minFreeHeapBytes = minBytes;
}

void OtaManager::setMinBatteryV(float minVolts) { tuning_.minBatteryV = minVolts; }

void OtaManager::setManifestKey(const uint8_t key[32]) {
  std::memcpy(manifestKey_, key, sizeof(manifestKey_));
  hasManifestKey_ = true;
}

void OtaManager::setRebootHook(RebootFn reboot) { reboot_ = reboot; }

void OtaManager::setInterval(uint32_t checkIntervalS) {
  tuning_.checkIntervalS = checkIntervalS;
}

bool OtaManager::safetyOk() const {
  if (freeHeap_ && freeHeap_() < tuning_.minFreeHeapBytes) return false;
  if (battery_ && tuning_.minBatteryV > 0.0f && battery_() < tuning_.minBatteryV)
    return false;
  return true;
}

void OtaManager::scheduleFailure(OtaState failureState, const char* event,
                                 LogLevel level) {
  state_ = failureState;
  logger_.event(level, event);
  nextCheckMonotonicMs_ =
      clock_.monotonicMs() +
      static_cast<uint64_t>(tuning_.checkIntervalS) * 1000ULL;
}

void OtaManager::resetDownload() {
  downloadReceived_ = 0;
  readerOpened_ = false;
}

void OtaManager::runCheck() {
  OtaRelease release{};
  if (!catalog_.fetchLatest(currentVersion_, release)) {
    state_ = OtaState::UpToDate;
    nextCheckMonotonicMs_ =
        clock_.monotonicMs() +
        static_cast<uint64_t>(tuning_.checkIntervalS) * 1000ULL;
    logger_.event(LogLevel::Debug, "OTA_UP_TO_DATE");
    return;
  }

  if (release.sha256Hex[0] == '\0' || release.totalSize == 0 ||
      release.url[0] == '\0') {
    scheduleFailure(OtaState::CheckFailed, "OTA_MANIFEST_INCOMPLETE",
                    LogLevel::Warn);
    return;
  }

  if (hasManifestKey_) {
    char canonical[256];
    std::snprintf(canonical, sizeof(canonical), "%s|%s|%s|%u", release.version,
                  release.sha256Hex, release.url,
                  static_cast<unsigned>(release.totalSize));
    uint8_t mac[32];
    hmacSha256(manifestKey_, sizeof(manifestKey_),
               reinterpret_cast<const uint8_t*>(canonical),
               std::strlen(canonical), mac);
    char expected[65];
    sha256ToHex(mac, expected);
    if (!secureEquals(expected, release.manifestHmacHex)) {
      scheduleFailure(OtaState::CheckFailed, "OTA_MANIFEST_SIGNATURE_INVALID",
                      LogLevel::Error);
      return;
    }
  }

  if (compareSemver(release.version, currentVersion_) <= 0) {
    state_ = OtaState::UpToDate;
    nextCheckMonotonicMs_ =
        clock_.monotonicMs() +
        static_cast<uint64_t>(tuning_.checkIntervalS) * 1000ULL;
    return;
  }

  if (!safetyOk()) {
    scheduleFailure(OtaState::CheckFailed, "OTA_SAFETY_GATE_BLOCKED",
                    LogLevel::Warn);
    return;
  }

  copyString(pendingVersion_, sizeof(pendingVersion_), release.version);
  pendingRelease_ = release;

  if (!installer_.beginInstall(release.totalSize)) {
    scheduleFailure(OtaState::InstallFailed, "OTA_INSTALL_BEGIN_FAILED",
                    LogLevel::Error);
    return;
  }
  sha256Begin(&shaCtx_);
  resetDownload();
  state_ = OtaState::Downloading;
}

bool OtaManager::startDownload() {
  if (!reader_.open(pendingRelease_.url)) {
    installer_.abortInstall();
    scheduleFailure(OtaState::InstallFailed, "OTA_DOWNLOAD_OPEN_FAILED",
                    LogLevel::Error);
    return false;
  }
  readerOpened_ = true;
  downloadReceived_ = 0;
  sha256Begin(&shaCtx_);
  return true;
}

bool OtaManager::pumpChunk() {
  uint8_t chunk[512];
  const size_t readSize = tuning_.chunkSize < sizeof(chunk)
                              ? tuning_.chunkSize
                              : sizeof(chunk);
  const size_t n = reader_.read(chunk, readSize);
  if (n == 0) return true;  // EOF
  if (downloadReceived_ + n > pendingRelease_.totalSize) {
    abortDownload("OTA_SIZE_EXCEEDED", OtaState::VerifyFailed, LogLevel::Error);
    return false;
  }
  sha256Append(&shaCtx_, chunk, n);
  if (!installer_.writeChunk(chunk, n)) {
    abortDownload("OTA_WRITE_FAILED", OtaState::InstallFailed, LogLevel::Error);
    return false;
  }
  downloadReceived_ += n;
  return true;
}

bool OtaManager::finishDownload() {
  if (downloadReceived_ != pendingRelease_.totalSize) {
    abortDownload("OTA_SIZE_MISMATCH", OtaState::VerifyFailed, LogLevel::Error);
    return false;
  }
  uint8_t digest[32];
  sha256Finish(&shaCtx_, digest);
  char actualHex[65];
  sha256ToHex(digest, actualHex);
  if (!secureEquals(actualHex, pendingRelease_.sha256Hex)) {
    abortDownload("OTA_HASH_MISMATCH", OtaState::VerifyFailed, LogLevel::Error);
    return false;
  }

  const InstallDecision decision = installer_.finishInstall();
  if (decision != InstallDecision::Proceed) {
    installer_.abortInstall();
    scheduleFailure(OtaState::VerifyFailed, "OTA_INSTALL_REJECTED",
                    LogLevel::Error);
    return false;
  }

  state_ = OtaState::RebootPending;
  logger_.eventf(LogLevel::Info, "OTA_APPLIED_REBOOT_PENDING", "version=%s",
                 pendingVersion_);
  if (reboot_) reboot_();
  return true;
}

void OtaManager::abortDownload(const char* event, OtaState failState,
                               LogLevel level) {
  if (readerOpened_) {
    reader_.close();
    readerOpened_ = false;
  }
  installer_.abortInstall();
  resetDownload();
  scheduleFailure(failState, event, level);
}

void OtaManager::tick() {
  switch (state_) {
    case OtaState::RebootPending:
      return;
    default:
      break;
  }

  // Progresiva: un chunk por tick cuando estamos en Downloading.
  if (state_ == OtaState::Downloading) {
    if (!readerOpened_) {
      if (!startDownload()) return;
    }
    if (!pumpChunk()) return;
    if (downloadReceived_ >= pendingRelease_.totalSize) {
      if (!readerOpened_) return;
      reader_.close();
      readerOpened_ = false;
      finishDownload();
    }
    return;
  }

  const uint32_t now = clock_.monotonicMs();
  if (now < nextCheckMonotonicMs_) return;
  nextCheckMonotonicMs_ =
      now + static_cast<uint64_t>(tuning_.checkIntervalS) * 1000ULL;
  state_ = OtaState::Checking;

  runCheck();

  if (state_ == OtaState::Downloading) {
    startDownload();
  }
}

}  // namespace cauce::app

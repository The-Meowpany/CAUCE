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
  for (const char* p = text; ; ++p) {
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
    case OtaState::Idle:
      return "IDLE";
    case OtaState::Checking:
      return "CHECKING";
    case OtaState::UpToDate:
      return "UP_TO_DATE";
    case OtaState::Downloading:
      return "DOWNLOADING";
    case OtaState::RebootPending:
      return "REBOOT_PENDING";
    case OtaState::CheckFailed:
      return "CHECK_FAILED";
    case OtaState::VerifyFailed:
      return "VERIFY_FAILED";
    case OtaState::InstallFailed:
      return "INSTALL_FAILED";
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

void OtaManager::setRebootHook(RebootFn reboot) { reboot_ = reboot; }

void OtaManager::setMinFreeHeapBytes(uint32_t minBytes) {
  tuning_.minFreeHeapBytes = minBytes;
}

void OtaManager::setMinBatteryV(float minVolts) {
  tuning_.minBatteryV = minVolts;
}

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
      clock_.monotonicMs() + static_cast<uint64_t>(tuning_.checkIntervalS) * 1000ULL;
}

void OtaManager::runCheck() {
  OtaRelease& release = pendingRelease_;
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
  state_ = OtaState::Downloading;
}

bool OtaManager::runDownload(const OtaRelease& release) {
  if (!installer_.beginInstall(release.totalSize)) {
    scheduleFailure(OtaState::InstallFailed, "OTA_INSTALL_BEGIN_FAILED",
                    LogLevel::Error);
    return false;
  }

  Sha256Ctx ctx;
  sha256Begin(&ctx);

  if (!reader_.open(release.url)) {
    installer_.abortInstall();
    scheduleFailure(OtaState::InstallFailed, "OTA_DOWNLOAD_OPEN_FAILED",
                    LogLevel::Error);
    return false;
  }

  uint8_t chunk[512];
  size_t received = 0;
  while (true) {
    const size_t n = reader_.read(chunk, sizeof(chunk));
    if (n == 0) break;
    if (received + n > release.totalSize) {
      reader_.close();
      installer_.abortInstall();
      scheduleFailure(OtaState::VerifyFailed, "OTA_SIZE_EXCEEDED",
                      LogLevel::Error);
      return false;
    }
    sha256Append(&ctx, chunk, n);
    if (!installer_.writeChunk(chunk, n)) {
      reader_.close();
      installer_.abortInstall();
      scheduleFailure(OtaState::InstallFailed, "OTA_WRITE_FAILED",
                      LogLevel::Error);
      return false;
    }
    received += n;
  }
  reader_.close();

  uint8_t digest[32];
  sha256Finish(&ctx, digest);
  char actualHex[65];
  sha256ToHex(digest, actualHex);

  if (received != release.totalSize ||
      !secureEquals(actualHex, release.sha256Hex)) {
    installer_.abortInstall();
    scheduleFailure(OtaState::VerifyFailed, "OTA_HASH_MISMATCH",
                    LogLevel::Error);
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

void OtaManager::tick() {
  switch (state_) {
    case OtaState::RebootPending:
    case OtaState::Downloading:
      return;
    default:
      break;
  }

  const uint32_t now = clock_.monotonicMs();
  if (state_ != OtaState::Idle && state_ != OtaState::UpToDate &&
      state_ != OtaState::CheckFailed && state_ != OtaState::VerifyFailed &&
      state_ != OtaState::InstallFailed)
    return;

  if (now < nextCheckMonotonicMs_) return;
  nextCheckMonotonicMs_ =
      now + static_cast<uint64_t>(tuning_.checkIntervalS) * 1000ULL;
  state_ = OtaState::Checking;
  pendingRelease_ = OtaRelease{};

  runCheck();

  if (state_ == OtaState::Downloading) runDownload(pendingRelease_);
}

}  // namespace cauce::app

#include <unity.h>

void registerValidationTests();
void registerCodecTests();
void registerStorageTests();
void registerConfigTests();
void registerSecurityTests();
void registerBme280Tests();
void registerSchedulerTests();
void registerExportTests();
void registerMetricsTests();
void registerNetworkTests();
void registerApiTests();
void registerSyncTests();
void registerSha512Tests();
void registerReplicationTests();
void registerEd25519PointsTests();
void registerEd25519GroupTests();
void registerOtaTests();
void registerOtaRollbackTests();
void registerOtaBootConfirmTests();
void registerLoRaTests();
void registerLoRaBatchTests();
void registerDiagnosticsTests();
void registerDeepSleepTests();
void registerCommandTests();
void registerTextBufferTests();

int main() {
  UNITY_BEGIN();
  registerValidationTests();
  registerCodecTests();
  registerStorageTests();
  registerConfigTests();
  registerSecurityTests();
  registerBme280Tests();
  registerSchedulerTests();
  registerExportTests();
  registerMetricsTests();
  registerNetworkTests();
  registerApiTests();
  registerSyncTests();
  registerSha512Tests();
  registerReplicationTests();
  registerEd25519PointsTests();
  registerEd25519GroupTests();
  registerOtaTests();
  registerOtaRollbackTests();
  registerOtaBootConfirmTests();
  registerLoRaTests();
  registerLoRaBatchTests();
  registerDiagnosticsTests();
  registerDeepSleepTests();
  registerCommandTests();
  registerTextBufferTests();
  return UNITY_END();
}

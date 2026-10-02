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
void registerOtaTests();
void registerOtaRollbackTests();
void registerOtaBootConfirmTests();
void registerLoRaTests();
void registerDiagnosticsTests();
void registerDeepSleepTests();
void registerCommandTests();

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
  registerOtaTests();
  registerOtaRollbackTests();
  registerOtaBootConfirmTests();
  registerLoRaTests();
  registerDiagnosticsTests();
  registerDeepSleepTests();
  registerCommandTests();
  return UNITY_END();
}

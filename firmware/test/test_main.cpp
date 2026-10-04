#include <unity.h>

#include <cstdio>

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
void registerNodeActuatorTests();
void registerSx1276Tests();
void registerTextBufferTests();
void registerSuiteIsolationTests();
void printIsolationReport();
unsigned isolationClashCount();

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
  registerNodeActuatorTests();
  registerSx1276Tests();
  registerTextBufferTests();
  registerSuiteIsolationTests();
  printIsolationReport();
  // A declared directory clash means two suites share a resource and run order
  // decides which one sees the other's leftovers. Reported above, failed here,
  // because a report nobody fails on is a convention. See test_suite_isolation.cpp.
  if (isolationClashCount() != 0) {
    printf("suite isolation: %u directory clash(es) declared\n", isolationClashCount());
    return 1;
  }
  return UNITY_END();
}

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
void registerNodeAuthTests();
void registerNodeAuthWireTests();
namespace cauce { namespace app { void registerSyncTransportAuthTests(); } }
namespace cauce { void registerBootDiagnosticsTests(); }
namespace cauce { namespace hal { void registerEsp32PeerLinkTests(); } }
namespace cauce { void registerNodeCredentialTests(); }
namespace cauce { void registerManifestKeyTests(); }
namespace cauce { void registerStorageContainsTests(); }
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
void TEST_SUITE_MUTATES(const char* suite, const char* what);
void TEST_SUITE_CLAIMS_DIRECTORY(const char* suite, const char* directory);

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
  registerNodeAuthTests();
  registerNodeAuthWireTests();
  cauce::app::registerSyncTransportAuthTests();
  cauce::registerBootDiagnosticsTests();
  cauce::hal::registerEsp32PeerLinkTests();
  cauce::registerNodeCredentialTests();
  cauce::registerManifestKeyTests();
  cauce::registerStorageContainsTests();
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
  // Declared here rather than inside each suite's own register*Tests(), because the point is
  // that the declaration is visible *before* any suite runs: a reader of the run order sees
  // every piece of shared state the whole binary touches, not just the ones whose author
  // remembered. STATUS.md claimed these were adopted while nothing called the function; the
  // claim was true the moment this line was added and false the moment before it.
  TEST_SUITE_MUTATES("test_commands.cpp", "g_calls, g_lastPayload");
  TEST_SUITE_MUTATES("test_ota.cpp", "g_feedCalls, g_feedsBeforeFetch, g_feedsBeforeOpen");
  TEST_SUITE_MUTATES("test_sync.cpp", "g_downlinkCalls, g_downlinkDetail");
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

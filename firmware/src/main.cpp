#include <cstdio>

#include "cauce/app/DeepSleepController.h"
#include "cauce/app/MeasurementScheduler.h"
#include "cauce/core/BootDiagnostics.h"
#include "cauce/core/DataExporter.h"
#include "cauce/core/LogStorageRepository.h"
#include "cauce/core/Logger.h"
#include "cauce/core/Metrics.h"
#include "cauce/core/Types.h"
#include "cauce/drivers/Bme280Driver.h"
#include "cauce/drivers/SimulatedSensorDriver.h"
#include "cauce/app/Esp32Sleeper.h"
#include "cauce/hal/MemoryFileSystem.h"

#ifdef ARDUINO
#ifdef ARDUINO_ARCH_ESP32

#include <Arduino.h>
#include <WebServer.h>
#include <esp_task_wdt.h>
#include <time.h>

#include "cauce/app/ApiRouter.h"
#include "cauce/app/CommandExecutor.h"
#include "cauce/core/NodeActuator.h"
#include "cauce/app/Esp32ApiServer.h"
#include "cauce/app/Esp32ChallengeSource.h"
#include "cauce/app/CertificateVerifier.h"
#include "cauce/app/NodeCertificateAuth.h"
#include "cauce/app/Esp32CaptivePortal.h"
#include "cauce/app/Esp32Ota.h"
#include "cauce/app/Esp32OtaControl.h"
#include "cauce/app/NetworkManager.h"
#include "cauce/app/OtaBootConfirm.h"
#include "cauce/app/OtaManager.h"
#include "cauce/app/SyncManager.h"
#include "cauce/hal/Esp32HttpSyncTransport.h"
#include "cauce/hal/Esp32PeerLink.h"
#include "cauce/hal/Esp32WifiController.h"
#include "cauce/app/Esp32PeerExchange.h"
#include "cauce/app/PeerWatermarks.h"
#include "cauce/app/SerialLogSink.h"
#include "cauce/core/ConfigManager.h"
#include "cauce/core/SecurityUtils.h"
#include "cauce/hal/Esp32Hal.h"

// Survives a watchdog reset, not a power cycle - which is the right trade, because the
// failure being chased is a reset loop and a power cycle is an operator action that produces a
// clean boot anyway. 64 bytes rather than the 28 the record needs, so a future field fits
// without a layout change that would invalidate every record already on a board.
RTC_NOINIT_ATTR static uint8_t g_bootRtc[64] __attribute__((aligned(4)));
static cauce::BootDiagnostics g_bootDiag{};

static cauce::hal::Esp32LittleFs g_fs;
static cauce::hal::Esp32Clock g_clock;
static cauce::hal::Esp32WireBus* g_bus = nullptr;
static cauce::app::SerialLogSink* g_sink = nullptr;
static cauce::Logger* g_logger = nullptr;
static cauce::ValidationEngine* g_validator = nullptr;
static cauce::LogStorageRepository* g_store = nullptr;
static cauce::app::MeasurementScheduler* g_scheduler = nullptr;
static cauce::drivers::Bme280Driver* g_bme = nullptr;
static cauce::ConfigManager* g_configManager = nullptr;
static cauce::app::SystemHealth g_health{};
static cauce::app::DeepSleepController g_sleepController{};
static size_t g_lastStoredSeen = 0;
static uint64_t g_lastMeasurementMs = 0;
static void maybeDeepSleep();
static void confirmBootIfPending(uint32_t measurementCount);
static cauce::app::ApiRouter* g_apiRouter = nullptr;
static WebServer* g_webServer = nullptr;
static cauce::app::Esp32ApiServer* g_apiServer = nullptr;
static cauce::hal::Esp32HttpSyncTransport* g_syncTransport = nullptr;
// Certificate authentication. The authenticator owns the credential; the bridge is what the
// transport can call, and the challenge source is the only thing that speaks HTTP to the
// central for it.
static cauce::app::NodeAuthenticator* g_nodeAuth = nullptr;
static cauce::app::NodeCertificateAuth* g_certAuth = nullptr;
static cauce::app::Esp32ChallengeSource* g_challengeSource = nullptr;
// Certificate verification. The node checks that its own certificate was signed by the CA and
// names this node, rather than presenting whatever JSON it was configured with.
static cauce::app::CertificateVerifier* g_certVerifier = nullptr;
// Peer-to-peer. Both objects are held for the node's life; the exchange is enabled by
// `peer_enabled`, and an absent radio or discovery leaves it disabled rather than failing.
static cauce::hal::Esp32PeerRadio* g_peerRadio = nullptr;
static cauce::hal::Esp32PeerDiscovery* g_peerDiscovery = nullptr;
static cauce::app::PeerWatermarks* g_peerMarks = nullptr;
static cauce::app::Esp32PeerExchange* g_peerExchange = nullptr;
// LoRa. The forwarding gateway path, separate from the HTTP one: a node that cannot reach the
// central directly relays through a peer with a radio.
// LoRa is NOT constructed here. `Sx1276Radio` needs an `ISpiBus` and an `IRadioControl`, and
// neither has an ESP32 implementation - so the driver, the frame format, fragmentation, the
// acknowledgement and the forwarding loop are all real and all unreachable from a node.
// Naming that gap precisely is more useful than a commented-out construction that looks like
// a decision rather than an omission.
static cauce::app::SyncManager* g_syncManager = nullptr;
static cauce::app::Esp32CaptivePortal* g_portal = nullptr;
static cauce::hal::Esp32WifiController* g_netController = nullptr;
static cauce::app::NetworkManager* g_networkManager = nullptr;
static cauce::app::Esp32ManifestSource* g_otaCatalog = nullptr;
static cauce::app::Esp32FirmwareReader* g_otaReader = nullptr;
static cauce::app::Esp32FirmwareInstaller* g_otaInstaller = nullptr;
static cauce::app::OtaManager* g_otaManager = nullptr;
static cauce::app::Esp32OtaControl* g_otaControl = nullptr;
static cauce::app::CommandExecutor* g_commands = nullptr;
static cauce::NodeActuator* g_actuator = nullptr;
static cauce::app::OtaBootConfirm* g_bootConfirm = nullptr;
static bool g_ntpConfigured = false;
static cauce::NodeConfig g_activeConfig{};

namespace {

uint32_t freeHeapBytes() { return ESP.getFreeHeap(); }

void espRestartNow() { ESP.restart(); }

// Feeds the task watchdog on behalf of the OTA manager. The return value is dropped on
// purpose: there is nothing useful to do about a failed feed other than continue, and a
// failure here means the window was missed, which the reset will report on the next boot.
void feedOtaWatchdog() { (void)esp_task_wdt_reset(); }

// Reads an unsigned integer out of a small JSON payload without pulling in a
// parser. Downlink commands are flat objects written by our own central, so a
// full parser would be more dependency than the job needs.
bool jsonUintField(const char* json, const char* field, uint32_t& out) {
  if (json == nullptr || field == nullptr) return false;
  char needle[48];
  const int n = snprintf(needle, sizeof(needle), "\"%s\":", field);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(needle)) return false;
  const char* found = strstr(json, needle);
  if (found == nullptr) return false;
  found += n;
  if (*found < '0' || *found > '9') return false;
  out = static_cast<uint32_t>(strtoul(found, nullptr, 10));
  return true;
}

// Actuating handlers.
//
// These used to validate and report without changing anything, on the grounds
// that a command able to stop a node reporting is a command nobody should be able
// to send by accident. That worry was right and the response was wrong: the answer
// is bounds plus an explicit refusal, not inaction. NodeActuator owns both, and
// what reaches the central is the value that was APPLIED, so a command clamped or
// refused is visible to the operator instead of looking successful.
bool cmdSetSamplingInterval(const char* payload, char* detail, size_t cap) {
  uint32_t seconds = 0;
  if (!jsonUintField(payload, "seconds", seconds)) {
    snprintf(detail, cap, "refused:seconds_missing");
    return false;
  }
  return g_actuator != nullptr &&
         g_actuator->setSamplingInterval(seconds, detail, cap);
}

bool cmdSetSyncInterval(const char* payload, char* detail, size_t cap) {
  uint32_t seconds = 0;
  if (!jsonUintField(payload, "seconds", seconds)) {
    snprintf(detail, cap, "refused:seconds_missing");
    return false;
  }
  return g_actuator != nullptr &&
         g_actuator->setSyncInterval(seconds, detail, cap);
}

bool cmdRequestResync(const char* payload, char* detail, size_t cap) {
  (void)payload;
  return g_actuator != nullptr &&
         g_actuator->requestResync(millis(), detail, cap);
}

bool cmdSetLedMode(const char* payload, char* detail, size_t cap) {
  uint32_t mode = 0;
  if (!jsonUintField(payload, "mode", mode)) {
    snprintf(detail, cap, "refused:mode_missing");
    return false;
  }
  return g_actuator != nullptr && g_actuator->setLedMode(mode, detail, cap);
}

}  // namespace


// WHY THE WATCHDOG IS FED HERE AND NOT ONLY IN loop()
//
// The board rebooted every ~9 s with rst:0x8 (TG1WDT_SYS_RESET) and the last thing in
// the log was CONFIG_LOADED - never BOOT_COMPLETE. TG1 is what esp_task_wdt uses, so that
// is the task watchdog, and the loop task is the one subscribed: on the Arduino core
// setup() and loop() both run on loopTask, so esp_task_wdt_add(NULL) inside setup()
// subscribes loopTask, not the idle task. That was worth ruling out, because an
// idle-task watchdog would trip for a different reason entirely.
//
// loop() feeds it on its first statement, so a hang inside loop() cannot be the cause.
// Nothing in setup() fed it, though, and setup() does the flash mount, the store open,
// Wi-Fi init, a web server and the OTA bookkeeping in one pass. Thirty seconds for all of
// that is not obviously generous on a cold mount, and when it is exceeded the board
// resets before BOOT_COMPLETE - which is exactly the signature observed, and exactly why
// nobody could tell which step was slow.
//
// So every step below announces itself, feeds the watchdog on entry and exit, and reports
// its own duration. A hang inside a step still resets after 30 s, but the log now names
// the step, which is what the previous code could not do.
namespace {

struct SetupStepScope {
  const char* name;
  uint32_t startedMs;
  uint16_t ordinal;

  explicit SetupStepScope(const char* stepName)
      : name(stepName), startedMs(millis()), ordinal(g_bootDiag.beginStep(stepName)) {
    esp_task_wdt_reset();
    Serial.printf("SETUP_STEP %s begin\n", name);
    Serial.flush();
  }

  ~SetupStepScope() {
    esp_task_wdt_reset();
    g_bootDiag.endStep(ordinal, true);
    Serial.printf("SETUP_STEP %s end ms=%lu\n", name,
                  static_cast<unsigned long>(millis() - startedMs));
    Serial.flush();
  }
};

}  // namespace

// The record's lines go to the serial log through the core's own reporter, so `cauce_core`
// stays free of `Arduino.h`. `Serial.printf` would be one line here and an `#include
// <Arduino.h>` in a portable component that has 13 host tests against it.
void reportBootSerialLine(void* context, const char* line) {
  (void)context;
  Serial.printf("%s\n", line);
  Serial.flush();
}

// Two levels of indirection because `##` suppresses expansion of its neighbour: pasting
// __LINE__ directly yields a variable literally named cauce_step___LINE__ and every
// invocation collides with the first.
#define CAUCE_STEP_CONCAT_INNER(a, b) a##b
#define CAUCE_STEP_CONCAT(a, b) CAUCE_STEP_CONCAT_INNER(a, b)
#define CAUCE_SETUP_STEP(name) \
  SetupStepScope CAUCE_STEP_CONCAT(cauce_step_, __LINE__)(name)

// The board's reset reason, in the form the record stores.
//
// `esp_reset_reason()` is called after the RTC read on purpose: reading it first would be fine
// on this boot, but the value belongs to *this* boot while the record describes the previous
// one, and reading it early is how the two get confused.
static uint32_t currentResetReason() {
  return static_cast<uint32_t>(esp_reset_reason());
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // Read the previous boot's record before anything else, and print it while the reason is
  // still worth printing. Every line after this one competes for the same 115200 baud.
  g_bootDiag.begin(g_bootRtc, sizeof(g_bootRtc), &reportBootSerialLine, nullptr);

  // THE WATCHDOG, AND WHY THE RETURN VALUES ARE NOW CHECKED
  //
  // `esp_task_wdt_init(30, true)` does not do what it looks like. The Arduino core
  // initialises the task watchdog itself in `initArduino()` and subscribes loopTask, so this
  // call returns ESP_ERR_INVALID_STATE and the timeout stays at whatever the core set. There
  // is no `esp_task_wdt_reconfigure` in the IDF this core ships (4.4), so the timeout cannot be
  // changed afterwards at all - only `esp_task_wdt_deinit()` and `init()` again.
  //
  // The effective timeout is therefore CONFIG_ESP_TASK_WDT_TIMEOUT_S = 5 seconds, not 30. The
  // original code assumed 30, which is the worst combination: it looked generous and was not.
  //
  // Deinit-then-init is the only way to actually get 30 seconds, and it is attempted - but a
  // failure is *reported* rather than assumed away, because the difference between "the
  // watchdog is 30 s" and "the watchdog is 5 s and we are running with 5 s" is exactly the
  // kind of thing an operator needs to be told rather than to assume.
  const esp_err_t wdtDeinit = esp_task_wdt_deinit();
  esp_err_t wdtInit = wdtDeinit == ESP_OK ? esp_task_wdt_init(30, true) : ESP_FAIL;
  esp_err_t wdtAdd = ESP_OK;
  if (wdtInit == ESP_OK) {
    wdtAdd = esp_task_wdt_add(NULL);
    if (wdtAdd != ESP_OK) {
      // Already subscribed by the core. That is fine: the subscription we want already exists,
      // and re-adding it is what returned the error.
      wdtAdd = ESP_OK;
    }
  }
  Serial.printf(
      "WDT init=%d add=%d timeout_s=%u note=%s\n",
      static_cast<int>(wdtInit), static_cast<int>(wdtAdd), 30u,
      (wdtInit == ESP_OK)
          ? "30s applied"
          : "FELL BACK to the core timeout (5s); a slow setup step can reset the board");
  Serial.flush();

  {
    CAUCE_SETUP_STEP("mount_fs");
    if (!g_fs.mount()) {
      Serial.println("ERROR STORAGE_MOUNT_FAILED fs=littlefs");
      while (true) delay(1000);
    }
  }
  g_bus = new cauce::hal::Esp32WireBus(21, 22, 100000);
  g_sink = new cauce::app::SerialLogSink(Serial);
  g_logger = new cauce::Logger(*g_sink);
  g_validator = new cauce::ValidationEngine(cauce::defaultThresholds());
  // Declared at function scope, not inside the config step: every later step reads
  // config.nodeId, and a block-scoped copy would be a wall of "'config' was not declared".
  cauce::NodeConfig config;

  {
    CAUCE_SETUP_STEP("open_store");
    g_store = new cauce::LogStorageRepository(g_fs, "/data", 64u * 1024u);
    g_configManager = new cauce::ConfigManager(g_fs, "/config/cauce.conf");

    const auto status = g_configManager->load(config);
    g_activeConfig = config;
    g_store->open();
    g_logger->eventf(cauce::LogLevel::Info, "CONFIG_LOADED", "status=%d node=%s",
                     static_cast<int>(status), config.nodeId);

    const auto validation = cauce::ConfigManager::validate(config);
    if (!validation.ok) {
      g_logger->event(cauce::LogLevel::Warn,
                      "CONFIG_INVALID_USING_DEGRADED_DEFAULTS");
    }
  }

  // Rollback bookkeeping starts as early as possible: if the image is bad
  // enough to crash before the OTA wiring, the attempt still has to be
  // counted or the guard would never reach its limit.
  {
    CAUCE_SETUP_STEP("ota_bookkeeping");
    g_otaControl = new cauce::app::Esp32OtaControl();
    g_bootConfirm = new cauce::app::OtaBootConfirm(*g_otaControl, g_fs,
                                                    *g_logger, "/state/ota_boot");
    g_bootConfirm->loadAttempts();
    if (g_otaControl->isPendingVerify()) {
      g_logger->eventf(cauce::LogLevel::Warn, "OTA_BOOT_PENDING",
                       "attempt=%lu",
                       static_cast<unsigned long>(g_bootConfirm->attempts()));
    }
  }
  {
    CAUCE_SETUP_STEP("scheduler");
    g_scheduler = new cauce::app::MeasurementScheduler(
        g_clock, *g_store, *g_validator, *g_logger);
    g_scheduler->setNodeId(config.nodeId);
    g_scheduler->setSamplingInterval(config.samplingIntervalS);
    g_scheduler->setSequenceStart(g_store->lastSequence());

    g_health.nodeState = cauce::NodeState::Ready;
    g_health.storageRecords = g_store->totalRecords();
    g_health.storageBytes = g_store->totalBytes();
  }

  {
    CAUCE_SETUP_STEP("sensors");
    g_bus->begin();
    g_bme = new cauce::drivers::Bme280Driver(*g_bus, g_clock, "BME280-1");
    g_scheduler->addSensor(g_bme);
    g_scheduler->beginAllSensors();
  }

  g_apiRouter = new cauce::app::ApiRouter(*g_store, *g_configManager, g_clock,
                                          *g_logger, g_health);
  // WiFi mode is set here, before anything opens a socket, and not left to
  // NetworkManager later.
  //
  // WebServer::begin() on arduino-esp32 2.0.17 restarts the chip on hardware if the
  // radio has no mode yet: NetworkServer::begin() reaches for the default netif and
  // aborts rather than failing. It showed up as rst:0xc immediately after the route
  // registrations with 263 KB of free heap, so it was not memory, and with no
  // backtrace, so it was abort() and not a crash in our code.
  //
  // NetworkManager still sets up station and softAP; this only establishes the mode
  // early enough that opening a socket is legal.
  CAUCE_SETUP_STEP("web_server");
  WiFi.mode(WIFI_STA);
  g_webServer = new WebServer(80);
  g_apiServer = new cauce::app::Esp32ApiServer(*g_webServer, *g_apiRouter);
  g_apiServer->begin();
  g_portal = new cauce::app::Esp32CaptivePortal();
  g_portal->begin();

  {
    CAUCE_SETUP_STEP("sync_state");
    g_syncTransport = new cauce::hal::Esp32HttpSyncTransport();
    g_syncManager = new cauce::app::SyncManager(*g_store, *g_syncTransport,
                                                g_clock, *g_logger, g_fs,
                                                "/state/sync_state");
    g_syncManager->setNodeId(config.nodeId);
    g_syncManager->configureEndpoint(config.syncServerUrl, "");
    g_syncManager->setDeviceSecret(config.syncDeviceKey);
    g_syncManager->loadState();

    // Certificate authentication, when a seed *and* a certificate are both present.
    //
    // Both are required and the test is deliberately conjunctive. A seed alone cannot be used -
    // presenting a certificate header commits the central to certificate auth, and there is no
    // certificate to present - so a half-provisioned node is reported and left on HMAC rather
    // than half-migrated. The reverse (a certificate with no seed) is the same situation.
    //
    // A node with neither keeps the shared secret and is unaffected. That is the migration
    // path: re-provision with a seed and a certificate, and the next sync upgrades itself.
    const bool hasSeed = config.syncAuthSeedHex[0] != '\0';
    const bool hasCert = config.syncCertificate[0] != '\0';
    if (hasSeed != hasCert) {
      g_logger->event(cauce::LogLevel::Warn,
                      "NODE_AUTH_HALF_CONFIGURED falling back to the shared secret");
    } else if (hasSeed && hasCert) {
      uint8_t seed[32];
      if (!cauce::decodeSeedHex(config.syncAuthSeedHex, seed, sizeof(seed))) {
        // Unreachable: the parser refuses a malformed seed, so a config that loaded cannot
        // hold one. Checked anyway because the failure mode if it were wrong is a node that
        // signs with a key nobody knows, which fails as an invalid signature.
        g_logger->event(cauce::LogLevel::Error,
                        "NODE_AUTH_SEED_UNREADABLE falling back to the shared secret");
      } else {
        // Verify the certificate before it is presented. A node that cannot tell a CA-signed
        // certificate from an attacker's has no defence of its own - only the central's - and a
        // valid certificate naming a *different* node is exactly the case that passes there and
        // should not.
        g_certVerifier = new cauce::app::CertificateVerifier();
        g_certVerifier->pinCaPublicKey(config.caPublicKeyHex);
        const cauce::app::CertVerifyStatus verdict = g_certVerifier->verify(
            config.syncCertificate, config.nodeId, g_clock.utcTimeValid() ? g_clock.utcMs() : 0);
        if (verdict != cauce::app::CertVerifyStatus::Ok) {
          g_logger->eventf(cauce::LogLevel::Warn, "NODE_AUTH_CERT_REJECTED",
                           "reason=%s (%s)",
                           static_cast<int>(verdict),
                           cauce::app::describeCertVerify(verdict));
        } else {
          g_logger->eventf(cauce::LogLevel::Info, "NODE_AUTH_CERT_OK", "serial=%s",
                           g_certVerifier->serial());
        }
        g_nodeAuth = new cauce::app::NodeAuthenticator();
        g_nodeAuth->setCredential(seed, config.syncCertificate);
        g_challengeSource = new cauce::app::Esp32ChallengeSource();
        g_certAuth = new cauce::app::NodeCertificateAuth();
        g_certAuth->setAuthenticator(g_nodeAuth);
        g_certAuth->setChallengeSource(g_challengeSource);
        g_syncTransport->setAuthHeaderSource(g_certAuth);
        g_syncTransport->setNodeId(config.nodeId);
        g_logger->event(cauce::LogLevel::Info, "NODE_AUTH_CERTIFICATE enabled");
      }
    }
  }

  // Downlink. The central keeps offering a command until it sees its receipt,
  // so the executor has to remember applied ids in flash or the node would
  // re-run them on every poll.
  //
  // The actuator is loaded before the handlers are registered, because a handler
  // that arrives before it would have nothing to apply to and would report a
  // refusal for a reason the operator would not understand.
  g_actuator = new cauce::NodeActuator(g_fs, *g_logger, "/state/settings");
  g_actuator->load();
  g_commands = new cauce::app::CommandExecutor(g_fs, *g_logger,
                                               "/state/applied_commands");
  g_commands->loadApplied();
  g_commands->setHandler("set_sampling_interval", cmdSetSamplingInterval);
  g_commands->setHandler("set_sync_interval", cmdSetSyncInterval);
  g_commands->setHandler("request_resync", cmdRequestResync);
  g_commands->setHandler("set_led_mode", cmdSetLedMode);
  g_syncManager->setCommandExecutor(g_commands);

  // Peer-to-peer and LoRa, both behind their configuration flags.
  //
  // Both subsystems have existed for a while with tests and no caller - a driver, a frame
  // format, fragmentation, acknowledgement and a forwarding loop that no node ever reached.
  // A component with tests and no caller is not half-done; it is a component that exists only
  // in the test binary, and every test of it proves something about code no board runs.
  {
    CAUCE_SETUP_STEP("peer_exchange");
    g_peerMarks = new cauce::app::PeerWatermarks(g_fs, config.peerStatePath);
    g_peerMarks->load();
    if (config.peerEnabled) {
      g_peerRadio = new cauce::hal::Esp32PeerRadio();
      g_peerDiscovery = new cauce::hal::Esp32PeerDiscovery(config.peerChannel);
      const bool radioUp = g_peerRadio->begin(config.peerChannel);
      const bool discoveryUp = g_peerDiscovery->begin();
      g_peerExchange = new cauce::app::Esp32PeerExchange(*g_store, *g_peerMarks,
                                                         g_peerRadio, g_peerDiscovery);
      if (g_peerExchange->enable()) {
        g_logger->eventf(cauce::LogLevel::Info, "PEER_EXCHANGE_ENABLED",
                         "channel=%u peers_seen=%u", static_cast<unsigned>(config.peerChannel),
                         static_cast<unsigned>(g_peerDiscovery->poll(nullptr, 0)));
      } else {
        // Said at boot because a node configured for peer exchange and not getting it is the
        // exact failure that otherwise presents as "the feature does not work".
        g_logger->eventf(cauce::LogLevel::Warn, "PEER_EXCHANGE_UNAVAILABLE",
                         "radio=%d discovery=%d channel=%u", radioUp ? 1 : 0,
                         discoveryUp ? 1 : 0, static_cast<unsigned>(config.peerChannel));
      }
    }
  }


  {
    CAUCE_SETUP_STEP("wifi_controller");
    g_netController = new cauce::hal::Esp32WifiController();
    g_networkManager = new cauce::app::NetworkManager(
        *g_netController, g_clock, *g_logger, config, config.nodeId);
  }

  {
    CAUCE_SETUP_STEP("ota_manager");
    g_otaCatalog = new cauce::app::Esp32ManifestSource();
    g_otaCatalog->configure(config.otaManifestUrl, config.nodeId);
    g_otaReader = new cauce::app::Esp32FirmwareReader();
    g_otaInstaller = new cauce::app::Esp32FirmwareInstaller();
    g_otaManager = new cauce::app::OtaManager(*g_otaCatalog, *g_otaReader,
                                              *g_otaInstaller, g_clock,
                                              *g_logger);
    g_otaManager->setFirmwareVersion(cauce::Versions::kFirmware);
    g_otaManager->setSafetyHooks(&freeHeapBytes, nullptr);
    g_otaManager->setRebootHook(&espRestartNow);
// A named wrapper rather than the function itself: `esp_task_wdt_reset` returns esp_err_t,
// and a function pointer is not convertible to `void(*)()` regardless of the caller
// ignoring the result. The wrapper says the return value is deliberately discarded.
g_otaManager->setFeedHook(&feedOtaWatchdog);
    if (config.syncDeviceKey[0] != '\0') {
      // `otaManifestKey` when configured, `syncDeviceKey` otherwise.
      //
      // The fallback is what the previous version did unconditionally, and it is the coupling
      // this field exists to remove: the secret that authorises a firmware image was the same
      // secret every measurement arrived under. It is kept, because a node provisioned before
      // this existed has nothing else, and failing closed here would strand every deployed node
      // on its next update - trading a real exposure for a guaranteed outage.
      //
      // Reported once, loudly, because a deployment that has been authorising firmware with
      // its data secret is exactly the one that needs to hear about it.
      const char* manifestSecret =
          config.otaManifestKey[0] != '\0' ? config.otaManifestKey : config.syncDeviceKey;
      if (config.otaManifestKey[0] == '\0') {
        g_logger->event(cauce::LogLevel::Warn,
                        "OTA_KEY_SHARED_WITH_DATA_SECRET set ota_manifest_key to separate "
                        "the update channel");
      }
      uint8_t manifestKey[32];
      cauce::sha256(reinterpret_cast<const uint8_t*>(manifestSecret),
                    std::strlen(manifestSecret), manifestKey);
      g_otaManager->setManifestKey(manifestKey);
    } else {
      // Said once, loudly, at boot. The manager will refuse every update with
      // OTA_NO_MANIFEST_KEY, and an operator who sees that in the field should be able to
      // find out here why, rather than discovering it as a node that never updates.
      g_logger->event(cauce::LogLevel::Warn,
                      "OTA_UNSIGNED_NO_DEVICE_KEY updates will be refused");
    }
  }

  esp_task_wdt_reset();
  // Marks this boot as having finished, so the *next* boot will not report it as a re-attempt.
  // Written after the log line: if the reset happens between the two, the honest record is
  // "did not complete", and this boot genuinely did not.
  g_bootDiag.complete(currentResetReason());
  g_logger->eventf(cauce::LogLevel::Info, "BOOT_COMPLETE",
                   "firmware=%s node=%s api=80", cauce::Versions::kFirmware,
                   config.nodeId);
}

// A factory check reachable from a serial monitor while the node is already running.
//
// Why this exists at all, given `bench_main.cpp` already has a self-test: the failing case is
// a board that *will not boot*. A separate firmware for the factory means the factory needs a
// second flash step and a second bootloader state, and the checks a line operator wants -
// does flash work, does the config round-trip, does storage survive a reopen - are all
// reachable from a node that booted. What genuinely needs the bench image is the sensor, the
// radio and the signed sync, and those are the ones that stay there.
//
// Deliberately not free-running: waiting for a character costs nothing and printing 2000
// records nobody asked for costs the watchdog's whole margin on a board that is also trying to
// serve HTTP.
void maybeRunSerialFactoryCheck() {
  if (Serial.available() <= 0) return;
  const char c = static_cast<char>(Serial.read());
  if (c != 't') return;

  Serial.println("\n=== CAUCE factory check ===");
  int failures = 0;
  int skipped = 0;
  char detail[96];

  const auto say = [&](const char* what, bool ok, const char* text) {
    Serial.printf("%-30s %s  %s\n", what, ok ? "PASS" : "FAIL", text);
    if (!ok) ++failures;
  };
  const auto skip = [&](const char* what, const char* why) {
    Serial.printf("%-30s %s  %s\n", what, "SKIP", why);
    ++skipped;
  };

  // The sensor is asked for here and not deferred to the bench image, because a unit with a
  // dead BME280 is the single most common field failure and it is one I2C transaction.
  CAUCE_SETUP_STEP("factory_sensor");
  {
    cauce::hal::Esp32WireBus bus(21, 22, 100000);
    bus.begin();
    cauce::drivers::Bme280Driver bme(bus, g_clock, "BME280-FACTORY");
    const bool began = bme.begin();
    cauce::drivers::Reading reading;
    const bool read = bme.read(cauce::Variable::AirTemperature, reading);
    // An absent bus reports a clean read of zero, and "0.00" on a line screen reads like a
    // real number. The plausibility bound is in the assertion rather than left to the operator.
    const bool plausible = reading.value > -40.0f && reading.value < 85.0f;
    std::snprintf(detail, sizeof(detail), "begin=%d status=%d value=%.2f",
                  began ? 1 : 0, static_cast<int>(reading.status),
                  static_cast<double>(reading.value));
    say("sensor read", began && read && plausible, detail);
  }

  CAUCE_SETUP_STEP("factory_config");
  {
    cauce::NodeConfig probe;
    cauce::ConfigManager reader(g_fs, "/config/cauce.conf");
    const auto status = reader.load(probe);
    std::snprintf(detail, sizeof(detail), "status=%d node=%s", static_cast<int>(status),
                  probe.nodeId);
    if (probe.syncDeviceKey[0] == '\0') {
      skip("provisioned", "no sync_device_key");
    } else {
      say("provisioned", status == cauce::ConfigLoadStatus::Loaded, detail);
    }
  }

  CAUCE_SETUP_STEP("factory_storage");
  {
    // Its own directory. Writing into /data would interleave with real measurements and make a
    // failing check ambiguous about what it damaged.
    cauce::LogStorageRepository store(g_fs, "/factory_check", 64u * 1024u);
    const bool opened = store.open();
    cauce::Measurement record{};
    std::snprintf(record.nodeId, sizeof(record.nodeId), "FACTORY-CHECK");
    record.sequence = 1;
    record.timestampUtcMs = 1787356800000ULL;
    record.value = 21.5f;
    record.variable = cauce::Variable::AirTemperature;
    record.quality = cauce::Quality::Valid;
    const bool appended = opened && store.append(record);

    // Reopening is the part that matters: it is what a power cycle does, and it is where the
    // checkpoint and the scan run, which is where the stack overflow lived.
    cauce::LogStorageRepository reopened(g_fs, "/factory_check", 64u * 1024u);
    const bool okReopen = reopened.open();
    cauce::Measurement first{};
    cauce::QueryStats stats{};
    const uint32_t found = reopened.query(0, UINT64_MAX, 0, &first, 1, stats);
    std::snprintf(detail, sizeof(detail), "records=%u value=%.2f",
                  static_cast<unsigned>(reopened.totalRecords()),
                  static_cast<double>(first.value));
    say("storage round trip", appended && okReopen && found == 1 &&
                                first.value == record.value,
        detail);
  }

  Serial.printf("\nFACTORY_RESULT %s failures=%d skipped=%d\n",
                failures == 0 ? "PASS" : "FAIL", failures, skipped);
  Serial.flush();
}

void loop() {
  esp_task_wdt_reset();
  maybeRunSerialFactoryCheck();
  // Downlink actuation, applied at the top of the loop rather than inside the
  // handler. The command usually arrives in the same batch as the sync response,
  // so a change made during ingestion would be racing the code that reads it.
  if (g_actuator != nullptr) {
    // A resync request is honoured by resetting the retry backoff rather than by
    // poking the scheduler: the command usually arrives with a sync response that
    // already failed, and the reason it is still failing is a backoff that has not
    // expired.
    if (g_actuator->consumeResyncRequest()) {
      g_syncManager->requestSyncNow();
      g_logger->event(cauce::LogLevel::Info, "resync_requested_by_command");
    }
    if (g_actuator->isDirty()) {
      // Written once per change rather than once per loop: flash does not care
      // that the setting did not change again ten milliseconds later.
      if (g_actuator->persist()) {
        const auto& s = g_actuator->settings();
        g_scheduler->setSamplingInterval(s.samplingIntervalSeconds);
        g_syncManager->setSyncIntervalS(s.syncIntervalSeconds);
        g_logger->eventf(cauce::LogLevel::Info, "settings_applied",
                         "sampling_s=%u sync_s=%u led=%u",
                         static_cast<unsigned>(s.samplingIntervalSeconds),
                         static_cast<unsigned>(s.syncIntervalSeconds),
                         static_cast<unsigned>(s.ledMode));
      }
    }
  }
  g_networkManager->tick();
  g_health.netState = g_networkManager->state();
  g_health.rssiDbm = g_netController->rssiDbm();
  g_scheduler->tick();
  const auto& counters = g_scheduler->counters();
  g_health.uptimeMs = g_clock.monotonicMs();
  g_health.utcTimeValid = g_clock.utcTimeValid();
  g_health.measurementCount = counters.measurementCount;
  g_health.storedCount = counters.storedCount;
  g_health.invalidCount = counters.invalidCount;
  g_health.suspectCount = counters.suspectCount;
  g_health.readFailures = counters.readFailures;
  g_health.storageFailures = counters.storageFailures;
  g_health.lastSuccessUtcMs = counters.lastSuccessUtcMs;
  g_health.storageRecords = g_store->totalRecords();
  g_health.storageBytes = g_store->totalBytes();
  const bool linkUp =
      (g_health.netState == cauce::app::NetState::Connected ||
       g_health.netState == cauce::app::NetState::Degraded);
  if (linkUp) {
    if (!g_ntpConfigured) {
      configTime(static_cast<long>(g_activeConfig.timezoneOffsetMin) * 60L, 0,
                 g_activeConfig.ntpServer);
      g_ntpConfigured = true;
    }
    g_syncManager->onNetworkConnected();
    g_syncManager->tick();
  } else {
    g_syncManager->onNetworkLost();
  }
  g_otaManager->tick();
  confirmBootIfPending(counters.measurementCount);
  g_portal->begin();
  g_portal->processNextRequest();
  g_apiServer->handleClient();
  maybeDeepSleep();
}

// A freshly flashed image stays PENDING_VERIFY until the node proves it can
// work. Without this an image that crashes on the first scheduler tick would
// stay installed forever, because nothing would ever mark it valid or roll it
// back. Evidence is deliberately weak-but-real: storage is writable, nothing
// failed writing, and either a measurement landed or the grace window passed.
void confirmBootIfPending(uint32_t measurementCount) {
  if (g_bootConfirm == nullptr) return;
  if (!g_otaControl->isPendingVerify()) return;
  cauce::app::BootSelfTest selfTest;
  selfTest.storageWritable = true;
  selfTest.measurementsStored = measurementCount;
  selfTest.storageFailures = g_health.storageFailures;
  selfTest.uptimeMs = static_cast<uint32_t>(g_clock.monotonicMs());
  if (!g_bootConfirm->tick(selfTest)) {
    g_health.storageFailures += 1;
  }
}

void maybeDeepSleep() {
  if (!g_activeConfig.deepSleepEnabled) return;
  const cauce::app::OtaState ota = g_otaManager->state();
  if (ota == cauce::app::OtaState::Downloading ||
      ota == cauce::app::OtaState::Checking ||
      ota == cauce::app::OtaState::RebootPending) {
    return;
  }
  const bool linkUp =
      (g_health.netState == cauce::app::NetState::Connected ||
       g_health.netState == cauce::app::NetState::Degraded);
  const size_t stored = g_store->totalRecords();
  // Stamp the last *newly stored* measurement, not "a loop where data was
  // pending". Otherwise msSinceLastMeasurement would reset on every
  // iteration while records await ack and the node would never sleep.
  if (stored != g_lastStoredSeen) {
    g_lastStoredSeen = stored;
    g_lastMeasurementMs = g_clock.monotonicMs();
  }
  cauce::app::DeepSleepInputs in;
  in.enabled = true;
  in.samplingIntervalS = g_activeConfig.samplingIntervalS;
  in.syncIntervalS = g_activeConfig.syncIntervalS;
  in.batteryV = g_health.batteryVoltageV;
  in.networkConnected = linkUp;
  in.portalActive = g_portal->started();
  in.storageHasPendingSync = stored > g_syncManager->lastAckedSequence();
  in.msSinceLastMeasurement = g_clock.monotonicMs() - g_lastMeasurementMs;
  in.msSinceBoot = g_clock.monotonicMs();
  const cauce::app::SleepPlan plan = g_sleepController.evaluate(in);
  if (plan.decision != cauce::app::SleepDecision::Sleep) return;
  g_logger->eventf(cauce::LogLevel::Info, "DEEP_SLEEP_ENTER",
                   "seconds=%u reason=%s", plan.sleepS, plan.reason);
  cauce::app::Esp32Sleeper::enterDeepSleep(plan.sleepS);
}

#else
#error "CAUCE firmware targets ESP32 or host simulation"
#endif

#else

#include "cauce/app/StdoutLogSink.h"
#include "cauce/hal/ManualClock.h"

using namespace cauce;

namespace {

uint64_t g_sharedUtcMs = 1787356800000ULL;

int runHostSimulation() {
  hal::MemoryFileSystem fs;
  LogStorageRepository store(fs, "data", 4096);
  hal::ManualClock clock(g_sharedUtcMs);
  app::StdoutLogSink sink;
  Logger logger(sink);
  ValidationEngine validator(defaultThresholds());

  logger.eventf(LogLevel::Info, "BOOT", "firmware=%s protocol=%u hw=%s",
                Versions::kFirmware, Versions::kProtocol,
                Versions::kHardwareRevision);

  app::MeasurementScheduler scheduler(clock, store, validator, logger);
  scheduler.setNodeId("CAUCE-SIM");
  scheduler.setSamplingInterval(60);

  drivers::SimulationProfile profile;
  drivers::SimulatedSensorDriver sensor("SIM-A", profile, &g_sharedUtcMs);
  scheduler.addSensor(&sensor);

  store.open();
  scheduler.setSequenceStart(store.lastSequence());
  scheduler.beginAllSensors();

  for (int cycle = 1; cycle <= 48; ++cycle) {
    if (cycle == 20) {
      sensor.injectFault(drivers::SimulatedSensorDriver::Fault::Frozen);
      logger.eventf(LogLevel::Warn, "SCENARIO", "event=FROZEN_SENSOR cycle=%d",
                    cycle);
    }
    if (cycle == 32) {
      sensor.injectFault(drivers::SimulatedSensorDriver::Fault::OutOfRange);
      logger.eventf(LogLevel::Warn, "SCENARIO", "event=OUT_OF_RANGE cycle=%d",
                    cycle);
    }
    if (cycle == 34) {
      sensor.injectFault(drivers::SimulatedSensorDriver::Fault::Disconnect);
      logger.eventf(LogLevel::Warn, "SCENARIO", "event=DISCONNECTED cycle=%d",
                    cycle);
    }
    clock.advanceMs(60000);
    g_sharedUtcMs = clock.utcMs();
    sensor.setUtcMs(g_sharedUtcMs);
    scheduler.tick();
  }
  sensor.injectFault(drivers::SimulatedSensorDriver::Fault::None);
  clock.advanceMs(60000);
  g_sharedUtcMs = clock.utcMs();
  sensor.setUtcMs(g_sharedUtcMs);
  scheduler.tick();

  Measurement latest{};
  if (store.latest(latest)) {
    logger.eventf(LogLevel::Info, "LATEST_RECORD",
                  "seq=%lu var=%s value=%.2f q=%s",
                  static_cast<unsigned long>(latest.sequence),
                  variableName(latest.variable),
                  static_cast<double>(latest.value),
                  qualityName(latest.quality));
  }

  Measurement page[16];
  QueryStats stats{};
  const size_t n = store.query(0, UINT64_MAX, 0, page, 16, stats);
  logger.eventf(LogLevel::Info, "QUERY_SAMPLE", "matched=%lu returned=%lu",
                static_cast<unsigned long>(stats.matched),
                static_cast<unsigned long>(n));

  const uint32_t corrupted = store.integrityCheck();
  logger.eventf(LogLevel::Info, "INTEGRITY_CHECK", "corrupted_frames=%lu total_records=%lu bytes=%lu",
                static_cast<unsigned long>(corrupted),
                static_cast<unsigned long>(store.totalRecords()),
                static_cast<unsigned long>(store.totalBytes()));

  {
    Measurement window[64];
    QueryStats wstats{};
    const size_t gotRecords =
        store.query(0, UINT64_MAX, 0, window, 64, wstats);

    float temps[64];
    const uint32_t nTemps =
        extractVariableValues(window, static_cast<uint32_t>(gotRecords),
                              Variable::AirTemperature, temps, 64);
    SampleStats stats;
    float scratch[64];
    if (computeSampleStats(temps, nTemps, stats, scratch, 64)) {
      logger.eventf(LogLevel::Info, "STATS_AIR_TEMPERATURE",
                    "n=%lu mean=%.2f min=%.2f max=%.2f std=%.2f p95=%.2f",
                    static_cast<unsigned long>(stats.count),
                    static_cast<double>(stats.mean),
                    static_cast<double>(stats.minValue),
                    static_cast<double>(stats.maxValue),
                    static_cast<double>(stats.stdDev),
                    static_cast<double>(stats.p95));
    }

    AggregateBucket buckets[8];
    const uint32_t nBuckets = aggregateBuckets(
        window, static_cast<uint32_t>(gotRecords),
        Variable::AirTemperature, 300, buckets, 8);
    logger.eventf(LogLevel::Info, "AGGREGATION_5MIN", "buckets=%lu",
                  static_cast<unsigned long>(nBuckets));

    const double exposureH =
        exposureHoursAbove(window, static_cast<uint32_t>(gotRecords), 30.0f);
    logger.eventf(LogLevel::Info, "EXPOSURE_HOURS_ABOVE_30C", "hours=%.3f",
                  exposureH);
  }

  {
    ChunkedExporter exporter(store, ChunkedExporter::Format::Csv, 0,
                             UINT64_MAX);
    char csvChunk[384];
    size_t printed = 0;
    while (!exporter.done() && printed < 2) {
      const size_t n = exporter.next(csvChunk, sizeof(csvChunk));
      if (n == 0) break;
      char firstLine[192] = {0};
      size_t lineLen = 0;
      while (lineLen + 1 < n && csvChunk[lineLen] != '\n') ++lineLen;
      std::memcpy(firstLine, csvChunk, lineLen < sizeof(firstLine) - 1
                                           ? lineLen
                                           : sizeof(firstLine) - 1);
      logger.eventf(LogLevel::Info, "CSV_PREVIEW", "%s", firstLine);
      ++printed;
    }
  }

  store.applyRetentionPolicy(2048);
  logger.eventf(LogLevel::Info, "RETENTION_APPLIED",
                "bytes_after=%lu segments=%lu",
                static_cast<unsigned long>(store.totalBytes()),
                static_cast<unsigned long>(store.segments().size()));

  const auto& c = scheduler.counters();
  logger.eventf(LogLevel::Info, "SUMMARY",
                "measured=%lu stored=%lu invalid=%lu suspect=%lu read_failures=%lu",
                static_cast<unsigned long>(c.measurementCount),
                static_cast<unsigned long>(c.storedCount),
                static_cast<unsigned long>(c.invalidCount),
                static_cast<unsigned long>(c.suspectCount),
                static_cast<unsigned long>(c.readFailures));
  return 0;
}

}  // namespace

int main() { return runHostSimulation(); }

#endif

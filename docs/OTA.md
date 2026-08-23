# CAUCE OTA — firmware updates

## Architecture (decision layer implemented and tested)

```
OtaManager (FSM)                     IManifestSource  → release catalog
  IDLE ──interval──▶ CHECKING        IFirmwareReader  → chunked download
  CHECKING: catalog + semver         IFirmwareInstaller → write/commit
    ├─ nothing new ──▶ UP_TO_DATE    hooks: free heap, battery, reboot
    ├─ gates blocked ▶ CHECK_FAILED
    └─ newer version ▶ DOWNLOADING
         DOWNLOADING: begin→chunks(streaming sha256)→finish
           ├─ hash+size OK, installer confirms ▶ REBOOT_PENDING
           ├─ hash/size invalid ▶ VERIFY_FAILED (+abort)
           └─ write failure        ▶ INSTALL_FAILED (+abort)
```

## Anti-brick rules

1. **The running image is never touched**: the installer writes to the
   alternate partition; `finishInstall()` stands for
   `esp_ota_set_boot_partition` (marking the new image as candidate).
2. **Verification before reboot**: streaming SHA-256 over received bytes
   must match the manifest exactly; size must match too.
3. **Safety gates** before downloading: minimum free heap (default 40 KB)
   and minimum battery (`setMinBatteryV`; disabled at 0).
4. **Operational rollback**: after restarting, the node must confirm health
   (boot-counter / `esp_ota_mark_app_valid_cancel_rollback` pattern).
   Wiring that native mechanism is the board-pending part.
5. Missing manifest or equal/lower version → no download at all.

## Node API

```cpp
OtaManager mgr(catalog, reader, installer, clock, logger);
mgr.setFirmwareVersion(Versions::kFirmware);
mgr.setSafetyHooks(freeHeapFn, batteryFn);   // optional
mgr.setMinBatteryV(3.3f);
mgr.setRebootHook(esp_restart);
mgr.tick();                                   // non-blocking by design
```

## Manifest contract

`IManifestSource.fetchLatest(current)` returns `{version, sha256Hex, url,
totalSize}` or false. The central backend can serve it via
`GET /v1/ota/manifest` reading a releases file (the ESP32-side HTTP reader
is the board-pending piece).

## Tests (10, host)

Semver pairs · streaming ≡ one-shot sha256 · full happy path (single
catalog fetch) · corrupted hash abort · size exceeded · same version skip ·
no release · installer rejection · heap gate · battery gate.

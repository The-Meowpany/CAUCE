# CAUCE OTA — firmware updates

## Architecture (decision layer implemented and tested)

```
OtaManager (FSM)                     IManifestSource  → release catalog
  IDLE ──interval──▶ CHECKING        IFirmwareReader  → chunked download
  CHECKING: catalog + semver         IFirmwareInstaller → write/commit
    ├─ nothing new ──▶ UP_TO_DATE    hooks: free heap, battery, reboot
    ├─ gates blocked ▶ CHECK_FAILED
    └─ newer version ▶ DOWNLOADING
         DOWNLOADING: begin→chunks(sha256 streaming)→finish
           ├─ hash+size OK, installer confirms ▶ REBOOT_PENDING
           ├─ hash/size invalid ▶ VERIFY_FAILED (+abort)
           └─ write failure        ▶ INSTALL_FAILED (+abort)
```

Ten host tests cover this FSM, plus three for the manifest JSON parser.
The ESP32 flash backend is implemented too (`Esp32Ota`: HTTP manifest
fetch, streaming `Update` write to the alternate partition, reboot
hook) and cross-compiles — but no board has run it yet, so treat the
whole flashing path as unvalidated until the bench says otherwise.

## Anti-brick rules

1. **The running image is never touched.** The installer writes to the
   alternate partition; `finishInstall()` means
   `esp_ota_set_boot_partition` (marking the new image as candidate,
   not committing to it blindly).
2. **Verify before reboot.** The streaming SHA-256 over received bytes
   has to match the manifest exactly, and so does the size. One byte
   off, no reboot.
3. **Safety gates first.** Minimum free heap (default 40 KB) and
   minimum battery (`setMinBatteryV`; 0 disables the check) are
   evaluated before a single byte downloads.
4. **Prove health after restart.** The node must confirm it's alive
   (boot-counter / `esp_ota_mark_app_valid_cancel_rollback` pattern).
   Wiring that native mechanism is still board-pending — until a board
   confirms it, updates mean physical access in practice.
5. No manifest, or an equal/lower version, means no download at all.
   An empty `ota_manifest_url` keeps OTA dormant: the catalog reports
   no release and the manager stays quiet.

## Node API

```cpp
OtaManager mgr(catalog, reader, installer, clock, logger);
mgr.setFirmwareVersion(Versions::kFirmware);
mgr.setSafetyHooks(freeHeapFn, batteryFn);   // optional
mgr.setMinBatteryV(3.3f);
mgr.setRebootHook(esp_restart);
mgr.tick();                                   // non-blocking by design
```

`tick()` never blocks: a chunk per call, state kept in the manager.
On the node the dashboard stays up during a check — blocking downloads
are a known backlog item (STATUS.md #3), not a surprise.

## Manifest contract

`IManifestSource.fetchLatest(current)` returns `{version, sha256Hex,
url, totalSize}` or false. The central backend serves it over
`GET /v1/ota/manifest` from a releases JSON file (`CAUCE_OTA_RELEASES`):
`{version, sha256, url, total_size}` plus a per-node `hmac` when
`node_id` belongs to a provisioned device (HMAC over
`version|url|total_size` with the device key). Without a releases file
the endpoint answers 404 and nodes stay put.

On the node, `Esp32ManifestSource` fetches that URL (with `?node_id=`)
and parses it with the dependency-free `parseOtaManifestJson` — the
same parser the host tests cover, so the JSON handling is proven even
though flashing itself awaits hardware.

## Tests (13, host)

Semver pairs · streaming ≡ one-shot sha256 · full happy path (single
catalog fetch) · corrupted hash abort · size exceeded · same version
skip · no release · installer rejection · heap gate · battery gate.

# CAUCE real system status

This document is the source of truth about what is implemented and what is
not. It overrides any aspirational claim elsewhere.

## Implemented and verified (207 firmware + 234 backend tests, E2E green)
| Component | Evidence |
| **Device identity security**: admin-gated provisioning with a per-node HMAC key; batches signed over the raw body; OTA validates the manifest signature before downloading | 5 tests (firmware HMAC RFC4231 x2 + backend matrix valid/bad-signature/no-signature + device-secret signing x2) |
| **Token administration over HTTP**: `POST/GET /v1/tokens` and `GET/DELETE /v1/tokens/{name}`, shared-admin only, plaintext returned once and never stored, SHA-256 digest with scopes and optional site | 14 backend tests |
| **Retention breadth**: `agg_daily`, `alert_log`, `node_diagnostics`, `maintenance_events` and `sync_batches` purged alongside raw measurements; only `acked` commands age out, `pending`/`delivered` survive, orphaned receipts are cleaned; every counter reported | 8 backend tests |
| **Node-signed LoRa frames**: the node appends HMAC-SHA-256 over header, payload **and CRC** to every frame it transmits; the gateway holds no key and verifies nothing, relaying bytes verbatim; the central verifies each frame against the provisioned key. Restores A-8 for the relay path | 5 firmware tests (known-answer HMAC vector, unsigned/small-buffer refusal, signed == unsigned + 32 bytes, tampering, CRC coverage) + 19 backend tests |
| **Coverage resolution cap**: the window guard bounds implied sample count as well as span, so a 400-day window at 1 s is refused as `too_many_buckets` while two years at hourly is allowed and points at `agg_hourly`/`agg_daily` | 4 backend tests |
| **Central calibration**: `calibration` table per (site, variable) plus a `maintenance_events` log; raw stays intact and the calibrated value is derived on read; applied in summary, compare, period-compare, summary-fast, hourly, heat-events, before-after/DiD, colocation, report and CSV | 16 backend tests |
| **Long-window analytics**: `granularity=auto` reads `agg_hourly` past 7 days when coverage is sufficient and says so in the response; `raw` remains available for exact min/max | 6 backend tests |
| **Cursor pagination**: opaque `(timestamp_utc_ms, sequence)` cursor for measurements and `node_id`-based cursor for nodes, stable under concurrent inserts; `limit`/`offset` untouched | 6 backend tests |
| **Deep sleep**: portable `DeepSleepController` (9 tests) plus `Esp32Sleeper`, behind a `deep_sleep_enabled` flag that is **off by default** until bench power measurement | 9 firmware tests + 2 config tests |
| **Non-blocking OTA chunks**: tri-state `ReadStatus` reader, no 10 s per-chunk wait, `OTA_STALLED` with a tick budget | 3 firmware tests |
| **TLS in front of the central**: Caddy with an internal CA and an env-driven domain; central bound to loopback only | 1 backend test + ESP32 build |
| **Downlink commands**: `(node_id, idempotency_key)` unique, delivered in the same `/v1/sync` response that acks a batch, node remembers applied ids in flash and reports receipts | 11 backend tests + 12 firmware tests |
| **OTA rollback wired**: `OtaBootConfirm` persists a boot-attempt counter, marks the image valid once storage proves itself, rolls back after 3 bad boots | 8 firmware tests; ESP32 build SUCCESS |
| **Honest LoRa delivery**: `ILoRaRadio::receive()`, gateway ack frame with XOR check, stale-ack drain, unconfirmed batch reported as a retryable error instead of an optimistic ack | 9 firmware tests |
| **Monotonic sync watermark**: a late acknowledgement can no longer rewind `last_acked_sequence`, for either transport | covered by the LoRa and downlink suites |
| **LoRa compact frame**: 68-byte records packed per spreading factor (SF7 3/uplink, SF9 1/uplink), CRC16 per frame, reassembly that refuses foreign, duplicated or corrupt fragments; SF10 and below refused rather than truncated | 11 firmware tests + 13 backend tests |
| **Cross-language LoRa format check**: the exact bytes the C++ encoder produces are pinned as known-answer vectors and decoded by an independent Python implementation that also serves as the gateway | 13 backend tests |
| **Calibration uncertainty**: absolute uncertainty plus its kind, scaling with `|scale|`, `null` kept distinct from zero, surviving partial updates, exported to CSV | 8 backend tests |
| **Per-principal authorization**: `api_tokens` with read/write/admin scopes and an optional site, digests stored, constant-time comparison; the shared admin token still works untouched | 10 backend tests |
| **Daily aggregates**: `agg_daily` maintained by trigger from `agg_hourly`, `granularity=daily`, and `auto` switching to it past 120 days | 14 backend tests |
| **Gateway forwarding loop**: frames to reassembly to `POST /v1/sync` to acknowledgement, driven against the real app and verified by reading rows back out of SQLite; noise counted, failures never acknowledged | 11 backend tests |
| **Acknowledgement pinned cross-language**: the exact bytes the gateway builds are asserted by the firmware parser and vice versa, so the two cannot drift apart while each still passes its own tests | 1 firmware + 1 backend test |
| **Identifier validation**: `site_id` restricted to `[A-Za-z0-9_-]` and 64 chars at every write endpoint, closing a stored-markup vector that the JSON API used to echo back; `nosniff` on every non-HTML response | 10 backend tests |
| **Bounded text building**: `TextBuffer` replaces unguarded `snprintf` accumulation in `ApiRouter` and `SyncManager`, so `used` can no longer pass the capacity and underflow the remaining-size arithmetic | 8 firmware tests |
| **Data coverage accounting**: expected vs received, longest gap, gap reasons (`no_data` / `measured_not_delivered` / `clock_uncertain`), node + site + CSV | 8 backend tests |
| **Control sites and difference-in-differences** in `before-after`, with distance to the treated site and automatic exclusion of under-sampled controls | 2 backend tests + `haversine_m`/`difference_in_differences` unit-covered |
| **Fleet triage** (`/v1/fleet`): firmware spread, last sync, storage, flags, `needs_visit`; `/system` renders it in en/es/pt | 3 backend tests + dashboard render test |
| **Scheduled retention**: daily purge + throttled VACUUM, state persisted in `maintenance_state`, visible on `/system` | 2 backend tests |
| **Pilot-scale load rehearsal** (`simulator/load_pilot.py`): 8 nodes x N days, injected outages, optional site/control/intervention seeding, throughput and size report | run against a live central: 110k rows at 12.4k rows/s

|---|---|
| HAL interfaces `IClock`, `II2cBus`, `IFileSystem`, `INetworkController`, `ISyncTransport` | Compiles on host and ESP32; native doubles + ESP32 impls |
| Normalized `Measurement` model (node, sequence, timestamp, variable, value, quality, reason bits) | Codec roundtrip tests |
| Validation engine: physical range, non-finite, rate-of-change, stuck sensor, duplicate sequence, time uncertainty | 9 unit tests |
| Append-only segment storage `.clog` with per-record CRC32; corrupt tail sealed off; rotation + retention | 6 tests incl. crash recovery |
| Versioned config manager (KV), range validation, `.bak` rollback chain | 9 tests |
| Own SHA-256 against NIST vectors; constant-time compare; streaming API | 4 tests |
| Simulated driver with fault injection (frozen, NaN, out-of-range, disconnect) | Integration tests |
| BME280 reference driver over HAL I2C; Bosch integer compensation + independent float mirror | Datasheet-vector tests |
| Event-driven scheduler (`tick()`), MISSING placeholders, health counters | 5 integration tests incl. storage failure without crash |
| **CSV (RFC4180) + JSON array chunked exporters** | 6 tests |
| **Metrics**: mean/median/sample stddev/percentiles, window aggregation, trapezoidal exposure hours, quality-aware extraction | 8 tests |
| ISO-8601 UTC format/parse (civil algorithm, leap years) | Exact roundtrips |
| **Network FSM logic**: OFFLINE/WAITING_RETRY/CONNECTING/CONNECTED/DEGRADED/AP_FALLBACK, exponential backoff, connect timeout | 7 tests with scripted controller |
| **Embedded API v1**: all 8 `/api/v1` endpoints, paginated streaming, Bearer + SHA-256 constant-time auth, fail-closed, 422 error lists, secret masking | 12 host router tests |
| **ESP32 HTTP transport** (`Esp32ApiServer` on WebServer, chunked) wired in `main.cpp` | Cross-compile SUCCESS — no board yet |
| **Sync client**: persistent watermark, idempotent `node_id+sequence` batches, exponential backoff, halt-on-reject | 8 tests |
| **Central backend** (FastAPI+SQLite): `/v1/sync` idempotent ingestion with honest acks (now HMAC per-device + rate-limit SQLite-backed), nodes/measurements queries (now paginated + per-node CSV export), analytics summary/compare/before-after/period-compare/heat-events/summary-fast, optional token auth (constant-time), sites & interventions, time-reconstruct | 30 pytest tests |
| **Local web dashboard** (embedded ~10KB SPA, dependency-free canvas chart, quality cards, 1h–7d ranges, export, admin config) with ES/EN switch + **captive portal DNS** | 2 HTML integrity tests; DNS compile-verified |
| **Central mini-dashboard** with Accept-Language localization (en/es/pt) + full-history CSV export | Localization test |
| **Scenario simulator CLI**: normal_day / heat_event / sensor_failure / network_outage / power_loss_recovery, CSV or direct `/v1/sync` feed | Real smoke: 5 nodes loaded into live backend |
| **OTA decision layer**: semver compare, manifest validation, streaming SHA-256 verification, heap/battery gates, alternate-partition anti-brick rules | 13 tests (10 FSM + 3 manifest-JSON parser) |
| Node-to-server **E2E integration**: C++ SyncManager (real sockets) against live FastAPI; initial/incremental/idempotent-replay phases verified directly in SQLite | `scripts/run-e2e.ps1` — caught a real JSON serialization bug |
| CI workflow: native tests · ESP32 build · backend pytest · E2E job | `.github/workflows/ci.yml` |

## Exists but NOT yet validated on physical hardware

- Real BME280 reads via Wire (datasheet-driven, passes simulated-bus tests).
- LittleFS on real flash (mount/wear/power-cut).
- Wi-Fi radio (`Esp32WifiController` implementing the tested FSM interface).
- OTA actual flash write (`Esp32Ota` on `Update`, cross-compiled) + boot-counter confirmation (`OtaBootConfirm` wired in `main.cpp`). Both unrun on hardware.
  boot-counter rollback confirmation (both unrun on hardware).
- NTP time source.
- LoRa sync transport (`LoRaSyncTransport` behind `ILoRaRadio`). The acknowledgement path is host-tested; the radio itself is not.
- OTA rollback: two-slot partition table (`firmware/partitions.csv`),
  `OtaRollbackGuard` policy (mark valid / retry / roll back) and
  `Esp32OtaControl`. Policy is host-tested; the flash path is not.
- Field diagnostics bundle (`GET /api/v1/diagnostics`, signed) and its
  central ingest at `POST /v1/nodes/{id}/diagnostics`.

## Not implemented yet

- Volumetric 3D viewer: discarded by design decision - the central keeps
  the dependency-free 2D schematic SVG map with IDW field and shared-scale
  co-location overlays instead.
- Calibration **uncertainty**: records can carry an absolute uncertainty and
  the kind it came from, and it scales with the correction, so a report can
  separate measurement from method. There is still no formal calibration
  procedure and no metrological traceability.
- Coverage accounting caps a query at 400 days and reports the top 20
  gaps with `gaps_truncated` set when there are more, so a very long
  window is bounded rather than complete.
- TLS ships ACME by default (`CAUCE_TLS_MODE` empty). A public deployment
  still needs a real DNS name pointing at the host.
- Deep sleep is wired but **disabled by default**: turning it on requires
  the bench measurement in `docs/en/BENCH_PLAN.md`.
- The LoRa frame format, fragmentation, acknowledgement and the forwarding
  loop are written and tested end to end against the central, but there is
  no radio driver and no link budget: the air interface is still unproven.
- Downlink kinds validate and report but do not reconfigure the node; real
  actuation needs hardware that can be actuated.

## Known technical debt / backlog (prioritized)

| # | Sev | Item | Trigger | State |
|---|---|---|---|---|
| 1 | HIGH | Transport security (TLS) + node crypto identity | Exposing beyond trusted LAN | Done: HMAC per-device signing plus Caddy TLS with an internal CA (`deployment/`); a real certificate is still required before public exposure |
| 2 | HIGH | Physical bench validation (B1-B5) | Field expansion | docs/en/BENCH_PLAN.md |
| 3 | MED | OTA download is synchronous/blocking, dashboard down during update | First real OTA | Done: chunked reader with tri-state `ReadStatus`, no per-chunk sleep, `OTA_STALLED` guard. The initial HTTP open is still blocking |
| 4 | MED | `LogStorageRepository` open() O(bytes) - **CK01 checkpoint implemented** (fast-path O(segments) + fallback scan; flush every 64 appends) | Storage budget >512 KiB | Done |
| 5 | MED | Backend `_RATE` in-memory; `sync_batches` unbounded; `/v1/nodes` unpaged | Central growth | Done: SQLite-backed rate limit, retention cap 5000, pagination `limit`/`offset` plus opaque cursors |
| 6 | LOW | `Logger::eventf` fixed buffers (silent truncation) - **increased to 512 B** | Long messages | Done |
| 7 | LOW | Unity single binary, cross-suite isolation via dedicated data dirs | - | Open |
| 8 | LOW | `Esp32LittleFs::listFiles` core-version sensitivity (`entry.name()` v2 vs v3) | arduino-esp32 upgrade | Done: portable shim (`String(entry.name())`, skip dirs and dot entries); ESP32 compiles |
| 9 | LOW | Long-window analytics over raw rows | Windows > 90 days on a node | Done: `granularity=auto` reads `agg_hourly` past 7 days and coverage gap detection is already a SQL window function (`LAG`) capped at 20 gaps, so no Python row loop remains |

### Closed this round

- **Sync envelope emitted invalid JSON.** `SyncManager` wrapped the already-escaped
  node id in a second pair of quotes, so the wire payload was
  `"node_id":""CAUCE-E2E""`. The central answered `400 invalid_json` and
  the E2E gate was red on arrival. The unit test missed it because
  `"node_id":"CAUCE-001"` also appears inside every record, so the
  assertion was satisfied by the array, not the envelope. Fixed the
  format string and tightened both tests to assert the envelope
  prefix and to reject `:""`.
- **CSV export streamed one HTTP chunk per row**: 3.3 s for 14.4k rows,
  which extrapolates to minutes over a 60-day pilot. Now formatted in
  SQL and yielded in 500-line blocks: 109 ms for the same export.
- **Field diagnostics JSON had the same double-quoting bug** as the sync
  envelope, plus a missing NUL terminator that leaked stack bytes into
  the `%s` expansion. Caught by a new known-answer test.


- Sync batch payload buffer moved from caller stack to manager member
  (4 KB off loopTask stack).
- Per-request shared statics removed (ApiRouter route buffer,
  Esp32ApiServer auth header).
- `addSensor` capacity overflow logs `SENSOR_CAPACITY_REACHED` instead of
  silently dropping.
- verify-all/run-e2e toolchain PATH hardened (prepend MinGW + explicit
  CC/CXX), fixing intermittent gcc-not-found when invoked as a fresh
  powershell child process.
- CK01 checkpoint for `LogStorageRepository` (see below) — implemented with
  3 new tests (fast-path, tampered size fallback, corrupt CRC fallback).
- Backend hardening: rate limiter moved to SQLite (survives restart), `nodes`
  paginated, `sync_batches` retention, per-node CSV export.
- Materialized `agg_hourly` + `summary-fast` O(buckets), and `time-reconstruct`
  endpoint (with honest margin-of-error note).
- Doc drift numbers synchronized (112 fw / 68 backend).
- Roadmap M0 wired on device: `main.cpp` now instantiates
  `Esp32WifiController` + `NetworkManager`, configures sync endpoint and
  device secret from `sync_server_url/sync_device_key`, disciplines NTP on
  first link-up, syncs on Connected/Degraded, retries captive-portal DNS,
  ticks OTA with null catalog/reader/installer.
- Backend closed auth/rate gaps: shared `ratelimit.check_rate`
  (proxy-aware) on API, evaluation and dashboard; bearer gate on CSV
  exports; sanitized export filename; `sync_batches.transport` in
  {wifi, lora}; before-after windows inclusive without overlap.
- LoRa scaffolding (M2/M3): `ILoRaRadio` + `LoRaSyncTransport`
  (`ISyncTransport` with payload budget + min-interval duty gate,
  optimistic ack) with 4 host tests; config keys `lora_enabled`,
  `lora_sync_interval_s`, `lora_region`; `docs/en/ROADMAP.md` created.
- Pilot frozen in `docs/en/PILOT_SPEC.md`: 8 identical nodes + 1 gateway +
  carrier PCB + single-prototype acceptance gate before any ×8 purchase;
  form short version (meta/resultado) separated from technical annex.
- Real OTA end to end: dependency-free manifest JSON parser (3 host
  tests) + `Esp32Ota` HTTP/`Update` backend wired in `main.cpp` with
  per-node manifest HMAC from the device key and `ESP.restart` hook;
  `ota_manifest_url` config key; `GET /v1/ota/manifest` serving a
  releases file with per-node signatures (2 backend tests). Flashing
  cross-compiles; bench validation still pending.

### CK01 checkpoint design (ready to implement)
File `<dir>/checkpoint.bin`: magic `CK01` + u32 totalRecords + u32 totalBytes +
u32 lastSequence + u16 numSegments + per-segment {u32 bytes, u32 records} +
60-byte last-record payload + CRC32 over all preceding bytes.
`open()` fast-path adopts state when segment count/sizes match exactly; any
mismatch falls back to the full-scan recovery path (current behavior). Flush
every 64 appends and after rotation/retention/integrityCheck.

## Reproduce from zero

```powershell
pip install platformio
winget install BrechtSanders.WinLibs.POSIX.UCRT   # or any MinGW-w64 = GCC 9
cd firmware && pio test -e native      # expect: 202 succeeded
pio run -e esp32dev                    # expect: SUCCESS
cd ..\backend && pip install -r requirements.txt
python -m pytest tests -q              # expect: 192 passed
..\scripts\run-e2e.ps1                 # expect: E2E PASSED
```

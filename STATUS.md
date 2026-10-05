# CAUCE real system status

This document is the source of truth about what is implemented and what is
not. It overrides any aspirational claim elsewhere.

## Implemented and verified (319 firmware + 463 backend tests, E2E green)
| Component | Evidence |
|---|---|
| **Repository hygiene is declared and enforced**: `.gitattributes` fixes line endings per file type, `.editorconfig` fixes indentation and final newlines for editors, and the tree is normalised to match — 262 files LF, only the three `.ps1` files CRLF, 44 files that had no final newline now have one. Without this, a whole-file change showed as every line changed on whichever machine produced it | Mechanical; `ruff check` enforces the Python half and the release gate's doc-list check proves its own lists match the tree |
| **Device identity security**: admin-gated provisioning with a per-node HMAC key; batches signed over the raw body; OTA validates the manifest signature before downloading | 5 tests (firmware HMAC RFC4231 x2 + backend matrix valid/bad-signature/no-signature + device-secret signing x2) |
| **Token administration over HTTP**: `POST/GET /v1/tokens` and `GET/DELETE /v1/tokens/{name}`, shared-admin only, plaintext returned once and never stored, SHA-256 digest with scopes and optional site | 14 backend tests |
| **Retention breadth**: `agg_daily`, `alert_log`, `node_diagnostics`, `maintenance_events` and `sync_batches` purged alongside raw measurements; only `acked` commands age out, `pending`/`delivered` survive, orphaned receipts are cleaned; every counter reported | 8 backend tests |
| **Node-signed LoRa frames**: the node appends HMAC-SHA-256 over header, payload **and CRC** to every frame it transmits; the gateway holds no key and verifies nothing, relaying bytes verbatim; the central verifies each frame against the provisioned key. Restores A-8 for the relay path | 5 firmware tests (known-answer HMAC vector, unsigned/small-buffer refusal, signed == unsigned + 32 bytes, tampering, CRC coverage) + 19 backend tests |
| **Coverage resolution cap**: the window guard bounds implied sample count as well as span, so a 400-day window at 1 s is refused as `too_many_buckets` while two years at hourly is allowed and points at `agg_hourly`/`agg_daily` | 4 backend tests |
| **Ed25519 frame signatures (RFC 8032)**: `device_key_algorithm` fixed at provisioning decides the check, so a frame cannot relabel itself into the cheaper one; a 32-byte trailer is HMAC and a 64-byte trailer is Ed25519, which is why the pinned frame header did not change. Central stores the public key only. **The firmware cannot sign Ed25519 yet**, so this is reachable from the replay path and the gateway but not from a deployed node | 30 signing tests incl. RFC 4231 and RFC 8032 §7.1 known-answer vectors, plus 9 backend tests on the relayed path |
| **SHA-512 in firmware** (FIPS 180-4), streaming, 128-byte blocks with the 128-bit length field. Present because Ed25519 is defined over a 512-bit hash: the seed is stretched into a signing scalar and a per-message nonce is derived from it, and no narrower hash substitutes | 8 firmware tests: empty/abc/two-block/896-byte/one-million-a from the standard, streaming equals one-shot at ten chunk splits, null and zero-length appends, exact block-boundary padding |
| **Capability scopes actually enforced**: every endpoint in `api.py` now calls `require_scope`, so a `read` token is refused on writes and admin work with 403 instead of being compared against the shared admin token; site-scoped principals are confined to their own site on node reads, measurements, compare and the node write path | 14 tests that fail against the previous state; the shared admin token still works everywhere |
| **`agg_15min`**: quarter-hour aggregates fed from the raw row at insert time, `granularity=15min`, and `auto` selecting them between 2 h and 2 days. Cannot be derived from `agg_hourly` the way `agg_daily` is, because an hour's sum/min/max do not say how the hour was distributed inside it. Capped at two days on purpose: past that a quarter-hour table returns more rows than the raw one it replaced | 20 tests: bucket alignment, null values skipped, min/max/sum agreeing with raw, mean equality, refusal when buckets are purged or mostly missing, retention counter, healthz |
| **P2P merge semantics** (C1, the correctness half): a set union on `(node_id, sequence)` with no coordinator, so merges are commutative, associative, idempotent and monotonic. The union element is the `(key, payload)` pair, not the key: keying on the key alone made a repeated conflicting merge from the same peer append a row every sync, so a diverging replica grew without bound and stopped being idempotent. Conflicts keep the incoming row and count the divergence rather than resolving it, because dropping a peer's measurement is the loss this exists to prevent, and there is no "last writer wins" because a node with a reconstructed clock would otherwise overwrite good data. Deletion is deliberately not propagated | 15 tests including convergence from both directions and out of order, repeated retries, a full replica refusing, and a repeated conflict not growing |
| **P2P transport interfaces** (`IPeerLink.h`): `IPeerDiscovery`, `IPeerRadio` and `PeerExchange` shaped like `ILoRaRadio`. The ESP-NOW driver and mDNS responder are **not** implemented; an untested driver behind a tested merge would make the merge look proven in a way it is not | Compiles on host and ESP32 |
| **Ed25519 field, group and point encoding** (firmware): radix-2^16 field arithmetic, twisted Edwards points, and compressed point decode/encode. `decode(encode(p))` is proven to be the identity against the RFC 8032 encodings, and the parity bit in 255 round-trips explicitly | 9 firmware tests: round trip over every public key and every R component, parity asserted directly, negating x shown to flip exactly bit 255, base point derived and matched, curve membership, non-canonical y refused, off-curve y refused, null arguments refused |
| **Central calibration**: `calibration` table per (site, variable) plus a `maintenance_events` log; raw stays intact and the calibrated value is derived on read; applied in summary, compare, period-compare, summary-fast, hourly, heat-events, before-after/DiD, colocation, report and CSV | 16 backend tests |
| **Long-window analytics**: `granularity=auto` reads `agg_hourly` past 7 days when coverage is sufficient and says so in the response; `raw` remains available for exact min/max | 6 backend tests |
| **Cursor pagination**: opaque `(timestamp_utc_ms, sequence)` cursor for measurements and `node_id`-based cursor for nodes, stable under concurrent inserts; `limit`/`offset` untouched | 6 backend tests |
| **Deep sleep**: portable `DeepSleepController` (9 tests) plus `Esp32Sleeper`, behind a `deep_sleep_enabled` flag that is **off by default** until bench power measurement | 9 firmware tests + 2 config tests |
| **Non-blocking OTA chunks**: tri-state `ReadStatus` reader, no 10 s per-chunk wait, `OTA_STALLED` with a tick budget | 3 firmware tests |
| **The watchdog is fed around the OTA calls that block**: `FeedFn` on `OtaManager`, wired to `esp_task_wdt_reset` in `main.cpp`, called before and after `fetchLatest` and `open` | 2 firmware tests. Ordered, not counted |
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
| **SX1276 driver register map corrected** (LoRa map, not the FSK/OAK one): the FIFO base-address writes were using the FSK/OOK register numbers 0x80/0x81/0x82 with the write flag stripped, and in the LoRa map 0x01 is RegOpMode. The line meant to park the RX base at the bottom of the FIFO therefore wrote zero to RegOpMode and cleared LongRangeMode, so the radio never left FSK and could not transmit a LoRa frame. RSSI was read from 0x1C, which is RegHopChannel in this map, so `lastRssiDbm()` reported a plausible number that changed whenever the chip hopped. `test_begin_actually_sets_the_lora_bit` had been failing and unregistered, and two earlier theories about the same failure had been wrong | 35 host tests in `test_sx1276.cpp`, up from 32. The previously failing test is now registered and passes, and two were added: RSSI includes the signed wideband term, and the RSSI read does not touch the hop channel. Mode assertions now compare the mode field rather than the whole byte, because the correct written value carries LongRangeMode. **Still host-only: this driver has not been on hardware** |
| **Dependency lock with hashes**: `backend/requirements.lock` pins all 30 packages in the central's closure, each with a SHA-256 for CPython 3.12 on win_amd64 and manylinux2014_x86_64. CI installs with `--require-hashes`, so the tree behind a build is nameable and a range bump cannot change it silently. `requirements.txt` stays as the statement of intent | 5 backend tests in `test_requirements_lock.py`: every direct requirement is pinned, every pinned version satisfies its range, every pin carries a hash, the lock covers more than the direct list, and no hash is a placeholder. The drift check was verified to fail when `requirements.txt` is bumped without regenerating |
| **Stored XSS through the `variable` and `sensor_id` fields** (found while checking CodeQL alert #3, which was itself a false positive): those fields were stored verbatim, and the dashboard pasted `json.dumps(series)` inside a `<script>` element. An HTML parser looks for the literal `</script>` regardless of JSON quoting, so a node could sync a variable called `</script><script>alert(1)</script>` and the central's own node page would render it as markup — stored XSS against whoever opened the dashboard. Only reproduced on a node whose variables were all unrecognised, because a node with a known variable charts the preferred set and the unknown name never reaches the script block. That is why the existing suite missed it: every test of this route used a normally seeded node | Fixed at both ends. **Input**: `NAME_PATTERN` restricts `variable` and `sensor_id` to `[A-Za-z0-9_.-]{1,64}`, the same shape as `site_id`, with 422 `invalid_variable`. A character class and not an allowlist, because the protocol is meant to grow new variables. **Output**: `dashboard._json_for_script` escapes `<`, `>`, `&`, U+2028 and U+2029, and every `json.dumps` result that reaches a page goes through it, so the sink is safe regardless of what any writer does. Both halves were verified to be load-bearing: against pristine HEAD the payload reflects raw into `/nodes/{id}`; with only the input fix it never reaches the database; with only the output fix it is escaped in the charted series | 6 backend tests: ingest refuses markup in `variable` and `sensor_id`, accepts the shapes the firmware sends, a value seeded straight into the database cannot break out of a script block, and two unit tests on the escaper including the invisible JavaScript line terminators |
| **OTA manifest signature, end to end for the first time**: the central keyed the HMAC with the ASCII `device_key` and signed `version\|url\|total_size`; the firmware keyed it with `SHA-256(device_key)` as 32 raw bytes and signed `version\|sha256\|url\|total_size`. Two independent mismatches, so **no node could ever accept a manifest**. It failed by refusing, which looks identical to a node not being offered one, and both suites were self-consistent so neither could see it. The central now derives and canonicalises exactly as the firmware does, and the signature is required rather than skipped when no key is configured | A cross-language known-answer vector (`424f819f…`) pinned on both sides: `test_the_manifest_signature_matches_the_firmware_known_answer` and `test_manifest_signature_matches_the_central`. Plus a test that the signature changes when the image hash changes, and one that an unsigned node refuses to update |
| **Stored XSS through `quality`**: the field was checked for presence and nothing else, and it reached an HTML class attribute - `class="q-{quality}"` in the node page and the report. A node could sync `x" onmouseover="alert(1)` and the central returned `<td class="q-x" onmouseover="alert(1)">`, which fires without a click. Reproduced against HEAD before fixing | Fixed at both ends. **Input**: `FIRMWARE_QUALITIES` restricts it to the eight values `cauce::core::qualityName` can emit, 422 `invalid_quality`. Deliberately a different set from `coverage.USABLE_QUALITIES`, which answers a different question: `INVALID` and `MISSING` are legitimate values that set excludes. **Output**: all five HTML sinks escape it, the same treatment `variable` and `unit` already had two lines above. 4 tests |
| **Setup is instrumented and fed, and the watchdog hypothesis was wrong**: the board rebooted every ~9 s with `rst:0x8 (TG1WDT_SYS_RESET)` and the log ended at `CONFIG_LOADED`, never `BOOT_COMPLETE`. This file previously blamed `esp_task_wdt_add(NULL)` for subscribing the idle task. It does not: on the Arduino core `setup()` and `loop()` both run on loopTask, so NULL is loopTask - and `loop()` feeds the watchdog on its first statement, so a hang in `loop()` cannot explain it either. **Nothing in `setup()` ever fed it**, while `setup()` does a flash mount, a store open, sensor bring-up, a web server and the OTA bookkeeping in one pass. Each step now announces itself, feeds the watchdog on entry and exit, and reports its own duration, so a slow step is named in the log instead of inferred. A node with no device key also says so at boot | 319 firmware tests; ESP32 and bench builds SUCCESS. **The fix itself is unverified on a board** - see the hardware-only list |
| **Central admin API fails closed on writes**: with `CAUCE_API_TOKEN` unset, every mutation answers `503 admin_api_not_configured` instead of being allowed. Reads stay open on purpose, so the dashboard still works on a trusted LAN, and setting the token closes reads too. Previously `require_bearer_token` returned early when no token was configured, which meant anyone who could reach the port could mint an API token or rewrite calibration | 3 backend tests: a write with no token is 503 with the machine code, a read with no token is 200, and an unauthenticated write with a token is 401. The whole suite now authenticates the way a configured deployment does, through `backend/tests/conftest.py` |
| **SBOM describes the service, not the build machine**: the component list is the transitive closure of what `requirements.txt` actually requests, following only unconditional requirements plus the extras that file asks for. It listed all 229 distributions installed on whichever machine ran it, including unrelated tools | 18 backend tests in `test_sbom.py`, up from 14. New: the SBOM is a strict subset of the interpreter, an unrequested extra is not followed, a requested one is, and a direct dependency that is not installed is reported instead of silently dropped. A test that wrote its output into `sbom/` and left a tracked artefact behind now writes to `tmp_path` |
| **Cross-suite isolation registry** for the single Unity binary: a suite declares the data directory it uses and the process state it mutates, the run prints everything every suite declared, and two suites claiming one directory fail the run at exit. This is detection, not prevention: it catches a shared directory that someone declares, not one they forget to. The three file-scope mutable statics in the suite declare themselves; the directory half has no callers, because no suite shares a filesystem today and a claim would be a false positive rather than a finding | 6 tests: the clash is recorded with the exact message, distinct directories stay clean, a suite re-claiming its own directory is recorded once rather than twice, a missing directory is reported as `"(null)"` instead of crashing the run, a declared mutation is recorded verbatim, and the report prints a live clash. The end-of-run gate was verified to fail the run when a clash is present |

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
  separate measurement from method. There is still no metrological traceability:
  the reference is another node, and the chain ends at a sensor datasheet.
- Coverage accounting caps a query at 400 days. The gap list is pageable
  (`gap_offset`, `gap_limit`, 200 per page), so a long window is bounded per
  response and complete across pages.
- TLS ships ACME by default (`CAUCE_TLS_MODE` empty). A public deployment
  still needs a real DNS name pointing at the host.
- Deep sleep is wired but **disabled by default**: turning it on requires
  the bench measurement in `docs/en/BENCH_PLAN.md`.
- The LoRa frame format, fragmentation, acknowledgement and the forwarding
  loop are written and tested end to end against the central, and the SX1276
  driver now configures the radio correctly, but there is still no measured
  link budget: the air interface is unproven. The driver has never been on a
  board, which is stated here rather than implied away.
- Downlink **actuation of physical actuators** needs hardware that can be
  actuated. The four node-level commands (`set_sampling_interval`,
  `set_sync_interval`, `request_resync`, `set_led_mode`) do now act, persist
  across a reboot and report the value applied rather than the value
  requested.
- Firmware **image signing**. A node verifies the manifest signature before it
  downloads anything, and the signature commits to the image hash. It is now
  *mandatory*: a node with no signing key refuses every update with
  `OTA_NO_MANIFEST_KEY` instead of accepting whatever manifest it is handed.
  What is still missing is a key hierarchy - the manifest key is derived from the
  node's own `sync_device_key`, so the same secret that authenticates batches also
  authorises firmware. Separating them is the remaining work. A certificate authority
  now exists for the other half of that problem - who vouched for a node's key - and it
  is not yet in TLS. `docs/en/RUNBOOK.md` says so plainly rather than leaving it
  implied.
- ESP-NOW / mDNS peer-to-peer. `Replication` and `IPeerLink` exist and are
  tested; the radio and discovery do not. This is a Phase 0 decision, not an
  oversight.
- A **signed downlink that has never been observed arriving**. The command path exists
  end to end and is host-tested; the bench now reaches the batch-building and
  credential-loading stages of the same path on real hardware, but no command has been
  seen to complete a round trip to a central and back.
- TLS still relies on the shared admin token for API access. A node **certificate**
  now exists - `CAUCE_CA_KEY` signs a binding of node identity to public key with an
  expiry, and a verifier needs only the CA public key - but it is not wired into TLS.
  Turning it into mutual TLS, or into the credential a node presents to `/v1/sync`,
  is the remaining work and it is not small: the firmware would have to hold and rotate
  a certificate rather than a seed.

## Roadmap: what is left, and why

Ordered by what unblocks the most, with the honest reason each item exists. This
replaces the old 400-day coverage note, which is resolved: the window guard now
bounds implied sample count as well as span.

### D1 - capability scopes on every endpoint (DONE)

`security.require_scope` resolves the caller from `api_tokens` and enforces
`read`/`write`/`admin` plus optional site scoping. This section used to say no
endpoint called it and that the feature was therefore inert. That was wrong and
contradicted the table above in the same file: there are 22 `require_scope` call
sites and the remaining 19 direct `require_bearer_token` calls are on read paths
and CSV exports, where a token check with no scope to enforce is the correct
instrument.

The one endpoint that did ignore the configured token was `/v1/ota/manifest`: its
handler took no `authorization` at all, so it published the exact version, URL,
size and per-node HMAC of the next firmware to anyone who asked even on a locked
down central. Fixed, with tests for the authenticated, anonymous, wrong-token and
no-token-configured cases.

Writes now fail closed when no admin token is configured, rather than being
allowed.

### D3 - sub-hourly aggregates (DONE)

`agg_hourly` and `agg_daily` exist. There is no `agg_15min`, so a query over the
last six hours reads `measurements` directly. That is affordable for a pilot and
is the first thing that stops being affordable as node count grows.

### C2 - Ed25519 signing in the firmware

The central verifies Ed25519 frame signatures today, checked against RFC 8032
section 7.1 known-answer vectors and interoperating with `cryptography`
(`backend/cauce_server/signing.py`). The firmware cannot produce them yet.

**What is verified, mechanically, against values computed independently:**

- Field layer: 14/14 (multiply, square, invert, `powP58`, limb normalisation,
  canonical encode/decode, curve constant, base y, sqrt(-1), squaring chains).
- Scalar layer: 6/6 (reduction mod L including all-ones and a random 511-bit
  value, and `scMulAdd`).
- Point layer: `decode(encode(p))` is the identity over every RFC 8032 public key
  and every signature's R component, with the parity bit asserted directly and
  shown to be the only thing negating x changes.

**Two real defects were found on the way and are fixed**, both now explained in
the source so they are not reintroduced: the usual carry pass leaves limb 0 large
because the 2^256 fold lands back on it, so limb magnitudes compound until
`int64` overflows; and a shift loop written high-to-low dropped bit 31 of every
limb, which made scalar reduction return the input's low 32 bits unreduced.

**The group law is now verified, and there was no fault in it.** What passes,
against multiples of the base point computed by an implementation written from the
curve definition:

- decoding gives the reference affine `x` and `y`, not merely an encoding that
  round-trips - the blind spot a round-trip test has, since re-encoding a wrong
  `x` reproduces the same bytes;
- `add(identity, P) == P`;
- `add(P, P) == 2P` for every multiple tested;
- `add(P, Q)` for **distinct** P and Q;
- every result is on the curve, and `add(P, -P)` is the identity;
- the four intermediates `A, B, C, D` of the formula, compared against the affine
  derivation for `2G + G`.

**What actually went wrong, twice, was the reference generator.** It built the
multiples by doubling while labelling them `1G, 2G, 3G, ...`, so a test asserting
`2G + G == 3G` was really comparing against `4G`. Before that it had an
off-by-one in the doubled encoding. Both faults produced a confident,
specific-looking "the group law is broken for distinct points" diagnosis, and both
were in the measurement rather than the code.

That is the same failure mode as the earlier hand-transcribed `powP58(2)`
reference and the hand-written `R` value: a wrong expected value is worse than no
test, because it reports a defect that does not exist. Every reference used by the
firmware tests is now generated from an independent implementation, and
`addPointsTrace` exists so the formula's intermediates can be compared directly
rather than inferred from a final result.

**Correction: the fixture is not corrupt. The previous note here was wrong.**

`Multiple` has five fields - `{scalar, affineX, affineY, encoding,
doubledEncoding}` - and reading them in that order, row 1 is:

```
{1,
 "1ad5258f602d56c9...",   // affineX  - G's x, correct
 "5866666666666666...",   // affineY  - G's y, correct
 "5866666666666666...",   // encoding - y, with the parity bit never set
 "c9a3f86aae465f0e..."},  // doubledEncoding - 2G's y
```

`affineX` and `affineY` are distinct and correct. The only real defect is that
`encoding` is written as plain `y` with bit 255 left clear instead of set from
`x & 1`. That is invisible to the group-law tests, because Ed25519 decoding
ignores bit 255 and returns the same affine coordinates either way - which is why
they passed legitimately, not by accident.

It is *not* invisible to a comparison against an encoder, because an encoder must
set that bit. `ed25519ScalarMultBase` sets it, so its output and this fixture's
`encoding` differ in the top bit whenever `x` is odd.

**The scalar ladder is fixed. Two defects, both in `selectGf`, and the second was
hiding the first.**

1. `selectGf` selected between `acc` and `sum` instead of between `doubled` and
   `sum`. The recurrence is `acc = bit ? 2*acc + p : 2*acc`, so the old accumulator
   is never a candidate. Selecting it anyway silently drops the doubling on every
   clear bit, giving `(2^popcount(scalar) - 1) * p`.
2. `selectGf` cast each limb to `int16_t` while `Gf` is `int64_t[16]`, truncating
   every coordinate to 16 bits.

The signature was the diagnostic. `(2^popcount - 1)` yields 1, 1, 3, 1, 3, 3, 7 for
the scalars 1 to 7, which is exactly what the ladder returned - every odd scalar
correct, every even scalar collapsed onto the odd one below it. That is why `[1]G`
and `[3]G` passed while `[2]G` did not, and it resembles a broken reference far more
than a broken ladder. Running all seven scalars instead of one at a time is what
exposed it.

Two things in my own diagnostics were wrong and are corrected here:

- **`ed25519DecodePoint` was never broken.** Several rounds concluded it was,
  including one that blamed it for a projective addition failing. The signature is
  `(encoded, outX, outY)` and the arguments had been reversed, so it was handed
  uninitialised buffers as its input.
- **The fixture's `doubledEncoding` column was wrong.** It held the encoding of the
  *next* row rather than of the double, so only row 1 was right and row 7 was empty.
  The column is removed. Doubling is now checked against the group law rather than
  against a fixture column that happened to agree with the broken ladder.

A third gap is closed by `test_the_core_decoder_matches_the_fixture`, which pins the
production decoder against the fixture's own affine coordinates for all seven rows.
Its absence is what allowed a real decoder fault to survive unnoticed: the existing
decode test uses `affineOf`, a helper local to the test file, so the production
decoder was never the thing under test.

**What the ladder is now tested against**, all registered:

- `test_every_reference_multiple_is_reproduced` - scalars 1 to 7 against the
  reference encodings. It includes even scalars deliberately; an odd-only test, or
  scalar 1 alone, would have passed throughout the entire breakage.
- `test_doubling_agrees_with_the_group_law` - `ladder(2n)` against `nG + nG` for all
  seven, tying the ladder to the independently verified group law so a ladder that
  is wrong but self-consistent cannot pass.
- `test_the_core_decoder_matches_the_fixture` - decode against `affineX`/`affineY`.

**Still open in C2: wiring the algorithm choice into `LoRaSyncTransport`.**

### C2b - signing (DONE)

`ed25519PublicKeyFromSeed`, `ed25519Sign` and `ed25519Verify` are implemented and
tested against RFC 8032 section 7.1, cross-checked against `cryptography` rather
than against themselves. `firmware/test/tools/gen_ed25519_vectors.py` is committed
so the fixture can be regenerated and argued with.

Two things that were wrong and are worth not repeating:

- **`kGroupOrder` was written by hand with a byte missing.** The array dropped the
  `0x58` in the middle, which shifted every later byte left and still declared 32
  elements, so it compiled. It produced a plausible-looking wrong scalar on every
  reduction. `L` is now `L.to_bytes(32, 'little')` copied from Python, with the hex
  written out in the comment so a future reader can check it without running
  anything.
- **The generator had `b"72"` where it meant the byte `0x72`.** Python reads that as
  the two ASCII characters, so the public keys came out right and the signatures
  wrong - which is exactly the shape of a broken implementation and would have been
  very hard to see. Messages are now given as hex strings and converted with
  `bytes.fromhex`, and `unhexHex` exists in the test for the same reason.

Signing rejects nothing on the way in beyond null arguments. Verification rejects a
non-canonical `S`, without which `S` and `S + L` would both validate and a signed
frame could be rewritten into a second, differently signed, equally valid frame.

Neither signing nor verification is constant-time, and that is a real limitation of
this implementation rather than an oversight. The comment on each entry point says
so, and says under what threat model it is acceptable.

`test_adding_the_identity_to_a_projective_point` stays registered and passing. The
ladder test and the projective doubling test stay in the tree, unregistered, with
their outputs above.

### C1 - peer-to-peer exchange (merge DONE, transport NOT)

`mDNS` discovery, `ESP-NOW` transport, node-to-node replication and CRDT merge
do not exist. This is greenfield rather than a fix, and the thesis treats it as
proposed evolution. It has no dependency on C2.

### Phases 0-6 to a frozen release

`docs/en/RELEASE_READINESS.md` (and the `docs/es/` mirror) holds the sequenced
plan for taking this from "a very good prototype" to a **frozen baseline under
maintenance-only**: Phase 0 freeze the specification, Phase 1 close the software
gaps, Phase 2 decide and close the air interface, Phase 3 record the bench
results, Phase 4 manufacturing and per-device seed provisioning, Phase 5 release
engineering and the release gate, Phase 6 declare the freeze.

It also fixes the vocabulary, because "finished, never updated again" has no
single name and the difference matters: a **feature freeze** ends capability,
**RC** cuts the tree, **GA** is the release as sold, a **baseline** is the frozen
artefact set, **LTS** means features frozen but security still patched, and **EOL**
means no patches at all. The target state here is LTS with a stated EOL date, not
EOL - this code terminates TLS and verifies signatures, and a build with no patch
channel is a liability rather than a finished product.

**Where each phase actually stands:**

| Phase | State |
|---|---|
| 0 - freeze the specification | Not started. Needs a decision on ESP-NOW and one on LoRa. |
| 1 - software gaps | Ed25519 in the transport, downlink actuation and exercised backup/restore **done**. Calibration procedure and uncertainty budget **done** (`backend/tools/calibrate.py` executes the documented fit and evaluates the acceptance limit; 15 tests). Coverage gap paging **done** (`gap_offset`/`gap_limit`). Node certificates **done** - `CAUCE_CA_KEY` signs a binding of identity to public key with an expiry, verifiable with the CA public key alone (41 tests); not yet in TLS. Test binary isolation partially done - the registry and gate exist and mutations are adopted, but no suite shares a filesystem so the directory half has no callers. |
| 2 - air interface | Driver written and host-tested after three real register bugs were found and fixed; no board, no measured link budget. See the SX1276 row above. |
| 3 - bench | Not started. Needs a board and `docs/en/BENCH_PLAN.md`. |
| 4 - manufacturing | Provisioning tool and identity retirement **done**. Factory self-test **done**: `bench_main.cpp` runs sensor read with a plausibility range, config, storage append/reopen with a value check, `listFiles` termination, provisioning, and the signed-batch path, ending in a `FACTORY_RESULT` line for a line-side script. Stages a bare unit cannot perform report `SKIP`, not `FAIL` - requiring a signed sync with no server would fail every unit for a reason unrelated to the unit, and a test everyone ignores is worse than none. An enclosure does not exist, and no unit has yet been run through it. |
| 5 - release engineering | SBOM tool (scoped to the real dependency closure, no longer a listing of the build machine), release gate (`.sh` and `.ps1`) and runbook **done**. Lockfile with hashes **done** (`backend/requirements.lock`, CI installs with `--require-hashes`). The image now installs that lock, which it previously ignored: `backend/Dockerfile` copied `requirements.txt` and resolved fresh versions at build time, so the pinned closure in the repository was documentation. The base image is a required `ARG BASE_IMAGE` with no default, so a build cannot pick a floating tag, and the gate fails a release whose Dockerfile does not. Gate also checks semver and that the tag matches the version the application reports. **Still open: no release tag**, and the base digest has no value recorded here because resolving one needs Docker, which is not installed here - supplying a digest unverified would be a correctness claim with no evidence behind it. |
| 6 - declare the freeze | Not started, and cannot start before Phase 3. |

The release gate is the one thing here that keeps the remaining phases honest:
`scripts/release-gate.sh` (or `.ps1`) refuses to pass unless the tree is clean, both
suites and the ESP32 build are green, the SBOM's pins hold, the documentation index
matches the tree in both languages, Ed25519 is actually wired into the transport and
codec, `kGroupOrder` still carries its verified hex, and no test is sitting in the
tree unregistered.

It reports **PASS** at present. That means nothing is obviously broken. It does not
certify the hardware, and the gate says so on the way out.

### Phase 3 - bench: ATTEMPTED, and it found a hard failure

**Result: the firmware does not run on real hardware.** Not "unproven" - broken.
An ESP32-D0WD-V3 (rev 3.0, WROOM, CH340 USB, **8 MB flash**, so the two-slot OTA
table fits) was flashed at `COM3` and reboots forever:

```
rst:0x8 (TG1WDT_SYS_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
[   243][E][vfs_api.cpp:105] open(): /littlefs/config/cauce.conf does not exist
[   253][E][vfs_api.cpp:105] open(): /littlefs/config/cauce.conf.bak does not exist
INFO CONFIG_LOADED status=1 node=CAUCE-001
rst:0x8 (TG1WDT_SYS_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
... repeats every ~9 seconds, forever
```

The upload succeeded, the hash verified, LittleFS mounted, and the config loaded
with defaults. Then the task watchdog fires before `loop()` ever logs anything.

**What this is worth.** 319 firmware tests pass, the ESP32 cross-build is clean, and
the node cannot boot. Every test in this repository runs on the host, and the host
build defines `CAUCE_HOST_SIMULATION` and links none of the ESP32 code. So "the
suite is green" said nothing about this, and it never could have. This is the
single most important finding in the project and it took a bare board and ninety
seconds to get.

**What is known and what is not.** Known: the fault is after `CONFIG_LOADED` and
before or inside the first `loop()`, and it is a task watchdog, not a crash - there
is no panic backtrace, so it is a task starving rather than an assertion. Not yet
determined: which of the remaining `setup()` steps or the first `loop()` iteration
blocks. The prime suspect is the first `loop()` statement, `g_networkManager->tick()`,
attempting a Wi-Fi association with no configured network - but "prime suspect" is a
guess and the next step is instrumentation, not a fix based on one.

The watchdog registration is itself suspicious and worth checking in the same pass:
`esp_task_wdt_init(30, true)` followed by `esp_task_wdt_add(NULL)` subscribes the
**idle** task, not the loop task. An idle-task watchdog trips when the loop task
never yields, which would produce exactly this signature with no backtrace.

### Hardware-only

SX1276 driver and link budget, OTA rollback on real flash, BME280/LittleFS/Wi-Fi
on the bench, deep-sleep power measurement, and real actuators. None of these can
be closed from a desk, and `docs/en/BENCH_PLAN.md` is the procedure.

## Known technical debt / backlog (prioritized)

| # | Sev | Item | Trigger | State |
|---|---|---|---|---|
| 1 | HIGH | Transport security (TLS) + node crypto identity | Exposing beyond trusted LAN | Done: HMAC per-device signing plus Caddy TLS with an internal CA (`deployment/`); a real certificate is still required before public exposure |
| 2 | HIGH | Physical bench validation (B1-B5) | Field expansion | docs/en/BENCH_PLAN.md |
| 3 | MED | OTA download is synchronous/blocking, dashboard down during update | First real OTA | Done: chunked reader with tri-state `ReadStatus`, no per-chunk sleep, `OTA_STALLED` guard. The two blocking HTTP calls that remain - `fetchLatest` and `IFirmwareReader::open`, each a whole GET inside `tick()` - now feed the task watchdog immediately before and after, via a `FeedFn` hook wired to `esp_task_wdt_reset`. They are still blocking calls; what changed is that the margin is an invariant instead of arithmetic. WDT is 30 s and the socket timeout 15 s, so one call fits, but a tick can open a download *and* fetch the next manifest, and 15 + 15 plus the rest of `loop()` was past half the budget. The failure would have been a board rebooting mid-update with nothing in the log, the same symptom class that made the original nine-second reboot hard to read. 2 firmware tests, and the assertion is *ordering*: the fakes stamp the feed count at entry to each blocking call, because a single feed at the top of `tick()` would satisfy a count and still leave the bug |
| 4 | MED | `LogStorageRepository` open() O(bytes) - **CK01 checkpoint implemented** (fast-path O(segments) + fallback scan; flush every 64 appends) | Storage budget >512 KiB | Done |
| 5 | MED | Backend `_RATE` in-memory; `sync_batches` unbounded; `/v1/nodes` unpaged | Central growth | Done: SQLite-backed rate limit, retention cap 5000, pagination `limit`/`offset` plus opaque cursors |
| 6 | LOW | `Logger::eventf` fixed buffers (silent truncation) - **increased to 512 B** | Long messages | Done |
| 7 | LOW | Unity single binary, cross-suite isolation via dedicated data dirs | - | Partially done: the declaration registry and the end-of-run gate are implemented and tested. **Mutations are adopted** - the three file-scope mutable statics in the suite declare themselves. The directory half still has no callers, because no suite shares a filesystem: each one that writes files builds its own in-memory or temporary store, so a claim would be a false positive rather than a finding. An earlier version of this row said adoption was zero, which was already wrong |
| 8 | LOW | `Esp32LittleFs::listFiles` core-version sensitivity (`entry.name()` v2 vs v3) | arduino-esp32 upgrade | Done: portable shim (`String(entry.name())`, skip dirs and dot entries); ESP32 compiles |
| 9 | LOW | Long-window analytics over raw rows | Windows > 90 days on a node | Done: `granularity=auto` reads `agg_hourly` past 7 days and coverage gap detection is a SQL window function (`LAG`), so no Python row loop remains |
| 11 | MED | A provisioned public key proved possession but nothing established that the central vouched for it: `nodes.device_key` is a row an operator wrote with the admin token | Adding a node and rotating a compromised key were the same operation with the same blast radius, and a key compromised once stayed compromised | **Done.** `CAUCE_CA_KEY` holds an Ed25519 seed for a CA, configured separately from the admin token, and signs a certificate binding node identity to public key with a serial and an expiry. The property that matters: a verifier holding **only the CA public key** checks a node with no database and no admin token, which the shared token cannot do at all. Fails closed - with no CA key every endpoint is `503`, because issuing documents signed by nothing would look authoritative and verify against no key. The key comes from `nodes.device_key` and never from the request; an HMAC node is refused (409) since publishing a shared secret to verifiers is worse than no certificate. Rotation replaces rather than accumulates, keeping the retired row because a table that destroys the old certificate cannot answer which key a node held when a measurement arrived. 25 unit + 16 endpoint tests |
| 12 | MED | `Ed25519PublicKey.from_public_bytes` accepts any 32 bytes, deferring the point check to verification time | A certificate could be issued for a key that can never verify anything, and the failure would appear on a node with the CA looking innocent | Fixed in `certificates.is_on_curve`: a `y` at or above p is non-canonical and rejected, and about half of all 32-byte values have no corresponding `x` and are refused at issuance. "Never works, much later" becomes "refused now". Also guarded the other direction, since a check that rejected every key would fail every provision in the fleet |
| 13 | LOW | Test modules set `os.environ["CAUCE_DB_PATH"]` believing it selects their database; it does not, because `config.Settings` snapshots the environment once at import | Harmless for the path - isolation comes from `db.reset_for_tests()` - but the same pattern on a value nothing resets is a silent order-dependent failure | **Found while adding the certificate tests.** `test_cert_endpoint.py` set `CAUCE_CA_KEY` that way and passed alone, then failed 7 times in the full suite, because whichever module triggered the first `cauce_server` import decided the CA key for all of them. It now assigns `settings.ca_private_key` through an autouse fixture, and `tests/conftest.py` says why the per-module lines are decorative. The rule is written down: a setting nothing resets is set on `settings`, not on `os.environ` |
| 14 | LOW | Double-escaped threshold on the events page - `html.escape(_form_value(x))` - so the two sinks disagreed and a value the user typed came back as `&amp;` | Not a vulnerability, and the tempting "fix" is to remove the helper's escaping, which would reintroduce the CodeQL finding it exists to close | Fixed to a single layer, with a test asserting `&amp;amp;` and friends appear nowhere on the page and that `32.5` survives intact, so it passes by not escaping rather than by escaping everything away. Escaping is not idempotent, and that is stated where the helper is |
| 10 | MED | The coverage response reported the 20 longest gaps and said only that more existed, so a caller could not reach the rest of a window's outage history | A site with thousands of gaps was unauditable below rank 20 | **Done.** `gap_offset`/`gap_limit` page the list, longest first. Four defects found while doing it, all of which would have shipped as a paging API that lied: `ORDER BY delta DESC` alone is not a total order, so `LIMIT/OFFSET` dropped and repeated equal-length gaps - the tiebreak on `ts` is what makes paging exact; `gaps_truncated` compared the total against the page remainder, so the last page claimed more forever and a caller following it never terminated; `gap_count_total` was read off `rows[0]`, so paging past the end reported a site with zero gaps; and `longest_gap_ms` came off the page, so paging made the worst outage look like it was shrinking. The trailing gap is synthesised in Python and has no rank in the SQL order, so it is reported in its own field on later pages instead of being appended to a list whose contract is "Nth longest first". 10 tests |

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
pip install platformio==6.1.19
winget install BrechtSanders.WinLibs.POSIX.UCRT   # or any MinGW-w64 = GCC 9
cd firmware && pio test -e native      # expect: 319 succeeded
pio run -e esp32dev                    # expect: SUCCESS
cd ..\backend && pip install --require-hashes -r requirements.lock
python -m pytest tests -q              # expect: 463 passed, 1 skipped
..\scripts\run-e2e.ps1                 # expect: E2E PASSED
```

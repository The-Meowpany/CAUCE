# CAUCE real system status

This document is the source of truth about what is implemented and what is
not. It overrides any aspirational claim elsewhere.

## Implemented and verified (461 firmware + 575 backend tests, E2E green)
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
| **The gateway was dropping every Ed25519 frame and reporting nothing**: `set_node_algorithm` was called from nowhere in the tree - not from a runner, because the gateway has no runnable entry point, not from `main.py`, not from a tool. So on a real deployment no node's algorithm was ever set, the gateway assumed a 32-byte trailer, read the last 32 bytes of a 64-byte signature as framing, and rejected every frame. Reproduced: `frames_rejected=3`, `measurements_forwarded=0`, and no diagnostic anywhere. It reads as a dead radio, not a missing configuration call | Fixed. The trailer length that actually decoded the frame is now carried forward, instead of being discarded and re-looked-up in a dictionary that did not have the node - that discard-and-lookup was the bug, because `None` means "32-byte trailer". The gateway also reads algorithms from the central's own database, and always tries both lengths, so a node provisioned after startup is self-healing rather than permanently dropped. Guessing is asymmetric: a wrong length costs one rejected frame, while trying only the right one drops a node forever. A missing database degrades to zero known nodes rather than raising, since taking down a serving radio is worse than not knowing an algorithm. 6 tests |
| **Gateway forwarding loop**: frames to reassembly to `POST /v1/sync` to acknowledgement, driven against the real app and verified by reading rows back out of SQLite; noise counted, failures never acknowledged | 11 backend tests |
| **Acknowledgement pinned cross-language**: the exact bytes the gateway builds are asserted by the firmware parser and vice versa, so the two cannot drift apart while each still passes its own tests | 1 firmware + 1 backend test |
| **Identifier validation**: `site_id` restricted to `[A-Za-z0-9_-]` and 64 chars at every write endpoint, closing a stored-markup vector that the JSON API used to echo back; `nosniff` on every non-HTML response | 10 backend tests |
| **`node_id` had no shape check at all, so a node could sync under a spreadsheet formula**: `variable` and `sensor_id` were both validated against a character class because the dashboard renders them; `node_id` only had to be a non-empty string, and it is stored, rendered, exported and used as a filename. Confirmed against HEAD: `=cmd\|'/C calc'!A0` was accepted with a 200 and came out of `/v1/export-all.csv` unquoted in the first column, so an operator opening the export in a spreadsheet evaluated it | Fixed at all three doors - `/v1/sync`, `/v1/provision` and the LoRa relay - via a shared `identifiers.py`, because a check at two of three is a check waiting to be forgotten at the third and the third is a physical device on a radio. 422 `invalid_node_id`, distinct from `missing_node_id` |
| **CSV formula injection in both writers, on both sides of the link**: quoting a CSV field does not make it safe in a spreadsheet - Excel and LibreOffice evaluate a cell starting with `=`, `+`, `-` or `@` **inside** the quotes. RFC 4180 says nothing about this, which is exactly why the existing RFC 4180 test passed throughout. The leading whitespace case matters too: the spreadsheet trims it and evaluates what follows, so a naive first-character check misses `"  =1+1"` | Fixed in the firmware's `escapeCsvField` and in the central's new `_csv_cell`, by prefixing an apostrophe inside the quotes. It prefixes rather than refuses, because dropping the row would make an export quietly incomplete. Numbers are exempt - the `-` of `-1.5` is the same byte as the formula prefix, and the backend test caught that within a minute of the central-side fix landing. Leading whitespace is preserved and the guard goes immediately before the `=`. 6 firmware + 2 backend tests |
| **`haversine_m` measured the wrong distance across the antimeridian**: longitudes were subtracted without normalising, so (0,179) to (0,-179) - 222 km apart - produced a 358-degree separation and a distance of 39,875 km | A site pair across the date line would be grouped as being on opposite sides of the planet, in the control-vs-treatment comparison, with a finite plausible-looking number and no error | Fixed by wrapping both longitudes into [-180,180) *before* the difference. Normalising after the subtraction would not have worked: 181-179 is already 2, so the wrap never runs, which is why there is a test with an out-of-range input as well as one across the line. 1 of 26 new tests |
| **Three decision functions had no test naming them**: `classify_gap` (why a gap happened - `no_data` vs `measured_not_delivered` vs `clock_uncertain`, which demand different responses), `is_identity` (whether a calibration does anything, and every reporting path branches on it) and `haversine_m`. All three were defined, called and reachable from a report with nothing asserting their answers | A wrong answer here is a study result rather than a visible error | 26 tests. `classify_gap`'s precedence is asserted rather than assumed - clock uncertainty outranks a delivered batch, because if when the samples were taken is unknown then whether a batch fell inside the gap cannot be concluded from its arrival time. `is_identity` has no threshold, and there is a test saying why: calling 1e-9 identity would make a real correction invisible, which is the failure the `calibrated` key exists to prevent |
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
- Coverage accounting caps a window at **730 days** (`MAX_WINDOW_MS` in `coverage.py`), and
  separately at `MAX_BUCKETS` samples implied by the window and the expected interval. The
  bucket cap is the one that does the work: two years of hourly data is 17,520 buckets and
  cheap, and two years of per-second data is refused with an error that names the aggregates
  to read instead. The gap list is pageable (`gap_offset`, `gap_limit`, 200 per page), so a
  long window is bounded per response and complete across pages.
  **This entry said 400 days for a while, and it was wrong in a way worth recording:** 400
  appears in a docstring as an example of a window that is too wide, and I read the example as
  the limit. It is the second time a documented number in this file was wrong - after D1's
  `require_scope` counts - and both times the number had been read rather than measured. The
  release gate now compares the documented cap against the constant, so a changed limit cannot
  leave a stale figure behind.
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
  node's own `sync_device_key`, so the same secret that authorises firmware is the one
  that used to authenticate batches. **Certificate authentication narrows that**: a node
  with an Ed25519 key no longer authenticates with a shared secret at all, so the secret
  that authorises firmware is no longer a credential. An HMAC node still has the old
  exposure, and the manifest key should be separated from `device_key` outright.
  `docs/en/RUNBOOK.md` says so plainly rather than leaving it implied.
  **Done.** `nodes.manifest_key` is a separate column with its own algorithm field, set at
  provisioning and validated to the same minimum as the data key. The firmware gained
  `ota_manifest_key` in `NodeConfig` and prefers it. A node with neither still gets a valid
  signature from its data key **and a warning on both sides**, because refusing would strand
  every deployed node on its next update - trading a real exposure for a guaranteed outage. The
  derivation is unchanged, so a node is separated by setting a column, with no firmware change
  on the verifying side. Re-provisioning the data key no longer silently wipes the separation,
  which would have been the worst version of the feature: the column exists, the operator set
  it, and a routine rotation removed it. `tools/provision.py` generates the key per unit
  (`--no-manifest-key` to opt out) and refuses two units sharing one. 5 firmware + 9 backend
  tests.
- **Two subsystems had tests and no caller.** `Esp32PeerExchange` and the whole LoRa stack -
  driver, frame format, fragmentation, acknowledgement and forwarding loop - were reachable only
  from the test binary. A component with tests and no caller is not half-done; it exists only
  where tests run, and every test of it proves something about code no board reaches. The peer
  exchange is now constructed by `main.cpp` behind `peer_enabled` with an explicit
  `peer_channel`, and reports `PEER_EXCHANGE_ENABLED` or `PEER_EXCHANGE_UNAVAILABLE` at boot.
  **LoRa is now wired too**, and the way it became wired is the useful part: an audit of every
  interface with pure virtuals against every implementation found that `ISpiBus` and
  `IRadioControl` had exactly one implementor each, and it was a test fake. `Esp32SpiBus` and
  `Esp32RadioControl` in `cauce_hal` are the two files' worth of glue that was missing, and with
  them `main.cpp` constructs the radio, its `SyncManager` and its state, behind `lora_enabled`.
  The region is mapped from a *name* to a frequency in code rather than taken as a number from
  the config, because 868 and 915 differ enough that a typo is silent: the radio transmits and
  nobody nearby hears it.
- **A node presented a certificate it never verified.** `NodeAuthenticator` handed whatever JSON
  it was configured with to the central and relied entirely on the central to notice. What
  survives that: a certificate the CA genuinely signed *for a different node* passes every check
  the central makes, because the signature is valid. `CertificateVerifier` now checks the CA
  signature over the backend's canonical body, the node id, and the validity window, against a CA
  public key pinned in the node's own configuration - never the one inside the document, which
  would let any certificate name the key that verifies it. Revocation is *not* checked here, and
  that is stated rather than implied: only the central can know, and it refuses revoked
  certificates regardless of what the node believes.
- ESP-NOW peer-to-peer: the driver now exists and compiles for `esp32dev`, and the
  on-air frame format is host-tested (17 tests: round trip, truncation at every
  length, every single-bit flip, foreign magic, unknown version, a count that
  contradicts the length, an unterminated node id). What no host test can reach is
  the radio. Discovery answers an announcement broadcast rather than using **mDNS**,
  which remains uncompiled.
  The arithmetic is what limits this link, and the driver states it: an ESP-NOW
  payload is 250 bytes and a record costs 59, so one frame carries **three** records
  - one node's minute of data at the default sampling interval. That is a link for a
  few nodes on one site, not a mesh.

  **The exchange loop now exists** as `Esp32PeerExchange`, implementing the per-peer
  discovery, nothing drains a received frame into the merge, and `main.cpp`
  constructs no peer radio. The driver and the frame are now reachable from
  `main.cpp` by anyone who wants them - a real difference from a component nobody can
  call - but it is not a working peer link, and calling it one would be the false
  positive this file keeps recording.

  The three things that were missing, and what happened to each:

  1. ~~A per-`(node_id, sequence)` presence check in the storage index.~~ **DONE:**
     `IStorageRepository::containsRecord(nodeId, sequence, afterSequenceHint)` and its
     `LogStorageRepository` implementation - newest segment first, newest frame first within a
     segment, with the caller's per-peer watermark as a short-circuit so the scan is bounded by
     how far behind the peer is rather than by store size. 12 tests. `mergeRecords` can answer
     "already held?" for a record from another node for the first time. `mergeRecords` takes
     an `apply` callback whose entire job is to answer "does the replica already hold this?", and
     `LogStorageRepository` can answer it only for the *local* node, via `lastSequence()`. The
     alternative is a full scan per received record - thousands of records for every frame on a
     512 KB store - and the merge's own docstring says why that shape is wrong: a shadow set
     drifts from real storage and then reports merges as duplicates that were never stored.
  2. ~~The variable conversion is lossy.~~ **DONE:** `parseVariable`, with an unrecognised
     name becoming `Unknown` and the measurement still stored. Asserted in both directions:
     a known name round-trips, an unknown one keeps the value and loses only the meaning. A
     peer's record is stored under `sensorId=peer`, not this node's sensor, so a peer's
     measurement is never attributed to this node's hardware.
  3. **No per-peer watermark.** Reconnecting to a peer means knowing how far this node got with
     *that* peer, which is per-peer state nothing currently stores.
- A **signed downlink that has never been observed arriving**. The command path exists
  end to end and is host-tested; the bench now reaches the batch-building and
  credential-loading stages of the same path on real hardware, but no command has been
  seen to complete a round trip to a central and back.
- TLS still relies on the shared admin token for API access. A node **certificate**
  now exists - `CAUCE_CA_KEY` signs a binding of node identity to public key with an
  expiry, and a verifier needs only the CA public key - and a node now **presents it**
  to `/v1/sync` as proof of possession rather than a shared secret. It is still not
  mutual TLS at the transport layer: Caddy terminates TLS with one certificate and the
  node's credential is an application-level header. What that leaves open is
  server-to-client authentication and channel binding - the node proves who it is, but
  over a channel whose peer it identified by DNS name. Closing that means the firmware
  holding and rotating a certificate rather than a seed, and Caddy verifying the client
  against `CAUCE_CA_KEY`. Not small, and not pretending to be done.

## Roadmap: what is left, and why

Ordered by what unblocks the most, with the honest reason each item exists. This
replaces the old 400-day coverage note, which is resolved: the window guard now
bounds implied sample count as well as span.

### D1 - capability scopes on every endpoint (DONE)

`security.require_scope` resolves the caller from `api_tokens` and enforces
`read`/`write`/`admin` plus optional site scoping. This section used to say no
endpoint called it and that the feature was therefore inert. That was wrong and
contradicted the table above in the same file.

The corrected counts, re-measured rather than remembered, because the first
correction got them wrong in the other direction and the file carried *that*
for a round: **25 `require_scope` call sites and 20 direct
`require_bearer_token` calls across `cauce_server`**, which has 65 endpoints
across 11 router modules. The `require_bearer_token` sites are on read paths and
CSV exports, where a token check with no scope to enforce is the correct
instrument; the write paths call `require_scope`.

`api.py` on its own is 18 endpoints, 15 `require_scope` and 2
`require_bearer_token` - which is why quoting a per-file number here was
misleading even when it was right about one file.

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

**LoRa transport wiring: closed, and the reason it was still listed is now recorded.**

`LoRaSyncTransport` does take the algorithm - `setFrameAlgorithm` plus `setDeviceKey`,
with `hasUsableAlgorithm()` so a misconfiguration is visible before the first transmission
rather than as a central silently dropping every frame. `signFrame` dispatches on it and
refuses any Ed25519 seed that is not exactly 32 bytes. Both directions are covered by
tests.

What was genuinely missing is narrower and now stated plainly: **no product firmware
constructs the LoRa transport at all.** `main.cpp` has no `LoRaSyncTransport` and no
`setFrameAlgorithm` call, because there is no LoRa radio wired into a node yet - Phase 2 is
unstarted and the SX1276 has never been on a board. So there was no wiring to complete: a
node cannot choose an algorithm for a transport it does not have.

That is a Phase 2 item, not a Phase 1 one, and moving a sentence to make C2 look finished
would have been the wrong trade. What C2 delivers is verified field arithmetic, a verified
scalar ladder, and a signing entry point the host suite exercises end to end.

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
| 1 - software gaps | Ed25519 in the transport, downlink actuation and exercised backup/restore **done**. Calibration procedure and uncertainty budget **done** (`backend/tools/calibrate.py` executes the documented fit and evaluates the acceptance limit; 15 tests). Coverage gap paging **done** (`gap_offset`/`gap_limit`). Node certificates **done** - `CAUCE_CA_KEY` signs a binding of identity to public key with an expiry, verifiable with the CA public key alone, and a node now presents it to `/v1/sync` as proof of possession instead of a shared secret (80 tests). Still not mutual TLS at the transport layer Test binary isolation partially done - the registry and gate exist and mutations are adopted, but no suite shares a filesystem so the directory half has no callers. |
| 2 - air interface | Driver written and host-tested after three real register bugs were found and fixed; no board, no measured link budget. See the SX1276 row above. **The flash artifacts a board would be flashed with are now validated** — esptool parses the bootloader and application, the partition table has two non-overlapping sector-aligned slots, and the image fills 56.8% of one. That covers everything checkable without silicon and nothing more: a register map that is correct on paper and wrong on a chip is still wrong. |
| 3 - bench | Not started. Needs a board and `docs/en/BENCH_PLAN.md`. |
| 4 - manufacturing | Provisioning tool and identity retirement **done**. Factory self-test **done**: `bench_main.cpp` runs sensor read with a plausibility range, config, storage append/reopen with a value check, `listFiles` termination, provisioning, and the signed-batch path, ending in a `FACTORY_RESULT` line for a line-side script. Stages a bare unit cannot perform report `SKIP`, not `FAIL` - requiring a signed sync with no server would fail every unit for a reason unrelated to the unit, and a test everyone ignores is worse than none. An enclosure does not exist, and no unit has yet been run through it. |
| 5 - release engineering | SBOM tool (scoped to the real dependency closure, no longer a listing of the build machine), release gate (`.sh` and `.ps1`) and runbook **done**. Lockfile with hashes **done** (`backend/requirements.lock`, CI installs with `--require-hashes`). The image now installs that lock, which it previously ignored: `backend/Dockerfile` copied `requirements.txt` and resolved fresh versions at build time, so the pinned closure in the repository was documentation. The base image is a required `ARG BASE_IMAGE` with no default, so a build cannot pick a floating tag, and the gate fails a release whose Dockerfile does not. Gate also checks semver, that the tag matches the version the application reports, and installs the dependency closure in a throwaway venv to prove the image's central claim rather than assert it. `v0.1.0` is tagged - annotated, and `0.1.0` deliberately rather than `1.0.0`, because the specification is not frozen, phases 0 and 2 are unstarted, and no board has been on a desk. **A fresh clone of it passes the full gate: 30 checks, 0 failures**, which is the check that matters and the one that found row 12b below. The base image is pinned to a **multi-platform index digest**, resolved from the registry rather than composed from memory, and recorded in both the Dockerfile and the gate so the two cannot drift. Index rather than per-architecture, because the pilot Raspberry Pis are arm64 and an amd64 manifest digest would fail there rather than fall back.

**The image is built, run and reproducible.** `scripts/verify-image.ps1` builds it with podman - rootless, no daemon, no elevation - and establishes, in order:

| | result |
|---|---|
| the pinned digest pulls | yes |
| the index carries `linux/arm64`, not just amd64 | yes - checked from the manifest, because a base can be republished for one architecture only and the digest would still resolve |
| the image builds from the real Dockerfile | 197 MB, 9 steps |
| `WORKDIR /app` + the `COPY` layers produce a servable filesystem | `cauce_server` imports from `/app` |
| the declared `CMD` serves, over real HTTP | `GET /healthz` → 200, version 0.1.0 |
| writes are refused without an admin token | 503, in the artifact rather than only on the host |
| the backend suite passes **inside the image** | 540 passed |
| two builds of one tree produce one image ID | identical, `8a25ba91e2b6…` |

That last row is the claim no lock file can make on its own. A lock proves the inputs are the
same; an identical image ID proves the output is too. The suite running inside the image is
also a different claim from the suite passing on a developer machine, and it is the one that
would catch a dependency the lock omits.

Wired into the release gate, which passes. What it does **not** establish: the arm64 image has
never been executed, only its presence in the index confirmed - this machine is amd64. A
Raspberry Pi build is a one-line `podman build --platform linux/arm64` away and still needs a
Pi. |
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

**What this is worth.** 461 firmware tests pass, the ESP32 cross-build is clean, and
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
| 3 | MED | OTA download is synchronous/blocking, dashboard down during update | First real OTA | **Largely done, and one claim in the previous version of this row was false.** Done: chunked reader with tri-state `ReadStatus`, no per-chunk sleep, `OTA_STALLED` guard, and a `FeedFn` hook wired to `esp_task_wdt_reset` around the two blocking calls, asserted by *ordering* (the fakes stamp the feed count at entry) because a single feed at the top of `tick()` would satisfy a count and still leave the bug. **`IFirmwareReader::open` no longer calls `GET()`** — `GET()` sends the request *and reads the entire body into a String*, and the body here is a 1 MB image, so the "open" blocked for the entire download and then `read()` re-read bytes already in memory. It now uses `sendRequest`, which reads only the status line and headers, so the transfer is bounded by the socket timeout instead of by the size of the image. **The previous version of this row said "WDT is 30 s"; that is not what the firmware had.** `esp_task_wdt_init(30, true)` fails with `ESP_ERR_INVALID_STATE` because the Arduino core initialises the task WDT in `initArduino()` and subscribes loopTask, and this IDF (4.4) has no `esp_task_wdt_reconfigure`, so the timeout could not be changed. The effective value was `CONFIG_ESP_TASK_WDT_TIMEOUT_S` = **5 s**, unchecked. Now deinit-then-init is attempted and the result printed, so the board says which timeout it is running under. Also worth recording: `HTTPClient::begin()` is pure string parsing with no DNS and no socket — treating it as the blocking step sent the original investigation to the wrong place |
| 4 | MED | `LogStorageRepository` open() O(bytes) - **CK01 checkpoint implemented** (fast-path O(segments) + fallback scan; flush every 64 appends) | Storage budget >512 KiB | Done |
| 5 | MED | Backend `_RATE` in-memory; `sync_batches` unbounded; `/v1/nodes` unpaged | Central growth | Done: SQLite-backed rate limit, retention cap 5000, pagination `limit`/`offset` plus opaque cursors |
| 6 | LOW | `Logger::eventf` fixed buffers (silent truncation) - **increased to 512 B** | Long messages | Done |
| 7 | LOW | Unity single binary, cross-suite isolation via dedicated data dirs | - | Partially done. The declaration registry and the end-of-run gate are implemented and tested. **Mutations are now genuinely adopted**, but only just: this row claimed adoption while *nothing called* `TEST_SUITE_MUTATES`. The three file-scope mutable groups (`test_commands.cpp`, `test_ota.cpp`, `test_sync.cpp`) are now declared - from `test_main.cpp`, not from inside each suite, because a reader scanning the run order should see the shared state of a 28-suite single binary on one screen rather than only in the files whose author remembered. The **directory half still has no callers**, and that remains accurate rather than unfinished: no suite shares a filesystem, since each one that writes files builds its own in-memory or temporary store, so a claim would be a false positive rather than a finding |
| 8 | LOW | `Esp32LittleFs::listFiles` core-version sensitivity (`entry.name()` v2 vs v3) | arduino-esp32 upgrade | Done: portable shim (`String(entry.name())`, skip dirs and dot entries); ESP32 compiles |
| 9 | LOW | Long-window analytics over raw rows | Windows > 90 days on a node | Done: `granularity=auto` reads `agg_hourly` past 7 days and coverage gap detection is a SQL window function (`LAG`), so no Python row loop remains |
| 11 | MED | A provisioned public key proved possession but nothing established that the central vouched for it: `nodes.device_key` is a row an operator wrote with the admin token | Adding a node and rotating a compromised key were the same operation with the same blast radius, and a key compromised once stayed compromised | **Done.** `CAUCE_CA_KEY` holds an Ed25519 seed for a CA, configured separately from the admin token, and signs a certificate binding node identity to public key with a serial and an expiry. The property that matters: a verifier holding **only the CA public key** checks a node with no database and no admin token, which the shared token cannot do at all. Fails closed - with no CA key every endpoint is `503`, because issuing documents signed by nothing would look authoritative and verify against no key. The key comes from `nodes.device_key` and never from the request; an HMAC node is refused (409) since publishing a shared secret to verifiers is worse than no certificate. Rotation replaces rather than accumulates, keeping the retired row because a table that destroys the old certificate cannot answer which key a node held when a measurement arrived. 25 unit + 16 endpoint tests |
| 12r | HIGH | **A node presented a certificate it never checked** | The central verifies the CA signature, so a forged certificate is caught - but that defence is on the other side of the wire. A certificate the CA genuinely signed *for a different node* passes every central check the central makes, because the signature is valid; the only thing that would catch it is the holder noticing it is not about them | `CertificateVerifier`, in `cauce_app`. The CA signature is checked first against a key pinned in `NodeConfig`, because `ca_public_key` travels *inside* the document it describes and trusting it would let any certificate name the key that verifies it. Then the node id, then the validity window - separately from the signature, since "expired" and "forged" are different problems and a node with a wrong clock needs to hear about the clock. Not checked: revocation, which only the central can know, and which still protects the system because the central refuses it regardless. 14 tests, against a certificate the real backend CA issued |
| 12s | HIGH | **Two whole subsystems had tests and no caller** | `Esp32PeerExchange` and the entire LoRa stack - driver, frame format, fragmentation, acknowledgement, forwarding loop - were reachable only from the test binary. A component with tests and no caller is not half-done; it is a component that exists only where tests run, and every test of it proves something about code no board ever reaches | The peer exchange is now built by `main.cpp` behind `peer_enabled` with an explicit `peer_channel`, and says at boot whether it came up. **LoRa still has no caller**, because `Sx1276Radio` needs `ISpiBus` and `IRadioControl` and neither has an ESP32 implementation - the construction was left out rather than written and commented, because a commented-out block reads as a decision |
| 12t | MED | **Three off-by-N buffer sizes in one new parser, each making every verification report `Malformed`** | `jsonField` read only quoted values, so `version` (a bare number) failed - the same bug class as `jsonUintField` refusing a string. Then `value[64]` for a 64-character `public_key` hex, one byte short of the terminator. Then `signatureHex[80]` for a 128-character Ed25519 signature, half of what it needed. Eight tests failing identically, three causes | All three fixed, and the parser now reads quoted values and bare numbers in one place rather than two functions that each assume a shape. Worth recording because a buffer one byte short does not crash on this target - it returns false, and false is indistinguishable from "this certificate is not yours" |
| 12u | HIGH | **The entire LoRa stack was reachable only from the test binary** | `Sx1276Radio` takes an `ISpiBus` and an `IRadioControl` so the driver never calls Arduino and the host suite can drive it with fakes. Those two interfaces had exactly one implementor each, and it was a test fake. Driver, frame format, fragmentation, acknowledgement and forwarding loop: all real, all tested, none reachable from a node | An audit of every interface with pure virtuals against every implementation found it in one pass - the same class of gap that hid the certificate exchange and the peer exchange. `Esp32SpiBus` and `Esp32RadioControl` are the missing glue: SPI is a byte-in-byte-out transfer with a chip select, and the control surface is five GPIO calls and a clock. `main.cpp` now builds the radio, its `SyncManager` and its state behind `lora_enabled`, and reports at boot whether it came up. The region is mapped from a name to a frequency **in code** rather than taken as a number from the config, because a typo there is silent - the radio transmits and nobody nearby hears it |
| 12v | HIGH | **`used += snprintf(out + used, capacity - used, ...)` writes past the buffer** | `snprintf` returns the length it *would* have written on truncation, so `used` grows past `capacity`; `capacity - used` then wraps - both `size_t` - into a length of nearly 4 GB. CodeQL flagged it twice. Nothing crashes on this target: the symptom is a corrupted adjacent member, which in a struct of counters is a number that is wrong rather than a fault that is loud | Every append goes through `appendFormatted`/`appendChar`, which refuse rather than compute: a result that does not fit ends the verification as `Malformed` rather than corrupting anything. The obvious fix - a bigger buffer - is the wrong one, because the bug is the arithmetic and a bigger buffer only moves where it shows up. 2 tests feeding certificates whose fields cannot fit in 512 bytes, which is the case the original corrupted memory on |
| 12c | HIGH | **The ESP32 flash artifacts were never validated, only compiled.** `pio run` producing a file says nothing about whether a board would accept it | A bootloader with a bad checksum, a partition table with one application slot instead of two, or an image larger than its slot all flash silently and fail at boot — the OTA rollback design provides no rollback because there is no second slot to roll back to | **Done.** `scripts/verify-firmware-image.ps1` runs esptool's `image_info` over the bootloader and the application (checksum **and** validation hash), and `scripts/partition_report.py` parses the table. Checks: the magic, two application slots, no overlap, sector alignment, every declared partition present in the binary, and the image against the declared slot. Current artifacts: bootloader checksum `6b` valid, application checksum `30` valid, entry point `40082e28`, **1090 KB filling 56.8% of the 1920 KB slot**. Artifact hashes recorded in `firmware/artifacts.sha256`. Wired into the gate |
| 12e | HIGH | **A node authenticated to the central with a shared secret.** `/v1/sync` verified an HMAC keyed by `nodes.device_key`, which is symmetric: every node holding that key can authenticate *as* that node, and the central stores a secret that both authenticates and forges | Compromise of any one node's key impersonates that node. Rotation is all-or-nothing, and there is no third party who can check a node's identity without holding the admin token | **Done for Ed25519 nodes.** `POST /v1/sync/challenge` issues a single-use 60-second nonce; the node signs it with the private key the central has only ever seen the public half of, and the central checks four things in a fixed order: the nonce, the CA's signature over the certificate, that the certificate names *this* node, that the certificate's key is the one registered (so a rotation actually closes the window), and finally the challenge signature. 22 unit + 17 endpoint tests |
| 12g | MED | **No node implemented the certificate exchange**, so the scheme the central had supported for two rounds reached no hardware | The central could authenticate a node by proof of possession and nothing on a node could answer, which meant the feature existed only in the deployment that runs on a laptop | **Done.** `NodeAuthenticator` in `cauce_app`: fetches the challenge, signs `sign_this` with the node's Ed25519 seed, and emits the four headers. The exact bytes to sign are stored verbatim from the central's answer rather than rebuilt, so the encoding has one owner instead of two that can drift. 20 firmware tests |
| 12h | MED | **The canonical challenge encoding is a cross-language contract and I broke it three ways in its own test fixture** | Both sides must produce byte-identical `sign_this`; a mismatch produces an invalid-signature error at the central that looks like a key problem, not a formatting one | Pinned on both sides — `test_the_canonical_challenge_matches_the_backends_format` and the backend's `test_the_canonical_challenge_is_stable`. The fixture had `\n` inside a **C++ raw string**, where it is a backslash followed by `n` and not a newline; the authenticator then correctly signed the literal backslashes and verification failed, which presented as broken signing. Also `ed25519PublicKeyFromSeed(out, seed)` called as `(seed, out)`, which silently produced a garbage key and gave the identical symptom. Two unrelated mistakes, same failure |
### The trap in row 12j, and why it was found at all

I got to the parser by breaking something else. Row 12h had me hand-edit C++ string literals
that contain `\n`, and I did it with a scripted search-and-replace rather than by typing the
new text. Once inside that code I read the parser closely enough to notice its comment
claimed the central emits no escapes, checked, and found it emitted three per challenge.

Two independent encoding mistakes, one uncovered while fixing the other. Neither would have
been caught by a test written from my belief about the format - only by asking the other
language what it actually puts on the wire.
| 12j | HIGH | **The firmware's JSON parser refused every escape, and a comment asserted the central emitted none** | `json.dumps` writes the canonical string's three newlines as three `\n` pairs on the wire. Every real challenge would have failed to parse, so a node on hardware would return `NoChallenge` on every sync, forever - while 345 host tests passed, because every fixture had been written by hand to match the parser it was testing. Both sides were perfectly consistent and jointly wrong | Decoded exactly the escapes `json.dumps` emits (`\n`, `\t`, `\r`, `\"`, `\\`) and refused the rest, including every `\uXXXX`: a value this module cannot decode faithfully must not become a string the node then signs. The failure mode matters - a bad escape fails here as `NoChallenge`, which points at the challenge, whereas decoding `\u0041` to `A` would sign bytes the central never wrote and fail there as an invalid signature, which points at the key. Now pinned to real bytes from both ends: `test_the_parser_reads_the_escapes_python_actually_emits`, and the backend's `test_the_serialised_form_is_still_what_dumps_produces` asserts against `json.dumps` itself rather than a copy of its output, so a change to the endpoint's spacing or escaping fails with the cause named |
| 12n | HIGH | **The gate was not reentrant, and the cause was two hashes inside the firmware image** | `verify-firmware-image.ps1` rewrote the committed `firmware/artifacts.sha256` with the hashes it had just built. In a fresh clone that made the first run dirty the tree, so the second run failed at step 1 on "uncommitted changes" — for a reason with nothing to do with the code. The committed hash was not stale: the image genuinely differs | `firmware.bin` embeds `app_elf_sha256` at offset 0xB0 and a SHA-256 image digest at the tail, and the linker takes the ELF hash over a file whose debug sections carry absolute source paths. Two clones of one commit built at different paths differ in **65 of 1,115,984 bytes** — those 64 and one length byte — and in nothing else; a rebuild in place is bit-identical. The container image is path-independent, the firmware image is not. `artifacts.sha256` is now compared and reported, never rewritten: `bootloader.bin` and `partitions.bin` must match exactly, `firmware.bin` differing is reported as the expected path-dependence rather than silently accepted or failed |
| 12p | HIGH | **The ESP32 transport could not be tested, because it is an `#ifdef ESP32` file, and my first fix made it worse** | The certificate exchange existed as `NodeAuthenticator` with 20 tests and no integration: the four header names lived in two files and neither was under test. The obvious fix - give `cauce_hal` an authenticator and a challenge source - **closed a dependency cycle** and broke the ESP32 build outright (`cauce_hal` could no longer reach `cauce/core/Ed25519Points.h`). The second attempt got as far as passing `HTTPClient` where an `IHeaderSink` was expected, which is a plausible-looking mistake: `HTTPClient` has an `addHeader`, but its signature takes `String`s, and HTTPClient is a *caller* of a sink, not one | Inverted the dependency instead of closing it. `cauce_hal` owns `IAuthHeaderSource` and the four header names - it is what puts them on the wire, so it should know them - and `cauce_app` supplies `NodeCertificateAuth`, which already depends on `cauce_hal` and can therefore implement it. One direction, no cycle. The `HTTPClient` gap is five lines of adapter, and the alternative signature would have dragged `Arduino.h` into a library the host suite links. **8 tests**, including the header names asserted against the central's spelling, and that any failure emits *nothing* |
| 12q | HIGH | **Falling back to HMAC is only safe because failure emits nothing** | The central commits to certificate auth the moment it sees `X-Cauce-Certificate` and will not fall back to the shared secret. A partial header set would earn a rejection where HMAC would have worked, so the fallback path - which is the normal path when the central is unreachable - would be a silent downgrade into guaranteed failure | `addAuthHeaders` returns false and emits zero headers on *any* failure, and the transport ignores the return value. Asserted directly: an unreachable central and an unusable answer both leave the sink at zero headers, and a second call after a successful one does not re-emit the first one's buffers - which is a stale signature for a spent nonce, the exact failure that assertion catches |
| 12o | MED | **Two SHA-256 clusters', and I had to disassemble the image to find out which fields they were** | Byte-comparing two builds says "65 bytes differ". Saying which fields those are, and why, needs the image layout — `app_elf_sha256` lives in the `esp_app_desc_t` at 0xB0, and the appended digest is at the tail. Without that, the honest options were to guess or to declare the difference unexplained | Clustered the differing offsets and printed them as two ranges (176..207 and the last 34 bytes), then decoded the surrounding bytes as ASCII to see they were digests, not code. The `readable` framing is the difference between "the build is not reproducible" and "the build is reproducible except for two self-referential digests that depend on the checkout path" || 12m | LOW | **My own new check was wrong twice before it worked** | The first version applied a firmware-flavoured pattern to the backend count, demanding a README line reading "541 firmware", which no line has ever said. The second got the word order backwards: the documents say "461 firmware + 575 backend", and the pattern asked for "firmware 461". Both versions failed loudly, which is the one thing that made them findable | One pattern, both numbers, in the order the docs actually phrase it. Verified by reverting the README to `345 firmware + 540 backend` and watching all four documents report stale, then restoring it. A check that has only been observed to pass is not yet evidence that it works; the failing case is the evidence |
| 12l | LOW | **Every documented test total was prose, and prose drifts** | The suite went 325 → 345 → 461 across three commits while the README, both language READMEs, STATUS, the thesis and `verify-all.ps1`'s own success line kept claiming the old number. Every test passed throughout; the totals were simply wrong, and nothing could say so. A release whose headline figure is unchecked is a release whose headline figure is decoration | `release-gate.ps1` now reads the counts back out of `verify-all.ps1`'s output and requires the README to quote them, so a stale number fails the gate instead of shipping. Proven by deliberately reverting the README to `345 firmware + 540 backend` and watching the check reject it, not by reading the code and agreeing it would work |
| 12k | MED | **A test that pins my own fixture pins nothing** | The two host fixtures above agreed with the parser because I wrote both, and the parser was wrong in a way only the server's real output could reveal. The lesson is narrower than "write better tests": a fixture must be produced by the *other* side, or it encodes my assumption rather than the contract | `test_node_auth_wire.cpp` carries the literal `json.dumps` output, copied with its spaces after the colons, and the backend test asserts against `json.dumps` live. A fixture generated by running the backend would pass by construction instead |
| 12i | LOW | `jsonUintField` would have accepted `0x10` and `+5`, which `strtoull` takes and JSON does not | An expiry read as sixteen instead of ten signs successfully over bytes the central never issued, and fails as a signature error rather than as a parse error | Digits only, with a trailing-character check. `test_a_numeric_field_refuses_hex_and_signs` |
| 12f | HIGH | **A node presenting a certificate could have fallen back to the shared secret**, which would have left the weaker scheme permanently available and made the stronger one decorative | An attacker holding one node's HMAC key would present it and skip the certificate entirely — every signature real, nothing checked. The scheme would have looked implemented and enforced nothing | **Not offered.** Presenting *any* certificate header commits the request to certificate auth; half the evidence is not half the decision, so a certificate with no nonce is `certificate_authentication_incomplete` rather than an HMAC request. Tested with the exact shape an attacker would send: a valid HMAC signature *and* a broken certificate. The HMAC path is unchanged for nodes provisioned before certificates existed, which is the other half of the trade |
| 12d | MED | **My partition parser had four wrong layouts before the right one, and every one printed output that read like a corrupt artifact rather than a parser with a bug** | A checker that reports "this file is broken" when it has merely picked the wrong offset sends somebody to hunt a problem that is not there — and a checker nobody trusts is worse than none | The layout is now documented from the artifact rather than from memory: a bare table image, no MD5 block, entries at `0x02` with a `0x20` stride, and — the one that cost the most — **the size field is in bytes, not sectors**. An earlier version multiplied by 4096 and reported `app0` as 7864320 KB, 7.8 GB, on a 4 MB part. What settled it was arithmetic against `firmware/partitions.csv`: a layout that reproduces nvs `0x9000/0x5000`, app0 `0x10000/0x1E0000` and the rest is the layout, however plausible the others looked |
| 12b | MED | **A fresh clone of the release tag could not pass its own release gate.** `backend/sbom/*.json` was marked `-diff linguist-generated=true` in `.gitattributes`, and `-diff` only suppresses the textual diff — it does not classify the file as non-text. With `core.autocrlf=true` the SBOM was checked out with CRLF, so every clone had a modified working tree and the gate's very first check failed | A released tag that fails its own gate is worse than no gate: the first thing a person cloning it does is discover the release is broken, and the plain "uncommitted changes" message sends them looking for a change they never made. Invisible from the machine that produced the tag, because that machine is Windows and autocrlf was already doing what the attribute asked | Fixed with `-text`, which is the attribute that actually means "do not normalise this". Found by cloning `v0.1.0` and running the gate **inside the clone** — the only way to see a defect that the producing machine's own configuration hides. The gate now also distinguishes line-ending dirt from real dirt and says which is which. `v0.1.0` was moved once to point at the fixed tree rather than shipping a `v0.1.1` for a packaging defect, and the tag message records that it moved. **A fresh clone of `v0.1.0` now passes the full gate: 30 checks, 0 failures** |
| 17 | HIGH | **The watchdog never ran at 30 s. The 30 in the source was a fiction, and both return values were discarded** | `esp_task_wdt_init(30, true)` returns `ESP_ERR_INVALID_STATE` because the Arduino core initialises the task WDT in `initArduino()` and subscribes `loopTask`; this IDF (4.4) has no `esp_task_wdt_reconfigure`, so the timeout could not be changed afterwards at all. The real value was `CONFIG_ESP_TASK_WDT_TIMEOUT_S` = **5 seconds**, and a 5 s margin is well inside what a cold LittleFS mount, a sensor probe and a web-server bind can take | A node that "does not boot at all" with `rst:0x8` in the log is exactly this. Now: deinit-then-init is attempted and the result is **printed**, including a line saying it fell back to 5 s, because an operator reading `WDT init=0 add=0 timeout_s=30` and an operator reading the same line when it failed are in completely different situations. And the board keeps the reason across the reset: `BootDiagnostics` writes the last step reached into `RTC_NOINIT_ATTR`, so the *next* boot prints `BOOT_HISTORY reattempt boots=N prev_step=K ordinal=M` before anything else competes for 115200 baud. Previously a reset loop left only a reset reason and the log scrolled past six identical boots. 13 host tests |
| 18 | MED | **A board in a reset loop produced no evidence, because the evidence died with the previous boot** | A watchdog reset destroys the serial log. What survives is whatever the bootloader prints: a reset reason, which says a watchdog fired and nothing about which step was running. The original symptom — reboots every ~9 s, last line `CONFIG_LOADED`, never `BOOT_COMPLETE` — was unreadable for exactly this reason | `BootDiagnostics` in `cauce_core`: a fixed-size, trivially copyable record with a magic, a schema version and an FNV-1a checksum, written to a caller-owned buffer. `begin()` reads the previous record and reports it; `beginStep`/`endStep` track position; `complete()` marks the boot finished so the *next* boot will not call it a re-attempt. A corrupt or future-schema record is **refused rather than half-trusted**, because a garbage record could claim any step ordinal. The step *names* are bounded at 24 while the ordinal keeps counting past that, since "how far did it get" is the useful number and clamping it makes 200 steps look like 24 |
| 19 | MED | **`GET()` is not "open the connection", and treating `begin()` as the blocking step sent the search the wrong way** | `HTTPClient::GET()` sends the request and reads the whole body into a String. `Esp32FirmwareReader::open` called it, so opening a 1 MB download blocked for the whole download and `read()` then re-read bytes already in memory — a second, quieter way for the same transfer to exhaust a few hundred KB of heap. Meanwhile `begin()` looks like the expensive call and is in fact pure string parsing | `sendRequest` reads only the status line and headers. `setReuse(false)` on the firmware transfer, because a keep-alive socket held across a flash erase is a resource the erase cannot reclaim and a stale connection only shows up on the *second* update. The comment in the source records why, so the next person does not re-add `GET()` believing it is cheaper |
| 20 | LOW | **A factory check needed a second firmware image, for checks a working node can already do** | `bench_main.cpp` is the honest place for sensor, radio and signed-sync. But it is a separate flash step and a separate bootloader state, and the failing unit is the one that will not boot the main image at all — so the factory line could not ask the question | Type `t` on a serial monitor on a running node. It probes the BME280 (with the plausibility bound in the assertion, because an absent bus reports a clean 0.00 that reads like a real number on a line screen), checks provisioning, and round-trips one record through storage including the reopen that a power cycle performs. Writes to `/factory_check`, never `/data`, so a failing check is unambiguous about what it did not damage. Prints the same `FACTORY_RESULT` line as the bench image || 12a | MED | The pinned closure was never demonstrated to work — only the base image digest was verified as resolvable, which says nothing about whether the code imports what the lock provides | A dependency the code imports but the lock omits passes CI and 500s in the image. `--require-hashes` cannot catch it: it verifies packages that *are* listed | **Done and measured twice.** `scripts/verify-lock.ps1` builds a throwaway venv, installs with `--require-hashes`, compares every installed version against the lock, imports the app with `PYTHONNOUSERSITE` so a user-site package cannot stand in for one the image would not have, and runs the suite on the host. Then `scripts/verify-image.ps1` does the stronger version — **540 passed inside the built image**, which is a different claim and the one that would catch the omission. Both wired into the gate. `backend/tests/test_container_contract.py` adds 6 standing tests, one asserting `ruff` is deliberately *absent* from the runtime lock so nobody "fixes" the omission |
| 12 | MED | `Ed25519PublicKey.from_public_bytes` accepts any 32 bytes, deferring the point check to verification time | A certificate could be issued for a key that can never verify anything, and the failure would appear on a node with the CA looking innocent | Fixed in `certificates.is_on_curve`: a `y` at or above p is non-canonical and rejected, and about half of all 32-byte values have no corresponding `x` and are refused at issuance. "Never works, much later" becomes "refused now". Also guarded the other direction, since a check that rejected every key would fail every provision in the fleet |
| 21 | MED | **The peer frame had no integrity check, and a bit flip produced a plausible wrong reading** | An ESP-NOW frame goes over an unencrypted radio and is parsed by a peer. Without a CRC, flipping one bit in the payload decoded cleanly into a measurement one ulp away from the true one, which then merged as real data. A test requiring every single-bit flip to be either refused or identical failed with **176 silent corruptions out of 664** — the format reporting honestly on itself | A CRC-32 over everything but the CRC field, verified before a single byte is read into the result. Documented as *not* a signature: it catches noise, it does nothing about a peer that chooses what to send, and both are needed. The same frame also had two more defects found by the same tests: the header constant was 22 against a 24-byte layout, so **every frame failed to decode**, and `kPeerFrameMaxRecords` was 3 while 49 bytes of the payload went unused — recorded in the source so nobody "fixes" the constant to match instead of asking why |
| 22 | MED | **STATUS.md claimed suite isolation declarations were adopted while nothing called the function** | Row 7 said "mutations are adopted — the three file-scope mutable statics declare themselves". `TEST_SUITE_MUTATES` had zero call sites. The header comment was honest about it; the inventory was not, which is worse, because the inventory is what people read | Declared, from `test_main.cpp` rather than from inside each suite: a reader scanning the run order should see the shared state of a 28-suite single binary on one screen, not only in the files whose author remembered. Row 7 now distinguishes the two halves truthfully — mutations are adopted, the directory half has no callers **because no suite shares a filesystem**, so a claim there would be a false positive rather than a finding |
| 23 | LOW | **D1 quoted counts that had been wrong in both directions** | D1 said "22 `require_scope` call sites and 19 `require_bearer_token`", after previously saying the feature was inert. Re-measured: **25 and 20 across `cauce_server`**, which has 65 endpoints across 11 router modules. Quoting a per-file number was misleading even when true about one file — `api.py` alone is 18 endpoints, 15 and 2 | The numbers are re-measured and the scope of each is stated. Three rounds of correcting this row is the argument for why the fix was re-measuring rather than rewriting the prose || 24 | MED | **The node's Ed25519 credential existed in a component but nowhere else - no config field, no wiring, no caller** | `NodeAuthenticator` had 20 tests and could sign a challenge, and `main.cpp` never constructed one. The whole certificate exchange existed on the central side, in a library, in tests, and in no configuration. A node that wanted to use it had no way to say so | `NodeConfig` gained `sync_auth_seed` (64 hex chars) and `sync_certificate`, both serialised and parsed. `main.cpp` decodes the seed, builds the authenticator, the bridge and `Esp32ChallengeSource`, and hands the bridge to the transport. The seed is **validated at parse time** against the shared `malformed > 3` tolerance and deliberately *not*: a 62-character seed is a provisioning typo, and forgiving it leaves a node that boots, looks correctly configured and fails every authentication as an invalid signature - pointing at the certificate rather than at the config line. 15 firmware tests. Half-configured (seed without certificate, or the reverse) is reported once and left on HMAC, because presenting no certificate commits the central to certificate auth and falls back to nothing |
| 25 | MED | **The ESP-NOW frame had no integrity check, a wrong header constant, and 49 unused bytes** | A bit flip decoded into a plausible wrong measurement: **176 silent corruptions of 664**. The header constant was 22 against a 24-byte layout, so *every* frame failed to decode - five tests, one cause. And `kPeerFrameMaxRecords` was 3 while 49 payload bytes went unused | CRC-32 over everything but the CRC, verified before a byte is read into the result, documented as *not* a signature. The frame uses explicit little-endian writers rather than a packed struct, so a host test means something. `receive()` returns 0 rather than buffering into a ring that would fill and silently drop the oldest frame. 17 tests || 13 | LOW | Test modules set `os.environ["CAUCE_DB_PATH"]` believing it selects their database; it does not, because `config.Settings` snapshots the environment once at import | Harmless for the path - isolation comes from `db.reset_for_tests()` - but the same pattern on a value nothing resets is a silent order-dependent failure | **Found while adding the certificate tests.** `test_cert_endpoint.py` set `CAUCE_CA_KEY` that way and passed alone, then failed 7 times in the full suite, because whichever module triggered the first `cauce_server` import decided the CA key for all of them. It now assigns `settings.ca_private_key` through an autouse fixture, and `tests/conftest.py` says why the per-module lines are decorative. The rule is written down: a setting nothing resets is set on `settings`, not on `os.environ` |
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
cd firmware && pio test -e native      # expect: 461 succeeded
pio run -e esp32dev                    # expect: SUCCESS
cd ..\backend && pip install --require-hashes -r requirements.lock
python -m pytest tests -q              # expect: 540 passed, 1 skipped
..\scripts\run-e2e.ps1                 # expect: E2E PASSED
```

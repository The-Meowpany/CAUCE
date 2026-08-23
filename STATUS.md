# CAUCE real system status

This document is the source of truth about what is implemented and what is
not. It overrides any aspirational claim elsewhere.

## Implemented and verified (97 firmware + 20 backend tests, E2E green)

| Component | Evidence |
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
| **Embedded API v1**: all 8 `/api/v1` endpoints, paginated streaming, Bearer→SHA-256 constant-time auth, fail-closed, 422 error lists, secret masking | 12 host router tests |
| **ESP32 HTTP transport** (`Esp32ApiServer` on WebServer, chunked) wired in `main.cpp` | Cross-compile SUCCESS — no board yet |
| **Sync client**: persistent watermark, idempotent `node_id+sequence` batches, exponential backoff, halt-on-reject | 8 tests |
| **Central backend** (FastAPI+SQLite): `/v1/sync` idempotent ingestion with honest acks, nodes/measurements queries, analytics summary/compare/before-after/period-compare/heat-events, optional token auth (constant-time), bounded rate limiting, sites & interventions | 22 pytest tests |
| **Local web dashboard** (embedded ~10KB SPA, dependency-free canvas chart, quality cards, 1h–7d ranges, export, admin config) with ES/EN switch + **captive portal DNS** | 2 HTML integrity tests; DNS compile-verified |
| **Central mini-dashboard** with Accept-Language localization (en/es/pt) + full-history CSV export | Localization test |
| **Scenario simulator CLI**: normal_day / heat_event / sensor_failure / network_outage / power_loss_recovery → CSV or direct `/v1/sync` feed | Real smoke: 5 nodes loaded into live backend |
| **OTA decision layer**: semver compare, manifest validation, streaming SHA-256 verification, heap/battery gates, alternate-partition anti-brick rules | 10 tests |
| Node↔server **E2E integration**: C++ SyncManager (real sockets) against live FastAPI; initial/incremental/idempotent-replay phases verified directly in SQLite | `scripts/run-e2e.ps1` — caught a real JSON serialization bug |
| CI workflow: native tests · ESP32 build · backend pytest · E2E job | `.github/workflows/ci.yml` |

## Exists but NOT yet validated on physical hardware

- Real BME280 reads via Wire (datasheet-driven, passes simulated-bus tests).
- LittleFS on real flash (mount/wear/power-cut).
- Wi-Fi radio (`Esp32WifiController` implementing the tested FSM interface).
- OTA actual flash write + boot-counter rollback confirmation.
- NTP time source.

## Not implemented yet

- Geographic map in central dashboard (current one is a dependency-free
  table; map deferred to avoid proprietary tile dependencies).
- Deep sleep application (policy evaluator shipped; application requires bench).

## Known technical debt

1. `Logger::eventf` fixed buffers (224 B) — fine for pilot scale.
2. `LogStorageRepository` opens with sequential scan — fine to ~10⁵ records.
3. Single Unity binary for all suites — state isolation via dedicated data dirs.
4. Historical docs partially Spanish — English migration tracked in git history;
   core docs already translated.
5. Windows needs MinGW-w64 on PATH for the `native` env; CI uses Linux containers.

## Reproduce from zero

```powershell
pip install platformio
winget install BrechtSanders.WinLibs.POSIX.UCRT   # or any MinGW-w64 ≥ GCC 9
cd firmware && pio test -e native      # expect: 97 succeeded
pio run -e esp32dev                    # expect: SUCCESS
cd ..\backend && pip install -r requirements.txt
python -m pytest tests -q              # expect: 22 passed
..\scripts\run-e2e.ps1                 # expect: E2E PASSED
```

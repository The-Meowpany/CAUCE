# CAUCE

*Leer en [español](README.es.md).*

**Microclimate observation that keeps working when the network doesn't.**

`MEASURE → VALIDATE → STORE → SYNC → COMPARE → VISUALIZE`

The system has two parts:

- **Node** — an ESP32 microstation. It reads a BME280, checks every
  sample (range, rate of change, stuck sensor, duplicates), stores it in
  an append-only log with CRCs, serves a small API and dashboard, and
  syncs signed batches whenever it gets connectivity. It also decides
  about firmware updates, though it can't flash itself yet.
- **Central** — a FastAPI + SQLite server. It takes in batches without
  ever duplicating them, answers honestly about what it actually stored,
  and offers basic analytics, sites and interventions, timestamp repair
  for clockless records, and a dashboard in three languages.

One thing we say upfront: our sensors aren't certified and we haven't
measured system accuracy in the field. What we give you is comparable
data with the uncertainty shown, not official meteorology.

## Quickstart

```powershell
cd firmware
pio test -e native          # 420 host tests (Unity)
pio run -e esp32dev         # ESP32 build
cd ..\backend
pip install -r requirements.txt
python -m pytest tests -q   # 540 tests
..\scripts\run-e2e.ps1      # C++ node → FastAPI → SQLite (3 phases)
..\scripts\verify-all.ps1   # everything above in one gate
```

To size the pilot before you field it:

```powershell
python simulator\load_pilot.py --nodes 8 --days 60 --dry-run
python simulator\load_pilot.py --nodes 8 --days 60 --sync-url http://localhost:8000/v1/sync
```

To see the node UI, join its Wi-Fi and open `http://192.168.4.1/`
(Spanish/English, no frameworks, works from flash). The central
dashboard lives at `http://<server>:8000/` and follows your browser's
`Accept-Language` (English/Spanish/Portuguese).

About sync auth: provisioned nodes sign each batch with
`X-CAUCE-Node` + `X-CAUCE-Signature`. If a node was never provisioned
and `CAUCE_SYNC_REQUIRE_AUTH=1`, the server answers 503 instead of
guessing. Node admin works with `Authorization: Bearer` checked against
a stored SHA-256 in constant time; with no token configured, admin
writes are refused outright.

The central behaves the same way, with one deliberate exception.
`CAUCE_API_TOKEN` unset means every **write** answers `503
admin_api_not_configured`, because there is no credential to check and
allowing it would leave the port open to anyone who can reach it. Reads
stay open so the dashboard works on a trusted LAN; setting the token
closes them too. Generate one with:

```powershell
python -c "import secrets; print(secrets.token_urlsafe(32))"
```

See `backend/.env.example` for the full environment.

## Node API (`/api/v1`)

```
GET  /node                         identity, versions, location
GET  /status                       FSM states, clock, uptime, latest
GET  /measurements/latest          last stored measurement
GET  /measurements?from=&to=       streaming JSON series (INCLUSIVE ends)
GET  /health                       counters, storage, failures
GET  /diagnostics                  field bundle (cauce.diag/1), signed and
                                   POST-able to the central for triage
GET  /config                       active config (secrets never exposed)
POST /config                       KV body, admin token required
GET  /export?format=csv|json       streaming export
```

## Central API (`/v1`)

```
POST /provision                    admin-gated device-key registration
POST /sync                         idempotent batch ingest (transport: wifi|lora)
GET  /nodes  /nodes/{id}           list / detail + latest
GET  /nodes/{id}/measurements      filterable series (limit…10000)
GET  /nodes/{id}/coverage          expected vs received, gaps with reasons
GET  /nodes/{id}/diagnostics       last field bundle (POST to ingest one)
POST /v1/sites  PUT /nodes/{id}/site
PUT /v1/sites/{id}/control         flag an untreated control site
POST /v1/interventions             registry (end_before_start→422)
GET  /analytics/summary|compare|before-after|period-compare|heat-events|summary-fast
POST /nodes/{id}/time-reconstruct  backfill time-uncertain records (flagged, caveated)
GET  /fleet                        per-node state, flags and needs_visit
GET  /maintenance/retention        retention config + last run
GET  /export-all.csv               full-history stream
GET  /system                       fleet triage, coverage, retention state
GET  /                            localized dashboard
```

## Known limitations

We'd rather list these here than have you trip over them:

- The physical bench (power cuts, radio soak, 14-day runs) hasn't
  happened yet. `docs/en/BENCH_PLAN.md` describes exactly what it covers.
- No TLS. Don't expose the central outside a trusted LAN without a
  reverse proxy in front.
- BME280 numbers are datasheet-grade. Nobody in this project has verified
  them in the field, and whole-system accuracy is unmeasured.
- Eight spread-out nodes produce a smooth interpolated field, not street
  resolution. The map says its effective resolution out loud.
- Firmware updates need physical access on a board provisioned with the
  Arduino default partition table; the two-slot table with rollback
  (`firmware/partitions.csv`) is what new boards get, and switching an
  existing board over requires a full erase.
- Retention is automatic by default (365 days). Raw rows older than the
  window are gone; take `/v1/maintenance/backup` if you need the history.
- The LoRa path and the rollback FSM are host-tested only. Neither has
  run on hardware yet, and both say so.

## Layout

```
firmware/               PlatformIO: cauce_hal → cauce_core → cauce_drivers → cauce_app
firmware/test/          Unity suites (single binary, register*Tests)
firmware/src/main.cpp   ESP32 composition root + host simulation demo
backend/cauce_server/   FastAPI + SQLite (api, evaluation, dashboard, analytics, db)
backend/tests/          pytest suite
simulator/              scenario CSV generator / backend feeder (CAUCE_LANG=es for Spanish)
scripts/                verify-all.ps1 (full gate) + run-e2e.ps1 (live integration)
configs/                node.example.conf (versioned KV schema)
protocol/v1/            68-byte frame + JSON payload spec
deployment/             docker-compose for the central
docs/en/ + docs/es/     English/Spanish mirrors (index below)
```

## Documentation

Full index: [docs/en/DOCUMENTATION_INDEX.md](docs/en/DOCUMENTATION_INDEX.md)
([español](docs/es/DOCUMENTATION_INDEX.md)).

| Category | Documents |
|---|---|
| Getting Started | [Testing](docs/en/TESTING.md) · [Deployment](docs/en/DEPLOYMENT.md) · [Hardware](docs/en/HARDWARE.md) |
| Architecture | [Architecture](docs/en/ARCHITECTURE.md) · [Data Model](docs/en/DATA_MODEL.md) · [Glossary](docs/en/DOMAIN_GLOSSARY.md) |
| Contracts | [Node API](docs/en/API.md) · [Sync](docs/en/SYNC.md) · [Backend](docs/en/BACKEND.md) |
| Trust | [Security](docs/en/SECURITY.md) · [Calibration](docs/en/CALIBRATION.md) · [OTA](docs/en/OTA.md) |
| Operations | [Dashboard](docs/en/DASHBOARD.md) · [Bench Plan](docs/en/BENCH_PLAN.md) |
| Planning | [Roadmap](docs/en/ROADMAP.md) · [Pilot Spec](docs/en/PILOT_SPEC.md) · [i18n Policy](docs/en/I18N.md) |

## Status

420 firmware + 575 backend tests + ESP32 build + E2E = **995 automated
checks green**. What exists and what doesn't: [STATUS.md](STATUS.md).

MIT — see [LICENSE](LICENSE).

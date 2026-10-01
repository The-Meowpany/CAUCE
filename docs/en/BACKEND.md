# CAUCE Central Backend

One server for many nodes: ingestion, history, query API, basic
analytics. **The nodes run fine without it** (offline-first) — this
service exists for the aggregated view, nothing more.

## Stack

Python 3.12 · FastAPI · SQLite (WAL), no ORM. If we ever need
PostgreSQL, the migration is scoped to `db.py` and nowhere else.

## Endpoints

| Method | Path | Description |
|---|---|---|
| GET | `/healthz` | Liveness |
| POST | `/v1/sync` | Batch ingestion (contract: SYNC.md). Provisioned nodes MUST sign with HMAC-SHA256 (`X-CAUCE-Node`, `X-CAUCE-Signature`) |
| POST | `/v1/provision` | Admin-gated device-key registration, one key per node |
| GET | `/v1/nodes` · `/v1/nodes/{id}` | List / detail + latest measurement |
| GET | `/v1/nodes/{id}/measurements` | Filterable series (`variable`,`quality`,`from_utc_ms`,`to_utc_ms`,`limit≤10000`,`offset`) with `total` |
| POST | `/v1/maintenance/retention` | Delete rows older than N days + VACUUM (cron-friendly; nodes resend retained rows unless purged there too) |
| GET | `/v1/maintenance/retention` | Retention config plus last run: `older_than_days`, `deleted_measurements`, `vacuumed`, `last_run_utc_ms` |
| GET | `/v1/nodes/{id}/coverage` | Expected-versus-received accounting for a node and variable: `coverage_pct`, longest gap, top gaps with a reason |
| GET | `/v1/sites/{id}/coverage` | Same pooled over the site's nodes, plus `worst_node` so one silent node stays visible |
| POST | `/v1/nodes/{id}/diagnostics` | Field diagnostics bundle ingest (HMAC-signed like `/v1/sync`, keeps the last 5 per node) |
| GET | `/v1/nodes/{id}/diagnostics` | Latest stored bundle summary |
| GET | `/v1/fleet` | Per-node fleet state: firmware, last sync, storage, flags, `needs_visit` |
| GET/POST | `/v1/sites` · PUT `/v1/sites/{id}/control` | Installation sites + assignment; flag a site as an untreated control |
| GET | `/v1/maintenance/backup` | Download a VACUUM INTO SQLite snapshot (auth-gated) |
| POST/GET | `/v1/sites` · PUT `/v1/nodes/{id}/site` | Installation sites + assignment |
| PUT | `/v1/sites/{id}/location` | Set site coordinates (validated lat/lon) |
| POST/GET/DELETE | `/v1/alerts/rules` · PATCH `/v1/alerts/rules/{id}` · GET `/v1/alerts/log` · POST+GET `/v1/alerts/check` | Threshold/stale rules (webhook/Telegram, validated inputs, enable toggle), delivery log with attempts, manual stale pass + pending redelivery |
| POST/GET | `/v1/interventions` | Intervention registry (`end_before_start`→422) |
| GET | `/v1/analytics/summary` | Descriptive stats per node+variable |
| GET | `/v1/analytics/compare` | Two nodes side by side + mean difference |
| GET | `/v1/analytics/before-after` | Pre/post split by intervention window; warns under 30 samples per period; with control sites it also returns `control_group.difference_in_differences` |
| GET | `/v1/analytics/period-compare` | Same node, two windows |
| GET | `/v1/analytics/heat-events` | How long it stayed above a threshold |
| GET | `/v1/analytics/summary-fast` | Same stats from materialized hourly aggregates (O(buckets)) |
| POST | `/v1/nodes/{id}/time-reconstruct` | Backfills timestamps of leading time-uncertain records from the first anchor + median interval; flags rows (`ts_reconstructed=1`) and states the margin-of-error caveat out loud |
| GET | `/v1/export-all.csv` | Full-history CSV stream |
| GET | `/v1/nodes/{id}/coverage.csv` | Coverage summary row followed by the gap table (same fields as the JSON endpoint) |
| GET | `/system` | Fleet view: per-node firmware, flags and visit badge, 7-day coverage table, retention state |
| GET | `/` | HTML dashboard in your language (Accept-Language: en/es/pt) |
| GET | `/legal/{terms,privacy,cookies,refunds}` | Versioned legal pages (content negotiated en/es) |
| GET | `/nodes/{id}` | Human node page: info, latest cards, chart, recent table (links to JSON/CSV) |
| GET | `/compare` | Side-by-side node comparison with overlaid chart (?a=&b=&variable=&days=) |
| GET | `/nodes/{id}/events` | Human heat-events page: form, event table or explained empty state |
| GET | `/nodes/{id}/report` | Printable evidence report: stats, qualities, heat events |
| GET | `/map` | SVG site map, nodes colored by latest temperature |
| GET | `/colocation` | All-node overlay chart (?variable=&days=) with divergence table |
| GET | `/alerts` | Rules CRUD, delivery log, manual check trigger |

Time windows are INCLUSIVE on both ends. Machine values stay English
codes; human labels localize at the presentation layer.

## Ingestion guarantees

- **Idempotency**: PK `(node_id, sequence)`. Re-sends never duplicate,
  by construction.
- **Honest ack**: the highest sequence actually sitting in the
  database. The server doesn't guess ahead.
- **Atomicity**: one bad record spoils the batch — full rollback, so
  the client fixes and resends instead of leaving half a batch behind.
- Every batch lands in `sync_batches` with its `transport` (wifi or
  lora; LoRa batches arrive through a gateway bridge).
- CSV exports need a bearer token when `CAUCE_API_TOKEN` is set, and
  they're rate-limited like everything else under `/v1/*`.

## Security

- `CAUCE_SYNC_TOKEN` / `CAUCE_API_TOKEN`: when set, every call needs
  `Authorization: Bearer <token>`, compared in constant time.
- **Per-device identity**: `/v1/provision` hands each node its own HMAC
  key; signed batches verify over the raw body with
  `hmac.compare_digest`. One leaked key compromises one node, not the
  fleet.
- Plain HTTP by design for now. Put a TLS-terminating reverse proxy in
  front before this box ever sees an untrusted network. Native ESP32
  TLS is a separate job.
- Per-IP rate limiting in SQLite (default 120 req/min) on API,
  evaluation and dashboard/CSV routes; honors `X-Forwarded-For` only
  with `CAUCE_TRUST_PROXY=1`.
- No personal data anywhere: environmental telemetry and node ids, full
  stop.

## Analytics limits

Every response carries a `metric_type: derived*` tag plus a
non-causality note, because a temperature difference between two nodes
is usually about placement, not proof. INVALID/MISSING/ESTIMATED are
excluded from the math; SUSPECT counts but its share is reported next
to the result.

## Evidence hygiene

Two things turn a comparison into evidence, and both are endpoints now:

- **Coverage first.** `/v1/nodes/{id}/coverage` answers "how much of
  the period did this node actually report". `coverage_pct` compares
  received samples against `expected_samples`, which assumes a constant
  sample period, so pass `expected_interval_ms` from the node config
  rather than quoting the default. Every gap carries a reason:
  `no_data` (nothing arrived), `measured_not_delivered` (the node
  synced in that window but the samples are missing) or
  `clock_uncertain`. A before/after computed over a period with a dead
  node is not evidence, and this is what catches that.
- **A control group.** Mark untreated sites with
  `PUT /v1/sites/{id}/control {"control": true}`. `before-after` then
  reports `difference_in_differences`: the treated site's shift minus
  the mean shift of the controls, so a regional heat wave is not
  credited to an intervention. Controls with fewer than 30 samples per
  window are listed but excluded, with the distance to the treated site
  so you can judge whether the match is fair. It is still a
  difference-in-differences, not a randomised trial, and the response
  says so.

## Retention

`POST /v1/maintenance/retention {"older_than_days": N}` purges
`measurements` and `agg_hourly` and vacuums. The same purge now runs on
its own: a daemon thread checks hourly and purges every
`CAUCE_RETENTION_INTERVAL_H` (default 24) when
`CAUCE_RETENTION_ENABLED=1` (default on) and `CAUCE_RETENTION_DAYS>=30`.
The scheduler never goes below 30 days whatever the environment says,
and VACUUM is throttled to `CAUCE_VACUUM_INTERVAL_H` because it
rewrites the whole file. `GET /v1/maintenance/retention` reports the
last run, and `/system` shows it. Eight nodes at 60 s for a year is
roughly 33 M rows, so something has to give; if you need the full
history, set the retention window above your analysis period and take
`/v1/maintenance/backup` snapshots.

## Run

```bash
cd backend
pip install -r requirements.txt
pytest tests -q                      # 88 tests
uvicorn cauce_server.main:app --port 8000
```

Docker: `docker compose -f deployment/docker-compose.yml up --build`

## Fleet triage

`GET /v1/fleet` exists so nobody drives to a site without a reason. A
node needs a visit when it has been offline more than 24 h, its clock
is unset, storage is nearly full, frames are corrupted, storage writes
failed, or the battery reads under 3.4 V. The absence of a diagnostics
bundle is itself a flag, because a node that cannot report why it is
broken is the node you should visit. `firmware_spread` tells you
whether the fleet is on one image, which matters while updates still
need physical access. Pass `storage_capacity_bytes` to turn stored
records into a percentage.

## Serverless notes

The app is stateless by construction: no websockets, no
background workers, no in-process caches — every byte of state lives in
the database. Heat alerts fire during ingest; stale rules evaluate on
`GET /v1/alerts/check`, which is the cron entrypoint (any scheduler
hitting it every 15 min replaces a worker). The one thread we do run is
the retention scheduler, and it holds no state: it re-reads its last-run
stamp from `maintenance_state` on every tick. All configuration comes
from `CAUCE_*` environment variables, including the database path. One
caveat: SQLite files don't survive serverless ephemeral filesystems —
a serverless deploy needs Postgres, and that migration is scoped to
`db.py` (the documented seam).

# CAUCE Central Backend

Central multi-node server (master plan §34–37): ingestion, historical
storage, query API and basic analytics. **Nodes run perfectly without it**
(offline-first); this service adds the aggregated view.

## Stack

Python 3.12 · FastAPI · SQLite (WAL), no ORM. PostgreSQL migration is scoped
to `db.py`.

## Endpoints

| Method | Path | Description |
|---|---|---|
| GET | `/healthz` | Liveness |
| POST | `/v1/sync` | Node batch ingestion (contract: docs/SYNC.md). Provisioned nodes MUST sign with HMAC-SHA256 (`X-CAUCE-Node`, `X-CAUCE-Signature`) |
| POST | `/v1/provision` | Admin-gated per-node device key registration |
| GET | `/v1/nodes` · `/v1/nodes/{id}` | List / detail + latest measurement |
| GET | `/v1/nodes/{id}/measurements` | Filterable series (`variable`,`quality`,`from_utc_ms`,`to_utc_ms`,`limit≤10000`) |
| POST/GET | `/v1/sites` · PUT `/v1/nodes/{id}/site` | Installation sites + assignment |
| POST/GET | `/v1/interventions` | Intervention registry (`end_before_start`→422) |
| GET | `/v1/analytics/summary` | Descriptive stats per node+variable |
| GET | `/v1/analytics/compare` | Two-node comparison + mean difference |
| GET | `/v1/analytics/before-after` | Pre/post evaluation by intervention window; flags `<30` samples per period |
| GET | `/v1/analytics/period-compare` | Same-node two-window comparison |
| GET | `/v1/analytics/heat-events` | Above-threshold event durations |
| GET | `/v1/analytics/summary-fast` | Stats from materialized hourly aggregates (O(buckets)) |
| POST | `/v1/nodes/{id}/time-reconstruct` | Backfill timestamps of leading time-uncertain records using first anchor + median interval; flags rows (`ts_reconstructed=1`) and reports the margin-of-error caveat |
| GET | `/v1/export-all.csv` | Full-history CSV stream |
| GET | `/` | Localized HTML dashboard (Accept-Language: en/es/pt) |

Time windows are INCLUSIVE on both ends. Machine values stay English codes;
human labels localize at the presentation layer.

## Ingestion guarantees

- **Idempotency**: PK `(node_id, sequence)`; re-sends never duplicate.
- **Honest ack**: highest sequence actually present in the database.
- **Atomicity**: invalid batch → full rollback.
- Every batch recorded in `sync_batches`.

## Security

- `CAUCE_SYNC_TOKEN` / `CAUCE_API_TOKEN`: when set, require
  `Authorization: Bearer <token>`, compared constant-time.
- **Per-device identity**: `/v1/provision` registers an HMAC device key per
  node; signed batches are verified over the raw body with
  `hmac.compare_digest`. Compromising one node's key does not allow forging
  another node's data.
- Transport is plain HTTP by design today; TLS termination (reverse proxy)
  or ESP32 native TLS is required before exposing beyond a trusted LAN.
- Per-IP rate limiting with bounded memory (default 120 req/min).
- No personal data: environmental telemetry and node identifiers only.

## Analytics limits

Responses are tagged `metric_type: derived*` and always include a
non-causality note. INVALID/MISSING/ESTIMATED qualities are excluded;
SUSPECT participates but must be reported alongside its share.

## Run

```bash
cd backend
pip install -r requirements.txt
pytest tests -q                      # 22 tests
uvicorn cauce_server.main:app --port 8000
```

Docker: `docker compose -f deployment/docker-compose.yml up --build`

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
| GET | `/v1/nodes` · `/v1/nodes/{id}` | List (also `cursor`-paginated) / detail + latest measurement |
| GET | `/v1/nodes/{id}/measurements` | Filterable series (`variable`,`quality`,`from_utc_ms`,`to_utc_ms`,`limit≤10000`,`offset`) with `total` |
| POST | `/v1/maintenance/retention` | Delete rows older than N days + VACUUM (cron-friendly; nodes resend retained rows unless purged there too) |
| GET | `/v1/maintenance/retention` | Retention config plus last run: `older_than_days`, `deleted_measurements`, `vacuumed`, `last_run_utc_ms` |
| GET | `/v1/nodes/{id}/coverage` | Expected-versus-received accounting for a node and variable: `coverage_pct`, longest gap, top gaps with a reason |
| GET | `/v1/sites/{id}/coverage` | Same pooled over the site's nodes, plus `worst_node` so one silent node stays visible |
| POST | `/v1/nodes/{id}/diagnostics` | Field diagnostics bundle ingest (HMAC-signed like `/v1/sync`, keeps the last 5 per node) |
| GET | `/v1/nodes/{id}/diagnostics` | Latest stored bundle summary |
| GET | `/v1/fleet` | Per-node fleet state: firmware, last sync, storage, flags, `needs_visit` |
| GET/POST | `/v1/sites` · PUT `/v1/sites/{id}/control` | Installation sites + assignment; flag a site as an untreated control |
| PUT/GET | `/v1/sites/{id}/calibration` | Per-(site,variable) offset/scale; raw rows are never rewritten, calibrated values are derived on read |
| POST/GET | `/v1/sites/{id}/maintenance` | Maintenance log: install, calibration, sensor replacement, relocation |
| GET | `/v1/maintenance/backup` | Download a VACUUM INTO SQLite snapshot (auth-gated) |
| POST/GET | `/v1/sites` · PUT `/v1/nodes/{id}/site` | Installation sites + assignment |
| PUT | `/v1/sites/{id}/location` | Set site coordinates (validated lat/lon) |
| POST/GET/DELETE | `/v1/alerts/rules` · PATCH `/v1/alerts/rules/{id}` · GET `/v1/alerts/log` · POST+GET `/v1/alerts/check` | Threshold/stale rules (webhook/Telegram, validated inputs, enable toggle), delivery log with attempts, manual stale pass + pending redelivery |
| POST/GET | `/v1/interventions` | Intervention registry (`end_before_start`→422) |
| GET | `/v1/analytics/summary` | Descriptive stats per node+variable; `granularity=auto|raw|hourly` switches between raw rows and the hourly buckets |
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

## Downlink

The central can send instructions to a node that already reports up. It
travels in the same `/v1/sync` response that acknowledges a batch, so it
adds no extra radio wakeup and no new failure mode on the node.

| Method | Path | Description |
|---|---|---|
| POST | `/v1/nodes/{id}/commands` | Queue a command. Admin token |
| GET | `/v1/nodes/{id}/commands` | List with `pending` / `delivered` / `acked` / `expired` state |

Two properties make it safe to retry, which is the only property that
matters when the far end is an offline-first device:

- **Idempotency.** `(node_id, idempotency_key)` is unique. Re-posting the
  same key returns the original command with `created: false` instead of
  queueing a second one. Reusing a key with a *different* body is a `409`,
  because that is a client bug and silently ignoring it would hide it.
- **Honest delivery.** A command stays `pending` until the node reports it
  in a `/v1/sync` round trip, becomes `delivered` when it arrives, and
  `acked` only when the node sends an outcome. An expired command stops
  being offered. Nothing is reported as done on the strength of having
  been queued.

The node answers with `command_receipts` in its next batch. A receipt for
another node's command, or for a command that does not exist, is counted
and ignored: the node is an authenticated peer but not an authority on
someone else's command.

Kinds are deliberately small and side-effect free: `set_sampling_interval`,
`set_sync_interval`, `request_resync`, `set_led_mode`. The shipped node
validates their arguments and reports back; it does not reconfigure itself
from the field. Actuation belongs with hardware that can be actuated.


## Calibration

The central owns calibration: `measurements.value` stays raw and the
calibrated value is derived on read, so a recalibration replays over the
whole history without touching a node. `calibrated_value = raw_value *
scale + offset`, recorded per (site, variable) through
`PUT /v1/sites/{id}/calibration`. Because the map is linear it is exact
on the hourly aggregates too, which is why `/summary-fast` and
`granularity=hourly` correct their buckets instead of re-reading rows.
Every analytics response says in a `calibration` object whether it was
applied; when it was not, the `calibrated` key is absent rather than
identical to the raw number. See `CALIBRATION.md` for the honesty rules
around what a co-location offset does and does not buy.

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
pytest tests -q                      # 384 tests
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

## LoRa wire format

A measurement frame is 68 bytes, and a LoRa payload at SF9 is 115, so a batch
never fits in one uplink. `LoRaBatchEncoder` slices a batch into frames and
`LoRaBatchReassembler` puts it back together on the receiving side.

Each frame carries enough header for the receiver to reject it without asking
for a retransmit: batch id, fragment index, fragment count, the record count the
whole batch should end with, and the payload length. The redundancy is
deliberate, because a lost fragment costs the entire batch and radio time is
the expensive resource.

```
frame   0..1   magic 0xCA | version 1
        2..3   batch_id
        4      fragment_index
        5      fragment_count
        6..7   record_count for the whole batch
        8..9   payload_bytes
        10..   record payloads, 60 bytes each
        last 2  CRC16-CCITT over everything before it
```

Spreading factor decides how many records ride one uplink:

| SF | Payload budget | Records per uplink | A 4-record batch |
|---|---|---|---|
| SF7 | 222 B | 3 | 2 uplinks |
| SF8 | 222 B | 3 | 2 uplinks |
| SF9 | 115 B | 1 | 4 uplinks |
| SF10 and below | 51 B or less | refused | refused |

SF10 is refused rather than truncated: a measurement cut in half is worse than
no measurement, so the node reports a retryable error and keeps the records.

`lora_frames.py` is the reference decoder and doubles as the gateway: it turns
reassembled frames into a `POST /v1/sync` body. The C++ encoder and the Python
decoder are deliberately independent implementations of the same format, and
`tests/test_lora_frames.py` pins the exact bytes the firmware produces, so a
change on either side shows up as a failing test rather than as a gateway that
quietly stops accepting nodes.

What this still does **not** do: there is no gateway firmware, no LoRa radio
driver, and no measurement of any link budget. The frame format and its
acknowledgement are host-tested; the air interface is unproven.


## Authorization

Two models, because one token is honest for one operator and wrong for more
than one:

- `CAUCE_API_TOKEN` is the shared admin token. It has every scope, and a
  single-operator deployment needs no configuration at all.
- `api_tokens` holds real principals. Each has a name, a SHA-256 digest of its
  token, a comma-separated scope list, and optionally one `site_id`. A principal
  with `site_id` set may only touch that site; one without is fleet-wide.
  `admin` in the scope list satisfies every scope.

Tokens are stored as digests, never in the clear, and compared in constant
time. A lost token means issuing a new one, not reading the old one back.

Scopes are enforced before payload validation, so a caller without the scope
learns nothing about the endpoint beyond its existence:

| Capability | Grants |
|---|---|
| `read` | Reading calibration, maintenance and command state |
| `write` | Changing calibration, logging maintenance, queueing commands |
| `admin` | Everything above |

Site scoping is the half that stops data leaking. Scopes alone limit *what* an
operator can do but never *where*, which would leave a per-site operator free to
read another site's data through a URL it guessed.


## Gateway

`LoRaGateway` is the node-side counterpart of the encoder: it receives frames,
reassembles batches, forwards them to `POST /v1/sync` and acknowledges what the
central stored. The whole loop is exercised against the real FastAPI app
through an injected transport, so `tests/test_lora_gateway.py` drives frames
made exactly as the firmware makes them and then reads the rows back out of
SQLite.

Everything except the radio driver is therefore ordinary tested logic. What
remains for M2 is an SX1276 driver, a process to run this on hardware, and a
link budget.

**The loop mirrors the node's**, because the two have to agree:

1. Frames are buffered until a batch is complete.
2. The batch is forwarded as one `POST /v1/sync`.
3. The acknowledgement carries the highest sequence the central **durably
   stored**, not the highest sequence that was sent. If they differ the node
   resends the remainder and the central dedupes, which is the correct outcome.
4. The reassembly buffer is released in a `finally`, so a forwarding failure
   does not leak the batch.

Noise is counted rather than raised. A radio delivers corrupt frames routinely
and a gateway that raised on every bad CRC would restart on every storm.

**Trust boundary, stated rather than implied.** A provisioned node is
authenticated with an HMAC over the raw request body, so a gateway forwarding
on a node's behalf must hold that node's `device_key`. That makes the gateway a
trusted party equivalent to every node it serves: whoever compromises it can
forge any of them. Key distribution to gateways is therefore a real cost of
this topology, not a detail.

The alternative is the node signing the compact frame and the central verifying
against the frame rather than the reconstructed body, which would move the
verification surface onto the wire format. That is not implemented here.

What *is* enforced is that a gateway configured without a key does not
silently downgrade a provisioned node to unsigned: the central returns 401, the
gateway raises, and nothing is acknowledged, so the node keeps its rows and
retries. A silent downgrade would have been the failure mode worth worrying
about, because it would look like working.


## Input shape and response hardening

Two rules that came out of reviewing the static-analysis findings, both because
the obvious version turned out to be weaker than it looked.

**Identifiers are restricted, free text is escaped.** A `site_id` must match
`[A-Za-z0-9_-]` and stay under 64 characters, the same shape `NodeConfig`
already enforces for a node id. It used to accept any non-empty string, which
meant a value containing markup could be stored and then served back inside an
API response. Restricting it at the door is the right layer, because a site id
also reaches URLs and export filenames. A site *name* stays free text, since
that is what a name is for, and is escaped on the way out instead.

**JSON responses declare their type.** `json.dumps` does not escape `<`, `>` or
`&`, so a stored identifier containing markup appears literally in the body.
Served as `application/json` that is inert, but a browser that guessed the type
would not be, so every non-HTML response carries
`X-Content-Type-Options: nosniff`. The dashboard pages deliberately do not get
it: they are the one place the browser is meant to render what we sent, and they
carry their own headers.

### What the XSS alert was and was not

The reflected-XSS finding pointed at the events dashboard. Investigated rather
than dismissed, and it is a **false positive on that route**: every reflected
value there is escaped, and a hostile `node_id` cannot even reach a 200 because
the path routing rejects it first. `tests/test_security.py` pins both halves, so
a change that unescapes something fails there instead of in a browser.

The investigation did find a real weakness next door, which is the `site_id`
rule above. Reporting an alert as a false positive should not mean the review
was wasted.


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

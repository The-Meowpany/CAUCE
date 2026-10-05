# CAUCE Embedded API — v1

Base: `http://<node-ip>/api/v1`

The node serves HTTP itself. All the logic sits in `ApiRouter`, which
is portable and host-tested; Arduino's `WebServer` is just the socket
layer (`Esp32ApiServer`). That split is why 14 router tests run on a PC.

## Endpoints

| Method | Path | Description |
|---|---|---|
| GET | `/node` | Who this node is: firmware/protocol/hardware versions, declared location |
| GET | `/status` | FSM states, clock validity, uptime, latest measurement |
| GET | `/measurements/latest` | Last stored measurement (404 if the store is empty) |
| GET | `/measurements?from=&to=` | History as a streaming JSON array |
| GET | `/health` | Full telemetry: counters, storage, failures |
| GET | `/diagnostics` | Field bundle (`cauce.diag/1`): identity, health, sync counters, radio, last error. POST it to the central to open a field ticket |
| GET | `/config` | Active config. **Secrets never come out** (`wifi_password: null`) |
| POST | `/config` | Apply configuration (KV body). Admin token required |
| GET | `/export?format=csv\|json&from=&to=` | CSV (RFC4180) or JSON, streamed |

Also served: `/` (localized dashboard SPA), `/favicon.ico` (204, so
browsers stop asking).

## Time parameters

`from`/`to` take epoch milliseconds or ISO-8601 UTC. Both ends are
INCLUSIVE — same rule as everywhere else. Percent-encode the awkward
characters (`:` → `%3A`).

## Admin authentication

- Only `POST /config` asks for credentials; reads are public. A weather
  node that hides its own temperature would be missing the point.
- Header: `Authorization: Bearer <token>`.
- The node keeps only the token's SHA-256 and compares in constant time.
- **Fail-closed**: no token configured means POST answers `503`, not
  "come on in". Wrong token → `401`.

## Status codes

| Code | Meaning |
|---|---|
| 200 | OK (streams may arrive in several chunks) |
| 400 | Malformed parameter/body |
| 401 | Invalid token |
| 404 | Unknown route, or no measurements yet |
| 405 | Method not allowed |
| 422 | Config parsed but invalid → `{"errors":[...]}` with every problem listed |
| 503 | Administration not configured, or no CA configured (`certificate_authority_not_configured`) |

## Node certificates (central)

Three endpoints, gated by `write` for issuing and `read` for the two reads.
They are in their own section because their trust story differs from the rest
of the API: everything else is gated by the shared admin token, these are
ultimately gated by a signature from a CA key configured separately in
`CAUCE_CA_KEY`.

| Method | Path | Description |
|---|---|---|
| POST | `/v1/nodes/{id}/certificate` | Issue one. Body: `{}` or `{"validity_seconds": N}`. Supersedes any previous certificate |
| GET | `/v1/nodes/{id}/certificate` | The node's current certificate, re-verified on the way out |
| GET | `/v1/certificates/{serial}` | Status of one: `signature_valid` and `trusted` reported separately |

Refusals worth knowing before calling these:

| Code | Detail | Why |
|---|---|---|
| 409 | `node_has_no_device_key_provision_it_first` | Nothing to certify. Provisioning is the only way a key arrives |
| 409 | `node_key_algorithm_is_not_public_key_based` | An HMAC node's key is a shared secret, and a certificate is meant to be handed to verifiers |
| 503 | `certificate_authority_not_configured` | `CAUCE_CA_KEY` unset. Failing closed rather than issuing documents signed by nothing |

The public key comes from the node's registered `device_key`, never from the
request — a CA that certified whatever it was handed would certify nothing.
`SECURITY.md` has the shape and the reasoning.

## Field diagnostics

`GET /api/v1/diagnostics` returns one JSON object meant to be copied,
not parsed by a human:

```json
{"schema":"cauce.diag/1",
 "identity":{"node_id":"CAUCE-001","site_id":"canelones-centro",
             "firmware":"1.4.0","hardware":""},
 "health":{"node_state":"Sampling","net_state":"Connected","uptime_ms":123456,
           "utc_time_valid":true,"rssi_dbm":-63,"battery_v":3.92,
           "stored":1150,"corrupted_frames":2,"sample_interval_s":60, "...":"..."},
 "sync":{"attempts":20,"failures":2},
 "radio":{"lora_enabled":false,"rssi_dbm":0,"snr_db":0,"sf":0},
 "errors":{"last":""}}
```

The node signs it with the same device key it uses for batches
(HMAC-SHA256 over the exact bytes) and POSTs it to
`/v1/nodes/{id}/diagnostics`; the central keeps the last five per node.
That is the whole point: when a node in the field misbehaves, the
diagnosis is one HTTP call instead of a serial cable, and it lands in
the database where `/v1/fleet` can flag it. Strings are escaped and
the response is refused rather than truncated if the buffer is too
small, so the signature always covers exactly what was sent.

## Streaming contract

Big responses go out in chunks; the minimum workable buffer is **384 B**
(512 or more recommended). One active stream per node — a new request
cancels the previous one, which keeps RAM usage flat. UI strings
localize client-side (ES/EN switch); domain values stay as neutral
codes.

## Examples

```bash
curl http://192.168.4.1/api/v1/status
curl "http://192.168.4.1/api/v1/export?format=json" > node.json
curl -X POST http://192.168.4.1/api/v1/config \
     -H "Authorization: Bearer $CAUCE_TOKEN" \
     --data-binary @new-config.conf
```

422 example:

```json
{"errors":["sampling_interval_s out of [10..3600]"]}
```

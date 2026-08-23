# CAUCE Embedded API — v1

Base: `http://<node-ip>/api/v1`

Transport: HTTP served by the node itself. Logic lives in `ApiRouter`
(portable, host-tested); Arduino `WebServer` is transport only
(`Esp32ApiServer`).

## Endpoints

| Method | Path | Description |
|---|---|---|
| GET | `/node` | Node identity, firmware/protocol/hardware versions, declared location |
| GET | `/status` | Node/network FSM states, time validity, uptime, latest measurement |
| GET | `/measurements/latest` | Last stored measurement (404 if none) |
| GET | `/measurements?from=&to=` | Historical series as streaming JSON array |
| GET | `/health` | Full system telemetry (counters, storage, failures) |
| GET | `/config` | Active config. **Never exposes secrets** (`wifi_password: null`) |
| POST | `/config` | Apply configuration (KV body). Requires admin token |
| GET | `/export?format=csv\|json&from=&to=` | CSV (RFC4180) or JSON streaming export |

Also served: `/` (localized dashboard SPA), `/favicon.ico` (204).

## Time parameters

`from`/`to` accept epoch milliseconds or ISO-8601 UTC. Both ends are
INCLUSIVE. Percent-encode special characters (`:` → `%3A`).

## Admin authentication

- Only `POST /config` requires authorization; reads are public.
- Header: `Authorization: Bearer <token>`.
- The node stores only the SHA-256 of the token; comparison is
  constant-time.
- **Fail-closed**: with no token configured, POST responds `503`.
  Wrong token → `401`.

## Status codes

| Code | Meaning |
|---|---|
| 200 | OK (streams may span multiple chunks) |
| 400 | Malformed parameter/body |
| 401 | Invalid token |
| 404 | Unknown route or no measurements yet |
| 405 | Method not allowed |
| 422 | Config parsed but invalid → `{"errors":[...]}` |
| 503 | Administration not configured |

## Streaming contract

Large responses stream in chunks; minimum buffer **384 B**
(recommended ≥512). One active stream per node; a new request cancels the
previous one. Public UI strings are localized client-side (ES/EN switch);
domain values remain language-neutral codes.

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

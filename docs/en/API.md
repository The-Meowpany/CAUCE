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
| 503 | Administration not configured |

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

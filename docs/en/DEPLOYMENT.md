# CAUCE Deployment

How to go from a clean checkout to an operating pilot (5–10 nodes plus
the central server). Followed in order, no steps skipped.

## 0. Requirements

| Component | Version | Purpose |
|---|---|---|
| Python | ≥3.10 (tested 3.12) | PlatformIO CLI + backend |
| PlatformIO Core | ≥6.1 | firmware build/test |
| GCC MinGW-w64 (Windows) / gcc (Linux) | ≥9 | native tests & binaries |
| Docker (optional) | — | backend container |
| Per-node hardware | see HARDWARE.md | ESP32 + BME280 |

## 1. Verify the environment from zero

```powershell
git clone <repo> && cd CAUCE
pip install platformio
cd firmware
pio test -e native        # expect 112 succeeded
pio run -e esp32dev       # expect SUCCESS
```

Backend:

```powershell
cd backend
pip install -r requirements.txt
python -m pytest tests -q # expect 68 passed
```

Node↔server integration:

```powershell
.\scripts\run-e2e.ps1    # expect E2E PASSED
```

One-command gate: `.\scripts\verify-all.ps1` (`CAUCE_LANG=en` for
English output).

## 2. Central server

```powershell
# Direct
cd backend
$env:CAUCE_DB_PATH = "C:\cauce\data\central.sqlite"
$env:CAUCE_SYNC_TOKEN = "<shared-node-secret>"
uvicorn cauce_server.main:app --host 0.0.0.0 --port 8000

# Docker
docker compose -f deployment/docker-compose.yml up -d --build
```

Check `GET http://<server>:8000/healthz`; the central dashboard is at
`/`. If `/healthz` doesn't answer, fix that before touching any node.

## 2b. TLS in front of the central

The backend speaks plain HTTP on purpose and is **not published to the
LAN**. Caddy terminates TLS and is the only published port:

```bash
cd deployment
export CAUCE_DOMAIN=cauce.local          # hostname the nodes will use
docker compose up -d --build
```

What this buys and what it costs:

- `http://CAUCE_DOMAIN` redirects to `https://CAUCE_DOMAIN`.
- The central answers on `127.0.0.1:8000` only, so an operator can debug
  from the host while the network sees 443 and nothing else.
- Batches are signed per device (HMAC-SHA256) as well as encrypted; TLS
  does not replace signing, and signing does not replace TLS.

**Certificate for a LAN-only pilot.** The default `tls internal` makes
Caddy issue from its own local CA. Export the root once and install it on
every node and on the operator machine, otherwise the nodes reject the
batch:

```bash
docker compose cp caddy:/data/caddy/pki/authorities/local/root.crt ./cauce-root.crt
```

**Certificate for a public name.** Delete the `tls internal` line from
`Caddyfile`, set `ACME_EMAIL`, and Caddy obtains and renews a real
certificate unattended. `CAUCE_DOMAIN` must resolve publicly.

Node side: point `sync_server_url` at `https://CAUCE_DOMAIN`, and install
the root certificate on the ESP32 if it is not a publicly trusted CA. From
there the checklist item "backend reachable over TLS" is satisfied.

## 3. Prepare each node


1. **Identity**: a unique `node_id` (`CAUCE-001`, …). No two nodes ever
   share one — the whole sync design assumes this.
2. **Config**: copy `configs/node.example.conf`, fill in identity,
   location, context, sampling interval.
3. **Hardware**: assemble per HARDWARE.md (BME280 on I2C 21/22, addr
   0x76, radiation screen mandatory — not recommended, mandatory).
4. **Flash**:
   ```powershell
   pio run -e esp32dev -t upload
   pio device monitor      # structured boot logs
   ```
5. **Secrets**: Wi-Fi and sync token go in through the local UI
   (`http://<node-ip>/` → Configuration) with the node's admin token.
   Flash keeps the hash, never the secret.

## 4. Field validation

- The node dashboard shows `SENSOR_DISCOVERED`, `VALID` quality cards,
  and a filling chart. If the chart stays empty, stop — the problem is
  at the sensor, not the server.
- Register the site and assign the node on the server
  (`POST /v1/sites`, `PUT /v1/nodes/{id}/site`).
- Confirm ingestion: `GET /v1/nodes/{id}/measurements?limit=5` returns
  your rows.
- Co-locate the nodes ≥48h for cross-node comparability
  (CALIBRATION.md). This is the cheapest accuracy you will ever buy.

## 5. Intervention evaluation

```http
POST /v1/interventions {"site_id":"...","kind":"shade",
                        "start_utc_ms":1787356800000}
GET  /v1/analytics/before-after?intervention_id=1&node_id=CAUCE-001&variable=air_temperature
```

Don't interpret anything until `sufficient_sample:true`. Under 30
samples per period, the endpoint tells you it's guessing — listen to
it.

## 6. Maintenance

- Structured USB serial logs (grep-friendly, event codes not prose).
- Remote health via `/v1/nodes/{id}` and the central panel.
- Node data rotates itself; permanent history lives centrally.
- Firmware updates: **the OTA decision layer is done, real flashing
  waits for hardware** — today it means physical access. Plan site
  visits accordingly.

## 7. Pilot acceptance checklist

- [ ] `pio test -e native` green
- [ ] Backend up with token + rate limit configured
- [ ] Central reachable over TLS through the Caddy terminator, and the
      root certificate installed on every node
- [ ] `run-e2e.ps1` green after any protocol change
- [ ] Every node: identified, geo-located, VALID quality reaching the
      center, installation metadata written down

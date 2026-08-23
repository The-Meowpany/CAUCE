# CAUCE Deployment

Consolidated guide: clean repository → operating pilot (5–10 nodes +
central server).

## 0. Requirements

| Component | Version | Purpose |
|---|---|---|
| Python | ≥3.10 (tested 3.12) | PlatformIO CLI + backend |
| PlatformIO Core | ≥6.1 | firmware build/test |
| GCC MinGW-w64 (Windows) / gcc (Linux) | ≥9 | native tests & binaries |
| Docker (optional) | — | backend container |
| Per-node hardware | see docs/HARDWARE.md | ESP32 + BME280 |

## 1. Verify environment from zero

```powershell
git clone <repo> && cd CAUCE
pip install platformio
cd firmware
pio test -e native        # expect 97 succeeded
pio run -e esp32dev       # expect SUCCESS
```

Backend:

```powershell
cd backend
pip install -r requirements.txt
python -m pytest tests -q # expect 22 passed
```

Node↔server integration:

```powershell
.\scripts\run-e2e.ps1    # expect E2E PASSED
```

One-command gate: `.\scripts\verify-all.ps1`.

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

Check `GET http://<server>:8000/healthz`; central dashboard at `/`.

## 3. Prepare each node

1. **Identity**: unique `node_id` (`CAUCE-001`, …).
2. **Config**: copy `configs/node.example.conf`, fill identity, location,
   context, sampling interval.
3. **Hardware**: assemble per docs/HARDWARE.md (BME280 on I2C 21/22, addr
   0x76, radiation screen mandatory).
4. **Flash**:
   ```powershell
   pio run -e esp32dev -t upload
   pio device monitor      # structured boot logs
   ```
5. **Provision secrets**: Wi-Fi and sync token via the local UI
   (`http://<node-ip>/` → Configuration) using the node's admin token;
   only the hash is stored on flash.

## 4. Field validation

- Node dashboard shows `SENSOR_DISCOVERED`, quality `VALID` cards, chart
  filling in.
- Register site + assignment on the server (`POST /v1/sites`,
  `PUT /v1/nodes/{id}/site`).
- Confirm ingestion: `GET /v1/nodes/{id}/measurements?limit=5`.
- Keep nodes co-located ≥48h for cross-node comparability
  (docs/CALIBRATION.md).

## 5. Intervention evaluation

```http
POST /v1/interventions {"site_id":"...","kind":"shade",
                        "start_utc_ms":1787356800000}
GET  /v1/analytics/before-after?intervention_id=1&node_id=CAUCE-001&variable=air_temperature
```

Do not interpret before `sufficient_sample:true`.

## 6. Maintenance

- Structured USB serial logs (grep-friendly).
- Remote health via `/v1/nodes/{id}` and the central panel.
- Node data rotation automatic; permanent history lives centrally.
- Firmware updates: **OTA decision layer shipped; actual flashing pending
  hardware** — today updates need physical access.

## 7. Pilot acceptance checklist

- [ ] `pio test -e native` green
- [ ] Backend up with token + rate limit configured
- [ ] `run-e2e.ps1` green after any protocol change
- [ ] Every node: identified, geo-located, VALID quality flowing to the
      center, installation metadata documented

# Despliegue CAUCE

Cómo ir de un checkout limpio a un piloto operativo (5–10 nodos más el
servidor central). En orden, sin saltear pasos.

## 0. Requisitos

| Componente | Versión | Propósito |
|---|---|---|
| Python | ≥3.10 (probado 3.12) | CLI PlatformIO + backend |
| PlatformIO Core | ≥6.1 | build/test firmware |
| GCC MinGW-w64 (Windows) / gcc (Linux) | ≥9 | tests nativos y binarios |
| Docker (opcional) | — | contenedor backend |
| Hardware por nodo | ver HARDWARE.md | ESP32 + BME280 |

## 1. Verificar el entorno desde cero

```powershell
git clone <repo> && cd CAUCE
pip install platformio
cd firmware
pio test -e native        # esperado 112 ok
pio run -e esp32dev       # esperado SUCCESS
```

Backend:

```powershell
cd backend
pip install -r requirements.txt
python -m pytest tests -q # esperado 68 passed
```

Integración nodo↔servidor:

```powershell
.\scripts\run-e2e.ps1    # esperado E2E PASSED
```

Gate de un comando: `.\scripts\verify-all.ps1` (`CAUCE_LANG=en` para
salida en inglés).

## 2. Servidor central

```powershell
# Directo
cd backend
$env:CAUCE_DB_PATH = "C:\cauce\data\central.sqlite"
$env:CAUCE_SYNC_TOKEN = "<shared-node-secret>"
uvicorn cauce_server.main:app --host 0.0.0.0 --port 8000

# Docker
docker compose -f deployment/docker-compose.yml up -d --build
```

Chequear `GET http://<server>:8000/healthz`; el dashboard central está
en `/`. Si `/healthz` no responde, arreglar eso antes de tocar ningún
nodo.

## 3. Preparar cada nodo

1. **Identidad**: un `node_id` único (`CAUCE-001`, …). Jamás dos nodos
   comparten uno — todo el diseño de sync lo asume.
2. **Config**: copiar `configs/node.example.conf`, completar identidad,
   ubicación, contexto, intervalo de muestreo.
3. **Hardware**: ensamblar según HARDWARE.md (BME280 en I2C 21/22, dir
   0x76, pantalla de radiación obligatoria — no recomendada,
   obligatoria).
4. **Flash**:
   ```powershell
   pio run -e esp32dev -t upload
   pio device monitor      # logs de boot estructurados
   ```
5. **Secretos**: Wi-Fi y sync token entran por la UI local
   (`http://<node-ip>/` → Configuration) con el admin token del nodo.
   La flash guarda el hash, nunca el secreto.

## 4. Validación en campo

- El dashboard del nodo muestra `SENSOR_DISCOVERED`, tarjetas con
  calidad `VALID`, y la gráfica llenándose. Si la gráfica queda vacía,
  frenar — el problema está en el sensor, no en el servidor.
- Registrar el sitio y asignar el nodo en el server
  (`POST /v1/sites`, `PUT /v1/nodes/{id}/site`).
- Confirmar ingesta: `GET /v1/nodes/{id}/measurements?limit=5`
  devuelve tus filas.
- Co-localizar los nodos ≥48h para comparabilidad entre nodos
  (CALIBRATION.md). Es la exactitud más barata que vas a comprar.

## 5. Evaluación de intervenciones

```http
POST /v1/interventions {"site_id":"...","kind":"shade",
                        "start_utc_ms":1787356800000}
GET  /v1/analytics/before-after?intervention_id=1&node_id=CAUCE-001&variable=air_temperature
```

No interpretar nada hasta `sufficient_sample:true`. Bajo 30 muestras
por período, el endpoint te dice que está adivinando — hacerle caso.

## 6. Mantenimiento

- Logs serie USB estructurados (grep-friendly, códigos de evento no
  prosa).
- Salud remota vía `/v1/nodes/{id}` y el panel central.
- Los datos del nodo rotan solos; la historia permanente vive en la
  central.
- Updates de firmware: **la capa de decisión OTA está hecha, el
  flasheo real espera hardware** — hoy significa acceso físico.
  Planificar las visitas consecuentemente.

## 7. Checklist de aceptación del piloto

- [ ] `pio test -e native` en verde
- [ ] Backend con token + rate limit configurados
- [ ] `run-e2e.ps1` en verde tras cualquier cambio de protocolo
- [ ] Cada nodo: identificado, geo-localizado, calidad VALID llegando
      al centro, metadata de instalación anotada

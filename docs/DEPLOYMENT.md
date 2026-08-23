# Despliegue CAUCE

Guía consolidada para pasar de repositorio limpio a red operativa
(piloto de 5–10 nodos + servidor central).

## 0. Requisitos

| Componente | Versión | Uso |
|---|---|---|
| Python | ≥3.10 (probado 3.12) | PlatformIO CLI + backend |
| PlatformIO Core | ≥6.1 | build/test firmware |
| GCC MinGW-w64 (Windows) o gcc (Linux) | ≥9 | tests y binarios nativos |
| Docker (opcional) | — | backend en contenedor |
| Hardware por nodo | ver docs/HARDWARE.md | ESP32 + BME280 |

## 1. Verificar el entorno desde cero

```powershell
git clone <repo> && cd CAUCE
pip install platformio
cd firmware
pio test -e native        # esperado: 87 succeeded
pio run -e esp32dev       # esperado: SUCCESS
```

Backend:

```powershell
cd backend
pip install -r requirements.txt
python -m pytest tests -q # esperado: 18 passed
```

Integración nodo↔servidor (requiere lo anterior):

```powershell
.\scripts\run-e2e.ps1    # esperado: E2E PASADO: 3 fases + base de datos
```

## 2. Servidor central

```powershell
# Opción A: directo
cd backend
$env:CAUCE_DB_PATH = "C:\cauce\data\central.sqlite"
$env:CAUCE_SYNC_TOKEN = "<secreto-compartido-nodos>"
uvicorn cauce_server.main:app --host 0.0.0.0 --port 8000

# Opción B: docker
Copy-Item deployment\docker-compose.yml .
$env:CAUCE_SYNC_TOKEN = "<secreto>"
docker compose up -d --build
```

Verificación: `GET http://<servidor>:8000/healthz` → `{"status":"ok"}`;
el dashboard central queda en `/`.

## 3. Preparar cada nodo

1. **Identidad**: elegir `node_id` único (`CAUCE-001`, `CAUCE-002`, …).
2. **Configuración**: copiar `configs/node.example.conf`, completar
   identidad, ubicación (`latitude/longitude/elevation_m`),
   contexto (`land_cover`, `shade_condition`) e `sampling_interval_s`.
   El token del backend de sync NO va en este archivo (ver paso 5).
3. **Hardware**: armar según docs/HARDWARE.md (BME280 en I2C 21/22,
   dirección 0x76, radiación screen obligatoria).
4. **Compilar y flashear**:
   ```powershell
   cd firmware
   pio run -e esp32dev -t upload
   pio device monitor            # observar logs estructurados de arranque
   ```
5. **Provisionar secretos**: Wi-Fi y token de sync se cargan por la UI
   local (`http://<ip-del-nodo>/` → Configuración) usando el token admin
   del nodo; el hash queda en su configuración, nunca el secreto.

## 4. Puesta en marcha y validación de campo

- Conectar el nodo; desde el dashboard local verificar:
  `SENSOR_DISCOVERED` en logs, tarjetas con calidad `VALID`,
  gráfico poblándose.
- Registrar el sitio y el nodo en el central:
  ```http
  POST /v1/sites {"site_id":"...","name":"..."}
  PUT  /v1/nodes/CAUCE-00X/site {"site_id":"..."}
  ```
- Confirmar llegada de datos: `GET /v1/nodes/CAUCE-00X/measurements?limit=5`
  y fila visible en el dashboard central.
- Dejar corriendo ≥48 h co-locados si se busca comparabilidad entre nodos
  (docs/CALIBRATION.md).

## 5. Evaluación antes/después de intervenciones

```http
POST /v1/interventions {"site_id":"...","kind":"sombra",
                        "start_utc_ms":1787356800000}
GET  /v1/analytics/before-after?intervention_id=1&node_id=CAUCE-001&variable=air_temperature
```

El endpoint marca `sufficient_sample:false` hasta que haya ≥30 muestras
válidas por período — no interpretar antes de ese umbral.

## 6. Mantenimiento

- Logs del nodo por serie USB con formato estructurado (grep-able).
- Salud remota: `GET /v1/nodes/{id}` y panel central.
- Rotación de datos del nodo automática (retención configurable);
  el histórico permanente vive en el central.
- Actualizaciones de firmware: **pendiente OTA** — hoy requiere acceso
  físico (USB). No desplegar flotas grandes sin plan de actualización.

## 7. Checklist final de aceptación del piloto

- [ ] `pio test -e native` verde en estación de trabajo
- [ ] Backend levantado con token y rate-limit configurados
- [ ] `run-e2e.ps1` verde tras cualquier cambio de protocolo
- [ ] Cada nodo: identificado, geolocalizado, con calidad VALID fluyendo
      al central, y metadata de instalación documentada

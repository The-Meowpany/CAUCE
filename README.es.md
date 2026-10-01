# CAUCE

*Read in [English](README.md).*

**Observación microclimática que sigue funcionando cuando la red no.**

`MEDIR → VALIDAR → GUARDAR → SINCRONIZAR → COMPARAR → VISUALIZAR`

El sistema tiene dos partes:

- **Nodo** — una microestación ESP32. Lee un BME280, revisa cada muestra
  (rango, tasa de cambio, sensor trabado, duplicados), la guarda en un log
  append-only con CRCs, sirve una API chica y un dashboard, y sincroniza
  lotes firmados cuando consigue conectividad. También decide sobre
  updates de firmware, aunque todavía no puede flashearse solo.
- **Central** — un servidor FastAPI + SQLite. Recibe lotes sin duplicar
  jamás, responde con honestidad lo que realmente guardó, y ofrece
  analítica básica, sitios e intervenciones, reparación de timestamps
  para records sin reloj, y un dashboard en tres idiomas.

Algo que decimos de entrada: nuestros sensores no están certificados y
no medimos la exactitud del sistema en campo. Lo que damos es dato
comparable con la incertidumbre a la vista, no meteorología oficial.

## Quickstart

```powershell
cd firmware
pio test -e native          # 132 tests en host (Unity)
pio run -e esp32dev         # build ESP32
cd ..\backend
pip install -r requirements.txt
python -m pytest tests -q   # 88 tests
..\scripts\run-e2e.ps1      # nodo C++ ↔ FastAPI ↔ SQLite (3 fases)
..\scripts\verify-all.ps1   # todo lo anterior en un gate
```
Para dimensionar el piloto antes de mandarlo a campo:

```powershell
python simulator\load_pilot.py --nodes 8 --days 60 --dry-run
python simulator\load_pilot.py --nodes 8 --days 60 --sync-url http://localhost:8000/v1/sync
```

Para ver la UI del nodo, unirse a su Wi-Fi y abrir
`http://192.168.4.1/` (español/inglés, sin frameworks, sale de flash).
El dashboard central vive en `http://<server>:8000/` y sigue el
`Accept-Language` del browser (inglés/español/portugués).

Sobre el auth de sync: los nodos provisionados firman cada lote con
`X-CAUCE-Node` + `X-CAUCE-Signature`. Si un nodo nunca se provisionó y
`CAUCE_SYNC_REQUIRE_AUTH=1`, el servidor responde 503 en vez de adivinar.
El admin del nodo usa `Authorization: Bearer` contra un SHA-256 guardado
en tiempo constante; sin token configurado, las escrituras admin se
rechazan de plano.

## API del nodo (`/api/v1`)

```
GET  /node                         identidad, versiones, ubicación
GET  /status                       estados FSM, reloj, uptime, última
GET  /measurements/latest          última medición guardada
GET  /measurements?from=&to=       serie JSON en streaming (extremos INCLUSIVOS)
GET  /health                       contadores, storage, fallos
GET  /diagnostics                  bundle de campo (cauce.diag/1), firmado y
                                   posteable al central para triaje
GET  /config                       config activa (secretos nunca expuestos)
POST /config                       cuerpo KV, requiere token admin
GET  /export?format=csv|json       exportación en streaming
```

## API central (`/v1`)

```
POST /provision                    registro de device-key (gated por admin)
POST /sync                         ingesta idempotente (transport: wifi|lora)
GET  /nodes  /nodes/{id}           listado / detalle + última
GET  /nodes/{id}/measurements      serie filtrable (limit…10000)
GET  /nodes/{id}/coverage          esperado vs recibido, brechas con motivo
GET  /nodes/{id}/diagnostics       último bundle de campo (POST para ingerir)
POST /v1/sites  PUT /nodes/{id}/site
PUT /v1/sites/{id}/control         marcar un sitio control sin tratamiento
POST /v1/interventions             registro (end_before_start→422)
GET  /analytics/summary|compare|before-after|period-compare|heat-events|summary-fast
POST /nodes/{id}/time-reconstruct  rellena records con tiempo incierto (marcados, con caveat)
GET  /fleet                        estado por nodo, señales y needs_visit
GET  /maintenance/retention        config de retención + última corrida
GET  /export-all.csv               stream de historia completa
GET  /system                       triaje de flota, cobertura, estado de retención
GET  /                             dashboard localizado
```

## Limitaciones conocidas

Preferimos listarlas acá antes de que te las encuentres a la mala:

- El banco físico (cortes de energía, soak de radio, corridas de 14
  días) aún no pasó. `docs/es/BENCH_PLAN.md` describe exactamente qué
  cubre.
- Sin TLS. No exponer la central fuera de una LAN confiable sin un
  reverse proxy adelante.
- Los números del BME280 son grado datasheet. Nadie en este proyecto los
  verificó en campo, y la exactitud del sistema está sin medir.
- Ocho nodos dispersos producen un campo interpolado suave, no
  resolución a nivel de calle. El mapa dice su resolución efectiva en voz
  alta.
- Los updates de firmware necesitan acceso físico en una placa provisionada
  con la tabla de particiones default de Arduino; la tabla de dos slots con
  rollback (`firmware/partitions.csv`) es lo que llevan las placas nuevas, y
  pasar una placa existente requiere un borrado completo.
- La retención es automática por defecto (365 días). Las filas crudas más
  viejas que la ventana ya no están; sacá `/v1/maintenance/backup` si
  necesitás la historia.
- El camino LoRa y la FSM de rollback están testeados solo en host. Ninguno
  corrió todavía en hardware, y ambos lo dicen.

## Layout

```
firmware/               PlatformIO: cauce_hal → cauce_core → cauce_drivers → cauce_app
firmware/test/          suites Unity (binario único, register*Tests)
firmware/src/main.cpp   composition root ESP32 + demo de simulación en host
backend/cauce_server/   FastAPI + SQLite (api, evaluation, dashboard, analytics, db)
backend/tests/          suite pytest
simulator/              generador de escenarios CSV / feeder del backend (CAUCE_LANG=es)
scripts/                verify-all.ps1 (gate completo) + run-e2e.ps1 (integración viva)
configs/                node.example.conf (esquema KV versionado)
protocol/v1/            frame 68 bytes + spec de payload JSON
deployment/             docker-compose de la central
docs/en/ + docs/es/     espejos inglés/español (índice abajo)
```

## Documentación

Índice completo: [docs/es/DOCUMENTATION_INDEX.md](docs/es/DOCUMENTATION_INDEX.md)
([English](docs/en/DOCUMENTATION_INDEX.md)).

| Categoría | Documentos |
|---|---|
| Primeros pasos | [Testing](docs/es/TESTING.md) · [Deployment](docs/es/DEPLOYMENT.md) · [Hardware](docs/es/HARDWARE.md) |
| Arquitectura | [Architecture](docs/es/ARCHITECTURE.md) · [Data Model](docs/es/DATA_MODEL.md) · [Glosario](docs/es/DOMAIN_GLOSSARY.md) |
| Contratos | [Node API](docs/es/API.md) · [Sync](docs/es/SYNC.md) · [Backend](docs/es/BACKEND.md) |
| Confianza | [Security](docs/es/SECURITY.md) · [Calibration](docs/es/CALIBRATION.md) · [OTA](docs/es/OTA.md) |
| Operación | [Dashboard](docs/es/DASHBOARD.md) · [Bench Plan](docs/es/BENCH_PLAN.md) |
| Planificación | [Roadmap](docs/es/ROADMAP.md) · [Pilot Spec](docs/es/PILOT_SPEC.md) · [Política i18n](docs/es/I18N.md) |

## Estado

112 firmware + 68 backend tests + build ESP32 + E2E = **180 chequeos
automatizados en verde**. Qué existe y qué no: [STATUS.md](STATUS.md)
(inglés).

MIT — ver [LICENSE](LICENSE).

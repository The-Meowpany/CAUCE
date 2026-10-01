# Backend central CAUCE

Un servidor para muchos nodos: ingesta, historia, API de consulta,
analítica básica. **Los nodos andan perfecto sin él** (offline-first) —
este servicio existe por la vista agregada, nada más.

## Stack

Python 3.12 · FastAPI · SQLite (WAL), sin ORM. Si alguna vez hace falta
PostgreSQL, la migración está acotada a `db.py` y a ningún otro lado.

## Endpoints

| Método | Ruta | Descripción |
|---|---|---|
| GET | `/healthz` | Liveness |
| POST | `/v1/sync` | Ingesta de lotes (contrato: SYNC.md). Los nodos provisionados DEBEN firmar con HMAC-SHA256 (`X-CAUCE-Node`, `X-CAUCE-Signature`) |
| POST | `/v1/provision` | Registro de device-key (gated por admin), una key por nodo |
| GET | `/v1/nodes` · `/v1/nodes/{id}` | Listado / detalle + última medición |
| GET | `/v1/nodes/{id}/measurements` | Serie filtrable (`variable`,`quality`,`from_utc_ms`,`to_utc_ms`,`limit≤10000`,`offset`) con `total` |
| POST | `/v1/maintenance/retention` | Borra filas viejas de N días + VACUUM (apto cron; los nodos reenvían lo borrado salvo purga allá también) |
| GET | `/v1/maintenance/backup` | Descarga snapshot SQLite vía VACUUM INTO (con auth) |
| POST/GET | `/v1/sites` · PUT `/v1/nodes/{id}/site` | Sitios de instalación + asignación |
| PUT | `/v1/sites/{id}/location` | Setear coordenadas del sitio (lat/lon validados) |
| POST/GET/DELETE | `/v1/alerts/rules` · PATCH `/v1/alerts/rules/{id}` · GET `/v1/alerts/log` · POST+GET `/v1/alerts/check` | Reglas de umbral/silencio (webhook/Telegram, inputs validados, toggle), historial con intentos, pasada manual + reenvío |
| POST/GET | `/v1/interventions` | Registro de intervenciones (`end_before_start`→422) |
| GET | `/v1/analytics/summary` | Stats descriptivos por nodo+variable |
| GET | `/v1/analytics/compare` | Dos nodos lado a lado + diferencia media |
| GET | `/v1/analytics/before-after` | Corte pre/post por ventana de intervención; avisa bajo 30 muestras por período |
| GET | `/v1/analytics/period-compare` | Mismo nodo, dos ventanas |
| GET | `/v1/analytics/heat-events` | Cuánto tiempo estuvo sobre un umbral |
| GET | `/v1/analytics/summary-fast` | Mismos stats desde agregados horarios materializados (O(buckets)) |
| POST | `/v1/nodes/{id}/time-reconstruct` | Rellena timestamps de records iniciales con tiempo incierto desde la primera ancla + intervalo mediano; marca filas (`ts_reconstructed=1`) y dice el caveat de margen de error en voz alta |
| GET | `/v1/export-all.csv` | Stream CSV de historia completa |
| GET | `/` | Dashboard HTML en tu idioma (Accept-Language: en/es/pt) |
| GET | `/legal/{terms,privacy,cookies,refunds}` | Páginas legales versionadas (contenido negociado en/es) |
| GET | `/nodes/{id}` | Página humana del nodo: info, tarjetas, gráfica, tabla reciente (links a JSON/CSV) |
| GET | `/compare` | Comparación lado a lado con gráfica superpuesta (?a=&b=&variable=&days=) |
| GET | `/nodes/{id}/events` | Página humana de eventos de calor: form, tabla o vacío explicado |
| GET | `/nodes/{id}/report` | Reporte de evidencia imprimible: stats, calidades, eventos de calor |
| GET | `/map` | Mapa SVG de sitios, nodos coloreados por última temperatura |
| GET | `/colocation` | Overlay de todos los nodos (?variable=&days=) con tabla de divergencias |
| GET | `/alerts` | CRUD de reglas, historial, trigger manual |

Ventanas de tiempo INCLUSIVAS en ambos extremos. Los valores máquina
quedan como códigos en inglés; las etiquetas humanas se localizan en
presentación.

## Garantías de ingesta

- **Idempotencia**: PK `(node_id, sequence)`. Los reenvíos nunca
  duplican, por construcción.
- **Ack honesto**: la máxima secuencia realmente sentada en la base. El
  servidor no adivina para adelante.
- **Atomicidad**: un record malo arruina el lote — rollback total, para
  que el cliente corrija y reenvíe en vez de dejar medio lote atrás.
- Cada lote queda en `sync_batches` con su `transport` (wifi o lora;
  los lotes LoRa llegan vía gateway bridge).
- Los exports CSV piden bearer token cuando `CAUCE_API_TOKEN` está
  seteado, y tienen rate-limit como todo lo demás bajo `/v1/*`.

## Seguridad

- `CAUCE_SYNC_TOKEN` / `CAUCE_API_TOKEN`: cuando están seteados, cada
  llamada necesita `Authorization: Bearer <token>`, comparado en tiempo
  constante.
- **Identidad por dispositivo**: `/v1/provision` le da a cada nodo su
  propia key HMAC; los lotes firmados se verifican sobre el cuerpo crudo
  con `hmac.compare_digest`. Una key filtrada compromete un nodo, no la
  flota.
- HTTP plano por diseño por ahora. Poner un reverse proxy con TLS
  adelante antes de que esta caja vea una red no confiable. El TLS
  nativo ESP32 es otro trabajo.
- Rate limiting por IP en SQLite (default 120 req/min) en API,
  evaluation y rutas dashboard/CSV; respeta `X-Forwarded-For` solo con
  `CAUCE_TRUST_PROXY=1`.
- Cero datos personales: telemetría ambiental e ids de nodo, punto.

## Límites de analítica

Cada respuesta lleva tag `metric_type: derived*` más nota de
no-causalidad, porque una diferencia de temperatura entre dos nodos
casi siempre es emplazamiento, no prueba. INVALID/MISSING/ESTIMATED
quedan fuera de la matemática; SUSPECT cuenta pero su proporción se
reporta junto al resultado.

## Ejecutar

```bash
cd backend
pip install -r requirements.txt
pytest tests -q                      # 68 tests
uvicorn cauce_server.main:app --port 8000
```

Docker: `docker compose -f deployment/docker-compose.yml up --build`

## Notas serverless

La app es stateless por construcción: sin threads, sin websockets, sin
workers de fondo, sin cachés en proceso — cada byte de estado vive en
la base. Las alertas de calor disparan en ingesta; las de silencio se
evalúan en `GET /v1/alerts/check`, que es el entrypoint de cron
(cualquier scheduler pegándole cada 15 min reemplaza un worker). Toda
la configuración sale de variables `CAUCE_*`, incluido el path de la
base. Un caveat: los archivos SQLite no sobreviven filesystems
efímeros serverless — un deploy serverless necesita Postgres, y esa
migración está acotada a `db.py` (la costura documentada).

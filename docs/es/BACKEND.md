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
| GET | `/v1/maintenance/retention` | Config de retención y última corrida: `older_than_days`, `deleted_measurements`, `vacuumed`, `last_run_utc_ms` |
| GET | `/v1/nodes/{id}/coverage` | Esperado vs recibido por nodo y variable: `coverage_pct`, brecha máxima, top de brechas con motivo |
| GET | `/v1/sites/{id}/coverage` | Igual agrupado por los nodos del sitio, más `worst_node` para que un nodo mudo no se esconda |
| POST | `/v1/nodes/{id}/diagnostics` | Ingesta del bundle de diagnóstico de campo (firmado HMAC como `/v1/sync`, guarda los últimos 5 por nodo) |
| GET | `/v1/nodes/{id}/diagnostics` | último bundle almacenado |
| GET | `/v1/fleet` | Estado por nodo: firmware, último sync, storage, señales, `needs_visit` |
| GET/POST | `/v1/sites` · PUT `/v1/sites/{id}/control` | Sitios de instalación + asignación; marcar un sitio como control sin tratamiento |
| GET | `/v1/maintenance/backup` | Descarga snapshot SQLite vía VACUUM INTO (con auth) |
| POST/GET | `/v1/sites` · PUT `/v1/nodes/{id}/site` | Sitios de instalación + asignación |
| PUT | `/v1/sites/{id}/location` | Setear coordenadas del sitio (lat/lon validados) |
| POST/GET/DELETE | `/v1/alerts/rules` · PATCH `/v1/alerts/rules/{id}` · GET `/v1/alerts/log` · POST+GET `/v1/alerts/check` | Reglas de umbral/silencio (webhook/Telegram, inputs validados, toggle), historial con intentos, pasada manual + reenvío |
| POST/GET | `/v1/interventions` | Registro de intervenciones (`end_before_start`→422) |
| GET | `/v1/analytics/summary` | Stats descriptivos por nodo+variable |
| GET | `/v1/analytics/compare` | Dos nodos lado a lado + diferencia media |
| GET | `/v1/analytics/before-after` | Corte pre/post por ventana de intervención; avisa bajo 30 muestras por período; con sitios control devuelve `control_group.difference_in_differences` |
| GET | `/v1/analytics/period-compare` | Mismo nodo, dos ventanas |
| GET | `/v1/analytics/heat-events` | Cuánto tiempo estuvo sobre un umbral |
| GET | `/v1/analytics/summary-fast` | Mismos stats desde agregados horarios materializados (O(buckets)) |
| POST | `/v1/nodes/{id}/time-reconstruct` | Rellena timestamps de records iniciales con tiempo incierto desde la primera ancla + intervalo mediano; marca filas (`ts_reconstructed=1`) y dice el caveat de margen de error en voz alta |
| GET | `/v1/export-all.csv` | Stream CSV de historia completa |
| GET | `/v1/nodes/{id}/coverage.csv` | Fila de resumen de cobertura y luego la tabla de brechas |
| GET | `/system` | Vista de flota: firmware por nodo, señales y aviso de visita, cobertura de 7 días, estado de retención |
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

## Higiene de la evidencia

Dos cosas convierten una comparación en evidencia, y las dos son endpoints:

- **Cobertura primero.** `/v1/nodes/{id}/coverage` responde cuánto de
  período reportó de verdad ese nodo. `coverage_pct` compara muestras
  recibidas contra `expected_samples`, que asume un período de muestreo
  constante: pasá `expected_interval_ms` desde la config del nodo en vez
  de citar el default. Cada brecha trae un motivo: `no_data` (no llegó
  nada), `measured_not_delivered` (el nodo sincronizó en esa ventana pero
  faltan las muestras) o `clock_uncertain`. Un before/after calculado
  sobre un período con un nodo muerto no es evidencia, y esto lo detecta.
- **Grupo de control.** Marcá los sitios sin tratar con
  `PUT /v1/sites/{id}/control {"control": true}`. `before-after` entonces
  devuelve `difference_in_differences`: el cambio del sitio tratado menos
  el cambio medio de los controles, para que una ola de calor regional no
  se le adjudique a una intervención. Los controles con menos de 30
  muestras por ventana aparecen pero quedan excluidos, con la distancia al
  sitio tratado para que judges si el emparejamiento es justo. Sigue
  siendo difference-in-differences, no un ensayo aleatorizado, y la
  respuesta lo dice.

## Retención

`POST /v1/maintenance/retention {"older_than_days": N}` purga
`measurements` y `agg_hourly` y hace vacuum. La misma purga ahora corre
sola: un hilo demonio revisa cada hora y purga cada
`CAUCE_RETENTION_INTERVAL_H` (24 por defecto) cuando
`CAUCE_RETENTION_ENABLED=1` (activo por defecto) y
`CAUCE_RETENTION_DAYS>=30`. El scheduler nunca baja de 30 días diga lo que
diga el entorno, y el VACUUM se limita a `CAUCE_VACUUM_INTERVAL_H` porque
reescribe el archivo entero. `GET /v1/maintenance/retention` informa la
última corrida y `/system` la muestra. Ocho nodos cada 60 s por año son
unos 33 M de filas, así que algo tiene que ceder; si necesitás la historia
completa, subí la ventana de retención por encima de tu período de
análisis y saca snapshots con `/v1/maintenance/backup`.

## Diagnóstico de flota

`GET /v1/fleet` existe para que nadie vaya a un sitio sin motivo. Un nodo
necesita visita cuando lleva más de 24 h caído, su reloj no está fijado, el
storage está casi lleno, hay tramas corruptas, fallaron escrituras, o la
batería marca menos de 3.4 V. La ausencia de un bundle de diagnóstico
también es una señal, porque el nodo que no puede explicar por qué está
roto es justo el que hay que visitar. `firmware_spread` dice si la flota
está en una sola imagen, lo cual importa mientras las actualizaciones
todavía necesiten acceso físico. Pasá `storage_capacity_bytes` para
convertir los registros almacenados en porcentaje.

## Ejecutar

```bash
cd backend
pip install -r requirements.txt
pytest tests -q                      # 88 tests
uvicorn cauce_server.main:app --port 8000
```

Docker: `docker compose -f deployment/docker-compose.yml up --build`

## Notas serverless

La app es stateless por construcción: sin websockets, sin
workers de fondo, sin cachés en proceso — cada byte de estado vive en
la base. Las alertas de calor disparan en ingesta; las de silencio se
evalúan en `GET /v1/alerts/check`, que es el entrypoint de cron
(cualquier scheduler pegándole cada 15 min reemplaza un worker). El único hilo que corremos es el scheduler de retención,
y no guarda estado: vuelve a leer su sello de última corrida desde `maintenance_state` en cada tick. Toda
la configuración sale de variables `CAUCE_*`, incluido el path de la
base. Un caveat: los archivos SQLite no sobreviven filesystems
efímeros serverless — un deploy serverless necesita Postgres, y esa
migración está acotada a `db.py` (la costura documentada).

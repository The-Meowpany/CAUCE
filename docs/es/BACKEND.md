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
| GET | `/v1/nodes/{id}/measurements` | Serie filtrable (con `cursor` para paginado estable y `next_cursor` de vuelta) (`variable`,`quality`,`from_utc_ms`,`to_utc_ms`,`limit≤10000`,`offset`) con `total` |
| POST | `/v1/maintenance/retention` | Borra filas viejas de N días + VACUUM (apto cron; los nodos reenvían lo borrado salvo purga allá también) |
| GET | `/v1/maintenance/retention` | Config de retención y última corrida: `older_than_days`, `deleted_measurements`, `vacuumed`, `last_run_utc_ms` |
| GET | `/v1/nodes/{id}/coverage` | Esperado vs recibido por nodo y variable: `coverage_pct`, brecha máxima, top de brechas con motivo |
| GET | `/v1/sites/{id}/coverage` | Igual agrupado por los nodos del sitio, más `worst_node` para que un nodo mudo no se esconda |
| POST | `/v1/nodes/{id}/diagnostics` | Ingesta del bundle de diagnóstico de campo (firmado HMAC como `/v1/sync`, guarda los últimos 5 por nodo) |
| GET | `/v1/nodes/{id}/diagnostics` | último bundle almacenado |
| GET | `/v1/fleet` | Estado por nodo: firmware, último sync, storage, señales, `needs_visit` |
| GET/POST | `/v1/sites` · PUT `/v1/sites/{id}/control` | Sitios de instalación + asignación; marcar un sitio como control sin tratamiento |
| PUT/GET | `/v1/sites/{id}/calibration` | Offset/escala por (sitio,variable); las filas crudas nunca se reescriben, el valor calibrado se deriva en la lectura |
| POST/GET | `/v1/sites/{id}/maintenance` | Bitácora: instalación, calibración, recambio de sensor, traslado |
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

## Downlink

El central puede mandar instrucciones a un nodo que ya reporta. Viajan en la
misma respuesta de `/v1/sync` que acusa un lote, así que no agrega un
despertar extra de radio ni un modo de falla nuevo en el nodo.

| Método | Ruta | Descripción |
|---|---|---|
| POST | `/v1/nodes/{id}/commands` | Encolar un comando. Token admin |
| GET | `/v1/nodes/{id}/commands` | Listar con estado `pending` / `delivered` / `acked` / `expired` |

Dos propiedades lo hacen seguro de reintentar, que es la única que importa
cuando el otro extremo es un dispositivo offline-first:

- **Idempotencia.** `(node_id, idempotency_key)` es único. Reenviar la misma
  clave devuelve el comando original con `created: false` en vez de encolar un
  segundo. Reusar una clave con un cuerpo *distinto* es `409`, porque eso es un
  bug del cliente e ignorarlo en silencio lo escondería.
- **Entrega honesta.** Un comando queda `pending` hasta que el nodo lo reporta
  en una vuelta de `/v1/sync`, pasa a `delivered` cuando llega, y a `acked`
  solo cuando el nodo manda un resultado. Un comando expirado deja de
  ofrecerse. Nada se reporta como hecho por el simple hecho de haber sido
  encolado.

El nodo responde con `command_receipts` en su siguiente lote. Un acuse para el
comando de otro nodo, o para un comando que no existe, se cuenta y se ignora:
el nodo es un par autenticado pero no es autoridad sobre el comando de otro.

Los tipos son pocos y sin efectos secundarios a propósito:
`set_sampling_interval`, `set_sync_interval`, `request_resync`, `set_led_mode`.
El nodo que va en el repo valida sus argumentos y responde; no se reconfigura a
sí mismo desde el campo. La actuación real viene con hardware que se pueda
actuar.


## Calibración

El central es dueño de la calibración: `measurements.value` sigue crudo y el
valor calibrado se deriva en la lectura, para que una recaliibración se
vuelva a aplicar sobre todo el histórico sin tocar un nodo.
`calibrated_value = raw_value * scale + offset`, registrado por (sitio,
variable) con `PUT /v1/sites/{id}/calibration`. Como el mapa es lineal
también es exacto sobre los agregados horarios, por eso `/summary-fast` y
`granularity=hourly` corrigen sus buckets en vez de releer filas crudas.
Toda respuesta de analítica dice en un objeto `calibration` si se aplicó;
cuando no, la clave `calibrated` está ausente en vez de ser idéntica al
valor crudo. Ver `CALIBRATION.md` para las reglas de honestidad sobre lo
que un offset de co-localización compra y lo que no.

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
pytest tests -q                      # 420 tests
uvicorn cauce_server.main:app --port 8000
```

Docker: `docker compose -f deployment/docker-compose.yml up --build`

## Formato de radio LoRa

Un frame de medicion son 68 bytes y un payload LoRa en SF9 son 115, asi que un
lote nunca entra en un solo uplink. `LoRaBatchEncoder` corta el lote en frames
y `LoRaBatchReassembler` lo rearma del lado receptor.

Cada frame lleva suficiente header para que el receptor lo rechace sin pedir
retransmision: batch id, indice de fragmento, cantidad de fragmentos, la
cantidad de registros que el lote completo deberia tener, y el largo del
payload. La redundancia es deliberada, porque un fragmento perdido cuesta el
lote entero y el tiempo de radio es el recurso caro.

```
frame   0..1   magic 0xCA | version 1
        2..3   batch_id
        4      fragment_index
        5      fragment_count
        6..7   record_count del lote completo
        8..9   payload_bytes
        10..   payloads de registros, 60 bytes cada uno
        ultimos 2  CRC16-CCITT sobre todo lo anterior
```

El factor de dispersion decide cuantos registros entran en un uplink:

| SF | Presupuesto | Registros por uplink | Un lote de 4 |
|---|---|---|---|
| SF7 | 222 B | 3 | 2 uplinks |
| SF8 | 222 B | 3 | 2 uplinks |
| SF9 | 115 B | 1 | 4 uplinks |
| SF10 y menos | 51 B o menos | rechazado | rechazado |

SF10 se rechaza en vez de truncar: una medicion cortada por la mitad es peor
que no tener medicion, asi que el nodo reporta un error reintentable y conserva
los registros.

`lora_frames.py` es el decoder de referencia y sirve de gateway: convierte los
frames rearmados en un body de `POST /v1/sync`. El encoder en C++ y el decoder
en Python son implementaciones deliberadamente independientes del mismo
formato, y `tests/test_lora_frames.py` fija los bytes exactos que produce el
firmware, asi que un cambio en cualquier lado aparece como un test que falla y
no como un gateway que en silencio deja de aceptar nodos.

Lo que esto **todavia no** hace: no hay firmware de gateway, no hay driver de
radio LoRa, y no hay medicion de ningun link budget. El formato del frame y su
acuse estan testeados en host; la interfaz de aire no esta probada.


## Autorización

Dos modelos, porque un token es honesto para un operador y equivocado para más
de uno:

- `CAUCE_API_TOKEN` es el token admin compartido. Tiene todos los scopes, y un
  despliegue de un solo operador no necesita configuración alguna.
- `api_tokens` guarda principales reales. Cada uno tiene nombre, el digest
  SHA-256 de su token, una lista de scopes separada por comas, y opcionalmente
  un `site_id`. Un principal con `site_id` solo puede tocar ese sitio; uno sin
  él es de flota completa. `admin` en la lista satisface todos los scopes.

Los tokens se guardan como digest, nunca en claro, y se comparan en tiempo
constante. Un token perdido implica emitir uno nuevo, no releer el viejo.

Los scopes se exigen antes de validar el payload, así que un llamador sin el
scope no aprende nada del endpoint más allá de que existe:

| Capacidad | Permite |
|---|---|
| `read` | Leer calibración, mantenimiento y estado de comandos |
| `write` | Cambiar calibración, registrar mantenimiento, encolar comandos |
| `admin` | Todo lo anterior |

El scoping por sitio es la mitad que evita que se filtren datos. Los scopes
solos limitan *qué* puede hacer un operador pero nunca *dónde*, lo que dejaría a
un operador de sitio libre de leer datos de otro sitio por una URL que adivinara.


## Gateway

`LoRaGateway` es la contraparte en el gateway del encoder: recibe frames,
 rearma lotes, los reenvía a `POST /v1/sync` y acusa lo que el central guardó.
Todo el bucle se ejercita contra la app real de FastAPI mediante un transport
inyectado, así que `tests/test_lora_gateway.py` maneja frames construidos
exactamente como los construye el firmware y después lee las filas de SQLite.

Todo excepto el driver de radio es, por tanto, lógica normal y testeada. Lo que
falta para M2 es un driver SX1276, un proceso que corra esto en hardware, y un
link budget.

**El bucle refleja el del nodo**, porque los dos tienen que coincidir:

1. Los frames se bufferean hasta completar un lote.
2. El lote se reenvía como un solo `POST /v1/sync`.
3. El acuse lleva la secuencia más alta que el central **guardó de verdad**, no
   la más alta que se envió. Si difieren, el nodo reenvía el resto y el central
   deduplica, que es el resultado correcto.
4. El buffer de reensamblado se libera en un `finally`, así que un fallo al
   reenviar no filtra el lote.

El ruido se cuenta en vez de lanzar excepción. Una radio entrega frames corruptos
a rutina y un gateway que lanzara con cada CRC malo reiniciaría en cada
tormenta.

**Límite de confianza, enunciado y no insinuado.** Un nodo provisionado se
autentica con un HMAC sobre el body crudo de la petición, así que un gateway que
reenvía en nombre de un nodo debe tener la `device_key` de ese nodo. Eso hace
del gateway una parte de confianza equivalente a todos los nodos que sirve: quien
lo comprometa puede falsificar cualquiera. La distribución de claves a gateways
es por tanto un costo real de esta topología, no un detalle.

La alternativa es que el nodo firme el frame compacto y el central verifique
contra el frame en vez de contra el body reconstruido, lo que movería la
superficie de verificación al formato de radio. Eso no está implementado aquí.

Lo que sí se exige es que un gateway configurado sin clave no degrade en
silencio a un nodo provisionado a no firmado: el central devuelve 401, el gateway
lanza, y no se acusa nada, así que el nodo conserva sus filas y reintenta. Un
downgrade silencioso habría sido el fallo preocupante, porque parecería que
funciona.


## Forma de entrada y endurecimiento de respuestas

Dos reglas que salieron de revisar los hallazgos del análisis estático, las dos
porque la versión obvia resultó más débil de lo que parecía.

**Los identificadores se restringen, el texto libre se escapa.** Un `site_id`
debe cumplir `[A-Za-z0-9_-]` y medir menos de 64 caracteres, la misma forma que
`NodeConfig` ya exige para un node id. Antes aceptaba cualquier string no vacío,
lo que permitía almacenar un valor con markup y luego devolverlo dentro de una
respuesta de la API. Restringirlo en la puerta es la capa correcta, porque un
site id además llega a URLs y a nombres de archivo de exportación. El *nombre*
del sitio sigue siendo texto libre, que es justo para lo que es un nombre, y se
escapa a la salida.

**Las respuestas JSON declaran su tipo.** `json.dumps` no escapa `<`, `>` ni `&`,
así que un identificador almacenado con markup aparece literal en el body.
Servido como `application/json` eso es inerte, pero un navegador que adivinara el
tipo no lo sería, así que toda respuesta que no sea HTML lleva
`X-Content-Type-Options: nosniff`. Las páginas del dashboard deliberadamente no
lo llevan: son el único sitio donde el navegador debe renderizar lo que
enviamos, y llevan sus propias cabeceras.

### Qué era y qué no era el alert de XSS

El hallazgo de XSS reflected señalaba el dashboard de eventos. Se investigó en
vez de descartarse, y es un **falso positivo en esa ruta**: ahí todos los
valores reflejados van escapados, y un `node_id` hostil ni siquiera llega a un
200 porque el routing del path lo rechaza primero.
`tests/test_security.py` fija ambas mitades, así que un cambio que quite el
escape falla ahí y no en un navegador.

La investigación sí encontró una debilidad real al lado, que es la regla de
`site_id` de arriba. Reportar un alert como falso positivo no debería
significar que la revisión fue perdida.


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

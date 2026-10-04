# Roadmap CAUCE

Hacia dónde va esto y en qué orden. `STATUS.md` dice qué existe hoy;
este archivo dice qué sigue. Las ventanas de tiempo quedan INCLUSIVAS
en ambos extremos, firmware y backend — esa decisión está cerrada.

## Principios

- Offline-first, siempre. Un hito que deja de medir sin internet es un
  hito fallido.
- Lugares primero, radios después. La elección de sitios manda sobre la
  de comunicaciones, no al revés.
- Sin mesh hasta que se demuestre necesaria. Los gateways escalan; el
  ruteo entre vecinos no, menos a batería.
- Payloads chicos por LoRa, resolución completa por Wi-Fi. Cada
  transporte hace lo que sabe hacer.

## M0 — Estabilizar la base (hecho)

- `main.cpp` por fin cablea todo lo que los tests asumían:
  `NetworkManager` + `Esp32WifiController`, endpoint de sync y device
  secret desde `sync_server_url/sync_device_key`, NTP al primer link,
  reintento de DNS del portal, sync en Connected/Degraded, OTA con
  catálogo nulo.
- Backend: un `ratelimit.check_rate` compartido (proxy-aware) en API,
  evaluation y dashboard; gate bearer en exports CSV; filename
  sanitizado; `sync_batches.transport` en {wifi, lora}; ventanas
  before-after inclusivas sin overlap.
- Config: `sync_server_url`, `lora_enabled`, `lora_sync_interval_s`,
  `lora_region`, validados, con ejemplo.
- Docs en 109/32, `SECURITY.md` raíz, este roadmap.
- i18n: ES/EN en la SPA del nodo, EN/ES/PT en el dashboard central,
  `CAUCE_LANG` en scripts y simulador; docs espejados en `docs/en` +
  `docs/es`.

Hecho significa: `verify-all.ps1` en verde más una placa sincronizando
de verdad por Wi-Fi. Ambas ciertas.

## M1 — Banco físico B1–B5 (requiere hardware)

Según `BENCH_PLAN.md` con 3–4 nodos: lecturas BME280 reales, LittleFS
bajo cortes, roam Wi-Fi STA/AP, disciplina NTP, perfilado de corriente.
Sin cambios de código esperados más allá de tuning de thresholds y
timeouts — si el banco exige un rewrite, los tests host mentían.

Hecho significa: un log de banco con RSSI, % entrega, consumo por
estado.

## M2 — Spike LoRa P2P, 2 nodos + 1 gateway casero (2 sem, barato)

El objetivo es link-budget en tus sitios reales, no elegancia de
protocolo.

- HAL: `ILoRaRadio {send/receive/sleep}` más `LoRaSyncTransport` tras
  `ISyncTransport::postBatch`, frame 60B + HMAC, respetando
  `lora_sync_interval_s` y el duty cycle EU868.
- Política: 1 uplink cada 10–15 min, SF7–9, solo agregado horario +
  eventos. Nada de firehose crudo de 60 s por LoRa, jamás.
- Gateway: un ESP32 + SX1276 forwardeando a `POST /v1/sync` con
  `"transport": "lora"`. La lógica de forwarding ya existe y está probada
  end to end contra el central (`lora_gateway.py`, including el
  reensamblado, el acknowledgement y la firma), así que lo que falta es el
  driver de radio y un proceso donde correrlo en hardware. Una
  consecuencia queda escrita en vez de asumida: como un nodo
  provisionado se autentica con un HMAC sobre el body del request, el
  gateway tiene que guardar la device key de ese nodo, lo que lo deja
  tan confiado como los nodos a los que sirve.
- OTA por LoRa afuera. Check de manifiesto como mucho.

Hecho significa: RSSI/SNR vs distancia, % entrega, y una decisión
documentada de SF y período.

## M3 — LoRaWAN de verdad (el camino recomendado)

- Los nodos entran por OTAA; las keys por dispositivo reusan el
  `device_key` de `POST /v1/provision`. Sin segunda infraestructura de
  keys.
- Gateway: comercial, o ChirpStack en Raspberry Pi si querés soberanía.
  Bridge `ChirpStack MQTT -> POST /v1/sync` con decoder del frame 68B.
- El backend ya guarda `transport`; agregar contadores por transporte y
  un log de calidad de link (RSSI/SNR/SF) para que el emplazamiento de
  gateways sea una decisión con datos.
- Firmware: muestreo cada 60 s local, agregado horario LoRa, batch
  completo por Wi-Fi; `SleepPolicy` usado de verdad entre muestras.

Hecho significa: 3 nodos × 7 días en central con `time-reconstruct`
cubriendo los huecos LoRa.

## M4 — Piloto de 8 nodos a escala km (congelado en PILOT_SPEC.md)

Elegir 8 sitios por interés ambiental, survey de altura y antena del
gateway, desplegar 1–2 gateways. Todo alimenta una base. Agregar
ChirpStack, backups y reverse proxy HTTPS según `DEPLOYMENT.md`. Un
prototipo con 6 pruebas de aceptación antes de cualquier compra ×8.

Hecho significa: una base de datos, un mapa de cobertura con logs
reales de link, y un runbook de operación que no sea ficción.

## M5 — Endurecimiento operativo

Terminación TLS, paginación por cursor, el mapa central, calibración runtime
(`CALIBRATION.md`). El CSV por nodo ya está entregado, y también lo que
decide si un despliegue en campo es sobrevivible:

- Retención de `measurements` + `VACUUM`, ahora en un scheduler además del
  endpoint, con la última corrida visible en `/system`.
- Contabilidad de cobertura (`/v1/nodes/{id}/coverage`, la variante de
  sitio, CSV) para poder contrastar un before/after con cuánta datos llegó
  de verdad.
- Sitios de control y `difference_in_differences` en `before-after`.
- Bundles de diagnóstico de campo, ingeridos en el central, más
  `/v1/fleet` respondiendo qué nodo necesita visita.
- `simulator/load_pilot.py` para ensayar el volumen del piloto.
- **Terminación TLS** vía `deployment/Caddyfile`: CA interna automática,
  dominio desde `CAUCE_DOMAIN`, y el central atado a `127.0.0.1` para que
  solo el proxy lo alcance.
- **Paginación por cursor** en measurements (`timestamp_utc_ms, sequence`)
  y en nodes (`node_id`), para que una exportación larga siga consistente
  mientras la flota sigue escribiendo. `limit`/`offset` siguen funcionando.
- **Descarga asíncrona de OTA**: el reader del firmware devuelve un
  tri-estado por chunk en vez de dormir 10 s esperando bytes, y el manager
  se rinde con `OTA_STALLED` en vez de bloquear el scheduler para siempre.
  La apertura HTTP inicial sigue siendo bloqueante.
- **Política de deep sleep**: `DeepSleepController` decide cuándo le está
  permitido dormir (no con datos pendientes sin sincronizar, no durante una
  OTA, no por debajo del gate de batería). Va **deshabilitado por
  defecto** porque nadie midió el consumo todavía.
- **Analítica de ventanas largas**: `granularity=auto` responde un pedido
  de 30 días desde los buckets de `agg_hourly` en vez de streamear cada
  fila cruda, y dice en la respuesta qué granularidad usó.
- **Downlink, idempotente por construcción.** El central encola un comando
  por `(node_id, idempotency_key)` y lo entrega en la misma respuesta de
  `/v1/sync` que acusa un batch, así que no cuesta un despertar de radio
  extra. El nodo recuerda los ids aplicados en flash y reporta un
  receipt; el central reofrece el comando hasta que llega. La entrega es
  honesta: `pending` significa que nadie lo confirmó, `expired` que envejeció,
  y ninguno se reporta en silencio como hecho. Los kinds que van son
  solo de validación (`set_sampling_interval`, `set_sync_interval`,
  `request_resync`, `set_led_mode`): chequean sus argumentos y reportan,
  pero no reconfiguran el nodo en curso, porque un comando que pueda parar
  un nodo reportando es uno que nadie debería mandar por accidente. La
  actuator real espera hardware que se pueda actuar.
- **Rollback de OTA ya cableado.** `OtaBootConfirm` lleva un contador de
  intentos de arranque en flash y marca la imagen válida cuando el nodo
  demuestra que puede guardar una medición, o hace rollback después de
  tres arranques malos. Sin él, una imagen que crasheó en el primer tick
  quedaría instalada para siempre.

Sigue abierto acá:

- **Rollback de OTA en hardware.** La política, el contador y las llamadas
  de partición del ESP32 están cableados y probados en host, pero nadie
  ha flasheado una placa, marcado una imagen válida, ni visto una imagen
  mala hacer rollback. Hasta que pase, las actualizaciones necesitan
  acceso físico.
- **Incertidumbre de calibración.** El central aplica offsets y escalas
  correctamente, pero nada lleva una estimación de incertidumbre y no hay
  procedimiento formal. Hasta entonces una lectura calibrada es una mejor
  comparación relativa, no una medición trazable.
- **Medición de consumo en deep sleep.** La política está probada; el
  ahorro no.
- Un certificado real si el central alguna vez se expone públicamente — el
  deployment ya pide ACME por defecto, así que esto es un problema de DNS,
  no de código.
- **LoRa en el cable, en el aire todavía no.** El frame compacto de 68
  bytes, la fragmentación, el reensamblado, el acknowledgement del gateway
  y un decoder de referencia existen y están contrastados entre el encoder
  C++ y el de Python. Lo que sigue faltando es un gateway, un driver de
  radio y cualquier link budget: el formato está probado, la interfaz de
  aire no.
- **La incertidumbre de calibración** se registra y se propaga, así que un
  reporte puede separar medición de método. Sigue sin haber procedimiento
  de calibración, que es lo que la trazabilidad realmente exigiría.
- **La autorización es por principal** cuando la querés: tokens nombrados
  con scopes y un sitio opcional. Un despliegue de un solo operador sigue
  usando el admin token compartido y no necesita cambiar nada.

## No-objetivos por ahora

Ruteo mesh completo, OTA de firmware por LoRa, MQTT/CoAP en nodos
constreñidos, roaming LoRaWAN multi-región, firmas asimétricas. Cada uno se
sugirió al menos una vez; cada uno espera su turno.

La OTA de firmware por LoRa ahora tiene un número adjunto en vez de una
opinión: un registro de 68 bytes no entra en un presupuesto de airtime de
222 bytes a ningún spreading factor que todavía llegue a un kilómetro, así
que el check del manifiesto es hasta donde esa idea puede llegar
honestamente.

Dos de ellos cambiaron de estado cuando se examinaron las interfaces:

- **Loops de control por downlink** eran un no-objetivo y ya no lo son, a
  nivel de transporte. `ILoRaRadio` e `ISyncTransport` eran ambos
  send-only, que es lo que en realidad lo bloqueaba; los dos tienen ahora
  camino de recepción. Lo que va es entrega e idempotencia, no
  actuator: ver M5.
- **El intercambio peer-to-peer** sigue siendo un no-objetivo, y agregar
  `receive()` no cambió eso. Una radio que escucha a un vecino no es un
  mesh: es el prerrequisito de un mesh, y la razón por la que es barato
  postergarlo es que el camino de sync ya es idempotente.

Ed25519 queda afuera por una razón concreta y no por gusto: el firmware
tiene SHA-256 y HMAC y nada de bignum, así que la aritmética de curvas
iría escrita de cero en una placa sin margen para revisarla. Keys
simétricas por dispositivo más TLS cubren el modelo de amenazas del
piloto; un esquema de firmas vale la pena cuando haya un problema de
distribución de keys que resolver que la simetría no resuelve.

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
  `"transport": "lora"`.
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

Sigue abierto acá:

- **Rollback de OTA en hardware.** La FSM de decisión y la política de
  rollback están testeadas en host y la tabla de particiones de dos slots
  viene en el repo, pero nadie ha flasheado una placa, marcado una imagen
  como válida, ni visto cómo una imagen mala hace rollback. Hasta que pase,
  las actualizaciones necesitan acceso físico.
- Terminación TLS, paginación por cursor, OTA asíncrona.

## No-objetivos por ahora

Ruteo mesh completo, OTA de firmware por LoRa, control loops por
downlink, MQTT/CoAP en nodos constreñidos, roaming LoRaWAN
multi-región. Cada uno se sugirió al menos una vez; cada uno espera su
turno.

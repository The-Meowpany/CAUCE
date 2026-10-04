# Pruebas CAUCE

## Ejecutar

```powershell
cd firmware
pio test -e native          # 315 tests en PC (Unity)
pio run -e native           # compila el demo de simulación en host
pio run -e esp32dev         # compila el firmware objetivo
```

```bash
cd backend
python -m pytest tests -q   # 384 tests de backend
..\scripts\run-e2e.ps1      # nodo C++ ↔ FastAPI ↔ SQLite (3 fases)
```

## Suites (firmware/test/)

| Archivo | Cubre |
|---|---|
| `test_validation.cpp` (9) | Rango físico, no-finitos, tasa de cambio, sensor congelado, duplicados, incertidumbre temporal |
| `test_codec.cpp` (5) | Vector conocido CRC32, roundtrip, detección de bit-flip, magic/versión/longitud |
| `test_storage.cpp` (6) | Append+query, paginación con skip, recuperación tras reboot, **aislamiento de cola corrupta con sellado**, rotación, retención |
| `test_config.cpp` (9) | Defaults, roundtrip KV, geo NAN, thresholds, save/load, restore de backup, validación, entrada basura |
| `test_security.cpp` (6) | Vectores NIST SHA-256 + comparación en tiempo constante, HMAC-SHA256 RFC 4231 |
| `test_bme280.cpp` (5) | Temperatura vs datasheet, acuerdo presión/humedad, driver I2C en bus simulado, sensor desconectado |
| `test_scheduler_integration.cpp` (5) | Ciclo completo medir→validar→guardar, continuidad de secuencia, MISSING al desconectar+recuperar, fallo de storage sin crash, incertidumbre temporal |
| `test_lora.cpp` (4) | Presupuesto de payload, puerta de intervalo mínimo, radio muerta, ack optimista |

## Suites de backend (backend/tests/test_api.py)

Idempotencia/reanudación/atomicidad de ingesta · auth por token (tiempo
constante, fail-closed) · rate limiting + memoria acotada · filtros de
nodos/mediciones · analítica (summary/compare/before-after/period/
heat-events) · localización de dashboard (en/es/pt) · export CSV ·
campo `transport` (wifi/lora).

## Simulador

```bash
python simulator/generate_scenarios.py --outdir data/sim --days 2
python simulator/generate_scenarios.py --sync-url http://localhost:8000/v1/sync --days 1
```

Escribe CSVs, o alimenta el backend directo vía `/v1/sync`. Cada
timestamp emite air_temperature, relative_humidity, pressure,
illuminance y battery_voltage. Inglés por
defecto, español con `CAUCE_LANG=es`.

## Filosofía

- Todo lo que puede romperse en campo tiene un test de fallo. Si una
  clase de bug no tiene test, ese es el próximo test a escribir.
- La matemática BME280 se chequea con dos implementaciones
  independientes más el vector del vendor — porque una sola
  transcripción de un datasheet es como nacen los bugs sutiles.
- La simulación de escenarios cubre los comportamientos largos que los
  tests no pueden pagar (congelamiento, desconexiones, recuperación en
  docenas de ciclos).

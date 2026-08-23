# Testing CAUCE

## Ejecutar

```powershell
cd firmware
pio test -e native
```

Esperado: `42 test cases: 42 succeeded`. Los tests corren en el PC (no
requieren placa) usando dobles de la HAL: `ManualClock`, `ScriptedI2cBus`,
`MemoryFileSystem`.

Requisitos Windows: MinGW-w64 (GCC ≥ 9) en PATH. Linux: `gcc` estándar.

## Suites (firmware/test/)

| Archivo | Cubre |
|---|---|
| `test_validation.cpp` (9) | Rango físico, no-finito, rate-of-change, congelado, duplicado, incertidumbre temporal |
| `test_codec.cpp` (5) | CRC32 vector conocido, roundtrip completo, detección de bit volteado, magic/version/len |
| `test_storage.cpp` (6) | Append+query, paginación con skip, recuperación tras reinicio, **aislamiento de cola corrupta con sellado**, rotación, retención |
| `test_config.cpp` (9) | Defaults, roundtrip KV, geo NAN, umbrales, save/load, restauración desde backup, validaciones, entrada basura |
| `test_security.cpp` (3) | SHA-256 contra vectores NIST ("", "abc", multiblock) |
| `test_bme280.cpp` (5) | Temperatura vs ejemplo del datasheet, presión entera vs espejo float, humedad ambos modelos, lectura I2C completa con bus simulado, sensor desconectado |
| `test_scheduler_integration.cpp` (5) | Ciclo completo medir→validar→almacenar, continuidad de secuencia entre "reinicios", MISSING ante desconexión y recuperación, falla de storage sin crash (`FailingFileSystem`), tiempo incierto durante apagón |
| `test_export.cpp` (6) | CSV cabecera+filas+ISO-8601, escapes RFC4180, JSON array tipado y cerrado, `[]` vacío, chunking equivalente a single-shot, filtro de rango temporal |
| `test_metrics.cpp` (8) | Stats con vectores conocidos (media/mediana/desv. muestral/percentiles), filtro por variable+calidad+NaN, agregación por ventanas con capacidad acotada, exposición térmica |
| `test_network.cpp` (7) | FSM completa: disabled, conexión, retry con backoff 5s/10s, fallback AP tras N intentos, timeout, degraded↔connected por RSSI, AP sin credenciales |
| `test_sync.cpp` (8) | Watermark persistida, reanudación **sin duplicados** tras fallo de red parcial, pérdida de watermark→reenvío idempotente, auth-backoff, halt ante rechazo, gating por red, formato del payload |

## Integración extremo a extremo (nodo real ↔ servidor real)

```powershell
.\scripts\run-e2e.ps1
```

Levanta el backend FastAPI real en un puerto efímero, ejecuta el binario
`integration` (el `SyncManager` C++ compilado para host con transporte
HTTP por sockets reales) y verifica directamente en SQLite:

1. **Inicial**: 50 mediciones → lotes encadenados → ack=50
2. **Incremental**: +5 nuevas → ack=55 sin reenviar las anteriores
3. **Reenvío idempotente**: watermark eliminada → retransmite todo → el
   servidor deduplica (`node_id+sequence`)
4. **SQLite**: `rows == distinct == 55`, `max_seq=55`, ≥2 registros en
   `sync_batches`

Este E2E ya detectó y corrigió un bug real: `"batch_size"` se serializaba
con ceros a la izquierda (JSON inválido) — el servidor rechazó y el cliente
aplicó halt exactamente como diseñado.

## Filosofía

- Todo lo que pueda fallar en campo tiene un test de fallo: sensor caído,
  storage lleno/corrupto, reinicio, tiempo perdido, red intermitente.
- El BME280 se valida con **dos implementaciones independientes** (entera
  del datasheet y espejo float) más el vector oficial — detecta errores de
  transcripción de fórmulas.
- La simulación de escenarios (`pio run -e native && .pio\build\native\program.exe`)
  complementa los tests: eventos largos (congelamiento sostenido,
  desconexión + recuperación) observables en logs estructurados, con
  estadísticas y vista previa CSV al final.

## Pendiente declarado (ver STATUS.md)

- Hardware-in-the-loop real (placa + sensor físico).
- Tests de LittleFS real y apagón físico.
- CI remoto (los comandos anteriores son exactamente los que debe correr).

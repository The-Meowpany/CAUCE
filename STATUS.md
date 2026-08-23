# Estado real del sistema — leer antes de cualquier suposición

Este documento es la fuente de verdad sobre qué está implementado y qué no.
Reemplaza cualquier afirmación aspiracional en otros archivos.

## Implementado y verificado (BUILD PASSED / 42 TESTS PASSED)

| Componente | Evidencia |
|---|---|
| HAL con interfaces `IClock`, `II2cBus`, `IFileSystem` | Compila en host y ESP32; implementaciones nativas + ESP32 (Wire/LittleFS) |
| Modelo `Measurement` normalizado (nodo, secuencia, timestamp, variable, valor, calidad, flags) | Tests de roundtrip de codec |
| Motor de validación: rango físico, no-finito, salto abrupto (RoC), valor congelado, secuencia duplicada, incertidumbre temporal | 9 tests unitarios |
| Almacenamiento append-only por segmentos `.clog` con CRC32 por registro | 6 tests: roundtrip, paginación, recuperación tras reinicio, aislamiento de cola corrupta, rotación, retención |
| Recuperación sin truncate: segmento con cola corrupta se **sella** y las escrituras rotan a un segmento nuevo | Test `corrupted_tail_is_isolated_on_reopen` |
| ConfigManager versionado (KV plano), validación de rangos, backup `.bak` y restauración automática | 9 tests |
| SHA-256 propio verificado contra vectores oficiales (para tokens admin hasheados) | 3 tests con vectores NIST |
| Driver simulado con inyección de fallas (congelado, NaN, fuera de rango, desconexión) | Tests de integración |
| Driver BME280 referencia (I2C puro sobre HAL, compensación entera del datasheet + doble implementación float de contraste) | Tests contra ejemplo del datasheet Bosch |
| Scheduler event-driven (`tick()`, sin delays bloqueantes) con máquina de estados básica, placeholders MISSING, contadores de salud | 5 tests de integración incluyendo falla de storage sin crash |
| **Exportación CSV (RFC4180, con escape) y JSON array en streaming paginado** (`ChunkedExporter`) | 6 tests: cabecera/filas, escapes RFC4180, array JSON completo y tipado, array vacío `[]`, igualdad chunk-vs-single-shot, filtro temporal |
| **Métricas**: media/mediana/desv. muestral/percentiles, extracción por variable excluyendo calidades no-medibles, agregación por ventanas, exposición térmica trapezoidal | 8 tests con vectores conocidos |
| **ISO-8601 UTC** format/parse (algoritmo civil, bisiestos verificados) | Roundtrips exactos incl. 2000-02-29 |
| **NetworkManager (lógica)**: FSM OFFLINE/WAITING_RETRY/CONNECTING/CONNECTED/DEGRADED/AP_FALLBACK con backoff exponencial, timeout de conexión y fallback a AP tras N intentos | 7 tests con controlador de red simulado |
| **API embebida v1 (lógica)**: los 8 endpoints `/api/v1` con streaming paginado, auth Bearer→SHA-256 constante-tiempo, fail-closed sin token, 422 con lista de errores, máscara de secretos en GET /config | 12 tests del router en host (`test_api.cpp`) |
| **Transporte HTTP ESP32** (Esp32ApiServer sobre WebServer, chunked) cableado en `main.cpp` | Verificado por compilaci�n cruzada SUCCESS � sin placa a�n |
| **Sincronizaci�n (l�gica)**: `SyncManager` con watermark persistente, lotes idempotentes `node_id+sequence`, backoff exponencial, halt ante rechazo, gating por red | 8 tests (`test_sync.cpp`): reanudaci�n sin duplicados, p�rdida de watermark?reenv�o idempotente, auth-backoff, rechazo, payload |
| **Dashboard web local** (SPA embebida ~9.5KB, canvas sin dependencias, cards con calidad, rangos 1h-7d, export, config admin) + **portal cautivo** (DNS wildcard) | 2 tests de integridad del HTML servido; DNS compilaci�n-verificada |
| **Backend central** (FastAPI+SQLite): ingesta /v1/sync idempotente con ack honesto, nodos/measurements filtrables, analytics summary+compare con disclaimers, auth opcional por token, rate limit por IP | 12 tests pytest + smoke real uvicorn (healthz/sync/nodes 200) |
| **E2E nodo?servidor**: SyncManager C++ (HTTP sockets reales) contra FastAPI vivo; 4 fases verificadas + inspecci�n directa de SQLite (0 duplicados tras replay completo) | `scripts/run-e2e.ps1` � detect� y corrigi� bug real de serializaci�n JSON |
| Demo de simulación en PC con escenarios (frozen / out-of-range / disconnect / recovery) + stats + vista previa CSV | Salida verificada manualmente |
| Build cruzado ESP32 (esp32dev/arduino) | SUCCESS, flash ~27% |

## Existe pero NO verificado con hardware físico

- Lectura real de BME280 vía Wire (el driver sigue el datasheet y pasa tests
  con bus I2C simulado; falta bench con chip real).
- LittleFS en ESP32 real (la implementación usa APIs estándar del framework;
  falta probar montaje, desgaste y apagón real).
- Reloj: `Esp32Clock` depende del epoch del sistema; NTP aún no integrado.

## No implementado todavía (fases siguientes del plan)

- Controlador Wi-Fi real ESP32 (`Esp32WifiController` implementando
  `INetworkController`): la FSM está probada; falta la implementación que
  toque el radio (requiere placa).
  falta servir la interfaz y el portal de provisioning.
- Deep sleep / gestión de energía (requiere bench para validar requisitos de
  almacenamiento durante sueño).
- Calibración aplicada en pipeline (el modelo offset/escala está documentado;
  la aplicación en runtime es fase siguiente).
- CI remoto (los scripts locales ya ejecutan lint implícito via -Wall
  -Wextra, tests y build).

## Deudas técnicas conocidas

1. `Logger::eventf` usa buffers fijos de 224 B — suficiente para el piloto,
   revisar si crecen los mensajes.
2. `LogStorageRepository` escanea secuencialmente al abrir: aceptable hasta
   ~10⁵ registros; luego requerirá índice lateral.
3. Los tests compilan en un solo binario Unity (un solo `main`): sin aislamiento
   de estado entre suites más allá de los directorios de datos dedicados.
4. `Measurement.timeUncertain` se propaga bien, pero aún no hay reconstrucción
   temporal post-NTP (re-sellar histórico).
5. Windows requiere MinGW-w64 en PATH para el entorno `native`; en CI usar
   contenedor Linux.

## Cómo reproducir este estado desde cero

```powershell
pip install platformio
winget install BrechtSanders.WinLibs.POSIX.UCRT   # o cualquier MinGW-w64 ≥ GCC 9
cd firmware
pio test -e native      # esperado: 42 succeeded
pio run -e esp32dev     # esperado: SUCCESS
```

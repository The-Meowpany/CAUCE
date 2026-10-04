# Arquitectura CAUCE

Cuatro reglas sostienen este código:

1. **El dominio nunca toca hardware.** Todo lo físico pasa por
   interfaces `cauce_hal`, así que el mismo código de dominio corre en un
   ESP32 y en tu PC. Cuando un test pasa en host, probó la lógica real —
   solo se cambian los drivers.
2. **Offline-first.** La conectividad es una extensión, no un requisito.
   Nada del núcleo espera una red.
3. **Fail-safe, no fail-silent.** El dato malo se guarda con flag, y la
   corrupción se cerca. Lo peor que hace este firmware con un fallo es
   contarlo.
4. **Sin delays bloqueantes.** Todo corre a `tick()`. Si alguna vez
   pasamos a FreeRTOS, será plomería, no rewrite.

## Capas

```
┌─────────────────────────────────────────────┐
│ main.cpp (composition root)                 │  cablea implementaciones
├─────────────────────────────────────────────┤
│ cauce_app                                   │  scheduler, FSM de red,
│                                             │  API router, sync, OTA
├─────────────────────────────────────────────┤
│ cauce_drivers                               │  ISensorDriver → BME280/Simulated
├─────────────────────────────────────────────┤
│ cauce_core                                  │  dominio puro: measurement,
│                                             │  validation, storage, config,
│                                             │  logging, security, metrics
├─────────────────────────────────────────────┤
│ cauce_hal (interfaces)                      │  IClock · II2cBus · IFileSystem ·
│                                             │  INetworkController · ISyncTransport ·
│                                             │  ILoRaRadio
├─────────────────────────────────────────────┤
│ Impls host: Native*/Manual*/Memory*         │  ESP32: Esp32Clock/WireBus/
│ (PC)                                        │  LittleFs/WifiController (+ radio)
└─────────────────────────────────────────────┘
```

Las dependencias apuntan solo hacia abajo. Los tests enchufan dobles
(`ManualClock`, `ScriptedI2cBus`, `MemoryFileSystem`, fakes scripteados
de red/sync) — así corren 315 tests en menos de un minuto sin placa.

## Flujo de medición

```
tick() [scheduler]
  └─ sensor->read(var)                    # driver concreto
       └─ ValidationEngine::evaluate()    # calidad + reason bits
            └─ LogStorageRepository::append()   # frame CRC32, append-only
                 └─ Logger::eventf("MEAS_STORED", ...)
```

Cualquier etapa puede fallar a mitad de ciclo sin frenar el loop: una
lectura fallida suma un contador (dos misses seguidos graban un
placeholder MISSING); un storage muerto suma otro y el próximo tick
simplemente reintenta.

Aguas abajo, dos consumidores leen del mismo repositorio:

```
ChunkedExporter (CSV/JSON)  ·  Metrics (stats/agregación/exposición)
```

Ambos paginan desde `IStorageRepository`. Nadie guarda una segunda copia
de los datos.

## Máquinas de estado

- **Nodo**: `BOOT → SENSOR_DISCOVERY → READY ⇄ MEASURING ⇄ STORING`.
- **Red** (lógica probada contra controlador scripteado):
  `OFFLINE ↔ WAITING_RETRY ↔ CONNECTING → CONNECTED ⇄ DEGRADED`,
  fallback AP tras N fallos, backoff exponencial 5s→300s. El cableado
  ESP32 (`Esp32WifiController`, disciplina NTP, reintento de DNS del
  portal) ya está en `main.cpp` — la FSM por fin maneja una radio real.
- **OTA**: IDLE → CHECKING → DOWNLOADING → REBOOT_PENDING con tres
  estados de fallo; reglas anti-brick de partición alterna (ver OTA.md).

## Despliegue (piloto)

```mermaid
flowchart TB
    U[Phone / Laptop<br/>http://192.168.4.1] --> N1 & N2 & N3
    subgraph Central["Central (opcional)"]
        C[FastAPI + SQLite<br/>dashboard + analytics]
    end
    N1["CAUCE-001<br/>BME280"] -. HMAC-signed batches .-> C
    N2["CAUCE-002"] -. .-> C
    N3["CAUCE-003<br/>...010"] -. .-> C
    N1 --- N2
    N2 --- N3
    classDef node fill:#182430,stroke:#39c2a7,color:#e8eef4
    class N1,N2,N3 node
```

## Almacenamiento

- Segmentos `data/meas_NNNNNN.clog`, append-only, siempre.
- Frame: `[0xCA][0x01][len u16][payload 60B][crc32]` = 68 B (4 cabecera +
  60 payload + 4 CRC).
- Abrir escanea cada segmento: contadores, última secuencia y último
  record se reconstruyen desde lo que realmente hay en flash. Una cola a
  medio escribir sella su segmento y las escrituras rotan a uno fresco —
  esa es toda la historia de los cortes de energía.
- Rotación por tamaño; la retención borra los más viejos pero siempre
  deja al menos uno.

## Configuración

KV plano versionado (`schema_version=1`) con validación estricta y la
lista exacta de qué está mal, backup automático del archivo previo en
cada save, y cadena de carga `main → .bak → defaults`. Los tokens admin
viven en flash solo como SHA-256.

## Decisiones clave

| Decisión | Alternativa rechazada | Por qué |
|---|---|---|
| Formato binario propio con CRC | SQLite embebido | Controlamos cada byte, el comportamiento ante cortes es demostrable, el footprint queda mínimo |
| Config KV plana | JSON embebido | Sin dependencias, parsea sin heap, los diffs se leen bien; JSON queda en las APIs |
| Compensación entera Bosch + espejo float | Solo float | La matemática entera es la referencia del vendor; el espejo ya pescó errores de transcripción |
| `tick()` sin RTOS | Tareas FreeRTOS ya | El dominio no las necesita; agregarlas después es plomería |
| Unity binario único en host | Tests on-target | Feedback en segundos, el CI queda trivial |

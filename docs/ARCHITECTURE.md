# Arquitectura CAUCE

## Principios

1. **El dominio nunca toca hardware.** Toda dependencia pasa por interfaces
   en `cauce_hal`. El mismo binario de dominio corre en ESP32 y en el host.
2. **Offline-first.** Nada del núcleo requiere red. La sincronización futura
   es una extensión, no un requisito.
3. **Fail-safe por diseño.** Los datos inválidos se almacenan marcados, no se
   descartan silenciosamente. La corrupción se aísla, no propaga.
4. **Sin delays bloqueantes.** Todo el flujo es `tick()`-driven; listo para
   FreeRTOS más adelante sin reescribir el dominio.

## Capas

```
┌─────────────────────────────────────────────┐
│ main.cpp (composition root)                 │  cablea implementaciones
├─────────────────────────────────────────────┤
│ cauce_app                                   │  scheduler, máquina de estados
├─────────────────────────────────────────────┤
│ cauce_drivers                               │  ISensorDriver → BME280 / Simulado
├─────────────────────────────────────────────┤
│ cauce_core                                  │  dominio puro: medición, validación,
│                                             │  storage, config, logging, seguridad
├─────────────────────────────────────────────┤
│ cauce_hal (interfaces)                      │  IClock · II2cBus · IFileSystem
├─────────────────────────────────────────────┤
│ Implementaciones: Native*/Manual*/Memory*   │  Esp32Clock/WireBus/LittleFs
│ (host)                                      │  (ESP32/Arduino)
└─────────────────────────────────────────────┘
```

Dependencias permitidas: solo hacia abajo. `cauce_core` no incluye nada de
HAL salvo las interfaces; los tests usan dobles (`ManualClock`,
`ScriptedI2cBus`, `MemoryFileSystem`) sin tocar hardware.

## Flujo de una medición

```
tick() [scheduler]
  └─ sensor->read(var)                    # driver específico
       └─ ValidationEngine::evaluate()    # calidad + bits de razón
            └─ LogStorageRepository::append()   # frame CRC32, append-only
                 └─ Logger::eventf("MEAS_STORED", ...)
```

Cada etapa puede fallar sin detener el ciclo: read fallido → contador +
placeholder MISSING tras 2 fallos consecutivos; storage caído → contador y
el siguiente tick reintenta.

Consumo posterior de los datos:

```
ChunkedExporter (CSV/JSON)  ·  Metrics (stats/agregación/exposición)
```

ambos leen por paginación desde `IStorageRepository` sin duplicar estado.

## Máquina de estados

Implementada hoy: `BOOT → SENSOR_DISCOVERY → READY ⇄ MEASURING ⇄ STORING`.
Red (lógica verificada con controlador simulado): `OFFLINE ↔
WAITING_RETRY ↔ CONNECTING → CONNECTED ⇄ DEGRADED`, con fallback a
`AP_FALLBACK` tras N intentos fallidos y backoff exponencial 5s→300s.
Pendientes de hardware: integración del radio real, `SERVING`, `SYNCING`.

## Almacenamiento

- Segmentos `data/meas_NNNNNN.clog`, append-only.
- Frame: `[0xCA][0x01][len u16][payload 60 B][crc32 u32]` = 72 B.
- Apertura escanea todos los segmentos: reconstruye contadores, última
  secuencia y último registro; la cola parcial corrupta sella el segmento.
- Rotación por tamaño; retención borra el segmento más viejo conservando
  siempre uno.

## Configuración

KV plano versionado (`schema_version=1`), validación estricta con lista de
errores, backup automático del archivo previo al guardar y cadena de carga
`principal → .bak → defaults`. Los tokens administrativos se guardan solo
como SHA-256.

## Decisiones y su rationale

| Decisión | Alternativa descartada | Motivo |
|---|---|---|
| Formato binario propio con CRC | SQLite embebida | Control total del formato, tolerancia a apagón demostrable, footprint mínimo |
| KV plano para config | JSON embebido | Cero dependencias, parseo sin heap, diffs legibles; JSON queda para la API |
| Compensación entera Bosch + espejo float | Solo float | La entera es la referencia del fabricante; la espejo valida independencia de implementación |
| `tick()` sin RTOS | Tareas FreeRTOS desde ya | El dominio no lo necesita; migrar luego es mecánico |
| Tests Unity en un binario nativo | Tests on-target | Ciclo de feedback en segundos, CI trivial |

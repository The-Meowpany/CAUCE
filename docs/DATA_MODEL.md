# Modelo de datos CAUCE

## Measurement (unidad atómica)

```cpp
struct Measurement {
  char     nodeId[16];       // "CAUCE-001"
  char     sensorId[24];     // "BME280-1"
  uint32_t sequence;         // monotónico, persiste entre reinicios
  uint64_t timestampUtcMs;   // epoch ms; 0 si el reloj no es confiable
  float    value;
  Variable variable;
  Quality  quality;
  uint8_t  reasonBits;       // por qué la calidad es lo que es
  bool     timeUncertain;    // separado de quality: el valor puede ser
                             // bueno aunque el tiempo no
};
```

Reglas:

- **Nunca se descarta silenciosamente.** Una lectura fuera de rango se
  almacena con `quality=INVALID` y su `reasonBits`.
- `timeUncertain` permite seguir operando sin NTP/RTC: los datos quedan
  registrables y auditables, marcados para reconstrucción temporal futura.
- La secuencia arranca del máximo almacenado +1 al reiniciar → no hay
  duplicados tras apagones.

## Calidad — semántica

| Estado | Significado |
|---|---|
| VALID | Pasó todas las verificaciones |
| CALIBRATED / UNCALIBRATED | Overlay futuro de calibración (no aplicado aún en runtime) |
| ESTIMATED | Reservado para agregación/imputación |
| SUSPECT | Plausible pero anómalo (salto abrupto o sensor congelado) |
| INVALID | Imposible (rango físico, NaN, duplicado) |
| MISSING | Ventana esperada sin dato (sensor caído) |

## Umbrales de validación (defaults, configurables por nodo)

| Variable | Rango | Máx. variación/min |
|---|---|---|
| air_temperature | -40..85 °C | ±5 |
| relative_humidity | 0..100 %RH | ±20 |
| pressure | 300..1100 hPa | ±2 |
| illuminance | 0..200000 lx | ±120000 |
| battery_voltage | 2.5..4.5 V | ±0.2 |

Detección de congelamiento: ≥6 lecturas idénticas dentro de épsilon 0.01.

## Repositorio

- Interfaz `IStorageRepository`: append / query paginada / latest /
  contadores / retención / integrityCheck.
- Implementación actual: `LogStorageRepository` (ver PROTOCOL.md).
- Consulta: `query(from, to, skip, out[], capacity)` — nunca carga el
  histórico completo en RAM; escanea secuencialmente con paginación.

## Configuración del nodo (`NodeConfig`)

Identidad (`node_id`, `site_id`), geolocalización opcional (lat/lon/
elevación, NAN = no declarada), contexto de instalación (land_cover,
shade_condition), operación (sampling_interval_s, sync_interval_s,
timezone), red (wifi, ntp), almacenamiento (storage_max_bytes,
segment_max_bytes) y umbrales de validación completos.

Garantías: validación con errores enumerados; backup `.bak` previo a cada
guardado; cadena de recuperación principal→backup→defaults.

## Formatos de exportación

**CSV (RFC4180)** — compatible con Excel/LibreOffice/Python/R:

```
node_id,sensor_id,sequence,timestamp_utc_ms,timestamp_iso,variable,value,unit,quality,reason_bits,time_uncertain
CAUCE-001,BME280-1,1842,1787356860000,2026-08-22T00:01:00Z,air_temperature,21.50,C,VALID,0,0
```

Campos de texto se entrecomillan y escapan según RFC4180. Codificación
ASCII, separador `,`, salto `\n`.

**JSON array** — un objeto por medición, mismo esquema del payload del
protocolo v1 (ver protocol/v1/PROTOCOL.md).

Ambos se generan en **streaming paginado** (`ChunkedExporter`): nunca se
carga el histórico completo en RAM; el consumidor escribe chunks hasta
`done()`.

Regla analítica: las métricas derivadas excluyen calidades
`INVALID`, `MISSING` y `ESTIMATED`; `SUSPECT` participa pero debe
reportarse junto al porcentaje de registros sospechosos.

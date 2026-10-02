# Modelo de datos CAUCE

## Measurement (unidad atómica)

```cpp
struct Measurement {
  char     nodeId[16];       // "CAUCE-001"
  char     sensorId[24];     // "BME280-1"
  uint32_t sequence;         // monótona, sobrevive reboots
  uint64_t timestampUtcMs;   // epoch ms; 0 cuando el reloj no es confiable
  float    value;
  Variable variable;
  Quality  quality;
  uint8_t  reasonBits;       // por qué la calidad es la que es
  bool     timeUncertain;    // ortogonal a quality
};
```

Tres reglas sin excepciones:

- **Nada se descarta en silencio.** Una lectura fuera de rango va a flash
  como `quality=INVALID` con sus reason bits. Siempre podés ver lo que el
  sensor realmente dijo.
- `timeUncertain` deja al nodo trabajar sin NTP ni RTC. El dato queda
  registrado y auditable, marcado para reconstrucción temporal posterior
  en vez de tirarse por no tener timestamp.
- Tras un reboot la secuencia sigue en max-guardado+1, así que los cortes
  de energía nunca crean duplicados.

## Semántica de calidad

| Estado | Significado |
|---|---|
| VALID | Pasó todos los checks |
| CALIBRATED / UNCALIBRATED | Lugar reservado para un futuro overlay de calibración (aún no aplicado en runtime) |
| ESTIMATED | Reservado para agregación/imputación |
| SUSPECT | Plausible pero raro (salto abrupto o sensor congelado) |
| INVALID | Imposible (rango físico, NaN, duplicado) |
| MISSING | Una ventana donde se esperaba dato y no hay |

## Thresholds de validación (defaults, configurables por nodo)

| Variable | Rango | Cambio máximo/minuto |
|---|---|---|
| air_temperature | -40..85 °C | ±5 |
| relative_humidity | 0..100 %RH | ±20 |
| pressure | 300..1100 hPa | ±2 |
| illuminance | 0..200000 lx | ±120000 |
| battery_voltage | 2.5..4.5 V | ±0.2 |

Detección de stuck: 6 o más lecturas idénticas dentro de epsilon 0.01 —
eso es un sensor muerto haciéndose el vivo.

## Calibración y mantenimiento (central)

`measurements.value` es la lectura cruda y nunca se reescribe. El valor
calibrado se deriva en la lectura:

```
calibrated_value = value * scale + offset
```

| Tabla | Clave | Contiene |
|---|---|---|
| `calibration` | `(site_id, variable)` | `scale`, `offset`, `method`, `calibration_reference`, `sensor_id`, `calibration_date`, `status`, `notes`, `updated_utc_ms` |
| `maintenance_events` | `event_id` | `site_id`, `kind`, `at_utc_ms`, `notes` |

`status` es `applied`, `provisional`, `retired` o `rejected`; todo lo que
no sea `retired` se aplica en analítica, CSV y dashboard, y los registros
retirados se quedan para auditoría. Como el mapa es lineal, la
transformación es exacta sobre los buckets de `agg_hourly`: las
estadísticas de ubicación se trasladan y la dispersión escala por
`|scale|`.

Nota de diseño: los códigos de calidad `CALIBRATED` / `UNCALIBRATED`
siguen reservados. La calibración es una capa aplicada en la lectura, no
una propiedad del registro, así que la calidad almacenada sigue
describiendo lo que vio el motor de validación.

## Repositorio

- Interfaz `IStorageRepository`: append / query paginada / query-por-
  secuencia / latest / contadores / retención / integrityCheck.
- Implementación: `LogStorageRepository` (protocol/v1/PROTOCOL.md).
- Las queries caminan secuencialmente — la historia completa nunca está
  en RAM de una vez.

## Configuración del nodo (`NodeConfig`)

Identidad (`node_id`, `site_id`), geo opcional (lat/lon/elevación, NAN =
no declarado), contexto de instalación (land cover, shade), operación
(intervalo de muestreo, intervalo de sync, timezone), red (Wi-Fi, NTP),
presupuestos de storage y thresholds completos de validación.

Lo que obtenés: validación que te dice cada error, backup `.bak` antes
de cada save, y cadena de recuperación main→backup→defaults.

## Formatos de exportación

**CSV (RFC4180)** — abre en Excel/LibreOffice/Python/R:

```
node_id,sensor_id,sequence,timestamp_utc_ms,timestamp_iso,variable,value,unit,quality,reason_bits,time_uncertain,calibrated_value,calibration_scale,calibration_offset
CAUCE-001,BME280-1,1842,1787356860000,2026-08-22T00:01:00Z,air_temperature,21.50,C,VALID,0,0,21.50,1.0,0.0
CAUCE-002,BME280-1,91,1787356860000,2026-08-22T00:01:00Z,air_temperature,22.60,C,VALID,0,0,21.50,1.0,-1.1
```

Campos de texto quoted/escaped per RFC4180. Encoding ASCII. Las tres
columnas de calibración van al final para que los lectores antigos
sigan funcionando; van vacías en una variable sin calibración.

**Arreglo JSON** — un objeto por medición, mismo esquema que el payload
del protocolo v1.

Ambos salen de `ChunkedExporter` — la historia se streamea, nunca se
carga entera.

Una regla analítica: las métricas derivadas dejan fuera `INVALID`,
`MISSING` y `ESTIMATED`. `SUSPECT` cuenta, pero su proporción tiene que
aparecer junto al número.

Las ventanas de tiempo son INCLUSIVAS en ambos extremos, firmware y
backend por igual — se decidió una vez, se cumple en todas partes.

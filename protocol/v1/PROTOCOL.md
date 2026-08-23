# Protocolo de datos CAUCE — v1

`protocol_version = 1` · `schema_version (config) = 1`

Dos formatos conviven por diseño:

1. **Registro en reposo** (binario, en el nodo) — optimizado para flash.
2. **Payload de intercambio** (JSON, para API/sync futuros) — optimizado
   para legibilidad y evolución.

## 1. Registro binario en reposo (`meas_NNNNNN.clog`)

Archivo append-only de frames de tamaño fijo **72 bytes**:

```
offset  tamaño  campo
0       1       magic      0xCA
1       1       version    0x01
2       2       payload_len  (=60, little-endian)
4       60      payload    (ver abajo)
64      4       crc32      CRC-32 (IEEE, poly 0xEDB88320) de bytes [0..63]
```

Reglas:

- Un frame con CRC inválido detiene la lectura secuencial del segmento
  (cola parcial por apagón). El segmento se **sella**: no recibe más
  appends; los nuevos registros van a un segmento nuevo.
- `payload_len ≠ 60` o `magic/version` incorrectos → segmento ignorado y
  contado como corrupto.

### Payload (60 bytes, little-endian)

```
offset  tamaño  campo
0       4       sequence        u32   monotónico por nodo
4       8       timestamp_utc_ms u64  epoch ms; 0 = tiempo desconocido
12      4       value           f32   IEEE-754 little-endian
16      1       variable        u8    enum Variable
17      1       quality         u8    enum Quality
18      1       reason_bits     u8    bitmask de validación
19      1       time_uncertain  u8    0/1
20      16      node_id         char[] NUL-terminated
36      24      sensor_id       char[] NUL-terminated
```

### Enumeraciones

| variable | valor | unidad |
|---|---|---|
| air_temperature | 0 | °C |
| relative_humidity | 1 | %RH |
| pressure | 2 | hPa |
| illuminance | 3 | lx |
| battery_voltage | 4 | V |

| quality | valor |
|---|---|
| VALID | 0 |
| CALIBRATED | 1 |
| UNCALIBRATED | 2 |
| ESTIMATED | 3 |
| SUSPECT | 4 |
| INVALID | 5 |
| MISSING | 6 |

### reason_bits

| bit | motivo |
|---|---|
| 0 | NON_FINITE (NaN/Inf) |
| 1 | OUT_OF_RANGE |
| 2 | RATE_OF_CHANGE |
| 3 | STUCK_VALUE |
| 4 | TIME_UNCERTAIN |
| 5 | DUPLICATE_SEQUENCE |
| 6 | SENSOR_UNHEALTHY |
| 7 | UNKNOWN_VARIABLE |

## 2. Payload JSON de intercambio (reservado para API/sync v1)

```json
{
  "protocol_version": 1,
  "node_id": "CAUCE-001",
  "sequence": 1842,
  "timestamp": "2026-10-12T14:30:00Z",
  "time_uncertain": false,
  "measurements": [
    { "variable": "air_temperature", "value": 28.4,
      "unit": "C", "quality": "VALID", "reason_bits": 0 }
  ],
  "battery": { "variable": "battery_voltage", "value": 3.91,
               "unit": "V", "quality": "UNCALIBRATED" },
  "firmware_version": "0.1.0",
  "sensor_metadata": [
    { "id": "BME280-1", "model": "BME280", "variables": ["air_temperature",
      "relative_humidity", "pressure"] }
  ]
}
```

## Reglas de evolución

- `version` del frame incrementa solo si cambia el layout del payload;
  lectores rechazan versiones desconocidas sin corromperse.
- Campos nuevos en JSON son aditivos; los consumidores deben ignorar claves
  desconocidas.
- `node_id + sequence` identifica inequívolamente una medición (clave de
  idempotencia para sync futuro).

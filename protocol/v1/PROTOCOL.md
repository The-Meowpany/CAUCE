# CAUCE Data Protocol — v1

`protocol_version = 1` · `schema_version` (config) = 1

Two formats coexist by design:

1. **At-rest record** (binary, on the node) — flash-optimized.
2. **Exchange payload** (JSON, for API/sync) — readability/evolution.

## 1. At-rest binary record (`meas_NNNNNN.clog`)

Append-only file of fixed **72-byte** frames:

```
offset  size  field
0       1     magic      0xCA
1       1     version    0x01
2       2     payload_len  (=60, little-endian)
4       60    payload
64      4     crc32      CRC-32 (IEEE, poly 0xEDB88320) over bytes [0..63]
```

Rules:

- A frame with bad CRC stops sequential reading of that segment (partial
  tail from power loss). The segment is **sealed**; new writes roll to a
  fresh segment.
- `payload_len != 60` or wrong magic/version → segment counted as corrupt.

### Payload (60 bytes, little-endian)

```
offset  size  field
0       4     sequence        u32   monotonic per node
4       8     timestamp_utc_ms u64  epoch ms; 0 = unknown time
12      4     value           f32   IEEE-754 LE
16      1     variable        u8    enum Variable
17      1     quality         u8    enum Quality
18      1     reason_bits     u8    validation bitmask
19      1     time_uncertain  u8    0/1
20      16    node_id         char[] NUL-terminated
36      24    sensor_id       char[] NUL-terminated
```

Enums, reason bits and the JSON payload shape are specified in full in
docs/DATA_MODEL.md and docs/API.md respectively.

## 2. JSON exchange payload (API/sync v1)

See docs/SYNC.md for the batch envelope and docs/DATA_MODEL.md for the
measurement object schema (`timestamp_utc_ms` included for lossless
roundtrips).

## Evolution rules

- Frame `version` bumps only when the payload layout changes; readers reject
  unknown versions without corrupting state.
- JSON fields evolve additively; consumers must ignore unknown keys.
- `node_id + sequence` uniquely identifies a measurement (sync idempotency).

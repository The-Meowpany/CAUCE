# CAUCE Data Protocol — v1

`protocol_version = 1` · `schema_version` (config) = 1

Two formats coexist by design:

1. **At-rest record** (binary, on the node) — flash-optimized.
2. **Exchange payload** (JSON, for API/sync) — readability/evolution.

## 1. At-rest binary record (`meas_NNNNNN.clog`)

Append-only file of fixed **68-byte** frames (4 B header + 60 B payload + 4 B CRC):

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
docs/en/DATA_MODEL.md and docs/en/API.md respectively.

## 2. JSON exchange payload (API/sync v1)

See docs/en/SYNC.md for the batch envelope and docs/en/DATA_MODEL.md for the
measurement object schema (`timestamp_utc_ms` included for lossless
roundtrips).

## Evolution rules

- Frame `version` bumps only when the payload layout changes; readers reject
  unknown versions without corrupting state.
- JSON fields evolve additively; consumers must ignore unknown keys.
- `node_id + sequence` uniquely identifies a measurement (sync idempotency).

## Annex: Test vector (hex dump)

Payload example (`node_id="CAUCE-001"`, `sequence=1`, `timestamp=0`, `value=0.0`, `variable=0`, `quality=5`, `reason=0`, `time_uncertain=1`):

```
0000: ca 01 3c 00 01 00 00 00 00 00 00 00 00 00 00 00
0010: 00 00 00 00 00 05 00 01 43 41 55 43 45 2d 30 30
0020: 31 00 00 00 00 00 00 00 42 4d 45 32 38 30 2d 31
0030: 00 00 00 00 00 00 00 00 00 00 00 00 a3 8f 12 4e
```

Frame CRC (`a3 8f 12 4e` big-endian `4e128fa3`) computed over bytes `0x00..0x3f` with `crc32()` (`RecordCodec.cpp:12`).

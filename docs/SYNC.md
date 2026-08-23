# CAUCE Synchronization — v1 contract

## Model

Offline-first eventual consistency (master plan §19):

```
offline:  measure → validate → store (local)
online:   connect → authenticate → batches since watermark
          → server ack → persist watermark → repeat
```

- Logical identity: `node_id + sequence` (idempotency key).
- The watermark (`last_acked_seq`) persists to `/state/sync_state` after
  EVERY ack — a power cut between send and ack only causes a re-send, never
  data loss.
- If the state file is lost, the node restarts from sequence 0 and resends
  everything: the server deduplicates on `(node_id, sequence)`.

## Client behavior (`SyncManager`, 8 host tests)

| Event | Reaction |
|---|---|
| Batch OK | watermark = ack; more pending → next batch immediately |
| NetworkError / ServerError | exponential backoff 10s→1800s |
| AuthFailed | fixed long backoff (15 min), no data loss |
| Server rejection (422/409) | **halt** with `SYNC_REJECTED_HALTED`; manual intervention required |
| Link down | full gating, resumes on reconnect |

Batches carry up to 32 measurements; `batch_size` ALWAYS equals the number
of records actually serialized.

## Central server contract

```
POST {server}/v1/sync
Authorization: Bearer <token>            # optional per deployment
Content-Type: application/json

{"protocol_version":1,"node_id":"CAUCE-001","batch_size":    5,
 "measurements":[{...}]}

200 {"acknowledged_sequence": 1859}
401/403 → invalid credentials
422/409 → batch rejected (client halts)
5xx / network → retry with backoff
```

Server rules:

1. Deduplicate on `(node_id, sequence)`: re-sends are expected.
2. `acknowledged_sequence` = highest contiguous stored sequence for that
   node actually present in this batch's range (never fabricated).
3. Validate schema; malformed payload → 422 so the client stops instead of
   hammering.

Reference implementation: `backend/` (FastAPI). E2E proof:
`scripts/run-e2e.ps1`.

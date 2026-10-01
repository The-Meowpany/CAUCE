# CAUCE Synchronization — v1 contract

## Model

Offline-first eventual consistency:

```
offline:  measure → validate → store (local)
online:   connect → authenticate → batches since watermark
          → server ack → persist watermark → repeat
```

- Logical identity is `node_id + sequence`. That's the idempotency key;
  there isn't another one.
- The watermark (`last_acked_seq`) hits `/state/sync_state` after EVERY
  ack. A power cut between send and ack costs a re-send, never data.
- Lose the state file and the node restarts from sequence 0, resending
  everything. The server shrugs and deduplicates on `(node_id,
  sequence)` — that's what the key is for.

## Client behavior (`SyncManager`, 8 host tests)

| Event | Reaction |
|---|---|
| Batch OK | watermark = ack; more pending → next batch goes out immediately |
| NetworkError / ServerError | exponential backoff 10s→1800s |
| AuthFailed | long fixed backoff (15 min), data stays put |
| Server rejection (422/409) | **halt** with `SYNC_REJECTED_HALTED`; a human has to look at it |
| Link down | everything gates, resumes on reconnect |

Batches carry up to 32 measurements, and `batch_size` ALWAYS equals the
number of records actually serialized — padded counts would corrupt the
server's bookkeeping.

## Central server contract

```
POST {server}/v1/sync
Authorization: Bearer <token>            # legacy/global path (optional)
X-CAUCE-Node: CAUCE-001                  # required when provisioned
X-CAUCE-Signature: <hex hmac-sha256>     # required when provisioned
Content-Type: application/json

Signature = HMAC_SHA256(device_key, raw_request_body). The server keeps
only what it needs to verify; stealing one device key doesn't let you
forge another node's batches.

{"protocol_version":1,"node_id":"CAUCE-001","batch_size":    5,
 "measurements":[{...}]}

200 {"acknowledged_sequence": 1859}
401/403 → bad credentials
422/409 → batch rejected (client halts)
5xx / network → retry with backoff
```

Server rules:

1. Deduplicate on `(node_id, sequence)`. Re-sends are normal traffic,
   not an error condition.
2. `acknowledged_sequence` is the highest contiguous stored sequence for
   that node actually present in this batch's range. Never made up —
   the E2E suite checks this against SQLite directly.
3. Malformed payload → 422, so the client stops instead of hammering a
   broken batch forever.

Reference implementation: `backend/` (FastAPI). E2E proof:
`scripts/run-e2e.ps1` — which once caught a real JSON serialization bug,
so we keep it honest.

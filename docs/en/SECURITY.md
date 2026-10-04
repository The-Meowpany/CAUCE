# CAUCE Security — current posture

What we actually do today, and what we openly don't.

## Implemented

- **Admin tokens live hashed.** The config keeps the SHA-256 hex
  (`admin_token_sha256`) and never the plaintext. Our SHA-256 is
  checked against NIST vectors, and comparisons run in constant time
  on both node and server — timing attacks get nothing.
- **Config validation with teeth.** Numeric ranges, fixed-size
  buffers, garbage rejection. Feed it several malformed lines and
  parsing fails instead of guessing.
- **No heap on hot paths.** Measurement, storage and config all use
  fixed buffers, which shrinks the overflow and fragmentation surface
  to almost nothing.
- **Size limits as design.** Fixed 60-byte record payload, capped
  segments, byte-budgeted retention. There's simply no room for a
  giant-input surprise.
- **Backend hardening.** Constant-time comparisons everywhere
  (`hmac.compare_digest`), per-IP rate limiting with bounded memory on
  every endpoint, separate optional tokens per scope (sync vs API).
- **Per-device identity and batch signing.** `/v1/provision` registers
  one HMAC key per node; provisioned nodes sign every sync batch over
  the raw body (`X-CAUCE-Signature`), computed with the same
  HMAC-SHA-256 verified against RFC 4231 vectors. A stolen key buys
  you one device, not the fleet.
- **OTA manifest gate.** With a manifest key configured, releases
  without a valid HMAC over `version|url|totalSize` die before any
  download — no manifest-substitution MITM.
- **Read/write split.** Public reads, authenticated writes, on the
  node API and the central server alike.

## Retiring a device

A device that is lost, stolen or suspected compromised can have its identity retired
without anyone reaching it:

```
POST /v1/nodes/{node_id}/revoke      {"reason": "..."}      -> admin scope
GET  /v1/nodes/{node_id}/revocation
```

Three properties, each a decision rather than an implementation detail:

- **Retirement is not deletion.** The measurements a node already contributed stay.
  A device compromised for a week made real observations during part of that week,
  and deleting the row cascades them away along with the evidence.
- **There is no un-revoke.** Reinstating means provisioning again with a *new* key,
  which is what clears the retirement. An endpoint that flipped a flag back would
  restore an identity without changing the thing that was compromised. Three
  plausible URL spellings are asserted to return 404.
- **The check runs before authentication.** Checking after a valid signature would
  spend the central's rate-limit budget verifying frames from a device already
  retired.

Retiring an unknown node id still records the retirement, because a retirement that
silently does nothing because the id was typed wrong is the failure mode that
matters: the operator walks away believing the device is off the network.

Rotation is a provisioning call, and `POST /v1/provision` reports
`"reinstated": true` when it reinstates a retired node, so a rotation does not have
to be discovered in the logs afterwards.

## Provisioning

`backend/tools/provision.py` generates one Ed25519 seed per device at end of line,
installs the **public** key at the central, and writes a `0600` manifest:

```
python tools/provision.py --central https://cauce.example --count 24 \
    --site rio-01 --out provisioning.json
python tools/provision.py --central https://cauce.example --manifest provisioning.json
python tools/provision.py --central https://cauce.example --retire CAUCE-014
```

The central is never handed a seed. There is deliberately no recover-my-seed path:
adding one would turn the manifest from a provisioning record into a key escrow,
which is a different product with a different threat model. A lost seed means a lost
identity — re-provision and retire the old key.

On Windows `chmod` cannot restrict anything, so the tool warns that the restriction
was not applied rather than implying it was.

## Signature algorithms

Frames are authenticated per node, and the algorithm is fixed by provisioning rather
than chosen by the request — a frame cannot relabel itself into whichever check the
central would rather run. The trailer length is what the central dispatches on.

| | trailer | what a reader of the central's database learns |
|---|---|---|
| HMAC-SHA-256 | 32 B | the shared secret, so every HMAC node can be impersonated |
| Ed25519 | 64 B | a public key, so it cannot sign |

Firmware holds an Ed25519 **seed**, never the expanded scalar, so a caller cannot
provision or persist the wrong half. Ed25519 requires exactly a 32-byte seed: 1, 16,
31, 33 and 64 bytes are all refused, because a padded or truncated seed signs
perfectly and verifies nowhere, and the only symptom is a central silently dropping
every frame.

Ed25519 in this firmware is **not constant-time** — the carries and the scalar ladder
act on values derived from the key. A hostile relay or central sees only the
signature and is unaffected; an attacker able to measure signing time locally at
high resolution may not be. Where the device itself must be assumed hostile, use a
constant-time library instead.

## Pending (later phases / hardware)

- Wi-Fi radio integration: credentials go in through the local UI, and
  the provisioning portal gets hardened along with it.
- Transport encryption (TLS): the decision layer already accepts
  https URLs through `ISyncTransport` — what's missing is the
  certificate and provisioning infrastructure, not the code path.
- OTA flashing: the decision layer is done; release-payload signing
  should land before any fleet rollout.
- Node-side throttling: the transport currently assumes LAN isolation.
  Revisit the day this leaves the pilot network.

## Non-negotiables, already applied

1. No plaintext secrets on flash. Ever.
2. No unvalidated external input reaches logic.
3. Fail closed: a bad component gets an explicit rejection plus a log
   line, not a silent pass.
4. Privacy: environmental telemetry and node ids only. No personal
   data, no user MACs, nothing identifying in URLs beyond ids and
   timestamps.

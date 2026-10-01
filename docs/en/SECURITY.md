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

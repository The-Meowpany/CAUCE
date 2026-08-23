# CAUCE Security — current posture

## Implemented

- **Admin tokens stored hashed**: config keeps only SHA-256 hex
  (`admin_token_sha256`); never plaintext. Own SHA-256 verified against NIST
  vectors; comparisons constant-time on both node and server.
- **Strict configuration validation**: numeric ranges, fixed-size buffers,
  garbage rejection (multiple malformed lines fail parsing).
- **No heap on hot paths**: fixed buffers across measurement/storage/config
  minimize overflow and fragmentation surface.
- **Size limits by design**: fixed 60-byte record payload, capped segments,
  byte-budgeted retention.
- **Backend hardening**: constant-time comparisons everywhere
  (`hmac.compare_digest`), per-IP rate limiting with bounded memory on every
  endpoint, optional tokens per scope (sync vs API).
- **Per-device identity & batch signing**: `/v1/provision` registers an HMAC
  key per node; provisioned nodes must sign every sync batch over the raw
  body (`X-CAUCE-Signature`). Firmware computes it with the same HMAC-SHA-256
  primitive verified against RFC 4231 vectors. Key compromise is scoped to a
  single device.
- **OTA manifest authentication gate**: when a manifest key is configured,
  releases without a valid HMAC over `version|url|totalSize` are rejected
  before any download (closes manifiesto-substitution MITM vector).
- **Read/write separation**: public reads vs authenticated writes on both
  node API and central server.

## Pending (later phases / hardware)

- Wi-Fi radio integration: credentials provisioned through the local UI;
  secure provisioning portal hardening comes with it.
- Transport encryption (TLS): decision layer ready (`ISyncTransport` accepts
  https URLs); requires certificate/provisioning infrastructure.
- OTA flashing: decision layer shipped; signing of release payloads should
  be added before fleet rollouts.
- Node-side request throttling: transport currently trusts LAN isolation;
  revisit once exposed beyond the pilot network.

## Non-negotiables already applied

1. No secrets in plaintext flash.
2. No unvalidated external input.
3. Fail closed: invalid component → explicit rejection + log.
4. Privacy: environmental telemetry and node identifiers only; no personal
   data, no user MACs, nothing in URLs beyond ids/timestamps.

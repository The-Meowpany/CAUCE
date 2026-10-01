# Security Policy — CAUCE

Canonical security model: `docs/en/SECURITY.md`.

Summary:

- Per-device HMAC (`X-CAUCE-Signature` over raw `/v1/sync` body) via
  `POST /v1/provision`; legacy global `CAUCE_SYNC_TOKEN` only for
  unprovisioned nodes; fail-closed with `503 sync_not_provisioned` when
  `CAUCE_SYNC_REQUIRE_AUTH=1`.
- Embedded API Bearer tokens compared with SHA-256 constant-time;
  fail-closed; secrets masked; 422 error lists.
- OTA manifest signature verified before download; streaming SHA-256;
  heap/battery gates.
- Central rate limit (SQLite-backed, 120/min default) on `/v1/*` API,
  evaluation and dashboard/CSV export routes.
- TLS is deferred to deployment (see `docs/en/DEPLOYMENT.md` and
  `docs/en/ROADMAP.md` M5); do not expose the central beyond a trusted LAN
  without an HTTPS reverse proxy.

Report vulnerabilities privately to security@cauce-project.org.
Do not open public issues for security reports.

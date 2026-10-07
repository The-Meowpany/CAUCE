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
- **The central fails closed on writes.** With `CAUCE_API_TOKEN` unset,
  every mutation answers `503 admin_api_not_configured` rather than
  being allowed: there is no credential to check, so allowing it would
  leave the port open to anyone who can reach it. Reads stay open,
  because the dashboard is meant to be reachable on a trusted LAN.
  Setting the token closes reads as well. This is the one asymmetry,
  and it is deliberate — see `backend/cauce_server/security.py`.
- **Node-supplied names are constrained, and the dashboard's script
  blocks are escaped for their context.** `variable` and `sensor_id`
  are stored verbatim, so they are restricted to `[A-Za-z0-9_.-]`, and
  every JSON value embedded in a `<script>` element is escaped:
  `json.dumps` alone is not safe there, because an HTML parser looks
  for a literal `</script>` regardless of the JSON quoting. Both halves
  exist because either one alone leaves the page safe only while
  everything else stays correct.

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

## Node certificates

A provisioned public key proves the node holds the private half. It does **not** establish
that the central ever vouched for the binding — `nodes.device_key` is a row an operator
wrote with the admin token, so adding a node and rotating a compromised key were the same
operation with the same blast radius, and a key compromised once stayed compromised.

`CAUCE_CA_KEY` holds an Ed25519 **seed** for a certificate authority, configured separately
from `CAUCE_API_TOKEN`. It signs a certificate binding node identity to public key, with a
serial and an expiry:

| | |
|---|---|
| `POST /v1/nodes/{id}/certificate` | issues one; requires `write` |
| `GET /v1/nodes/{id}/certificate` | the node's current one, re-verified on the way out |
| `GET /v1/certificates/{serial}` | status: signature validity and authorisation, separately |

What this buys, precisely: **a verifier holding only the CA public key** can check a node's
identity with no database and no admin token. That is the property the shared token cannot
provide at all.

Four things it deliberately does not do:

- **It is not X.509.** No ASN.1, no chain, no name constraints. One certificate, one CA,
  fixed shape. Do not plan interoperability around it.
- **The CA key cannot be rolled over by this code.** Rotation means changing the key every
  verifier holds, which is a deployment decision. A CA that re-signed with a new key while
  verifiers trusted the old one would not be rotating, it would be re-signing.
- **The public key comes from `nodes.device_key`, never from the request.** A CA that
  certified whatever it was handed would certify nothing.
- **An HMAC node cannot be certified** (409). An HMAC key is a shared secret; putting it in
  a document meant to be handed to verifiers would distribute the secret to all of them.

With `CAUCE_CA_KEY` unset every endpoint answers `503
certificate_authority_not_configured`. The alternative — issuing documents signed by
nothing — would produce certificates that look authoritative and verify against no key.

### A node proves possession instead of holding a secret

For a node provisioned with an Ed25519 key, `/v1/sync` can authenticate the node by
**certificate** rather than by shared secret. The HMAC scheme is symmetric: any holder
of `nodes.device_key` can authenticate *as* that node, and the central stores a secret
that both authenticates and forges.

The exchange is two requests:

```
POST /v1/sync/challenge   {"node_id": "CAUCE-001"}
-> {"nonce": "...", "expires_utc_ms": ..., "sign_this": "cauce-sync-challenge\n..."}

POST /v1/sync
   X-Cause-Node:             CAUCE-001
   X-Cause-Certificate:      <base64 of the certificate JSON>
   X-Cause-Nonce:            <the nonce>
   X-Cause-Nonce-Signature:  <base64 of Ed25519 over sign_this>
```

The node signs `sign_this`, which the challenge endpoint returns verbatim so the
firmware does not have to reimplement the encoding. The central then checks, in this
order:

1. **the nonce** — single use, 60 seconds, bound to the node it was issued to;
2. **the CA's signature** over the certificate, and that the certificate is unexpired;
3. **the certificate's subject** — that it names *this* node;
4. **that the certificate's key is the one registered**, so a key rotation actually
   closes the window rather than leaving old certificates working;
5. **the challenge signature**, against the node's public key.

#### Two things that are deliberate

**A presented certificate cannot fall back to the HMAC path.** Presenting *any*
certificate header commits the request to certificate authentication, and a certificate
with no nonce is refused rather than quietly treated as an HMAC request. A fallback
would leave the weaker scheme permanently available and make the stronger one
decorative: an attacker holding one node's HMAC key would present it and skip the
certificate entirely. `test_a_bad_certificate_cannot_fall_back_to_the_hmac_secret`
sends exactly that request.

**A rejected attempt still consumes the nonce.** An attacker cannot make a node burn
unlimited nonces, and a captured request cannot be replayed — and since neither the
certificate nor the signature changes between attempts, single-use *is* the replay
defence. The cost is that a node with a misconfigured certificate fetches another.

The HMAC path is unchanged, because every node provisioned before certificates existed
has an HMAC key and must keep working. The two coexist; the choice is made by what the
request presents, not by a flag the caller sets.

#### What this is not

Not mutual TLS. Caddy terminates TLS with one server certificate and the node's
credential is an application-level header, so the node proves its identity over a
channel whose peer it identified by DNS name. Server-to-client authentication and
channel binding still require the firmware to hold a certificate rather than a seed.

`NodeConfig` now carries the credential — `sync_auth_seed`, 64 hex characters, and
`sync_certificate`, stored verbatim as JSON — and `main.cpp` decodes it, builds the authenticator
with `Esp32ChallengeSource`, and hands `NodeCertificateAuth` to the transport. The seed is
validated **at parse time**, and deliberately outside the parser's forgiving `malformed > 3`
tolerance: a 62-character seed is a provisioning typo, and accepting it leaves a node that boots,
looks correctly configured, and fails every authentication as an invalid signature — which points
at the certificate rather than at the config line that is wrong. A node with a seed but no
certificate, or the reverse, is reported once at boot and left on HMAC, because presenting no
certificate commits the central to certificate authentication and falls back to nothing.

## Firmware updates are authorised by a different secret than data is

The manifest signature used to be derived from `nodes.device_key`. For an HMAC node that key is
symmetric, so it authenticates *as* that node as well as for it — and while it also authorised
firmware, compromising the data channel compromised the update channel. A node that had stolen
one device key could therefore authorise firmware.

`nodes.manifest_key` is now a separate column with its own algorithm field, set at provisioning
and validated to the same minimum as the data key; the firmware gained `ota_manifest_key` in
`NodeConfig` and prefers it. The derivation is unchanged, so separating a node is a matter of
setting a column, with no change on the verifying side.

**The fallback is deliberate and loud.** A node provisioned before the column existed has no
separate update key and still receives a valid signature from its data key, with a warning on both
sides. Failing closed would strand every deployed node on its next update, which trades a real
exposure for a guaranteed outage. Re-provisioning the data key no longer silently wipes the
separation — which would have been the worst version of this feature: the column exists, the
operator set it, and a routine rotation removed the protection they had added.

Both halves exist and are joined. The central side is `node_auth.py`. The device side is
`NodeAuthenticator` in `cauce_app` — it fetches the challenge, signs `sign_this` with the
node's Ed25519 seed and produces the four headers — and `NodeCertificateAuth`, which implements
`hal::IAuthHeaderSource` so that `Esp32HttpSyncTransport` can call it from inside a POST.

Neither has met the other on real hardware: the exchange compiles for `esp32dev`, passes 28 host
tests, and the flash artifact checks and the bench self-test still report no board.

Two properties of that join are worth stating because both are easy to get wrong:

**Failure emits nothing.** The central commits to certificate authentication the moment it sees
`X-Cauce-Certificate` and will not fall back to the shared secret, so a partial header set
earns a rejection where HMAC would have worked. `addAuthHeaders` returns false and emits zero
headers on any failure, and the transport ignores the return value — which is what makes
falling back to HMAC a downgrade in *strength* rather than a path to guaranteed rejection.

**The dependency points one way.** `cauce_hal` owns `IAuthHeaderSource` and the four header
names, because it is what puts them on the wire. The credential lives in `cauce_app`, which
already depends on `cauce_hal`. The first attempt gave `cauce_hal` an authenticator directly
and closed a cycle, and the ESP32 build failed on it.

The one thing the device side deliberately does **not** do is rebuild `sign_this` from the
nonce and the expiry. The central returns the exact string it will verify against, and
storing it verbatim means the encoding has one owner instead of two that can drift.

Getting that wrong produces an invalid-signature error at the central that points at the *key*
rather than at the formatting, which is why the encoding is pinned on both sides — but note
what each pin actually checks. `jsonStringField` refuses any escape it cannot decode
faithfully, `\uXXXX` included, because decoding `\u0041` to `A` would have the node sign bytes
the central never wrote. The escapes `json.dumps` does emit — `\n`, `\t`, `\r`, `\"`, `\\` —
are decoded, since the canonical string's three newlines arrive as three `\n` pairs and a
parser that refused them could never read a real challenge.

Both sides pin the *serialised* form, not just the string being signed:
`test_node_auth_wire.cpp` carries the literal `json.dumps` output, and the backend's
`test_the_serialised_form_is_still_what_dumps_produces` asserts against `json.dumps` live
rather than against a copy of its output, so a change to spacing or escaping fails with the
cause named.

Issuing refuses a validity above 366 days rather than clamping it, and refuses a public key
that is not a curve point. The second check is not a length check: `from_public_bytes`
accepts all 32 bytes and defers the failure to verification time, so about half of all
32-byte values are accepted as keys and can never verify anything. Checking the curve point
turns "never works, much later, on a node" into "refused now".

Rotation **replaces**. Re-issuing marks the previous certificate `retired` and keeps the
row, because a table where rotation destroyed the old certificate could not answer which key
a node was using when a measurement arrived. `trusted` is the conjunction of signature
validity, `status='active'` and not-expired; a retired certificate is still a genuine
document from the CA, and collapsing that distinction is how "valid" starts meaning
"authorised".

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

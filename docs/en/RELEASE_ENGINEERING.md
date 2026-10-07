# Release engineering

What has to be true before a tag means something, and what each part of `scripts/release-gate.ps1`
is actually proving.

## The principle

A release gate that reports green has to be reporting something true. That is a stronger
requirement than "the tests pass", and the difference is where most of the work in this file went.

Two failure modes cost real time in this project, and both are the same shape:

- **A claim nothing checks.** The documented test totals were prose. The suite moved 325 → 345 →
  349 across three commits and four documents kept quoting the old number, because no
  executable compared them. `release-gate.ps1` now reads the counts out of `verify-all.ps1`'s
  output and refuses a release whose documents disagree.
- **A check that cannot fail.** The first version of that check demanded a README line reading
  `"541 firmware"`, because it reused the firmware pattern for the backend count. It failed
  loudly, which is the only reason it was findable. Every check below was verified by making it
  fail, not by reading it and agreeing it would work.

## What the gate proves, and what it does not

`scripts/release-gate.ps1 -Tag v0.1.0`

| Check | What it actually establishes |
|---|---|
| the tree is clean | no uncommitted release |
| version and tag | the tag names this commit |
| backend image is pinned | the base digest is the one this gate expects, not the one in the file it is checking |
| full verification | firmware tests, ESP32 build, backend suite, live E2E — and the documented totals match what ran |
| ESP32 flash artifacts | esptool parses the bootloader and application; two application slots, no overlap, sector aligned; the image fits its slot |
| container image | builds from the real Dockerfile, serves `/healthz` over HTTP, refuses writes without an admin token, passes the suite inside the image, and two builds of one tree produce one image ID |
| pinned closure | every dependency hash verifies and the app runs from it in a virtualenv containing nothing else |
| SBOM | every pin in the SBOM agrees with the lock |
| CA fails closed | with no CA key configured, the certificate endpoints are `503` |
| documentation | every document the index lists exists, in both languages |
| firmware signs | the codec declares, signs and selects both algorithms, and refuses a bad key length |
| group order | the Ed25519 group order constant is still the verified one |
| no unregistered tests | no firmware test file mentions `NOT REGISTERED` |

## What it does not prove

Stated plainly, because a gate's value is entirely in the boundary of its claim:

- **No hardware.** Nothing here executes firmware on an ESP32. QEMU models dc232b and lx6 and
  has no esp32 machine, so there is no emulator that would let it. The flash checks are the
  most that is knowable offline, and a board can still fail every one of them by rebooting.
- **The firmware image is not reproducible across directories.** It embeds `app_elf_sha256` and
  an appended image digest, both taken over a file whose debug sections carry absolute paths.
  Two clones of one commit at different paths differ in 65 of 1,115,984 bytes. `artifacts.sha256`
  is therefore compared, never rewritten — rewriting it made the first gate run dirty the tree
  and the second run fail for a reason unrelated to the code.
- **The arm64 image is in the pinned index but has never been executed.** This machine is amd64.
- **The certificate exchange has never met a board.** Both halves compile and both are tested
  against each other's bytes; nothing has proven they interoperate on silicon.

## Versioning

`0.1.0` is deliberate and the reasoning is in the tag message: the specification is not frozen,
phases 0 and 2 are unstarted, and the board has never been on a desk. A `1.0.0` here would be a
claim about readiness that nothing in the repository supports.

The tag is annotated and its message is the release notes. It is 7 kB and states what the
release does **not** establish, because a release note that lists only achievements is a
marketing document.

## Release cadence

There is none, deliberately, until there is a fleet to release to. When there is:

- **Patch** — a fix, no schema change, no protocol change.
- **Minor** — a backwards-compatible addition: a new endpoint, a new field, a new firmware
  version that reads and writes the same format.
- **Major** — anything that makes an old central reject a new node or the reverse. This includes
  a change to the canonical challenge encoding, the signed frame format, or the coverage
  pagination, because each of those has a node on the other side of it.

## Rotation procedures

Two credentials need rotating on a schedule, and both are documented where they are used:

- **`CAUCE_CA_KEY`** — the Ed25519 node-identity CA. `docs/en/SECURITY.md`. Rotation replaces
  rather than accumulates, keeping the retired row, because a table that destroys the old
  certificate cannot answer which key a node held when a measurement arrived.
- **The X.509 CA in `backend/tools/pki.py`** — for the central's own TLS. `pki.py inventory`
  lists every certificate and its remaining validity; it warns inside 90 days for a CA. This is a
  second trust root on purpose: the Ed25519 CA signs JSON and cannot be presented in a TLS
  handshake, so it is not an alternative to this one, it is a different thing.

## Running it

```sh
./scripts/verify-all.ps1                     # fast: tests, ESP32 build, backend, live E2E
./scripts/release-gate.ps1 -Tag v0.1.0       # the full gate, including containers
./scripts/release-gate.ps1 -SkipContainer    # without podman; the container checks are skipped and noted
```

The gate is idempotent by construction and that is tested: it has been run twice consecutively
on the same clone, passing both times, with the tree clean after each. A gate whose second run
fails is not a gate.
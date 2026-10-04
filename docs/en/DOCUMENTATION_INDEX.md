# CAUCE Documentation Index

Spanish mirror: [docs/es/DOCUMENTATION_INDEX.md](../es/DOCUMENTATION_INDEX.md).

Single source of truth for implemented-vs-planned: [STATUS.md](../../STATUS.md).
It outranks everything here when they disagree — which shouldn't happen,
but paper is patient.

| Document | What | Status |
|---|---|---|
| [TESTING.md](TESTING.md) | How to run every suite; what coverage actually means | Stable |
| [DEPLOYMENT.md](DEPLOYMENT.md) | Zero → operating pilot, in order, no skipped steps | Stable |
| [HARDWARE.md](HARDWARE.md) | Draft BOM, reference wiring, siting rules | Draft v0, no hardware yet |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Layers, flows, state machines, and why | Stable |
| [DATA_MODEL.md](DATA_MODEL.md) | Measurement, quality, storage, exports, inclusive windows | Stable |
| [DOMAIN_GLOSSARY.md](DOMAIN_GLOSSARY.md) | One term per concept, enforced everywhere | Stable |
| [API.md](API.md) | Node `/api/v1`: endpoints, auth, streaming, codes | Stable |
| [SYNC.md](SYNC.md) | Node↔central contract, watermark, HMAC envelope | Stable |
| [BACKEND.md](BACKEND.md) | Central: endpoints, ingestion guarantees, limits | Stable |
| [SECURITY.md](SECURITY.md) | What we do, what we openly don't | Stable |
| [CALIBRATION.md](CALIBRATION.md) | Offset/scale model, honest limits, co-location plan | Model defined, runtime pending |
| [OTA.md](OTA.md) | Decision FSM, anti-brick rules, manifest contract | Decision shipped, flashing pending |
| [DASHBOARD.md](DASHBOARD.md) | Local SPA + captive portal, and why it's dependency-free | Stable |
| [BENCH_PLAN.md](BENCH_PLAN.md) | B1–B5 physical validation with exit criteria | Plan, not executed |
| [ROADMAP.md](ROADMAP.md) | Sequencing M0–M5, principles, non-goals | Stable |
| [RELEASE_READINESS.md](RELEASE_READINESS.md) | Phases 0–6 to a frozen baseline, and the gate checklist | Draft |
| [PILOT_SPEC.md](PILOT_SPEC.md) | Frozen pilot: nodes, budget, acceptance gate | Frozen |
| [I18N.md](I18N.md) | English-only code; localized surfaces and docs | Stable |

Protocol specs live in [protocol/v1/](../../protocol/v1/PROTOCOL.md).
Repo operating files: `STATUS.md`, `SECURITY.md`, `CONTRIBUTING.md`.

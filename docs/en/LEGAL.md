# CAUCE Legal System — developer notes

Internal documentation. The public legal texts live in code
(`backend/cauce_server/legal.py`, `DOCS` dict); this file explains the
machinery. Nothing here is legal advice.

## Architecture

One module owns everything legal: versioned documents, negotiation,
footer injection. There is no CMS, no database table, no remote fetch —
legal text ships with the release it describes, so the text can never
drift from the behavior it documents.

- Documents: `terms`, `privacy`, `cookies`, `refunds`. Current version
  `2026.09-v1` (`LEGAL_VERSION`), effective 2026-09-25.
- Only `status == "published"` is served. Anything else (drafts,
  internal notes) is a 404 by construction — the allowlist is the
  `DOCS` keys, not a directory listing.
- Content negotiation: `Accept-Language` en/es, default es (same
  default as the dashboards). No per-user, per-organization or
  per-country branching exists, because CAUCE has no users or
  organizations: `user_type` is always null, explicitly, by design.
- Every HTML page gets the footer + skip link through `_with_legal()`
  in `dashboard.py` (single injection point, string-level, after the
  page is rendered).

## Country and identity resolution

There is no cascade: CAUCE serves one pilot jurisdiction (Uruguay,
Canelones). No `organization_id` parameter exists anywhere, so there
is no IDOR surface. No `country` parameter exists, so there is no
document-confusion vector. If multi-country operation ever starts,
this section must be rewritten first — see finding F-08.

## Consent

Central server: sets zero cookies, runs zero analytics, loads zero
third-party resources (asserted by tests on every page). With nothing
to consent to, no banner is shown — and a test pins that claim, so a
future tracker breaks the build instead of shipping silently. Node
dashboard: one `localStorage` key (`cauce-lang`, UI language,
on-device, never transmitted).

## Versioning and traceability

Each document carries version/effective/updated/status, rendered on
the page. Consent events don't exist (no consent-gated processing
exists); if they are ever introduced, the accepted version must be
recorded alongside — currently a documented non-requirement, not an
oversight.

## Fallback

No remote dependency means no degraded mode is needed: the documents
are static strings in the release. The only fallback in the system is
language negotiation defaulting to Spanish.

## Testing

`test_legal_pages_render_versioned_no_inventions`,
`test_legal_footer_on_every_page`,
`test_no_cookies_no_third_party_loads`,
`test_forms_have_labels_and_named_buttons`. Network-behavior checks
(analytics-before-consent flows) are not applicable: packet capture
would show the same thing the no-load test asserts — zero third-party
requests, because zero third-party code ships.

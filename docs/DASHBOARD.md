# CAUCE Local Web Dashboard

## Access

Join the node's Wi-Fi (or the same network in station mode) and open:

```
http://<node-ip>/            (e.g. http://192.168.4.1 in AP mode)
```

With the captive portal active, any domain the phone tries to resolve
redirects to the dashboard.

## Languages

ES/EN switch on-page (persisted in `localStorage`, initial choice from
browser language). All static labels go through a translation dictionary;
domain values stay language-neutral codes.

## What it shows

| Section | Content |
|---|---|
| Main cards | Latest temperature/humidity with unit, data **quality**, time since last reading, node clock state |
| Chart | Native `<canvas>` time series (no libraries), 1h / 6h / 24h / 7d ranges, Temperature ↔ RH toggle |
| Export | Direct CSV and JSON links for the last 24h |
| Health | Full `/api/v1/health` JSON |
| Configuration | Minimal form (interval + token) that builds the KV body for `POST /api/v1/config` |

## Design decisions

- **Zero external dependencies**: no CDNs, remote fonts or frameworks —
  fully offline from flash (~13KB embedded).
- Mobile-first: adaptive grid, system typography, high contrast.
- Data quality is first-class: every card shows VALID/SUSPECT/INVALID/
  MISSING state with color.
- Staggered auto-refresh: cards 30s, health 60s, chart 120s.
- Errors visible: "no data" red pill; config failures show the exact API
  error list.

## Implementation

HTML/CSS/JS embedded as a C++ string (`WebAssets.cpp`) streamed by
`ApiRouter` as a paginated stream (`StreamKind::Html`) — same chunk
mechanics as export; HTML integrity covered by tests (starts `<!DOCTYPE`,
ends `</html>`, full length delivered).

Captive portal: `Esp32CaptivePortal` (wildcard DNS→node IP);
compile-verified, real behavior pending hardware.

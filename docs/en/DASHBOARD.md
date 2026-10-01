# CAUCE Local Web Dashboard

## Access

Join the node's Wi-Fi (or the same LAN in station mode) and open:

```
http://<node-ip>/            (e.g. http://192.168.4.1 in AP mode)
```

With the captive portal running, any domain the phone tries becomes
the dashboard — no need to guess the IP.

## Languages

ES/EN switch on the page (saved in `localStorage`, first guess from
the browser language). Static labels come from a translation
dictionary; domain values stay as neutral codes, so `VALID` never
turns into a translation argument.

## What it shows

| Section | Content |
|---|---|
| Main cards | Latest temperature/humidity with unit, **quality** badge, time since last reading, node clock state |
| Chart | Hand-rolled `<canvas>` time series (no libraries to download), 1h / 6h / 24h / 7d ranges, Temperature ↔ RH toggle |
| Export | Direct CSV and JSON links for the last 24h |
| Health | The whole `/api/v1/health` JSON, unfiltered |
| Configuration | Small form (interval + token) that builds the KV body for `POST /api/v1/config` |

## Design decisions

- **Zero external dependencies.** No CDNs, no remote fonts, no
  frameworks — the page works from flash with no internet at all
  (~13KB embedded). A dashboard that needs the cloud to show you a
  sensor two meters away would be embarrassing.
- Mobile-first: adaptive grid, system typography, high contrast for
  sunlight.
- Data quality is visible, not buried: every card carries its
  VALID/SUSPECT/INVALID/MISSING state in color.
- Staggered refresh so the little CPU survives: cards 30s, health 60s,
  chart 120s.
- Errors in plain sight: a red "no data" pill; config failures print
  the exact API error list instead of a generic frown.

## Implementation

The HTML/CSS/JS lives as a C++ string (`WebAssets.cpp`) and
`ApiRouter` serves it as a paginated stream (`StreamKind::Html`) —
same chunk machinery as export. Tests pin the integrity (starts with
`<!DOCTYPE`, ends with `</html>`, full length delivered).

Captive portal: `Esp32CaptivePortal` (wildcard DNS→node IP).
Compile-verified; real phone behavior waits for hardware.

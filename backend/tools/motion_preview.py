#!/usr/bin/env python3
"""Copy the served HTML to disk with every motion rule ungated.

Windows has an accessibility setting, *Animation effects: Off*, which makes browsers report
`prefers-reduced-motion: reduce`. Every animation in this dashboard is behind that query by
design, so on a machine with it enabled the motion layer is 3,292 characters of CSS and a
script that returns before touching the DOM. The page renders, the tests pass, and the motion
is simply never executed - which looks exactly like a frontend that was not changed.

That is the correct behaviour for the setting and a miserable thing to debug. This script
takes what the server actually sends and writes it with the gate removed, so the motion can be
seen and judged on a machine regardless of the OS preference.

    python backend/tools/motion_preview.py --out build/motion-preview

It is a viewing tool. Nothing in the application reads what it writes.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path
from urllib.request import urlopen

GATE = re.compile(r"@media\s*\(prefers-reduced-motion\s*:\s*no-preference\)\s*\{")
JS_GUARD = re.compile(r"var REDUCED = [^;]+;")

PAGES = [("/", "index"), ("/map", "map"), ("/colocation", "colocation"),
         ("/alerts", "alerts"), ("/heat", "heat"), ("/system", "system")]


def ungate(html: str) -> str:
    """Remove the media gate so the rules apply unconditionally."""
    # Track brace depth so the query's closing brace is found rather than the first `}`.
    m = GATE.search(html)
    if not m:
        return html
    depth = 0
    for i in range(m.end() - 1, len(html)):
        if html[i] == "{":
            depth += 1
        elif html[i] == "}":
            depth -= 1
            if depth == 0:
                return html[:m.start()] + html[m.end() - 1:i] + html[i + 1:]
    return html


def unguard(html: str) -> str:
    """Force the script's reduced-motion flag false."""
    return JS_GUARD.sub("var REDUCED = false;", html, count=1)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", default="http://127.0.0.1:8000")
    ap.add_argument("--out", default="build/motion-preview")
    ap.add_argument("--token", default="mock-admin-token")
    args = ap.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    for path, name in PAGES:
        try:
            req = f"{args.base}{path}"
            with urlopen(req, timeout=30) as r:
                html = r.read().decode("utf-8", "replace")
        except Exception as exc:  # noqa: BLE001 - a viewing tool should not fail hard
            print(f"  {path:<14} SKIPPED: {exc}")
            continue

        fixed = unguard(ungate(html))
        target = out / f"{name}.html"
        target.write_text(fixed, encoding="utf-8")

        gated = "prefers-reduced-motion" in html
        added = len(fixed) - len(html)
        print(f"  {target}  {len(fixed):>6}b  gate removed: {gated}  delta: {added:+d}b")

    print("\nopen these directly in a browser: motion runs regardless of the OS setting")
    print("  they read nothing from the server, so they will not refresh on their own")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

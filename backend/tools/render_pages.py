"""Render every dashboard page to real HTML so the CSS can be analysed as CSS.

`impeccable detect` reports nothing on `dashboard.py`, and that result is worthless rather
than reassuring: the stylesheets live inside Python string literals, so a tool that reads
HTML sees no markup and no rules. The clean bill of health it printed was the absence of a
subject, not the absence of defects.

This module is what the detector is pointed at instead. It writes each page to a `.html` file
with its placeholders filled in plausibly, which is also the honest way to check that a
template's braces resolve - the dashboard already has a test for unresolved placeholders, and
this does not replace it, it catches the CSS.

Not part of the application. Run it, point the detector at the output, delete the output.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

DASHBOARD = Path(__file__).resolve().parents[1] / "cauce_server" / "dashboard.py"

SAMPLE = {
    "lang": "es", "title": "CAUCE Central", "nid": "CAUCE-SIM-001",
    "h_ev": "Eventos de calor", "back_node": "Nodo", "back": "todos los nodos",
    "h_var": "Variable", "opts_v": '<option selected>temperatura del aire</option>',
    "h_thr": "Umbral", "thr": "32.0", "h_dur": "Duración mínima", "dur": "60",
    "apply": "Aplicar", "body": "<p class=\"mut\">contenido</p>",
    "disclaimer": "Datos comparativos; no causalidad.", "h_map": "Mapa",
    "h_heat": "Eventos de calor", "heat_fleet": "En toda la red",
    "heat_window": "Ventana (horas atrás)", "hours": "168", "summary": "",
    "heat_open_note": "Solo tramos cerrados.", "map": "Mapa",
    "back_site": "Sitio", "site": "Sitio", "h_coloc": "Co-localización",
    "h_days": "Días", "sel1": "", "sel7": " selected", "sel30": "",
    "series_json": "[]", "h_div": "Divergencia", "h_node": "Nodo",
    "h_n": "N", "h_mean": "Media", "h_bias": "Sesgo", "h_maxdev": "Desv. máx.",
    "divrows": "", "cal_note": "", "h_info": "Info", "h_report": "Informe",
    "h_sys": "Sistema", "h_alerts": "Alertas", "h_compare": "Comparar",
    "h_export": "Exportar", "h_csv": "CSV", "h_sel": "Selección",
    "h_legal": "Legal", "h_privacy": "Privacidad", "h_refunds": "Reembolsos",
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="build/pages")
    args = ap.parse_args()

    src = DASHBOARD.read_text(encoding="utf-8")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    templates = re.findall(r'(_[A-Z_]+) = """<!DOCTYPE html>(.*?)"""', src, re.S)
    if not templates:
        print("no templates found - the pattern in this script is stale")
        return 1

    # Pull the real base stylesheet out of the module, the same way `_with_legal` does at
    # runtime. Without this the rendered pages carry no styles at all, and a detector run
    # against them reports nothing because there is nothing to report.
    base_match = re.search(r'_BASE_CSS = """(.*?)"""', src, re.S)
    base_css = base_match.group(1) if base_match else ""
    if not base_css:
        print("  WARNING: _BASE_CSS not found; pages will render unstyled")

    for name, body in templates:
        page = f"<!DOCTYPE html>{body}"
        page = page.replace("{base}", base_css)
        for key, value in SAMPLE.items():
            page = page.replace(f"{{{key}}}", value)
        leftovers = sorted(set(re.findall(r"\{(\w+)\}", page)))
        path = out / f"{name.lstrip('_').lower()}.html"
        path.write_text(page, encoding="utf-8")
        note = f"  UNRESOLVED {leftovers}" if leftovers else ""
        print(f"  {path}  {len(page):>6}b{note}")

    print(f"\n{len(templates)} pages written to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

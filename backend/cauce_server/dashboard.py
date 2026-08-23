from __future__ import annotations

from datetime import UTC, datetime

from fastapi import APIRouter, Request
from fastapi.responses import HTMLResponse, StreamingResponse

from .db import query

router = APIRouter()

LABELS = {
    "en": {
        "title": "CAUCE Central",
        "subtitle": "Community microstation network · refreshes every 60s ·",
        "node": "Node", "site": "Site", "last": "Last measurement (UTC)",
        "variable": "Variable", "value": "Value", "quality": "Quality",
        "total": "Total", "api": "API", "export": "export full CSV",
        "disclaimer": "Environmental comparative data. Differences between nodes may "
                      "reflect placement or calibration; they do not establish causality.",
        "none": "—",
    },
    "es": {
        "title": "CAUCE Central",
        "subtitle": "Red comunitaria de microestaciones · se actualiza cada 60s ·",
        "node": "Nodo", "site": "Sitio", "last": "Última medición (UTC)",
        "variable": "Variable", "value": "Valor", "quality": "Calidad",
        "total": "Total", "api": "API", "export": "export CSV completo",
        "disclaimer": "Datos ambientales comparativos. Las diferencias entre nodos pueden "
                      "reflejar ubicación o calibración; no constituyen causalidad.",
        "none": "—",
    },
    "pt": {
        "title": "CAUCE Central",
        "subtitle": "Rede comunitária de microestações · atualiza a cada 60s ·",
        "node": "Nó", "site": "Local", "last": "Última medição (UTC)",
        "variable": "Variável", "value": "Valor", "quality": "Qualidade",
        "total": "Total", "api": "API", "export": "exportar CSV completo",
        "disclaimer": "Dados ambientais comparativos. Diferenças entre nós podem refletir "
                      "localização ou calibração; não constituem causalidade.",
        "none": "—",
    },
}


def pick_labels(request: Request) -> dict:
    header = request.headers.get("accept-language", "es")
    candidates: list[str] = []
    for part in header.split(","):
        code = part.split(";")[0].strip().lower()
        if "*" in code:
            continue
        base = code.split("-")[0]
        if base:
            candidates.append(base)
    for lang in candidates:
        if lang in LABELS:
            return LABELS[lang]
    return LABELS["es"]


_PAGE = """<!DOCTYPE html>
<html lang="{lang}"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="60">
<title>{title}</title>
<style>
body{{margin:0;background:#0f1720;color:#e8eef4;font:15px/1.45 system-ui,sans-serif}}
main{{max-width:900px;margin:0 auto;padding:1rem}}
h1{{color:#39c2a7;letter-spacing:.12em;font-size:1.2rem}}
table{{width:100%;border-collapse:collapse;background:#182430;border-radius:12px;overflow:hidden}}
th{{background:#223140;text-align:left;padding:.55rem .8rem;font-size:.75rem;text-transform:uppercase;color:#8aa0b4}}
td{{padding:.55rem .8rem;border-top:1px solid #223140}}
.q-VALID{{color:#39c2a7}}.q-CALIBRATED{{color:#39c2a7}}.q-SUSPECT{{color:#e6b455}}.q-INVALID,.q-MISSING{{color:#e26d5a}}
a{{color:#7cc4ff;text-decoration:none}}a:hover{{text-decoration:underline}}
.mut{{color:#8aa0b4;font-size:.85rem}}
</style></head><body><main>
<h1>{title}</h1>
<p class="mut">{subtitle}
<a href="/docs">{api}</a> &middot;
<a href="/v1/export-all.csv">{export}</a></p>
<table>
<tr><th>{h_node}</th><th>{h_site}</th><th>{h_last}</th><th>{h_var}</th><th>{h_val}</th><th>{h_q}</th><th>{h_total}</th></tr>
{rows}
</table>
<p class="mut">{disclaimer}</p>
</main></body></html>"""

_ROW = (
    "<tr><td><a href=\"/v1/nodes/{nid}/measurements?limit=50\">{nid}</a></td>"
    "<td>{site}</td><td>{ts}</td><td>{var}</td>"
    "<td>{val} {unit}</td><td class=\"q-{q}\">{q}</td><td>{count}</td></tr>"
)


def _fmt_utc(ms: int | None) -> str:
    if not ms:
        return "—"
    dt = datetime.fromtimestamp(ms / 1000, tz=UTC)
    return dt.strftime("%Y-%m-%d %H:%M")


@router.get("/", response_class=HTMLResponse)
def dashboard(request: Request) -> HTMLResponse:
    labels = pick_labels(request)
    rows = query(
        """SELECT n.node_id, n.site_id,
                  (SELECT COUNT(*) FROM measurements m WHERE m.node_id=n.node_id) AS cnt,
                  m.variable AS var, m.value AS val, m.unit AS unit,
                  m.quality AS q, m.timestamp_utc_ms AS ts
           FROM nodes n
           LEFT JOIN measurements m
             ON m.node_id = n.node_id AND m.sequence = (
                  SELECT MAX(sequence) FROM measurements x WHERE x.node_id=n.node_id)
           ORDER BY n.node_id"""
    )
    body_rows = [
        _ROW.format(
            nid=r["node_id"],
            site=r["site_id"] or labels["none"],
            ts=_fmt_utc(r["ts"]) or labels["none"],
            var=(r["var"] or "").replace("_", " ") if r["var"] else labels["none"],
            val=("—" if r["val"] is None else f"{r['val']:.1f}"),
            unit=r["unit"] or "",
            q=r["q"] or "MISSING",
            count=r["cnt"],
        )
        for r in rows
    ]
    html = (
        _PAGE.replace("{rows}", "\n".join(body_rows))
        .replace("{lang}", "en")
        .replace("{title}", labels["title"])
        .replace("{subtitle}", labels["subtitle"])
        .replace("{api}", labels["api"])
        .replace("{export}", labels["export"])
        .replace("{h_node}", labels["node"])
        .replace("{h_site}", labels["site"])
        .replace("{h_last}", labels["last"])
        .replace("{h_var}", labels["variable"])
        .replace("{h_val}", labels["value"])
        .replace("{h_q}", labels["quality"])
        .replace("{h_total}", labels["total"])
        .replace("{disclaimer}", labels["disclaimer"])
    )
    return HTMLResponse(html)


@router.get("/v1/export-all.csv")
def export_all_csv() -> StreamingResponse:
    def generate():
        yield ("node_id,sensor_id,sequence,timestamp_utc_ms,timestamp_iso,"
               "variable,value,unit,quality,reason_bits,time_uncertain\n")
        cursor = query("SELECT * FROM measurements ORDER BY node_id, sequence")
        for r in cursor:
            iso = datetime.fromtimestamp(
                r["timestamp_utc_ms"] / 1000, tz=UTC
            ).strftime("%Y-%m-%dT%H:%M:%SZ")
            value = "" if r["value"] is None else f"{r['value']}"
            yield (
                f"{r['node_id']},{r['sensor_id'] or ''},{r['sequence']},"
                f"{r['timestamp_utc_ms']},{iso},{r['variable']},{value},"
                f"{r['unit'] or ''},{r['quality']},{r['reason_bits']},"
                f"{r['time_uncertain']}\n"
            )

    return StreamingResponse(
        generate(),
        media_type="text/csv",
        headers={"Content-Disposition": "attachment; filename=cauce-all.csv"},
    )

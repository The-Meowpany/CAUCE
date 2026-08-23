from __future__ import annotations

from datetime import datetime, timezone

from fastapi import APIRouter
from fastapi.responses import HTMLResponse, StreamingResponse

from .db import query

router = APIRouter()

_PAGE = """<!DOCTYPE html>
<html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="60">
<title>CAUCE Central</title>
<style>
body{margin:0;background:#0f1720;color:#e8eef4;font:15px/1.45 system-ui,sans-serif}
main{max-width:900px;margin:0 auto;padding:1rem}
h1{color:#39c2a7;letter-spacing:.12em;font-size:1.2rem}
table{width:100%;border-collapse:collapse;background:#182430;border-radius:12px;overflow:hidden}
th{background:#223140;text-align:left;padding:.55rem .8rem;font-size:.75rem;text-transform:uppercase;color:#8aa0b4}
td{padding:.55rem .8rem;border-top:1px solid #223140}
.q-VALID{color:#39c2a7}.q-SUSPECT{color:#e6b455}.q-INVALID,.q-MISSING{color:#e26d5a}
a{color:#7cc4ff;text-decoration:none}a:hover{text-decoration:underline}
.mut{color:#8aa0b4;font-size:.85rem}
</style></head><body><main>
<h1>CAUCE Central</h1>
<p class="mut">Red comunitaria de microestaciones &middot; se actualiza cada 60s &middot;
<a href="/docs">API</a> &middot;
<a href="/v1/export-all.csv">export CSV completo</a></p>
<table>
<tr><th>Nodo</th><th>Sitio</th><th>&Uacute;ltima medici&oacute;n (UTC)</th><th>Variable</th><th>Valor</th><th>Calidad</th><th>Total</th></tr>
{rows}
</table>
<p class="mut">Datos ambientales comparativos. Las diferencias entre nodos pueden
reflejar ubicaci&oacute;n o calibraci&oacute;n; no constituyen causalidad.</p>
</main></body></html>"""

_ROW = (
    "<tr><td><a href=\"/v1/nodes/{nid}/measurements?limit=50\">{nid}</a></td>"
    "<td>{site}</td><td>{ts}</td><td>{var}</td>"
    "<td>{val} {unit}</td><td class=\"q-{q}\">{q}</td><td>{count}</td></tr>"
)


def _fmt_utc(ms: int | None) -> str:
    if not ms:
        return "&mdash;"
    dt = datetime.fromtimestamp(ms / 1000, tz=timezone.utc)
    return dt.strftime("%Y-%m-%d %H:%M")


@router.get("/", response_class=HTMLResponse)
def dashboard() -> HTMLResponse:
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
            site=r["site_id"] or "&mdash;",
            ts=_fmt_utc(r["ts"]),
            var=(r["var"] or "").replace("_", " ") if r["var"] else "&mdash;",
            val=("&mdash;" if r["val"] is None else f"{r['val']:.1f}"),
            unit=r["unit"] or "",
            q=r["q"] or "MISSING",
            count=r["cnt"],
        )
        for r in rows
    ]
    html = _PAGE.replace("{rows}", "\n".join(body_rows))
    return HTMLResponse(html)


@router.get("/v1/export-all.csv")
def export_all_csv() -> StreamingResponse:
    def generate():
        yield "node_id,sensor_id,sequence,timestamp_utc_ms,timestamp_iso,variable,value,unit,quality,reason_bits,time_uncertain\n"
        cursor = query(
            "SELECT * FROM measurements ORDER BY node_id, sequence"
        )
        for r in cursor:
            iso = (
                datetime.fromtimestamp(r["timestamp_utc_ms"] / 1000, tz=timezone.utc)
                .strftime("%Y-%m-%dT%H:%M:%SZ")
            )
            yield (
                f"{r['node_id']},{r['sensor_id'] or ''},{r['sequence']},"
                f"{r['timestamp_utc_ms']},{iso},{r['variable']},"
                f"{r['value'] if r['value'] is not None else ''},{r['unit'] or ''},"
                f"{r['quality']},{r['reason_bits']},{r['time_uncertain']}\n"
            )

    return StreamingResponse(
        generate(),
        media_type="text/csv",
        headers={"Content-Disposition": "attachment; filename=cauce-all.csv"},
    )

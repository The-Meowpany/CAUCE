from __future__ import annotations

import html
import json
import time
from datetime import UTC, datetime
from typing import Annotated

from fastapi import APIRouter, Header, HTTPException, Request
from fastapi.responses import HTMLResponse, StreamingResponse

from .analytics import detect_heat_events, heat_summary, summary_stats
from .api import analytics_heat_events
from .calibration import (
    apply_value,
    calibrated_uncertainty,
    calibration_for,
    is_identity,
    site_calibrations,
    transform_stats,
)
from .config import settings
from .db import engine, query
from .ratelimit import check_rate
from .security import require_bearer_token

router = APIRouter()

_LABELS = {
    "en": {
    "map_never": 'never reported',
    "map_fresh": 'within window',
    "map_stale": 'stale',
    "map_outside_scale": 'outside 0-40 \\u00b0C',
    "map_colour_fixed": 'Colour scale is fixed, so the same colour means the same temperature on every load.',
    "map_no_window": 'no reading in window',
    "map_window": 'Window',
    "heat_scale_note": 'Threshold from the form below applies to every row.',
    "heat_no_any": 'No node crossed the threshold in this window.',
    "heat_open_note": 'Counts closed runs only. A run still above the threshold when the data ends is not counted, because its end was not observed.',
    "heat_longest": 'Longest',
    "heat_hours_above": 'Hours above',
    "heat_nodes_with": 'nodes above threshold',
    "heat_window": 'Window (hours back)',
    "heat_fleet": 'Heat events across the fleet',
    "heat_title": 'Heat events',"title": "CAUCE Central", "subtitle": "Live microclimate readings from community microstations · auto-refreshes every 60 seconds",
           "node": "Node", "site": "Site", "last": "Last measurement (UTC)", "variable": "Variable",
           "value": "Value", "quality": "Quality", "total": "Total", "api": "API",
           "export": "export full CSV", "disclaimer": "Environmental comparative data. Differences between nodes may reflect placement or calibration; they do not establish causality.", "none": "—",
           "back": "← all nodes", "info": "Node info", "first_seen": "First seen (UTC)",
           "records": "Measurements", "latest": "Latest readings", "recent": "Recent measurements",
           "time": "Time (UTC)", "chart": "Last hours: temperature (°C) and humidity (%RH)",
           "view_json": "view raw JSON", "export_node": "export node CSV", "no_data": "No measurements yet.",
           "readings": "Readings", "last_sync": "Last sync (UTC)", "hottest": "Hottest 7d",
           "events": "heat events", "compare": "Compare nodes →", "range": "Range",
           "variables": "Variables", "apply": "Apply", "stat_min": "Min", "stat_max": "Max",
           "stat_mean": "Mean", "stat_n": "N", "qbreak": "Quality breakdown",
           "node_a": "Node A", "node_b": "Node B", "node_c": "Node C", "days": "Days", "mean_diff": "Mean difference",
           "d1": "24h", "d7": "7d", "d30": "30d", "nodes": "Nodes", "stats": "Statistics",
           "alerts": "Alerts", "map": "Map", "coloc": "Co-location", "report": "Evidence report",
           "rules": "Rules", "log": "Alert log", "channel": "Channel",
           "target": "Target (URL or chat id)", "cooldown": "Cooldown (min)",
           "stale_after": "Stale after (min)", "create": "Create rule", "delete": "delete",
           "enabled": "On", "delivered": "Sent", "when": "When (UTC)", "message": "Message",
           "kind_heat": "heat", "kind_stale": "stale",
           "no_coords": "No located sites yet — set coordinates via PUT /v1/sites/{id}/location.",
           "bias": "Bias vs mean", "maxdev": "Max deviation",
           "from": "From", "to": "To", "custom": "Custom range", "print": "Print", "check_now": "Check now", "check_hint": "Stale rules need a trigger: call POST /v1/alerts/check from cron (heat rules fire on ingest).", "field": "Interpolated field (IDW, illustrative)", "legend_low": "cool", "legend_high": "warm",
           "ev_title": "Heat events", "threshold": "Threshold (°C)", "min_dur": "Min duration (min)",
           "peak": "Peak", "duration": "Duration", "start": "Start (UTC)", "end": "End (UTC)",
           "no_events": "No events found.", "no_events_hint": "Highest recorded value is below the threshold — lower it or widen the range.",
           "max_seen": "Highest recorded",
           "uptime": "Uptime", "database": "Database", "version": "Version",
           "dtable": "Table", "drows": "Rows", "per_node": "Per node", "zin": "Zoom in", "zout": "Zoom out", "zreset": "Reset view",
            "sys_status": "System status",
            "firmware": "Firmware", "flags": "Flags", "visit": "Visit",
            "no_visit": "no visit needed", "coverage": "Coverage",
            "gaps": "Gaps", "fleet": "Fleet", "retention": "Retention",
            "last_run": "Last run", "expected": "Expected", "received": "Received",
            "longest_gap": "Longest gap", "worst_node": "Worst node",
            "reason": "Reason", "no_gaps": "No gaps above the sampling interval.",
            "calibrated_marker": "* calibrated value (site calibration applied)",
            "calibration_none": "Raw values: no site calibration recorded.",
            "calibration": "Calibration",
            "cal_none": "No calibration recorded for this site.",
            "fleet_hint": "A visit is needed when a node is offline, its clock is unset, storage is nearly full or frames are corrupted.",
            "yes": "yes", "no": "no",},
    "es": {
    "map_never": 'sin datos nunca',
    "map_fresh": 'en ventana',
    "map_stale": 'antiguo',
    "map_outside_scale": 'fuera de 0-40 \\u00b0C',
    "map_colour_fixed": 'La escala de color es fija, así el mismo color significa la misma temperatura en cada carga.',
    "map_no_window": 'sin lectura en la ventana',
    "map_window": 'Ventana',
    "heat_scale_note": 'El umbral del formulario de abajo se aplica a todas las filas.',
    "heat_no_any": 'Ningún nodo cruzó el umbral en esta ventana.',
    "heat_open_note": 'Solo cuenta tramos cerrados. Un tramo que sigue sobre el umbral cuando acaban los datos no se cuenta, porque su final no se observó.',
    "heat_longest": 'Más larga',
    "heat_hours_above": 'Horas por encima',
    "heat_nodes_with": 'nodos sobre el umbral',
    "heat_window": 'Ventana (horas atrás)',
    "heat_fleet": 'Eventos de calor en toda la red',
    "heat_title": 'Eventos de calor',"title": "CAUCE Central", "subtitle": "Lecturas microclimáticas en vivo de la red comunitaria · se actualiza cada 60 segundos",
           "node": "Nodo", "site": "Sitio", "last": "Última medición (UTC)", "variable": "Variable",
           "value": "Valor", "quality": "Calidad", "total": "Total", "api": "API",
           "export": "export CSV completo", "disclaimer": "Datos ambientales comparativos. Las diferencias entre nodos pueden reflejar ubicación o calibración; no constituyen causalidad.", "none": "—",
           "back": "← todos los nodos", "info": "Info del nodo", "first_seen": "Primera vez (UTC)",
           "records": "Mediciones", "latest": "Últimas lecturas", "recent": "Mediciones recientes",
           "time": "Hora (UTC)", "chart": "Últimas horas: temperatura (°C) y humedad (%HR)",
           "view_json": "ver JSON crudo", "export_node": "exportar CSV del nodo", "no_data": "Aún sin mediciones.",
           "readings": "Lecturas", "last_sync": "Último sync (UTC)", "hottest": "Máxima 7d",
           "events": "eventos de calor", "compare": "Comparar nodos →", "range": "Rango",
           "variables": "Variables", "apply": "Aplicar", "stat_min": "Mín", "stat_max": "Máx",
           "stat_mean": "Media", "stat_n": "N", "qbreak": "Calidades",
           "node_a": "Nodo A", "node_b": "Nodo B", "node_c": "Nodo C", "days": "Días", "mean_diff": "Diferencia media",
           "d1": "24h", "d7": "7d", "d30": "30d", "nodes": "Nodos", "stats": "Estadísticas",
           "alerts": "Alertas", "map": "Mapa", "coloc": "Co-localización", "report": "Reporte de evidencia",
           "rules": "Reglas", "log": "Historial", "channel": "Canal",
           "target": "Destino (URL o chat id)", "cooldown": "Enfriamiento (min)",
           "stale_after": "Silencio tras (min)", "create": "Crear regla", "delete": "borrar",
           "enabled": "Activa", "delivered": "Enviada", "when": "Cuándo (UTC)", "message": "Mensaje",
           "kind_heat": "calor", "kind_stale": "silencio",
           "no_coords": "Aún sin sitios localizados — setear coordenadas vía PUT /v1/sites/{id}/location.",
           "bias": "Sesgo vs media", "maxdev": "Desvío máx",
           "from": "Desde", "to": "Hasta", "custom": "Rango custom", "print": "Imprimir", "check_now": "Chequear ahora", "check_hint": "Las reglas de silencio necesitan un trigger: llamar POST /v1/alerts/check desde cron (las de calor disparan en ingesta).", "field": "Campo interpolado (IDW, ilustrativo)", "legend_low": "frio", "legend_high": "calor",
           "ev_title": "Eventos de calor", "threshold": "Umbral (°C)", "min_dur": "Duración mín (min)",
           "peak": "Pico", "duration": "Duración", "start": "Inicio (UTC)", "end": "Fin (UTC)",
           "no_events": "Sin eventos.",            "no_events_hint": "El valor máximo registrado está bajo el umbral — bajalo o ampliá el rango.",
           "max_seen": "Máximo registrado",
           "uptime": "Uptime", "database": "Base", "version": "Versión",
           "dtable": "Tabla", "drows": "Filas", "per_node": "Por nodo", "zin": "Acercar", "zout": "Alejar", "zreset": "Ver todo",
            "sys_status": "Estado del sistema",
            "firmware": "Firmware", "flags": "Señales", "visit": "Visita",
            "no_visit": "sin visita pendiente", "coverage": "Cobertura",
            "gaps": "Brechas", "fleet": "Flota", "retention": "Retención",
            "last_run": "Última corrida", "expected": "Esperadas", "received": "Recibidas",
            "longest_gap": "Brecha máx", "worst_node": "Peor nodo",
            "reason": "Motivo", "no_gaps": "Sin brechas por encima del intervalo de muestreo.",
            "calibrated_marker": "* valor calibrado (calibración del sitio aplicada)",
            "calibration_none": "Valores crudos: no hay calibración registrada para el sitio.",
            "calibration": "Calibración",
            "cal_none": "No hay calibración registrada para este sitio.",
            "fleet_hint": "Hace falta visita cuando un nodo está caído, su reloj no está fijado, el almacenamiento se llena o hay tramas corruptas.",
            "yes": "sí", "no": "no",},

    "pt": {
    "map_never": 'sem dados nunca',
    "map_fresh": 'na janela',
    "map_stale": 'antigo',
    "map_outside_scale": 'fora de 0-40 \\u00b0C',
    "map_colour_fixed": 'A escala de cor e fixa, portanto a mesma cor significa a mesma temperatura em cada carga.',
    "map_no_window": 'sem leitura na janela',
    "map_window": 'Janela',
    "heat_scale_note": 'O limiar do formulario abaixo aplica-se a todas as linhas.',
    "heat_no_any": 'Nenhum nó cruzou o limiar nesta janela.',
    "heat_open_note": 'Apenas conta trechos fechados. Um trecho ainda acima do limiar quando os dados acabam nao e contado, porque o fim nao foi observado.',
    "heat_longest": 'Mais longa',
    "heat_hours_above": 'Horas acima',
    "heat_nodes_with": 'nos acima do limiar',
    "heat_window": 'Janela (horas atrás)',
    "heat_fleet": 'Eventos de calor em toda a rede',
    "heat_title": 'Eventos de calor',"title": "CAUCE Central", "subtitle": "Leituras microclimáticas ao vivo da rede comunitária · atualiza a cada 60 segundos",
           "node": "Nó", "site": "Local", "last": "Última medição (UTC)", "variable": "Variável",
           "value": "Valor", "quality": "Qualidade", "total": "Total", "api": "API",
           "export": "exportar CSV completo", "disclaimer": "Dados ambientais comparativos. Diferenças entre nós podem refletir localização ou calibração; não constituem causalidade.", "none": "—",
           "back": "← todos os nós", "info": "Info do nó", "first_seen": "Primeira vez (UTC)",
           "records": "Medições", "latest": "Últimas leituras", "recent": "Medições recentes",
           "time": "Hora (UTC)", "chart": "Últimas horas: temperatura (°C) e umidade (%UR)",
           "view_json": "ver JSON bruto", "export_node": "exportar CSV do nó", "no_data": "Ainda sem medições.",
           "readings": "Leituras", "last_sync": "Último sync (UTC)", "hottest": "Máxima 7d",
           "events": "eventos de calor", "compare": "Comparar nós →", "range": "Período",
           "variables": "Variáveis", "apply": "Aplicar", "stat_min": "Mín", "stat_max": "Máx",
           "stat_mean": "Média", "stat_n": "N", "qbreak": "Qualidades",
           "node_a": "Nó A", "node_b": "Nó B", "node_c": "Nó C", "days": "Dias", "mean_diff": "Diferença média",
           "d1": "24h", "d7": "7d", "d30": "30d", "nodes": "Nós", "stats": "Estatísticas",
           "alerts": "Alertas", "map": "Mapa", "coloc": "Colocalização", "report": "Relatório de evidência",
           "rules": "Regras", "log": "Histórico", "channel": "Canal",
           "target": "Destino (URL ou chat id)", "cooldown": "Espera (min)",
           "stale_after": "Silêncio após (min)", "create": "Criar regra", "delete": "excluir",
           "enabled": "Ativa", "delivered": "Enviada", "when": "Quando (UTC)", "message": "Mensagem",
           "kind_heat": "calor", "kind_stale": "silêncio",
           "no_coords": "Ainda sem locais — definir coordenadas via PUT /v1/sites/{id}/location.",
           "bias": "Viés vs média", "maxdev": "Desvio máx",
           "from": "De", "to": "Até", "custom": "Período custom", "print": "Imprimir", "check_now": "Checar agora", "check_hint": "Regras de silêncio precisam de gatilho: chamar POST /v1/alerts/check via cron (as de calor disparam na ingestão).", "field": "Campo interpolado (IDW, ilustrativo)", "legend_low": "frio", "legend_high": "quente",
           "ev_title": "Eventos de calor", "threshold": "Limite (°C)", "min_dur": "Duração mín (min)",
           "peak": "Pico", "duration": "Duração", "start": "Início (UTC)", "end": "Fim (UTC)",
           "no_events": "Sem eventos.",            "no_events_hint": "O valor máximo registrado está abaixo do limite — reduza-o ou amplie o período.",
           "max_seen": "Máximo registrado",
           "uptime": "Uptime", "database": "Banco", "version": "Versão",
           "dtable": "Tabela", "drows": "Linhas", "per_node": "Por nó", "zin": "Aproximar", "zout": "Afastar", "zreset": "Ver tudo",
            "sys_status": "Estado do sistema",
            "firmware": "Firmware", "flags": "Sinais", "visit": "Visita",
            "no_visit": "sem visita pendente", "coverage": "Cobertura",
            "gaps": "Lacunas", "fleet": "Frota", "retention": "Retenção",
            "last_run": "Última execução", "expected": "Esperadas", "received": "Recebidas",
            "longest_gap": "Maior lacuna", "worst_node": "Pior nó",
            "reason": "Motivo", "no_gaps": "Sem lacunas acima do intervalo de amostragem.",
            "calibrated_marker": "* valor calibrado (calibração do local aplicada)",
            "calibration_none": "Valores brutos: não há calibração registrada para o local.",
            "calibration": "Calibração",
            "cal_none": "Não há calibração registrada para este local.",
            "fleet_hint": "É preciso visitar quando um nó está offline, o relógio não está ajustado, o armazenamento está cheio ou há quadros corrompidos.",
            "yes": "sim", "no": "não",},

}


def _form_value(value) -> str:
    """A request- or database-derived value interpolated into a page.

    Escaping a number looks redundant, and for `threshold: float` it is: FastAPI validates
    the parameter before the handler runs, a hostile value gets a JSON 422 that never
    reaches this module, and the value arriving here can only be a float. CodeQL's
    reflected-XSS alert on the events page is a false positive for exactly that reason, and
    that was confirmed by probing the route rather than by reading the annotation.

    It is done anyway. Two reasons, both about the code rather than about the alert:

    - A static analyser cannot see pydantic's validation, so every unescaped interpolation
      of a request parameter is a finding somebody has to triage by hand. This is the third
      time that has been true here, and the alert cost more to explain than the escape costs
      to write.
    - The safety currently rests entirely on an annotation three files away from the sink.
      That is a load-bearing assumption with no test that fails when someone widens the
      annotation, and `{thr}` sits inside an HTML attribute, where a wrong type would be an
      attribute-breakout rather than a stray tag.

    If you are tempted to remove the call because the type makes it redundant: the type is
    what makes it redundant, and the type is not enforced here.
    """
    return html.escape(str(value))


def _json_for_script(value) -> str:
    """JSON that is safe to paste inside a `<script>` element.

    `json.dumps` alone is not safe there, and the reason is specific: an HTML parser
    looks for the literal `</script>` and does not care that it is inside a JSON string.
    So a stored value containing that sequence closes the script block early, and
    everything after it is parsed as markup. That is a stored XSS reachable from any
    value the database holds, and it does not need a quote or an angle bracket in the
    usual sense.

    `<`, `>` and `&` are escaped as their JSON unicode forms, which is invisible to
    JavaScript and invisible to the HTML tokenizer. U+2028 and U+2029 are escaped because
    they are line terminators in JavaScript but not in JSON, so a raw one is a syntax error
    that silently kills the rest of the block.

    Every `json.dumps` result that lands in a page goes through here. Validating the
    input is the other half of the fix and is not a substitute: this function is the sink,
    and the sink should be safe regardless of what reaches it.
    """
    encoded = json.dumps(value)
    return (encoded
            .replace("<", "\\u003c")
            .replace(">", "\\u003e")
            .replace("&", "\\u0026")
            .replace("\u2028", "\\u2028")
            .replace("\u2029", "\\u2029"))


def _pick(request) -> tuple:
    q = (request.query_params.get("lang") or "").lower()
    if q in _LABELS:
        return q, _LABELS[q]
    header = request.headers.get("accept-language", "es")
    for part in header.split(","):
        base = part.split(";")[0].strip().split("-")[0].lower()
        if base in _LABELS:
            return base, _LABELS[base]
    return "es", _LABELS["es"]


def _legal_footer(code: str) -> str:
    from .legal import footer_html
    return footer_html(code)


def _refresh_meta() -> str:
    """The meta-refresh tag, or nothing when the refresh is turned off.

    Every page template carries a `__REFRESH__` token where the interval belongs, and this
    is the one place the token is resolved. Two reasons it lives here rather than in each of
    the nine render sites: the interval is a deployment decision, not a template decision,
    and nine copies of a literal is nine chances to drift. `0` emits no tag at all rather
    than `content="0"`, which some browsers treat as "reload as fast as possible" - the
    opposite of what turning it off is supposed to mean.
    """
    seconds = settings.dashboard_refresh_s
    if seconds <= 0:
        return ""
    return f'<meta http-equiv="refresh" content="{int(seconds)}">'


def _with_legal(page: str, labels, code: str) -> str:
    page = page.replace("<main>", "<main id=\"main\">", 1)
    page = page.replace("__REFRESH__", _refresh_meta())
    return page.replace("</main>", _legal_footer(code) + "</main>")


_PAGE = """<!DOCTYPE html>
<html lang="{lang}"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
__REFRESH__<title>{title}</title>
<style>
body{margin:0;background:#0f1720;color:#e8eef4;font:15px/1.45 system-ui,sans-serif}
main{max-width:1000px;margin:0 auto;padding:1rem}
h1{color:#39c2a7;letter-spacing:.12em;font-size:1.2rem}
h2{color:#8aa0b4;font-size:.95rem;margin:1.4rem 0 .5rem}
table{width:100%;border-collapse:collapse;background:#182430;border-radius:12px;overflow:hidden}
.tbl{overflow-x:auto;border-radius:12px;margin-bottom:.9rem}
.tbl table{white-space:nowrap}
th{background:#223140;text-align:left;padding:.55rem .8rem;font-size:.75rem;text-transform:uppercase;color:#8aa0b4}
td{padding:.55rem .8rem;border-top:1px solid #223140}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:.6rem;margin:.8rem 0}
.stat{background:#182430;border-radius:12px;padding:.7rem .9rem}
.stat .v{font-size:1.4rem;font-weight:600}
.stat .u{color:#8aa0b4;font-size:.8rem}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:.6rem}
.card{background:#182430;border-radius:12px;padding:.8rem 1rem}
.card h3{margin:.1rem 0 .4rem;font-size:1rem}
.card h3 a{color:#e8eef4}
.card .big{font-size:1.6rem;font-weight:600}
canvas.spark{width:100%;height:64px}
.q-VALID,.q-CALIBRATED{color:#39c2a7}.q-SUSPECT{color:#e6b455}.q-INVALID,.q-MISSING{color:#e26d5a}
a{color:#7cc4ff;text-decoration:none}a:hover{text-decoration:underline}
@media(max-width:640px){main{padding:.6rem}th,td{padding:.4rem .45rem;font-size:.82rem}.stat .v{font-size:1.15rem}.card .big{font-size:1.3rem}canvas#chart{height:170px}}
select,button,input{font:inherit;background:#223140;color:#e8eef4;border:1px solid #39c2a7;border-radius:8px;padding:.3rem .6rem;margin:.15rem}
input[type=checkbox]{accent-color:#39c2a7;width:1rem;height:1rem;vertical-align:-.15rem;padding:0}
:focus-visible{outline:2px solid #39c2a7;outline-offset:2px}
::-webkit-scrollbar{height:8px;width:8px}::-webkit-scrollbar-track{background:#0f1720}::-webkit-scrollbar-thumb{background:#223140;border-radius:4px}*{scrollbar-width:thin;scrollbar-color:#223140 #0f1720}
@media(max-width:640px){table.rules td:nth-child(6),table.rules th:nth-child(6),table.rules td:nth-child(7),table.rules th:nth-child(7){display:none}}
.mut{color:#8aa0b4;font-size:.85rem}
nav.mut a{margin-right:1rem}
#langsw{float:right;display:inline-flex;border:1px solid #39c2a7;border-radius:8px;overflow:hidden}
#langsw a{margin:0;padding:.1rem .5rem;color:#8aa0b4;font-size:.8rem}
#langsw a:hover{text-decoration:none;background:#223140}
#langsw strong{padding:.1rem .5rem;font-size:.8rem;background:#39c2a7;color:#0f1720}
</style></head><body><main>
<h1>{title}</h1><p class="mut">{subtitle}</p>
<nav class="mut"><a href="/map">{map}</a><a href="/compare">{compare}</a><a href="/colocation">{coloc}</a><a href="/alerts">{alerts}</a><a href="/docs">{api}</a><a href="/v1/export-all.csv">{export}</a><span id="langsw">{langswitch}</span></nav>
<div class="stats">{stats}</div>
<h2>{h_nodes}</h2>
<div class="grid">{cards}</div>
<p class="mut">{disclaimer}</p></main>
<script>
document.querySelectorAll("canvas.spark").forEach(function(cv){
  var pts = JSON.parse(cv.getAttribute("data-pts") || "[]");
  if (pts.length < 2) return;
  var ctx = cv.getContext("2d"), W = cv.width = cv.offsetWidth || 260, H = cv.height = 64;
  var vs = pts.map(function(p){ return p[1]; });
  var lo = Math.min.apply(null, vs), hi = Math.max.apply(null, vs);
  if (hi === lo) hi = lo + 1;
  var t0 = pts[0][0], t1 = pts[pts.length - 1][0] || (t0 + 1);
  ctx.strokeStyle = "#39c2a7"; ctx.lineWidth = 1.5; ctx.beginPath();
  pts.forEach(function(p, i){
    var x = (p[0] - t0) / (t1 - t0) * W, y = H - 4 - (p[1] - lo) / (hi - lo) * (H - 8);
    if (i) ctx.lineTo(x, y); else ctx.moveTo(x, y);
  });
  ctx.stroke();
});
</script></body></html>"""

_STAT = ("<div class=\"stat\"><div class=\"u\">{label}</div>"
         "<div class=\"v\">{value}</div></div>")

_NODE_CARD = ("""<div class="card"><h3><a href="/nodes/{nid}">{nid}</a></h3>
<div class="mut">{site} · {last}</div>
<div class="big">{temp} <span class="u">°C</span> · {hum} <span class="u">%RH</span></div>
<div class="mut">{qline}</div>
<canvas class="spark" data-pts='{spark}'></canvas>
<div class="mut">{hottest}: {hot} °C · <a href="/nodes/{nid}/events">{events}</a></div>
</div>""")


_NODE_PAGE = """<!DOCTYPE html>
<html lang="{lang}"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
__REFRESH__<title>{title} — {nid}</title>
<style>
body{margin:0;background:#0f1720;color:#e8eef4;font:15px/1.45 system-ui,sans-serif}
main{max-width:900px;margin:0 auto;padding:1rem}
h1{color:#39c2a7;letter-spacing:.12em;font-size:1.2rem}
h2{color:#8aa0b4;font-size:.95rem;margin:1.4rem 0 .5rem}
table{width:100%;border-collapse:collapse;background:#182430;border-radius:12px;overflow:hidden}
.tbl{overflow-x:auto;border-radius:12px;margin-bottom:.9rem}
.tbl table{white-space:nowrap}
th{background:#223140;text-align:left;padding:.55rem .8rem;font-size:.75rem;text-transform:uppercase;color:#8aa0b4}
td{padding:.55rem .8rem;border-top:1px solid #223140}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:.6rem}
.card{background:#182430;border-radius:12px;padding:.7rem .9rem}
.card .v{font-size:1.5rem;font-weight:600}
.card .u{color:#8aa0b4;font-size:.8rem}
.q-VALID,.q-CALIBRATED{color:#39c2a7}.q-SUSPECT{color:#e6b455}.q-INVALID,.q-MISSING{color:#e26d5a}
a{color:#7cc4ff;text-decoration:none}a:hover{text-decoration:underline}
@media(max-width:640px){main{padding:.6rem}th,td{padding:.4rem .45rem;font-size:.82rem}.stat .v{font-size:1.15rem}.card .big{font-size:1.3rem}canvas#chart{height:170px}}
select,button,input{font:inherit;background:#223140;color:#e8eef4;border:1px solid #39c2a7;border-radius:8px;padding:.3rem .6rem;margin:.15rem}
input[type=checkbox]{accent-color:#39c2a7;width:1rem;height:1rem;vertical-align:-.15rem;padding:0}
:focus-visible{outline:2px solid #39c2a7;outline-offset:2px}
::-webkit-scrollbar{height:8px;width:8px}::-webkit-scrollbar-track{background:#0f1720}::-webkit-scrollbar-thumb{background:#223140;border-radius:4px}*{scrollbar-width:thin;scrollbar-color:#223140 #0f1720}
@media(max-width:640px){table.rules td:nth-child(6),table.rules th:nth-child(6),table.rules td:nth-child(7),table.rules th:nth-child(7){display:none}}
.mut{color:#8aa0b4;font-size:.85rem}
canvas#chart{width:100%;height:220px;background:#182430;border-radius:12px}
</style></head><body><main>
<p><a href="/">{back}</a></p>
<h1>{title} — {nid}</h1>
<h2>{h_info}</h2>
<div class="tbl"><table><tr><th>{h_site}</th><th>{h_first}</th><th>{h_last}</th><th>{h_total}</th></tr>
<tr><td>{site}</td><td>{first}</td><td>{last}</td><td>{count}</td></tr></table></div>
<h2>{h_latest}</h2>
<div class="cards">{cards}</div>
<h2>{h_chart}</h2>
<form method="get" action="#chart" class="mut">{h_range}:
<select name="days"><option value="1"{sel1}>24h</option><option value="7"{sel7}>7d</option><option value="30"{sel30}>30d</option></select>
{h_vars}: {varboxes} <button type="submit">{apply}</button><br>
{h_custom}: <label>{h_from} <input type="datetime-local" name="from_s" value="{fromv}"></label> <label>{h_to} <input type="datetime-local" name="to_s" value="{tov}"></label></form>
<div id="legend" class="mut"></div>
<canvas id="chart" width="860" height="220"></canvas>
<script>
var series = {series_json};
(function(){
  var cv = document.getElementById("chart"), ctx = cv.getContext("2d");
  var W = cv.width, H = cv.height, pad = 36;
  var leg = document.getElementById("legend");
  var t0 = Infinity, t1 = -Infinity;
  series.forEach(function(s){ s.pts.forEach(function(p){ if (p[0] < t0) t0 = p[0]; if (p[0] > t1) t1 = p[0]; }); });
  if (t1 < t0) return;
  if (t1 === t0) t1 = t0 + 1;
  function x(ts){ return pad + (ts - t0) / (t1 - t0) * (W - 2 * pad); }
  ctx.strokeStyle = "#223140"; ctx.beginPath(); ctx.moveTo(pad, H - pad); ctx.lineTo(W - pad, H - pad); ctx.stroke();
  ctx.font = "11px system-ui";
  series.forEach(function(s){
    if (!s.pts.length) return;
    var vs = s.pts.map(function(p){ return p[1]; });
    var lo = Math.min.apply(null, vs), hi = Math.max.apply(null, vs);
    if (hi === lo) hi = lo + 1;
    function y(v){ return H - pad - (v - lo) / (hi - lo) * (H - 2 * pad); }
    ctx.strokeStyle = s.color; ctx.lineWidth = 1.6; ctx.beginPath();
    s.pts.forEach(function(p, i){ var px = x(p[0]), py = y(p[1]); if (i) ctx.lineTo(px, py); else ctx.moveTo(px, py); });
    ctx.stroke();
    ctx.fillStyle = s.color;
    leg.innerHTML += "<span style='margin-right:.8rem'>" + s.label + " [" + lo.toFixed(1) + ".." + hi.toFixed(1) + "]</span>";
  });
  function hhmm(ms){ var d = new Date(ms); return ("0" + d.getUTCHours()).slice(-2) + ":" + ("0" + d.getUTCMinutes()).slice(-2); }
  ctx.fillStyle = "#8aa0b4";
  ctx.fillText(hhmm(t0), pad, H - 8);
  var e = hhmm(t1);
  ctx.fillText(e, W - pad - ctx.measureText(e).width, H - 8);
})();
</script>
<h2>{h_stats}</h2>
{statrows}
</table></div>
<h2>{h_qbreak}</h2>
<div class="tbl"><table><tr><th>{h_q}</th><th>{h_n}</th></tr>
{qrows}
</table></div>
<h2>{h_recent}</h2>
<div class="tbl"><table><tr><th>{h_time}</th><th>{h_var}</th><th>{h_val}</th><th>{h_q}</th></tr>
{rows}
</table></div>
<p class="mut"><a href="/v1/nodes/{nid}/measurements?limit=50">{view_json}</a> &middot;
<a href="/v1/nodes/{nid}/export.csv">{export_node}</a> &middot;
<a href="/nodes/{nid}/events">{events}</a> &middot;
<a href="/nodes/{nid}/report">{report}</a></p>
<p class="mut">{disclaimer}</p></main></body></html>"""

_CARD = ("<div class=\"card\"><div class=\"u\">{var}</div>"
         "<div class=\"v\">{val} <span class=\"u\">{unit}</span></div>"
         "<div class=\"q-{q}\">{q} · {ts}</div></div>")

_RECENT_ROW = ("<tr><td>{ts}</td><td>{var}</td>"
               "<td>{val} {unit}</td><td class=\"q-{q}\">{q}</td></tr>")

_STAT_ROW = ("<tr><td>{k}</td><td>{a}</td><td>{b}</td></tr>")

_COLOC_PAGE = """<!DOCTYPE html>
<html lang="{lang}"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{title} — {h_coloc}</title>
<style>
body{margin:0;background:#0f1720;color:#e8eef4;font:15px/1.45 system-ui,sans-serif}
main{max-width:900px;margin:0 auto;padding:1rem}
h1{color:#39c2a7;letter-spacing:.12em;font-size:1.2rem}
h2{color:#8aa0b4;font-size:.95rem;margin:1.4rem 0 .5rem}
table{width:100%;border-collapse:collapse;background:#182430;border-radius:12px;overflow:hidden}
.tbl{overflow-x:auto;border-radius:12px;margin-bottom:.9rem}
.tbl table{white-space:nowrap}
th{background:#223140;text-align:left;padding:.55rem .8rem;font-size:.75rem;text-transform:uppercase;color:#8aa0b4}
td{padding:.55rem .8rem;border-top:1px solid #223140}
a{color:#7cc4ff;text-decoration:none}a:hover{text-decoration:underline}
.mut{color:#8aa0b4;font-size:.85rem}
select,button,input{font:inherit;background:#223140;color:#e8eef4;border:1px solid #39c2a7;border-radius:8px;padding:.3rem .6rem;margin:.15rem}
input[type=checkbox]{accent-color:#39c2a7;width:1rem;height:1rem;vertical-align:-.15rem;padding:0}
:focus-visible{outline:2px solid #39c2a7;outline-offset:2px}
::-webkit-scrollbar{height:8px;width:8px}::-webkit-scrollbar-track{background:#0f1720}::-webkit-scrollbar-thumb{background:#223140;border-radius:4px}*{scrollbar-width:thin;scrollbar-color:#223140 #0f1720}
@media(max-width:640px){table.rules td:nth-child(6),table.rules th:nth-child(6),table.rules td:nth-child(7),table.rules th:nth-child(7){display:none}}
@media(max-width:640px){main{padding:.6rem}th,td{padding:.4rem .45rem;font-size:.82rem}.stat .v{font-size:1.15rem}.card .big{font-size:1.3rem}canvas#chart{height:170px}}
select,button,input{font:inherit;background:#223140;color:#e8eef4;border:1px solid #39c2a7;border-radius:8px;padding:.3rem .6rem;margin:.15rem}
input[type=checkbox]{accent-color:#39c2a7;width:1rem;height:1rem;vertical-align:-.15rem;padding:0}
:focus-visible{outline:2px solid #39c2a7;outline-offset:2px}
::-webkit-scrollbar{height:8px;width:8px}::-webkit-scrollbar-track{background:#0f1720}::-webkit-scrollbar-thumb{background:#223140;border-radius:4px}*{scrollbar-width:thin;scrollbar-color:#223140 #0f1720}
@media(max-width:640px){table.rules td:nth-child(6),table.rules th:nth-child(6),table.rules td:nth-child(7),table.rules th:nth-child(7){display:none}}
canvas#chart{width:100%;height:220px;background:#182430;border-radius:12px}
</style></head><body><main>
<p><a href="/">{back}</a></p>
<h1>{title} — {h_coloc}</h1>
<form method="get" action="#chart" class="mut">{h_var}: <select name="variable">{opts_v}</select>
{h_days}: <select name="days"><option value="1"{sel1}>24h</option><option value="7"{sel7}>7d</option><option value="30"{sel30}>30d</option></select>
<button type="submit">{apply}</button></form>
<div id="legend" class="mut"></div>
<canvas id="chart" width="860" height="220"></canvas>
<script>
var series = {series_json};
(function(){
  var cv = document.getElementById("chart"), ctx = cv.getContext("2d");
  var W = cv.width, H = cv.height, pad = 36;
  var leg = document.getElementById("legend");
  var t0 = Infinity, t1 = -Infinity;
  series.forEach(function(s){ s.pts.forEach(function(p){ if (p[0] < t0) t0 = p[0]; if (p[0] > t1) t1 = p[0]; }); });
  if (t1 < t0) return;
  if (t1 === t0) t1 = t0 + 1;
  function x(ts){ return pad + (ts - t0) / (t1 - t0) * (W - 2 * pad); }
  var glo = Infinity, ghi = -Infinity;
  series.forEach(function(s){ s.pts.forEach(function(p){ if (p[1] < glo) glo = p[1]; if (p[1] > ghi) ghi = p[1]; }); });
  if (ghi === glo) ghi = glo + 1;
  function y(v){ return H - pad - (v - glo) / (ghi - glo) * (H - 2 * pad); }
  ctx.fillStyle = "#8aa0b4"; ctx.font = "11px system-ui";
  ctx.fillText(glo.toFixed(1), 2, H - pad); ctx.fillText(ghi.toFixed(1), 2, pad + 4);
  series.forEach(function(s){
    if (!s.pts.length) return;
    ctx.strokeStyle = s.color; ctx.lineWidth = 1.5; ctx.beginPath();
    s.pts.forEach(function(p, i){ var px = x(p[0]), py = y(p[1]); if (i) ctx.lineTo(px, py); else ctx.moveTo(px, py); });
    ctx.stroke();
    ctx.fillStyle = s.color;
    leg.innerHTML += "<span style='margin-right:.8rem'>" + s.label + "</span>";
  });
  function hhmm(ms){ var d = new Date(ms); return ("0" + d.getUTCHours()).slice(-2) + ":" + ("0" + d.getUTCMinutes()).slice(-2); }
  ctx.fillStyle = "#8aa0b4"; ctx.font = "11px system-ui";
  ctx.fillText(hhmm(t0), pad, H - 8);
  var e = hhmm(t1);
  ctx.fillText(e, W - pad - ctx.measureText(e).width, H - 8);
})();
</script>
<h2>{h_div}</h2>
  <p class="mut">{cal_note}</p>
  <div class="tbl"><table><tr><th>{h_node}</th><th>{h_n}</th><th>{h_mean}</th><th>{h_bias}</th><th>{h_maxdev}</th></tr>
{divrows}
</table></div>
<p class="mut">{disclaimer}</p></main></body></html>"""

_ALERTS_PAGE = """<!DOCTYPE html>
<html lang="{lang}"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{title} — {h_alerts}</title>
<style>
body{margin:0;background:#0f1720;color:#e8eef4;font:15px/1.45 system-ui,sans-serif}
main{max-width:900px;margin:0 auto;padding:1rem}
h1{color:#39c2a7;letter-spacing:.12em;font-size:1.2rem}
h2{color:#8aa0b4;font-size:.95rem;margin:1.4rem 0 .5rem}
table{width:100%;border-collapse:collapse;background:#182430;border-radius:12px;overflow:hidden}
.tbl{overflow-x:auto;border-radius:12px;margin-bottom:.9rem}
.tbl table{white-space:nowrap}
th{background:#223140;text-align:left;padding:.55rem .8rem;font-size:.75rem;text-transform:uppercase;color:#8aa0b4}
td{padding:.55rem .8rem;border-top:1px solid #223140}
a{color:#7cc4ff;text-decoration:none}a:hover{text-decoration:underline}
.mut{color:#8aa0b4;font-size:.85rem}
select,button,input{font:inherit;background:#223140;color:#e8eef4;border:1px solid #39c2a7;border-radius:8px;padding:.3rem .6rem;margin:.15rem}
input[type=checkbox]{accent-color:#39c2a7;width:1rem;height:1rem;vertical-align:-.15rem;padding:0}
:focus-visible{outline:2px solid #39c2a7;outline-offset:2px}
::-webkit-scrollbar{height:8px;width:8px}::-webkit-scrollbar-track{background:#0f1720}::-webkit-scrollbar-thumb{background:#223140;border-radius:4px}*{scrollbar-width:thin;scrollbar-color:#223140 #0f1720}
@media(max-width:640px){table.rules td:nth-child(6),table.rules th:nth-child(6),table.rules td:nth-child(7),table.rules th:nth-child(7){display:none}}
@media(max-width:640px){main{padding:.6rem}th,td{padding:.4rem .45rem;font-size:.82rem}.stat .v{font-size:1.15rem}.card .big{font-size:1.3rem}canvas#chart{height:170px}}
select,button,input{font:inherit;background:#223140;color:#e8eef4;border:1px solid #39c2a7;border-radius:8px;padding:.3rem .6rem;margin:.15rem}
input[type=checkbox]{accent-color:#39c2a7;width:1rem;height:1rem;vertical-align:-.15rem;padding:0}
:focus-visible{outline:2px solid #39c2a7;outline-offset:2px}
::-webkit-scrollbar{height:8px;width:8px}::-webkit-scrollbar-track{background:#0f1720}::-webkit-scrollbar-thumb{background:#223140;border-radius:4px}*{scrollbar-width:thin;scrollbar-color:#223140 #0f1720}
@media(max-width:640px){table.rules td:nth-child(6),table.rules th:nth-child(6),table.rules td:nth-child(7),table.rules th:nth-child(7){display:none}}
form.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:.4rem;background:#182430;border-radius:12px;padding:.8rem}
form.grid label{display:flex;flex-direction:column;font-size:.8rem;color:#8aa0b4}
.ok{color:#39c2a7}.fail{color:#e26d5a}
</style></head><body><main>
<p><a href="/">{back}</a></p>
<h1>{title} — {h_alerts}</h1>
<h2>{h_rules}</h2>
<div class="tbl"><table class="rules"><tr><th>ID</th><th>{h_node}</th><th>{h_kind}</th><th>{h_thr}</th><th>{h_chan}</th><th>{h_target}</th><th>{h_cool}</th><th>{h_last}</th><th></th></tr>
{rulerows}
</table></div>
<h2>{h_create}</h2>
<form id="newrule" class="grid">
<label>{h_node} (id or *)<input name="node_id" value="*" required></label>
<label>{h_kind}<select name="kind"><option value="heat">{kind_heat}</option><option value="stale">{kind_stale}</option></select></label>
<label>{h_thr}<input name="threshold" type="number" step="0.5" value="32"></label>
<label>{h_stale}<input name="stale_min" type="number" step="5" value="60"></label>
<label>{h_chan}<select name="channel"><option value="webhook">webhook</option><option value="telegram">telegram</option></select></label>
<label>{h_target}<input name="target" placeholder="https://… / chat id"></label>
<label>{h_cool}<input name="cooldown_min" type="number" step="5" value="60"></label>
<button type="submit">{create}</button>
</form>
<p class="mut">{check_hint} <button id="checkbtn">{h_check}</button> <span id="checkres"></span></p>
<h2>{h_log}</h2>
<div class="tbl"><table><tr><th>{h_when}</th><th>{h_node}</th><th>{h_msg}</th><th>{h_sent}</th></tr>
{logrows}
</table></div>
<p class="mut">{disclaimer}</p></main>
<script>
document.getElementById("newrule").addEventListener("submit", function(ev){
  ev.preventDefault();
  var fd = new FormData(ev.target), body = {};
  ["node_id", "kind", "channel", "target"].forEach(function(k){ body[k] = fd.get(k); });
  ["threshold", "stale_min", "cooldown_min"].forEach(function(k){ var v = parseFloat(fd.get(k)); if (!isNaN(v)) body[k] = v; });
  fetch("/v1/alerts/rules", {method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify(body)})
    .then(function(r){ if (r.ok) location.reload(); });
});
function delRule(id){
  fetch("/v1/alerts/rules/" + id, {method: "DELETE"}).then(function(r){ if (r.ok) location.reload(); });
}
document.getElementById("checkbtn").addEventListener("click", function(){
  fetch("/v1/alerts/check", {method: "POST"}).then(function(r){ return r.json(); }).then(function(j){
    document.getElementById("checkres").textContent = JSON.stringify(j.fired || []);
  });
});
</script></body></html>"""

_REPORT_PAGE = """<!DOCTYPE html>
<html lang="{lang}"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{title} — {nid} — {h_report}</title>
<style>
body{margin:0;background:#0f1720;color:#e8eef4;font:15px/1.45 system-ui,sans-serif}
main{max-width:900px;margin:0 auto;padding:1rem}
h1{color:#39c2a7;letter-spacing:.12em;font-size:1.2rem}
h2{color:#8aa0b4;font-size:.95rem;margin:1.4rem 0 .5rem}
table{width:100%;border-collapse:collapse;background:#182430;border-radius:12px;overflow:hidden}
.tbl{overflow-x:auto;border-radius:12px;margin-bottom:.9rem}
.tbl table{white-space:nowrap}
th{background:#223140;text-align:left;padding:.55rem .8rem;font-size:.75rem;text-transform:uppercase;color:#8aa0b4}
td{padding:.55rem .8rem;border-top:1px solid #223140}
a{color:#7cc4ff;text-decoration:none}a:hover{text-decoration:underline}
.mut{color:#8aa0b4;font-size:.85rem}
select,button,input{font:inherit;background:#223140;color:#e8eef4;border:1px solid #39c2a7;border-radius:8px;padding:.3rem .6rem;margin:.15rem}
input[type=checkbox]{accent-color:#39c2a7;width:1rem;height:1rem;vertical-align:-.15rem;padding:0}
:focus-visible{outline:2px solid #39c2a7;outline-offset:2px}
::-webkit-scrollbar{height:8px;width:8px}::-webkit-scrollbar-track{background:#0f1720}::-webkit-scrollbar-thumb{background:#223140;border-radius:4px}*{scrollbar-width:thin;scrollbar-color:#223140 #0f1720}
@media(max-width:640px){table.rules td:nth-child(6),table.rules th:nth-child(6),table.rules td:nth-child(7),table.rules th:nth-child(7){display:none}}
@media(max-width:640px){main{padding:.6rem}th,td{padding:.4rem .45rem;font-size:.82rem}.stat .v{font-size:1.15rem}.card .big{font-size:1.3rem}canvas#chart{height:170px}}
select,button,input{font:inherit;background:#223140;color:#e8eef4;border:1px solid #39c2a7;border-radius:8px;padding:.3rem .6rem;margin:.15rem}
input[type=checkbox]{accent-color:#39c2a7;width:1rem;height:1rem;vertical-align:-.15rem;padding:0}
:focus-visible{outline:2px solid #39c2a7;outline-offset:2px}
::-webkit-scrollbar{height:8px;width:8px}::-webkit-scrollbar-track{background:#0f1720}::-webkit-scrollbar-thumb{background:#223140;border-radius:4px}*{scrollbar-width:thin;scrollbar-color:#223140 #0f1720}
@media(max-width:640px){table.rules td:nth-child(6),table.rules th:nth-child(6),table.rules td:nth-child(7),table.rules th:nth-child(7){display:none}}
@media print{body{background:#fff;color:#000}main{max-width:100%}table{background:#fff}th{background:#eee;color:#000}td{border-top:1px solid #ccc}a{color:#000}.noprint{display:none}h1{color:#000}h2{color:#333}}
</style></head><body><main>
<p class="noprint"><a href="/nodes/{nid}">{back_node}</a> · <a href="/">{back}</a> · <button onclick="window.print()">{print}</button></p>
<h1>{title} — {nid} — {h_report}</h1>
<p class="mut">{gen}: {gentime} · {h_range}: {wfrom} → {wto}</p>
<h2>{h_stats}</h2>
  <p class="mut">{cal_note}</p>
<div class="tbl"><table><tr><th>{h_var}</th><th>{h_min}</th><th>{h_max}</th><th>{h_mean}</th><th>{h_n}</th><th>{h_cal_min}</th><th>{h_cal_max}</th><th>{h_cal_mean}</th></tr>
{statrows}
</table></div>
<h2>{h_qbreak}</h2>
<div class="tbl"><table><tr><th>{h_q}</th><th>{h_n}</th></tr>
{qrows}
</table></div>
<h2>{h_ev}</h2>
{evbody}
<p class="mut">{disclaimer}</p></main></body></html>"""

_HEAT_PAGE = """<!DOCTYPE html>
<html lang="{lang}"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
__REFRESH__
<title>{title} \u00b7 {h_heat}</title>
<style>
body{margin:0;background:#0f1720;color:#e8eef4;font:15px/1.45 system-ui,sans-serif}
main{max-width:1100px;margin:0 auto;padding:1rem}
h1{color:#39c2a7;letter-spacing:.12em;font-size:1.2rem}
h2{color:#8aa0b4;font-size:.95rem;margin:1.4rem 0 .5rem}
a{color:#7cc4ff;text-decoration:none}a:hover{text-decoration:underline}
.mut{color:#8aa0b4;font-size:.85rem}
.empty{background:#182430;border-radius:12px;padding:1rem 1.2rem}
.warn{background:#3a2410;border-radius:6px;padding:.6rem .9rem;
      box-shadow:inset 0 0 0 1px #6b4a24;color:#f0c99a;font-size:.85rem;margin:.6rem 0}
form{display:flex;gap:.8rem;flex-wrap:wrap;align-items:flex-end;background:#182430;
     border-radius:12px;padding:.8rem 1rem;margin:.8rem 0}
label{display:flex;flex-direction:column;gap:.2rem;font-size:.78rem;color:#8aa0b4}
input,select,button{background:#0f1720;color:#e8eef4;border:1px solid #2b3b4d;
     border-radius:6px;padding:.35rem .5rem;font:inherit}
button{background:#39c2a7;color:#06231d;border:0;cursor:pointer}
.tbl{overflow-x:auto;border-radius:12px;margin-bottom:.9rem}
table{width:100%;border-collapse:collapse;background:#182430}
th{background:#223140;text-align:left;padding:.55rem .8rem;font-size:.75rem;
   text-transform:uppercase;color:#8aa0b4}
td{padding:.55rem .8rem;border-top:1px solid #2b3b4d}
tr.hot td{background:#2a1512}
tr.hot td:first-child{box-shadow:inset 2px 0 0 #e2725b}
.pill{display:inline-block;padding:.05rem .45rem;border-radius:99px;font-size:.72rem;
      background:#39c2a7;color:#06231d}
</style></head><body><main>
<p><a href="/">{back}</a> \u00b7 <a href="/map">{map}</a></p>
<h1>{h_heat}</h1>
<p class="mut">{heat_fleet}</p>
<form method="get">
<label>{h_thr} (\u00b0C)<input name="threshold" value="{thr}"></label>
<label>{h_dur} (min)<input name="min_duration" value="{dur}"></label>
<label>{heat_window}<input name="hours" value="{hours}"></label>
<button>{apply}</button>
</form>
{summary}
<div class="warn">{heat_open_note}</div>
{body}
<p class="mut">{disclaimer}</p></main></body></html>"""


_MAP_PAGE = """<!DOCTYPE html>
<html lang="{lang}"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{title} — {h_map}</title>
<style>
body{margin:0;background:#0f1720;color:#e8eef4;font:15px/1.45 system-ui,sans-serif}
main{max-width:900px;margin:0 auto;padding:1rem}
h1{color:#39c2a7;letter-spacing:.12em;font-size:1.2rem}
a{color:#7cc4ff;text-decoration:none}a:hover{text-decoration:underline}
.mut{color:#8aa0b4;font-size:.85rem}
.empty{background:#182430;border-radius:12px;padding:1rem 1.2rem}
svg{width:100%;height:auto;background:#182430;border-radius:12px}
</style></head><body><main>
<p><a href="/heat">{events}</a> · <a href="/">{back}</a></p>
<h1>{title} — {h_map}</h1>
{body}
<p class="mut">{disclaimer}</p></main></body></html>"""


# The colour scale is fixed in degrees Celsius, and the map legend is drawn from this
# same range. A per-request scale would make two screenshots of the same hour impossible
# to compare, and would make a 2 C spread look as alarming as a 20 C one.
_TEMP_SCALE_LO = 0.0
_TEMP_SCALE_HI = 40.0


def _temp_color(value: float | None) -> str:
    if value is None:
        return "#8aa0b4"
    t = max(_TEMP_SCALE_LO, min(_TEMP_SCALE_HI, value)) / _TEMP_SCALE_HI
    r = int(124 + (224 - 124) * t)
    g = int(196 + (82 - 196) * t)
    b = int(255 + (82 - 255) * t)
    return f"#{r:02x}{g:02x}{b:02x}"

_EV_ROW = ("<tr><td>{start}</td><td>{end}</td>"
           "<td>{dur} min</td><td>{peak}</td></tr>")

_EVENTS_PAGE = """<!DOCTYPE html>
<html lang="{lang}"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{title} — {nid} — {h_ev}</title>
<style>
body{margin:0;background:#0f1720;color:#e8eef4;font:15px/1.45 system-ui,sans-serif}
main{max-width:900px;margin:0 auto;padding:1rem}
h1{color:#39c2a7;letter-spacing:.12em;font-size:1.2rem}
h2{color:#8aa0b4;font-size:.95rem;margin:1.4rem 0 .5rem}
table{width:100%;border-collapse:collapse;background:#182430;border-radius:12px;overflow:hidden}
.tbl{overflow-x:auto;border-radius:12px;margin-bottom:.9rem}
.tbl table{white-space:nowrap}
th{background:#223140;text-align:left;padding:.55rem .8rem;font-size:.75rem;text-transform:uppercase;color:#8aa0b4}
td{padding:.55rem .8rem;border-top:1px solid #223140}
a{color:#7cc4ff;text-decoration:none}a:hover{text-decoration:underline}
@media(max-width:640px){main{padding:.6rem}th,td{padding:.4rem .45rem;font-size:.82rem}.stat .v{font-size:1.15rem}.card .big{font-size:1.3rem}canvas#chart{height:170px}}
select,button,input{font:inherit;background:#223140;color:#e8eef4;border:1px solid #39c2a7;border-radius:8px;padding:.3rem .6rem;margin:.15rem}
input[type=checkbox]{accent-color:#39c2a7;width:1rem;height:1rem;vertical-align:-.15rem;padding:0}
:focus-visible{outline:2px solid #39c2a7;outline-offset:2px}
::-webkit-scrollbar{height:8px;width:8px}::-webkit-scrollbar-track{background:#0f1720}::-webkit-scrollbar-thumb{background:#223140;border-radius:4px}*{scrollbar-width:thin;scrollbar-color:#223140 #0f1720}
@media(max-width:640px){table.rules td:nth-child(6),table.rules th:nth-child(6),table.rules td:nth-child(7),table.rules th:nth-child(7){display:none}}
.mut{color:#8aa0b4;font-size:.85rem}
input,select,button{font:inherit;background:#223140;color:#e8eef4;border:1px solid #39c2a7;border-radius:8px;padding:.3rem .6rem;margin:.15rem}
.empty{background:#182430;border-radius:12px;padding:1rem 1.2rem}
</style></head><body><main>
<p><a href="/nodes/{nid}">{back_node}</a> · <a href="/heat">{events}</a> · <a href="/">{back}</a></p>
<h1>{title} — {nid} — {h_ev}</h1>
<form method="get" action="#results" class="mut">{h_var}: <select name="variable">{opts_v}</select>
{h_thr}: <input type="number" name="threshold" step="0.5" value="{thr}" style="width:5rem">
{h_dur}: <input type="number" name="min_duration_min" step="5" value="{dur}" style="width:5rem">
<button type="submit">{apply}</button></form>
<div id="results">{body}</div>
<p class="mut">{disclaimer}</p></main></body></html>"""

_COMPARE_PAGE = """<!DOCTYPE html>
<html lang="{lang}"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{title} — {h_compare}</title>
<style>
body{margin:0;background:#0f1720;color:#e8eef4;font:15px/1.45 system-ui,sans-serif}
main{max-width:900px;margin:0 auto;padding:1rem}
h1{color:#39c2a7;letter-spacing:.12em;font-size:1.2rem}
h2{color:#8aa0b4;font-size:.95rem;margin:1.4rem 0 .5rem}
table{width:100%;border-collapse:collapse;background:#182430;border-radius:12px;overflow:hidden}
.tbl{overflow-x:auto;border-radius:12px;margin-bottom:.9rem}
.tbl table{white-space:nowrap}
th{background:#223140;text-align:left;padding:.55rem .8rem;font-size:.75rem;text-transform:uppercase;color:#8aa0b4}
td{padding:.55rem .8rem;border-top:1px solid #223140}
a{color:#7cc4ff;text-decoration:none}a:hover{text-decoration:underline}
@media(max-width:640px){main{padding:.6rem}th,td{padding:.4rem .45rem;font-size:.82rem}.stat .v{font-size:1.15rem}.card .big{font-size:1.3rem}canvas#chart{height:170px}}
select,button,input{font:inherit;background:#223140;color:#e8eef4;border:1px solid #39c2a7;border-radius:8px;padding:.3rem .6rem;margin:.15rem}
input[type=checkbox]{accent-color:#39c2a7;width:1rem;height:1rem;vertical-align:-.15rem;padding:0}
:focus-visible{outline:2px solid #39c2a7;outline-offset:2px}
::-webkit-scrollbar{height:8px;width:8px}::-webkit-scrollbar-track{background:#0f1720}::-webkit-scrollbar-thumb{background:#223140;border-radius:4px}*{scrollbar-width:thin;scrollbar-color:#223140 #0f1720}
@media(max-width:640px){table.rules td:nth-child(6),table.rules th:nth-child(6),table.rules td:nth-child(7),table.rules th:nth-child(7){display:none}}
.mut{color:#8aa0b4;font-size:.85rem}
canvas#chart{width:100%;height:220px;background:#182430;border-radius:12px}
select,button{font:inherit;background:#223140;color:#e8eef4;border:1px solid #39c2a7;border-radius:8px;padding:.3rem .6rem;margin:.15rem}
</style></head><body><main>
<p><a href="/">{back}</a></p>
<h1>{title} — {h_compare}</h1>
<form method="get" action="#chart" class="mut">{h_a}: <select name="a">{opts_a}</select>
{h_b}: <select name="b">{opts_b}</select>
{h_c}: <select name="c"><option value="">—</option>{opts_c}</select>
{h_var}: <select name="variable">{opts_v}</select>
{h_days}: <select name="days"><option value="1"{sel1}>24h</option><option value="7"{sel7}>7d</option><option value="30"{sel30}>30d</option></select>
<button type="submit">{apply}</button></form>
{results}
<p class="mut">{disclaimer}</p></main></body></html>"""


def _human_var(var: str | None) -> str:
    return (var or "").replace("_", " ") or "—"


def _fmt_utc(ms):
    if not ms:
        return "—"
    return datetime.fromtimestamp(ms / 1000, tz=UTC).strftime("%Y-%m-%d %H:%M")


@router.get("/", response_class=HTMLResponse)
def dashboard(request: Request):
    check_rate(request)
    code, labels = _pick(request)
    info = query(
        """SELECT n.node_id, n.site_id, n.last_seen_utc_ms AS seen,
                  (SELECT COUNT(*) FROM measurements m
                   WHERE m.node_id=n.node_id) AS cnt,
                  (SELECT MAX(timestamp_utc_ms) FROM measurements m
                   WHERE m.node_id=n.node_id) AS ts
           FROM nodes n ORDER BY n.node_id""")
    total = sum(r["cnt"] or 0 for r in info)
    last_sync = max((r["seen"] or 0 for r in info), default=0)
    stats = [
        _STAT.format(label=labels["nodes"], value=len(info)),
        _STAT.format(label=labels["records"], value=f"{total}"),
        _STAT.format(label=labels["last_sync"], value=_fmt_utc(last_sync)),
    ]
    cards = []
    for r in info:
        nid = r["node_id"]
        latest = query(
            """SELECT m.variable, m.value, m.unit, m.quality
               FROM measurements m JOIN
                 (SELECT variable AS v, MAX(sequence) AS s FROM measurements
                  WHERE node_id=? GROUP BY variable) t
                 ON m.variable=t.v AND m.sequence=t.s AND m.node_id=?
               ORDER BY m.variable""", (nid, nid))
        by_var = {row["variable"]: dict(row) for row in latest}
        temp = by_var.get("air_temperature", {})
        hum = by_var.get("relative_humidity", {})
        tval = "—" if temp.get("value") is None else f"{temp['value']:.1f}"
        hval = "—" if hum.get("value") is None else f"{hum['value']:.1f}"
        quals = sorted({row["quality"] for row in latest if row["quality"]})
        qline = (" · ".join(
            f"<span class=\"q-{html.escape(q)}\">{html.escape(q)}</span>"
            for q in quals)
                 or labels["none"])
        pts = query(
            "SELECT timestamp_utc_ms, value FROM measurements"
            " WHERE node_id=? AND variable='air_temperature'"
            " AND value IS NOT NULL ORDER BY sequence DESC LIMIT 120",
            (nid,))
        spark = _json_for_script([[p["timestamp_utc_ms"], p["value"]]
                            for p in reversed(pts)])
        hot = query(
            "SELECT MAX(value) AS m FROM measurements WHERE node_id=?"
            " AND variable='air_temperature' AND value IS NOT NULL"
            " AND timestamp_utc_ms>=?",
            (nid, (r["ts"] or 0) - 7 * 86400000))
        hotv = hot[0]["m"] if hot and hot[0]["m"] is not None else None
        cards.append(_NODE_CARD.format(
            nid=html.escape(nid),
            site=html.escape(r["site_id"] or labels["none"]),
            last=_fmt_utc(r["ts"]), temp=tval, hum=hval, qline=qline,
            spark=spark, hottest=labels["hottest"],
            hot=("—" if hotv is None else f"{hotv:.1f}"),
            events=labels["events"]))
    switcher = "".join(
        f"<strong aria-current=\"true\">{label}</strong>" if lang_code == code
        else f"<a href=\"/?lang={lang_code}\">{label}</a>"
        for lang_code, label in (("es", "ES"), ("en", "EN"), ("pt", "PT")))
    page = (_PAGE.replace("{stats}", "\n".join(stats))
            .replace("{lang}", code).replace("{title}", labels["title"])
            .replace("{subtitle}", labels["subtitle"])
            .replace("{langswitch}", switcher)
            .replace("{compare}", labels["compare"])
            .replace("{map}", labels["map"])
            .replace("{coloc}", labels["coloc"])
            .replace("{alerts}", labels["alerts"])
            .replace("{api}", labels["api"]).replace("{export}", labels["export"])
            .replace("{h_nodes}", labels["nodes"])
            .replace("{cards}", "\n".join(cards))
            .replace("{disclaimer}", labels["disclaimer"]))
    return HTMLResponse(_with_legal(page, labels, code))


def _parse_local(value: str | None) -> int | None:
    if not value:
        return None
    try:
        dt = datetime.strptime(value, "%Y-%m-%dT%H:%M").replace(tzinfo=UTC)
        return int(dt.timestamp() * 1000)
    except (ValueError, TypeError, OverflowError):
        return None


def _fmt_local(ms) -> str:
    if not ms:
        return ""
    return datetime.fromtimestamp(ms / 1000, tz=UTC).strftime("%Y-%m-%dT%H:%M")


@router.get("/nodes/{node_id}", response_class=HTMLResponse)
def node_page(node_id: str, request: Request, days: int = 1,
              from_s: str | None = None, to_s: str | None = None):
    check_rate(request)
    code, labels = _pick(request)
    requested_vars = request.query_params.getlist("var")
    nodes = query(
        "SELECT node_id, site_id, firmware_version, first_seen_utc_ms,"
        " last_seen_utc_ms FROM nodes WHERE node_id=?", (node_id,))
    if not nodes:
        raise HTTPException(status_code=404, detail="node_not_found")
    node = nodes[0]
    safe_id = html.escape(node["node_id"])
    if days not in (1, 7, 30):
        days = 1
    count = query("SELECT COUNT(*) AS c FROM measurements WHERE node_id=?",
                  (node_id,))[0]["c"]
    latest = query(
        """SELECT m.variable, m.value, m.unit, m.quality, m.timestamp_utc_ms
           FROM measurements m JOIN
             (SELECT variable AS v, MAX(sequence) AS s FROM measurements
              WHERE node_id=? GROUP BY variable) t
             ON m.variable=t.v AND m.sequence=t.s AND m.node_id=?
           ORDER BY m.variable""", (node_id, node_id))
    cards = []
    for r in latest:
        val = labels["none"] if r["value"] is None else f"{r['value']:.1f}"
        cards.append(_CARD.format(
            var=html.escape(_human_var(r["variable"])), val=val,
            unit=html.escape(r["unit"] or ""), q=html.escape(r["quality"]),
            ts=_fmt_utc(r["timestamp_utc_ms"])))
    if not cards:
        cards.append(f"<p class=\"mut\">{labels['no_data']}</p>")
    recent = query(
        "SELECT timestamp_utc_ms, variable, value, unit, quality"
        " FROM measurements WHERE node_id=? ORDER BY sequence DESC LIMIT 20",
        (node_id,))
    rows = []
    for r in recent:
        val = labels["none"] if r["value"] is None else f"{r['value']:.1f}"
        rows.append(_RECENT_ROW.format(
            ts=_fmt_utc(r["timestamp_utc_ms"]),
            var=html.escape(_human_var(r["variable"])), val=val,
            unit=html.escape(r["unit"] or ""), q=html.escape(r["quality"])))
    all_vars = [r["variable"] for r in query(
        "SELECT DISTINCT variable FROM measurements WHERE node_id=?"
        " ORDER BY variable", (node_id,))]
    if requested_vars:
        selected = [v for v in requested_vars if v in all_vars] or all_vars
    else:
        preferred = [v for v in ("air_temperature", "relative_humidity")
                     if v in all_vars]
        selected = preferred or all_vars
    max_ts = query("SELECT MAX(timestamp_utc_ms) AS m FROM measurements"
                   " WHERE node_id=?", (node_id,))[0]["m"] or 0
    flo, fhi = _parse_local(from_s), _parse_local(to_s)
    if flo is not None and fhi is not None and flo < fhi:
        since, until = flo, fhi
    else:
        since, until = max_ts - days * 86400000, max_ts
    colors = {"air_temperature": "#39c2a7", "relative_humidity": "#7cc4ff",
              "pressure": "#e6b455"}
    series = []
    statrows = []
    for v in selected:
        pts = query(
            "SELECT timestamp_utc_ms, value FROM measurements"
            " WHERE node_id=? AND variable=? AND value IS NOT NULL"
            " AND timestamp_utc_ms>=? AND timestamp_utc_ms<=?"
            " ORDER BY sequence", (node_id, v, since, until))
        step = max(1, len(pts) // 400)
        thin = pts[::step]
        series.append({"label": _human_var(v),
                       "color": colors.get(v, "#b0b0b0"),
                       "pts": [[p["timestamp_utc_ms"], p["value"]]
                               for p in thin]})
        st = summary_stats([p["value"] for p in thin])
        statrows.append(
            f"<tr><td>{html.escape(_human_var(v))}</td>"
            f"<td>{st['min']}</td><td>{st['max']}</td>"
            f"<td>{st['mean']}</td><td>{st['count']}</td></tr>")
    qbreak = query(
        "SELECT quality, COUNT(*) AS c FROM measurements WHERE node_id=?"
        " AND timestamp_utc_ms>=? AND timestamp_utc_ms<=?"
        " GROUP BY quality ORDER BY c DESC",
        (node_id, since, until))
    qrows = "".join(
        f"<tr><td class=\"q-{html.escape(r['quality'])}\">"
        f"{html.escape(r['quality'])}</td>"
        f"<td>{r['c']}</td></tr>" for r in qbreak)
    varboxes = " ".join(
        f"<label><input type=\"checkbox\" name=\"var\" value=\"{html.escape(v)}\""
        f"{' checked' if v in selected else ''}> {html.escape(_human_var(v))}</label>"
        for v in all_vars)
    page = _NODE_PAGE
    page = (page.replace("{series_json}", _json_for_script(series))
            .replace("{nid}", safe_id)
            .replace("{lang}", code).replace("{title}", labels["title"])
            .replace("{back}", labels["back"]).replace("{h_info}", labels["info"])
            .replace("{h_site}", labels["site"]).replace("{h_first}", labels["first_seen"])
            .replace("{h_last}", labels["last"]).replace("{h_total}", labels["records"])
            .replace("{site}", html.escape(node["site_id"] or labels["none"]))
            .replace("{first}", _fmt_utc(node["first_seen_utc_ms"]))
            .replace("{last}", _fmt_utc(node["last_seen_utc_ms"]))
            .replace("{count}", _form_value(count))
            .replace("{h_latest}", labels["latest"])
            .replace("{cards}", "\n".join(cards))
            .replace("{h_chart}", labels["chart"])
            .replace("{h_range}", labels["range"])
            .replace("{sel1}", " selected" if days == 1 else "")
            .replace("{sel7}", " selected" if days == 7 else "")
            .replace("{sel30}", " selected" if days == 30 else "")
            .replace("{h_vars}", labels["variables"])
            .replace("{varboxes}", varboxes)
            .replace("{apply}", labels["apply"])
            .replace("{h_custom}", labels["custom"])
            .replace("{h_from}", labels["from"])
            .replace("{fromv}", _fmt_local(since))
            .replace("{h_to}", labels["to"])
            .replace("{tov}", _fmt_local(until))
            .replace("{h_stats}", labels["stats"])
            .replace("{h_var}", labels["variable"])
            .replace("{h_val}", labels["value"])
            .replace("{h_min}", labels["stat_min"])
            .replace("{h_max}", labels["stat_max"])
            .replace("{h_mean}", labels["stat_mean"])
            .replace("{h_n}", labels["stat_n"])
            .replace("{statrows}", "\n".join(statrows))
            .replace("{h_qbreak}", labels["qbreak"])
            .replace("{h_q}", labels["quality"])
            .replace("{qrows}", qrows or f"<tr><td colspan=\"2\">{labels['no_data']}</td></tr>")
            .replace("{h_recent}", labels["recent"])
            .replace("{h_time}", labels["time"])
            .replace("{rows}", "\n".join(rows))
            .replace("{view_json}", labels["view_json"])
            .replace("{export_node}", labels["export_node"])
            .replace("{events}", labels["events"])
            .replace("{report}", labels["report"])
            .replace("{disclaimer}", labels["disclaimer"]))
    return HTMLResponse(_with_legal(page, labels, code))


def _heat_rows(node_id: str, variable: str,
               from_utc_ms: int | None = None,
               to_utc_ms: int | None = None) -> list[dict]:
    """Samples for heat detection, calibrated, in the same shape `detect_heat_events` wants.

    The quality filter matches the API endpoint exactly. It is not a trust signal - a SUSPECT
    or UNCALIBRATED reading is still a real measurement and hiding it would make the heat
    count disagree with the chart above it - but a NULL value is not a measurement at all,
    and comparing None against a threshold raises.
    """
    sql = ("SELECT timestamp_utc_ms, value FROM measurements"
           " WHERE node_id=? AND variable=? AND value IS NOT NULL"
           " AND quality IN ('VALID','CALIBRATED','SUSPECT','UNCALIBRATED')")
    params: list = [node_id, variable]
    if from_utc_ms is not None:
        sql += " AND timestamp_utc_ms>=?"
        params.append(from_utc_ms)
    if to_utc_ms is not None:
        sql += " AND timestamp_utc_ms<=?"
        params.append(to_utc_ms)
    sql += " ORDER BY timestamp_utc_ms"
    rows = query(sql, tuple(params))
    calibration = calibration_for(node_id, variable)
    if calibration and not is_identity(calibration["scale"],
                                      calibration["offset"]):
        rows = [{"timestamp_utc_ms": r["timestamp_utc_ms"],
                 "value": apply_value(r["value"], calibration)} for r in rows]
    return rows


@router.get("/nodes/{node_id}/events", response_class=HTMLResponse)
def node_events_page(node_id: str, request: Request,
                     variable: str = "air_temperature",
                     threshold: float = 32.0,
                     min_duration_min: int = 60):
    # This page used to forward the browser's `Authorization` header into
    # `api.analytics_heat_events`, which requires a read scope. A browser cannot set that
    # header from an address bar, so the one page whose entire job is to show heat events was
    # the only dashboard page that answered 401 - and the node page linked straight into it.
    # The credential bought nothing: every reading this page shows is already rendered
    # unauthenticated by `/`, `/map`, `/colocation` and `/nodes/{id}`.
    check_rate(request)
    code, labels = _pick(request)
    nodes = query("SELECT node_id FROM nodes WHERE node_id=?", (node_id,))
    if not nodes:
        raise HTTPException(status_code=404, detail="node_not_found")
    variables = [r["variable"] for r in query(
        "SELECT DISTINCT variable FROM measurements WHERE node_id=?"
        " ORDER BY variable", (node_id,))]
    if variable not in variables:
        variable = "air_temperature" if "air_temperature" in variables else (
            variables[0] if variables else "air_temperature")
    events = detect_heat_events(
        _heat_rows(node_id, variable), threshold, min_duration_min)
    peak_seen = query(
        "SELECT MAX(value) AS m FROM measurements WHERE node_id=?"
        " AND variable=? AND value IS NOT NULL", (node_id, variable))[0]["m"]
    if events:
        rows = "".join(
            _EV_ROW.format(start=_fmt_utc(e["start_utc_ms"]),
                           end=_fmt_utc(e["end_utc_ms"]),
                           dur=e["duration_min"], peak=e["peak_value"])
            for e in events)
        body = (
            "<div class=\"tbl\">"
            f"<table><tr><th>{labels['start']}</th><th>{labels['end']}</th>"
            f"<th>{labels['duration']}</th><th>{labels['peak']} (°C)</th></tr>"
            + rows + "</table></div>")
    else:
        peak_txt = "—" if peak_seen is None else f"{peak_seen:.1f} °C"
        # `_form_value` already escapes. Escaping its result a second time turns a value
        # the user typed into the literal text they see: type `1 &amp; 2` and the page
        # would say `1 &amp;amp; 2`. Escaping is not idempotent, so exactly one layer
        # belongs here and it is the one inside the helper.
        safe_threshold_txt = _form_value(threshold)
        body = (
            f"<div class=\"empty\"><p>{labels['no_events']}</p>"
            f"<p class=\"mut\">{labels['max_seen']}: {peak_txt} · "
            f"{labels['threshold']}: {safe_threshold_txt} °C</p>"
            f"<p class=\"mut\">{labels['no_events_hint']}</p></div>")
    opts_v = "".join(
        f"<option value=\"{html.escape(v)}\""
        f"{' selected' if v == variable else ''}>{html.escape(_human_var(v))}</option>"
        for v in variables)
    page = (_EVENTS_PAGE.replace("{lang}", code)
            .replace("{title}", labels["title"])
            .replace("{nid}", html.escape(node_id))
            .replace("{h_ev}", labels["ev_title"])
            .replace("{events}", labels["events"])
            .replace("{back_node}", labels["node"])
            .replace("{back}", labels["back"])
            .replace("{h_var}", labels["variable"])
            .replace("{opts_v}", opts_v)
            .replace("{h_thr}", labels["threshold"])
            .replace("{thr}", _form_value(threshold))
            .replace("{h_dur}", labels["min_dur"])
            .replace("{dur}", _form_value(min_duration_min))
            .replace("{apply}", labels["apply"])
            .replace("{body}", body)
            .replace("{disclaimer}", labels["disclaimer"]))
    return HTMLResponse(_with_legal(page, labels, code))


_CSV_HEADER = ("node_id,sensor_id,sequence,timestamp_utc_ms,timestamp_iso,"
               "variable,value,unit,quality,reason_bits,time_uncertain,"
               "calibrated_value,calibration_scale,calibration_offset,"
               "calibration_uncertainty\n")

_CSV_SELECT = """
    SELECT node_id, COALESCE(sensor_id,''), sequence, timestamp_utc_ms,
           strftime('%Y-%m-%dT%H:%M:%SZ', timestamp_utc_ms/1000, 'unixepoch'),
           variable, value, COALESCE(unit,''), quality, reason_bits,
           time_uncertain
    FROM measurements
    """

_CSV_ROW = ("{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12},{13},{14}\n")
_CSV_CHUNK_LINES = 500


def _csv_cell(value) -> str:
    """One CSV field, safe in a spreadsheet as well as safe in the grammar.

    Two separate problems, and quoting only solves the first.

    The grammar problem: a value containing a comma, a quote or a newline has to be quoted
    and its quotes doubled, per RFC 4180. That is the part `str` formatting gets wrong by
    default and the only part most CSV code bothers with.

    The spreadsheet problem: Excel and LibreOffice treat a cell whose text begins with `=`,
    `+`, `-` or `@` as a formula, and they do so *inside* the quotes. A perfectly valid,
    correctly quoted `=cmd|'/C calc'!A0` is executed by whoever opens the file. Confirmed
    against HEAD: a node could sync under that id, it was accepted with a 200, and it came
    out of this export in the first column unquoted.

    Fixed with a leading apostrophe, which a spreadsheet reads as "this is text" and which
    disappears once the cell is displayed. It prefixes the value rather than refusing it,
    because a node with an `=` in its id should still report its data - dropping the row
    would make an export quietly incomplete, which is worse than a slightly odd character.

    `node_id` is now shape-checked on the way in, so this is defence in depth rather than the
    only guard: an operator pasting a row, or a future importer, would otherwise put a formula
    back.
    """
    if value is None:
        return ""
    text = str(value)
    if text == "":
        return ""
    # A number is not a formula, and a negative one is not a formula either. Without this the
    # `calibration_offset` column came out as `'-1.5` for every site with a negative
    # correction, which a test caught immediately - a good argument for testing the CSV
    # writer's output rather than only the endpoint's status code.
    #
    # The test is numeric rather than "does not start with a digit", so `-1.5e3` and `-0` are
    # still numbers, and `+1.5` too: a leading `+` is numeric syntax even though the formula
    # prefix list includes it.
    stripped = text.strip()
    try:
        float(stripped)
        return text if not any(c in text for c in (",", '"', "\n", "\r")) else (
            '"' + text.replace('"', '""') + '"')
    except ValueError:
        pass
    # Leading whitespace defeats a naive first-character check: the spreadsheet trims it and
    # evaluates what follows.
    probe = text.lstrip(" \t")
    if probe[:1] in ("=", "+", "-", "@"):
        text = "'" + text
    if any(c in text for c in (",", '"', "\n", "\r")):
        return '"' + text.replace('"', '""') + '"'
    return text


def _csv_chunks(sql: str, params: tuple = (), size: int = 2000):
    """Formats rows in SQL and streams them in blocks, so a 60-day export
    does not spend its time in Python datetime formatting nor in one HTTP
    chunk per row. Calibration is resolved once for the whole fleet."""
    from .calibration import node_calibration_map

    calibrations = node_calibration_map()
    yield _CSV_HEADER
    cursor = engine().execute(sql, params)
    block: list[str] = []
    try:
        while True:
            rows = cursor.fetchmany(size)
            if not rows:
                break
            for r in rows:
                cal = calibrations.get(r[0], {}).get(r[5])
                value = "" if r[6] is None else f"{r[6]}"
                calibrated = apply_value(r[6], cal)
                # Empty means nobody characterised the uncertainty, which is a
                # different fact from an uncertainty of zero.
                uncertainty = calibrated_uncertainty(cal)
                block.append(_CSV_ROW.format(
                    # Every field through the escaper, not just the identifier. `unit` and
                    # `sensor_id` come from the node too, and the fix is only a fix if it is
                    # applied to the row rather than to the one column that happened to be
                    # demonstrated.
                    *(_csv_cell(x) for x in (
                        r[0], r[1], r[2], r[3], r[4], r[5], value, r[7], r[8],
                        r[9], r[10],
                        "" if calibrated is None else f"{calibrated}",
                        "" if not cal else f"{cal['scale']}",
                        "" if not cal else f"{cal['offset']}",
                        "" if uncertainty is None else f"{uncertainty}",
                    ))))
                if len(block) >= _CSV_CHUNK_LINES:
                    yield "".join(block)
                    block = []
        if block:
            yield "".join(block)
    finally:
        cursor.close()


@router.get("/v1/export-all.csv")
def export_all_csv(
    request: Request,
    authorization: str | None = Header(default=None),
) -> StreamingResponse:
    check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    return StreamingResponse(
        _csv_chunks(_CSV_SELECT + " ORDER BY node_id, sequence"),
        media_type="text/csv",
        headers={"Content-Disposition": "attachment; filename=cauce-all.csv"})


@router.get("/v1/nodes/{node_id}/export.csv")
def export_node_csv(
    node_id: str,
    request: Request,
    authorization: str | None = Header(default=None),
) -> StreamingResponse:
    check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    safe_node_id = "".join(
        c for c in node_id if c.isalnum() or c in ("-", "_")
    ) or "node"
    return StreamingResponse(
        _csv_chunks(_CSV_SELECT + " WHERE node_id=? ORDER BY sequence",
                    (node_id,)),
        media_type="text/csv",
        headers={"Content-Disposition":
                 f"attachment; filename={safe_node_id}.csv"})


@router.get("/v1/nodes/{node_id}/coverage.csv")
def export_node_coverage_csv(
    node_id: str,
    request: Request,
    variable: str = "air_temperature",
    from_utc_ms: int | None = None,
    to_utc_ms: int | None = None,
    expected_interval_ms: int | None = None,
    authorization: str | None = Header(default=None),
) -> StreamingResponse:
    check_rate(request)
    require_bearer_token(authorization, settings.api_token)
    from .coverage import _interval, _window, node_coverage

    if not query("SELECT 1 FROM nodes WHERE node_id=?", (node_id,)):
        raise HTTPException(status_code=404, detail="node_not_found")
    interval = _interval(expected_interval_ms)
    from_ms, to_ms = _window(from_utc_ms, to_utc_ms, interval)
    cov = node_coverage(node_id, variable, from_ms, to_ms, interval)
    safe_node_id = "".join(
        c for c in node_id if c.isalnum() or c in ("-", "_")
    ) or "node"

    def gen():
        yield ("node_id,variable,from_utc_ms,to_utc_ms,expected_interval_ms,"
               "expected_samples,received_samples,usable_samples,coverage_pct,"
               "longest_gap_ms,uncertain_samples,reconstructed_samples\n")
        yield (f"{node_id},{variable},{from_ms},{to_ms},"
               f"{cov['expected_interval_ms']},{cov['expected_samples']},"
               f"{cov['received_samples']},{cov['usable_samples']},"
               f"{cov['coverage_pct']},{cov['longest_gap_ms']},"
               f"{cov['uncertain_samples']},{cov['reconstructed_samples']}\n")
        yield ("\nstart_utc_ms,end_utc_ms,duration_ms,missing_samples,reason\n")
        for gap in cov["gaps"]:
            yield (f"{gap['start_utc_ms']},{gap['end_utc_ms']},"
                   f"{gap['duration_ms']},{gap['missing_samples']},"
                   f"{gap['reason']}\n")

    return StreamingResponse(
        gen(), media_type="text/csv",
        headers={"Content-Disposition":
                 f"attachment; filename={safe_node_id}-coverage.csv"})


@router.get("/alerts", response_class=HTMLResponse)
def alerts_page(request: Request):
    check_rate(request)
    code, labels = _pick(request)
    rules = query("SELECT * FROM alert_rules ORDER BY rule_id")
    rulerows = []
    for r in rules:
        detail = (f"{labels['threshold']}: {r['threshold']} °C"
                  if r["kind"] == "heat"
                  else f"{labels['stale_after']}: {r['stale_min']} min")
        last = _fmt_utc(r["last_fired_utc_ms"]) if r["last_fired_utc_ms"] else "—"
        rulerows.append(
            f"<tr><td>{r['rule_id']}</td><td>{html.escape(r['node_id'])}</td>"
            f"<td>{html.escape(r['kind'])}</td><td>{detail}</td>"
            f"<td>{html.escape(r['channel'])}</td>"
            f"<td>{html.escape((r['target'] or '')[:40])}</td>"
            f"<td>{r['cooldown_min']} min</td><td>{last}</td>"
            f"<td><button onclick=\"delRule({r['rule_id']})\">"
            f"{labels['delete']}</button></td></tr>")
    if not rulerows:
        rulerows.append(f"<tr><td colspan=\"9\">{labels['no_data']}</td></tr>")
    logs = query("SELECT * FROM alert_log ORDER BY log_id DESC LIMIT 20")
    logrows = "".join(
        f"<tr><td>{_fmt_utc(r['fired_utc_ms'])}</td>"
        f"<td>{html.escape(r['node_id'])}</td>"
        f"<td>{html.escape(r['message'])}</td>"
        f"<td class=\"{'ok' if r['delivered'] else 'fail'}\">"
        f"{labels['delivered'] if r['delivered'] else '✗'}</td></tr>"
        for r in logs)
    if not logrows:
        logrows = f"<tr><td colspan=\"4\">{labels['no_data']}</td></tr>"
    page = (_ALERTS_PAGE.replace("{lang}", code)
            .replace("{title}", labels["title"])
            .replace("{h_alerts}", labels["alerts"])
            .replace("{back}", labels["back"])
            .replace("{h_rules}", labels["rules"])
            .replace("{h_node}", labels["node"])
            .replace("{h_kind}", labels["kind_heat"] + "/" + labels["kind_stale"])
            .replace("{kind_heat}", labels["kind_heat"])
            .replace("{kind_stale}", labels["kind_stale"])
            .replace("{h_thr}", labels["threshold"])
            .replace("{h_chan}", labels["channel"])
            .replace("{h_target}", labels["target"])
            .replace("{h_cool}", labels["cooldown"])
            .replace("{h_last}", labels["last"])
            .replace("{rulerows}", "\n".join(rulerows))
            .replace("{h_create}", labels["create"])
            .replace("{h_stale}", labels["stale_after"])
            .replace("{create}", labels["create"])
            .replace("{check_hint}", labels["check_hint"])
            .replace("{h_check}", labels["check_now"])
            .replace("{h_log}", labels["log"])
            .replace("{h_when}", labels["when"])
            .replace("{h_msg}", labels["message"])
            .replace("{h_sent}", labels["delivered"])
            .replace("{logrows}", logrows)
            .replace("{disclaimer}", labels["disclaimer"]))
    return HTMLResponse(_with_legal(page, labels, code))


_COLOC_COLORS = ["#39c2a7", "#7cc4ff", "#e6b455", "#e26d5a",
                 "#b48be0", "#7de08a", "#e08ab4", "#8ad6e0"]


@router.get("/colocation", response_class=HTMLResponse)
def colocation_page(request: Request, variable: str = "air_temperature",
                    days: int = 7):
    check_rate(request)
    code, labels = _pick(request)
    variables = [r["variable"] for r in query(
        "SELECT DISTINCT variable FROM measurements ORDER BY variable")]
    if days not in (1, 7, 30):
        days = 7
    if variable not in variables:
        variable = "air_temperature" if "air_temperature" in variables else (
            variables[0] if variables else "air_temperature")
    max_ts = query("SELECT MAX(timestamp_utc_ms) AS m FROM measurements")[0]["m"] or 0
    since = max_ts - days * 86400000
    nodes = [r["node_id"] for r in query(
        "SELECT DISTINCT node_id FROM measurements"
        " WHERE variable=? AND timestamp_utc_ms>=? ORDER BY node_id",
        (variable, since))]
    series = []
    divrows = []
    all_vals: list[float] = []
    per_node: list[tuple] = []
    calibrations = {
        r["node_id"]: site_calibrations(r["site_id"]).get(variable)
        for r in query("SELECT node_id, site_id FROM nodes")
    }
    calibrated_any = False
    for i, nid in enumerate(nodes[:8]):
        pts = query(
            "SELECT timestamp_utc_ms, value FROM measurements"
            " WHERE node_id=? AND variable=? AND value IS NOT NULL"
            " AND timestamp_utc_ms>=? ORDER BY sequence", (nid, variable, since))
        cal = calibrations.get(nid)
        if cal and not is_identity(cal["scale"], cal["offset"]):
            calibrated_any = True
            pts = [{"timestamp_utc_ms": p["timestamp_utc_ms"],
                    "value": apply_value(p["value"], cal)} for p in pts]
        step = max(1, len(pts) // 400)
        thin = pts[::step]
        vals = [p["value"] for p in thin]
        all_vals.extend(vals)
        per_node.append((nid, vals))
        series.append({"label": nid,
                       "color": _COLOC_COLORS[i % len(_COLOC_COLORS)],
                       "pts": [[p["timestamp_utc_ms"], p["value"]] for p in thin]})
    gmean = round(sum(all_vals) / len(all_vals), 3) if all_vals else None
    for nid, vals in per_node:
        st = summary_stats(vals)
        bias = (round(st["mean"] - gmean, 3)
                if st["mean"] is not None and gmean is not None else "—")
        maxdev = ("—" if gmean is None or not vals else
                  round(max(abs(st["min"] - gmean), abs(st["max"] - gmean)), 3))
        cal = calibrations.get(nid)
        flag = "" if not cal or is_identity(cal["scale"], cal["offset"]) else " *"
        divrows.append(
            f"<tr><td><a href=\"/nodes/{html.escape(nid)}\">{html.escape(nid)}</a>{flag}</td>"
            f"<td>{st['count']}</td><td>{st['mean']}</td>"
            f"<td>{bias}</td><td>{maxdev}</td></tr>")
    if not divrows:
        divrows.append(f"<tr><td colspan=\"5\">{labels['no_data']}</td></tr>")
    cal_note = (labels["calibrated_marker"] if calibrated_any
                else labels["calibration_none"])
    opts_v = "".join(
        f"<option value=\"{html.escape(v)}\""
        f"{' selected' if v == variable else ''}>{html.escape(_human_var(v))}</option>"
        for v in variables)
    page = (_COLOC_PAGE.replace("{lang}", code)
            .replace("{title}", labels["title"])
            .replace("{h_coloc}", labels["coloc"])
            .replace("{back}", labels["back"])
            .replace("{h_var}", labels["variable"])
            .replace("{opts_v}", opts_v)
            .replace("{h_days}", labels["days"])
            .replace("{sel1}", " selected" if days == 1 else "")
            .replace("{sel7}", " selected" if days == 7 else "")
            .replace("{sel30}", " selected" if days == 30 else "")
            .replace("{apply}", labels["apply"])
            .replace("{series_json}", _json_for_script(series))
            .replace("{h_div}", labels["coloc"])
            .replace("{h_node}", labels["node"])
            .replace("{h_n}", labels["stat_n"])
            .replace("{h_mean}", labels["stat_mean"])
            .replace("{h_bias}", labels["bias"])
            .replace("{h_maxdev}", labels["maxdev"])
            .replace("{divrows}", "\n".join(divrows))
            .replace("{cal_note}", cal_note)
            .replace("{disclaimer}", labels["disclaimer"]))
    return HTMLResponse(_with_legal(page, labels, code))



@router.get("/heat", response_class=HTMLResponse)
def heat_fleet_page(request: Request, hours: int = 168, threshold: float = 32.0,
                    min_duration_min: int = 60):
    """Every node's heat events in one table.

    The per-node page at `/nodes/{id}/events` existed and worked, and answering "which nodes
    had a heat event" meant visiting each one in turn and reading the number. For the question
    an operator actually asks during a warm spell - who is above threshold, and since when -
    the fleet view is the page that answers it, so this is it.

    Window and threshold are query parameters rather than form fields alone, so a URL can be
    shared and bookmarked. Both are clamped: an unbounded window would scan the whole table
    on every refresh, and this page reloads itself.
    """
    check_rate(request)
    code, labels = _pick(request)
    hours = max(1, min(int(hours), 24 * 365))
    threshold = max(-80.0, min(float(threshold), 80.0))
    min_duration_min = max(1, min(int(min_duration_min), 24 * 60))

    to_ms = query("SELECT MAX(timestamp_utc_ms) AS m FROM measurements")[0]["m"] or 0
    from_ms = to_ms - hours * 3600000

    per_node = []
    for r in query("SELECT node_id FROM nodes ORDER BY node_id"):
        nid = r["node_id"]
        events = detect_heat_events(_heat_rows(nid, "air_temperature", from_ms, to_ms),
                                    threshold, min_duration_min)
        summary = heat_summary(events)
        last = query("SELECT MAX(timestamp_utc_ms) AS m FROM measurements"
                     " WHERE node_id=? AND variable='air_temperature'"
                     " AND value IS NOT NULL", (nid,))[0]["m"]
        # `event_list`, not `events`: `heat_summary` returns a count under the key
        # `events`, and a later `**` silently overwrites the list with an int - which
        # failed at the first `max()` rather than at the assignment.
        per_node.append({"node_id": nid, "event_list": events, **summary,
                        "last_utc_ms": last})

    with_events = [p for p in per_node if p["events"] > 0]
    rows_html = ""
    for p in sorted(per_node, key=lambda x: (-x["events"], x["node_id"])):
        if p["events"]:
            recent = max(p["event_list"], key=lambda e: e["end_utc_ms"])
            last_ev = (f"<a href=\"/nodes/{html.escape(p['node_id'])}/events"
                       f"?threshold={threshold:g}&min_duration={min_duration_min}\">"
                       f"{_fmt_utc(recent['end_utc_ms'])}</a>")
        else:
            last_ev = '<span class="mut">\u2014</span>'
        cls = " class=\"hot\"" if p["events"] else ""
        peak = p["peak_value"]
        peak_txt = "—" if peak is None else f"{peak:.1f}"
        rows_html += (
            f"<tr{cls}><td><a href=\"/nodes/{html.escape(p['node_id'])}\">"
            f"{html.escape(p['node_id'])}</a></td>"
            f"<td>{p['events']}</td>"
            f"<td>{round(p['total_minutes'] / 60, 1)}</td>"
            f"<td>{p['longest_min']}</td>"
            f"<td>{peak_txt}</td>"
            f"<td>{last_ev}</td></tr>")

    if with_events:
        hottest = max(with_events, key=lambda x: x["peak_value"] or -99)
        longest = max(with_events, key=lambda x: x["total_minutes"])
        summary = (
            f"<p><span class=\"pill\">{len(with_events)}</span> "
            f"{labels['heat_nodes_with']} "
            f"\u00b7 {labels['heat_hours_above']}: "
            f"{round(sum(p['total_minutes'] for p in with_events) / 60, 1)} h "
            f"\u00b7 {labels['worst_node']}: "
            f"<a href=\"/nodes/{html.escape(hottest['node_id'])}\">"
            f"{html.escape(hottest['node_id'])}</a> "
            f"({hottest['peak_value']:.1f} \u00b0C) "
            f"\u00b7 {labels['heat_longest']}: "
            f"<a href=\"/nodes/{html.escape(longest['node_id'])}\">"
            f"{html.escape(longest['node_id'])}</a> "
            f"({round(longest['total_minutes'] / 60, 1)} h)</p>")
    else:
        summary = f"<div class=\"empty\"><p>{labels['heat_no_any']}</p></div>"

    body = (
        "<div class=\"tbl\"><table><tr>"
        f"<th>{labels['node']}</th><th>{labels['events']}</th>"
        f"<th>{labels['heat_hours_above']} (h)</th>"
        f"<th>{labels['heat_longest']} (min)</th>"
        f"<th>{labels['peak']} (\u00b0C)</th>"
        f"<th>{labels['end']}</th></tr>"
        + rows_html + "</table></div>")

    page = (_HEAT_PAGE.replace("{lang}", code)
            .replace("{title}", labels["title"])
            .replace("{h_heat}", labels["heat_title"])
            .replace("{heat_fleet}", labels["heat_fleet"])
            .replace("{back}", labels["back"])
            .replace("{map}", labels["map"])
            .replace("{h_thr}", labels["threshold"])
            .replace("{thr}", _form_value(threshold))
            .replace("{h_dur}", labels["min_dur"])
            .replace("{dur}", _form_value(min_duration_min))
            .replace("{heat_window}", labels["heat_window"])
            .replace("{hours}", _form_value(hours))
            .replace("{apply}", labels["apply"])
            .replace("{summary}", summary)
            .replace("{body}", body)
            .replace("{heat_open_note}", labels["heat_open_note"])
            .replace("{disclaimer}", labels["disclaimer"]))
    return HTMLResponse(_with_legal(page, labels, code))

@router.get("/map", response_class=HTMLResponse)
def map_page(request: Request, hours: int = 24):
    check_rate(request)
    code, labels = _pick(request)
    # Clamped: this page reloads itself, and an unbounded window means re-scanning the
    # whole measurements table on every refresh.
    hours = max(1, min(int(hours), 24 * 30))
    sites = query(
        "SELECT site_id, name, lat, lon FROM sites"
        " WHERE lat IS NOT NULL AND lon IS NOT NULL")
    max_ts = query("SELECT MAX(timestamp_utc_ms) AS m FROM measurements")[0]["m"] or 0
    # The window does NOT filter which reading is used. Filtering here was the first version
    # and it was wrong: a node whose last reading predates the window came back with a NULL
    # temperature, so the map drew it as an empty marker - the same as hiding it, which is
    # what the window was supposed to avoid. A node that reported three days ago still has a
    # temperature. It is drawn, marked stale, and weighted down in the interpolation.
    nodes = query(
        """SELECT n.node_id, n.site_id,
                  (SELECT value FROM measurements m WHERE m.node_id=n.node_id
                   AND variable='air_temperature' AND value IS NOT NULL
                   ORDER BY sequence DESC LIMIT 1) AS temp,
                  (SELECT timestamp_utc_ms FROM measurements m WHERE m.node_id=n.node_id
                   AND variable='air_temperature' AND value IS NOT NULL
                   ORDER BY sequence DESC LIMIT 1) AS temp_at
           FROM nodes n ORDER BY n.node_id""")
    placed = [n for n in nodes
              if any(s["site_id"] == n["site_id"] for s in sites)]
    if not placed:
        body = f"<div class=\"empty\"><p>{labels['no_coords']}</p></div>"
    else:
        lats = [s["lat"] for s in sites]
        lons = [s["lon"] for s in sites]
        import math
        lat0 = sum(lats) / len(lats)
        kx = math.cos(math.radians(lat0)) or 1.0
        span_x = (max(lons) - min(lons)) * kx or 0.01
        span_y = max(lats) - min(lats) or 0.01
        W, H, pad = 860, 420, 60
        site_xy = {}
        for s in sites:
            x = pad + (s["lon"] - min(lons)) * kx / span_x * (W - 2 * pad)
            y = pad + (max(lats) - s["lat"]) / span_y * (H - 2 * pad)
            site_xy[s["site_id"]] = (x, y)
        by_site: dict[str, list] = {}
        for n in placed:
            by_site.setdefault(n["site_id"], []).append(n)
        members_xy = []
        for sid, members in by_site.items():
            cx, cy = site_xy[sid]
            for i, n in enumerate(members):
                ang = 2 * math.pi * i / max(1, len(members))
                dx = 20 * math.cos(ang) if len(members) > 1 else 0
                dy = 20 * math.sin(ang) if len(members) > 1 else 0
                members_xy.append((n, cx + dx, cy + dy))
        field_svg = ""
        legend_svg = ""
        with_temp = [(n, x, y) for n, x, y in members_xy
                     if n["temp"] is not None]
        if len(with_temp) >= 2:
            temps = [n["temp"] for n, _, _ in with_temp]
            tmin, tmax = min(temps), max(temps)
            cols, rows_n = 36, 24
            cw, chh = (W - 2 * pad) / cols, (H - 2 * pad) / rows_n
            cells = []
            for gi in range(cols):
                for gj in range(rows_n):
                    px = pad + (gi + 0.5) * cw
                    py = pad + (gj + 0.5) * chh
                    num = den = 0.0
                    for nd, nx, ny in with_temp:
                        d2 = (px - nx) ** 2 + (py - ny) ** 2 + 400.0
                        w = 1.0 / d2
                        # Down-weight a node whose reading is old, so the field reflects
                        # the neighbourhood now rather than being dragged by a reading from
                        # yesterday. Not excluded: an excluded node would leave a hole in
                        # the interpolation exactly where you most want to know the value.
                        nd_at = nd["temp_at"]
                        if nd_at and max_ts:
                            age_h = (max_ts - nd_at) / 3600000.0
                            if age_h > hours:
                                w *= 0.25
                        num += w * nd["temp"]
                        den += w
                    cells.append(
                        f"<rect x=\"{px - cw / 2:.1f}\" y=\"{py - chh / 2:.1f}\""
                        f" width=\"{cw + 0.5:.1f}\" height=\"{chh + 0.5:.1f}\""
                        f" fill=\"{_temp_color(num / den)}\" opacity=\"0.5\"/>")
            field_svg = "".join(cells)
            # The legend used to be a linear gradient with its two endpoints pinned to the
            # lowest and highest value present in the data, labelled with those two numbers.
            #
            # That is a lie even though `_temp_color` is on a fixed 0-40 C scale. The field
            # cells are painted with `_temp_color(value)`; the bar interpolated between
            # `_temp_color(tmin)` and `_temp_color(tmax)` instead of being that ramp. So when
            # the data spanned 10-20 C, the bar ran from colour(10) to colour(20) through
            # every colour in between - none of which is what colour(15) looks like on the
            # scale the map actually uses. A reader matching a cell to the bar would read the
            # wrong temperature, confidently, for every value in the middle.
            #
            # The bar is now the real ramp, sampled from the same function, with ticks at
            # the scale's own values. It looks similar when the data happens to fill the
            # scale and is simply correct otherwise.
            ramp_w, ramp_y, ramp_h = 220, H - 20, 12
            steps = 40
            ramp = "".join(
                f"<rect x=\"{ramp_w * i / steps:.1f}\" y=\"{ramp_y}\""
                f" width=\"{ramp_w / steps + 0.5:.2f}\" height=\"{ramp_h}\""
                f" fill=\"{_temp_color(_TEMP_SCALE_HI * i / steps)}\""
                f"{' opacity=\"0.35\"' if tmin > _TEMP_SCALE_HI or tmax < 0 else ''}/>"
                for i in range(steps))
            ticks = ""
            for tick in range(0, int(_TEMP_SCALE_HI) + 1, 10):
                tx = ramp_w * tick / _TEMP_SCALE_HI
                ticks += (f"<line x1=\"{tx:.1f}\" y1=\"{ramp_y + ramp_h}\""
                          f" x2=\"{tx:.1f}\" y2=\"{ramp_y + ramp_h + 4}\""
                          f" stroke=\"#8aa0b4\" stroke-width=\"1\"/>"
                          f"<text x=\"{tx:.1f}\" y=\"{ramp_y + ramp_h + 16}\""
                          f" fill=\"#8aa0b4\" font-size=\"10\" text-anchor=\"middle\">"
                          f"{tick}\u00b0</text>")
            outside = ""
            if tmin > _TEMP_SCALE_HI or tmax < 0:
                outside = (f"<text x=\"{ramp_w + 6}\" y=\"{ramp_y + 10}\""
                           f" fill=\"#d98b3a\" font-size=\"10\">"
                           f"{html.escape(labels['map_outside_scale'])}</text>")
            legend_svg = (
                f"<rect x=\"{pad - 8}\" y=\"{ramp_y - 15}\" width=\"{ramp_w + 16}\""
                f" height=\"{ramp_h + 34}\" fill=\"#0f1720\" opacity=\"0.75\""
                f" rx=\"4\"/>"
                + ramp + ticks + outside +
                f"<text x=\"{pad - 8}\" y=\"{ramp_y - 4}\" fill=\"#8aa0b4\""
                f" font-size=\"10\">{html.escape(labels['field'])} \u00b7"
                f" {html.escape(labels['map_colour_fixed'])}</text>")

        circles = []
        for n, x, y in members_xy:
            t = n["temp"]
            at = n["temp_at"]
            age_h = None if not at else (max_ts - at) / 3600000.0
            stale = age_h is not None and age_h > hours
            if t is None:
                ttxt = "\u2014"
            else:
                ttxt = f"{t:.1f}\u00b0"
            if age_h is None:
                # No reading in the window *and* none at all outside it. A node with an old
                # reading is handled below, so this branch is genuinely "never reported".
                when = labels["map_never"]
            elif stale:
                age_txt = (f"{age_h / 24:.1f} {labels['days']}" if age_h >= 48
                           else f"{age_h:.0f}h")
                when = f"{age_txt} \u00b7 {labels['map_stale']}"
            else:
                when = labels["map_fresh"]
            # A stale node keeps its colour but loses its fill: hollow reads as "this value
            # is old" without inventing a second colour, which would then need its own
            # legend entry and would imply the hue means something it does not.
            fill = 'fill="none"' if stale else f'fill="{_temp_color(t)}"'
            dash = ('stroke-dasharray="4 3"' if stale else "")
            label_fill = "#8aa0b4" if stale else "#e8eef4"
            circles.append(
                f"<a href=\"/nodes/{html.escape(n['node_id'])}\">"
                f"<circle cx=\"{x:.0f}\" cy=\"{y:.0f}\" r=\"16\" {fill}"
                f" stroke=\"{'#d98b3a' if stale else '#0f1720'}\""
                f" stroke-width=\"2\" {dash}>"
                f"<title>{html.escape(n['node_id'])}: {ttxt} - "
                f"{html.escape(when)}</title></circle>"
                f"<text x=\"{x:.0f}\" y=\"{y + 32:.0f}\""
                f" text-anchor=\"middle\" fill=\"{label_fill}\" font-size=\"12\">"
                f"{html.escape(n['node_id'])}</text></a>")

        labels_svg = "".join(
            f"<text x=\"{x:.0f}\" y=\"{y - 24:.0f}\" text-anchor=\"middle\""
            f" fill=\"#8aa0b4\" font-size=\"12\">{html.escape(sid)}</text>"
            for sid, (x, y) in site_xy.items())
        links = " ".join(
            f"<a href=\"/map?hours={h}&lang={code}\""
            f"{' style=\"color:#39c2a7;font-weight:600\"' if h == hours else ''}>"
            f"{labels['map_window']}: {h}h</a>"
            for h in (1, 6, 24, 72, 168))
        body = (f"<p class=\"mut\">{links}</p>"
                f"<p class=\"mut\"><button id=\"zin\" aria-label=\"{labels['zin']}\">+</button> "
                f"<button id=\"zout\" aria-label=\"{labels['zout']}\">−</button> "
                f"<button id=\"zreset\" aria-label=\"{labels['zreset']}\">⟲</button></p>"
                f"<svg id=\"map\" viewBox=\"0 0 {W} {H}\">{field_svg}{labels_svg}"
                + "".join(circles) + legend_svg + "</svg>"
                "<script>(function(){"
                "var svg=document.getElementById('map');"
                "if(!svg)return;"
                f"var home=[0,0,{W},{H}],vb=home.slice();"
                "function apply(){svg.setAttribute('viewBox',vb.join(' '));}"
                "function zoom(f,cx,cy){"
                "var r=svg.getBoundingClientRect();"
                "var mx=(cx===undefined?r.width/2:cx-r.left)/r.width*vb[2]+vb[0];"
                "var my=(cy===undefined?r.height/2:cy-r.top)/r.height*vb[3]+vb[1];"
                "vb[0]=mx-(mx-vb[0])*f;vb[1]=my-(my-vb[1])*f;vb[2]*=f;vb[3]*=f;apply();}"
                "svg.addEventListener('wheel',function(e){e.preventDefault();zoom(e.deltaY>0?1.25:0.8,e.clientX,e.clientY);},{passive:false});"
                "var drag=null;"
                "svg.addEventListener('pointerdown',function(e){drag=[e.clientX,e.clientY,vb[0],vb[1]];svg.setPointerCapture(e.pointerId);});"
                "svg.addEventListener('pointermove',function(e){if(!drag)return;"
                "var r=svg.getBoundingClientRect();"
                "vb[0]=drag[2]-(e.clientX-drag[0])/r.width*vb[2];"
                "vb[1]=drag[3]-(e.clientY-drag[1])/r.height*vb[3];apply();});"
                "svg.addEventListener('pointerup',function(){drag=null;});"
                "document.getElementById('zin').addEventListener('click',function(){zoom(0.8);});"
                "document.getElementById('zout').addEventListener('click',function(){zoom(1.25);});"
                "document.getElementById('zreset').addEventListener('click',function(){vb=home.slice();apply();});"
                "})();</script>")
    page = (_MAP_PAGE.replace("{lang}", code)
            .replace("{title}", labels["title"])
            .replace("{h_map}", labels["map"])
            .replace("{events}", labels["events"])
            .replace("{back}", labels["back"])
            .replace("{body}", body)
            .replace("{disclaimer}", labels["disclaimer"]))
    return HTMLResponse(_with_legal(page, labels, code))


@router.get("/nodes/{node_id}/report", response_class=HTMLResponse)
def node_report(node_id: str, request: Request,
                authorization: Annotated[str | None, Header()] = None):
    check_rate(request)
    code, labels = _pick(request)
    nodes = query("SELECT node_id, site_id FROM nodes WHERE node_id=?",
                  (node_id,))
    if not nodes:
        raise HTTPException(status_code=404, detail="node_not_found")
    node = nodes[0]
    safe_id = html.escape(node["node_id"])
    bounds = query("SELECT MIN(timestamp_utc_ms) AS lo,"
                   " MAX(timestamp_utc_ms) AS hi, COUNT(*) AS c"
                   " FROM measurements WHERE node_id=?", (node_id,))[0]
    variables = [r["variable"] for r in query(
        "SELECT DISTINCT variable FROM measurements WHERE node_id=?"
        " ORDER BY variable", (node_id,))]
    statrows = []
    cals = site_calibrations(node["site_id"])
    for v in variables:
        vals = [r["value"] for r in query(
            "SELECT value FROM measurements WHERE node_id=? AND variable=?"
            " AND value IS NOT NULL", (node_id, v))]
        st = summary_stats(vals)
        cal = cals.get(v)
        if cal and not is_identity(cal["scale"], cal["offset"]):
            cal_st = transform_stats(st, cal)
            extra = (f"<td>{cal_st['min']}</td><td>{cal_st['max']}</td>"
                     f"<td>{cal_st['mean']}</td>")
            marker = " *"
        else:
            extra = "<td>—</td><td>—</td><td>—</td>"
            marker = ""
        statrows.append(
            f"<tr><td>{html.escape(_human_var(v))}{marker}</td>"
            f"<td>{st['min']}</td><td>{st['max']}</td>"
            f"<td>{st['mean']}</td><td>{st['count']}</td>{extra}</tr>")
    cal_note = labels["calibrated_marker"] if cals else labels["cal_none"]
    qbreak = query("SELECT quality, COUNT(*) AS c FROM measurements"
                   " WHERE node_id=? GROUP BY quality ORDER BY c DESC",
                   (node_id,))
    qrows = "".join(
        f"<tr><td class=\"q-{html.escape(r['quality'])}\">"
        f"{html.escape(r['quality'])}</td>"
        f"<td>{r['c']}</td></tr>" for r in qbreak)
    try:
        heat = analytics_heat_events(node_id=node_id, request=request,
                                     authorization=authorization)
        evs = heat["events"]
    except Exception:
        evs = []
    if evs:
        evbody = ("<div class=\"tbl\"><table><tr>"
                  f"<th>{labels['start']}</th><th>{labels['end']}</th>"
                  f"<th>{labels['duration']}</th><th>{labels['peak']} (°C)</th></tr>"
                  + "".join(
                      _EV_ROW.format(start=_fmt_utc(e["start_utc_ms"]),
                                     end=_fmt_utc(e["end_utc_ms"]),
                                     dur=e["duration_min"], peak=e["peak_value"])
                      for e in evs) + "</table></div>")
    else:
        evbody = f"<p class=\"mut\">{labels['no_events']}</p>"
    now_ms = int(time.time() * 1000)
    page = (_REPORT_PAGE.replace("{lang}", code)
            .replace("{title}", labels["title"])
            .replace("{nid}", safe_id)
            .replace("{h_report}", labels["report"])
            .replace("{back_node}", labels["node"])
            .replace("{back}", labels["back"])
            .replace("{print}", labels["print"])
            .replace("{gen}", labels["last_sync"].split(" (")[0])
            .replace("{gentime}", _fmt_utc(now_ms))
            .replace("{h_range}", labels["range"])
            .replace("{wfrom}", _fmt_utc(bounds["lo"]))
            .replace("{wto}", _fmt_utc(bounds["hi"]))
            .replace("{h_stats}", labels["stats"])
            .replace("{h_var}", labels["variable"])
            .replace("{h_min}", labels["stat_min"])
            .replace("{h_max}", labels["stat_max"])
            .replace("{h_mean}", labels["stat_mean"])
            .replace("{h_n}", labels["stat_n"])
            .replace("{h_cal}", labels["calibration"])
            .replace("{cal_note}", cal_note)
            .replace("{h_cal_min}", labels["stat_min"] + " (cal)")
            .replace("{h_cal_max}", labels["stat_max"] + " (cal)")
            .replace("{h_cal_mean}", labels["stat_mean"] + " (cal)")
            .replace("{statrows}", "\n".join(statrows))
            .replace("{h_qbreak}", labels["qbreak"])
            .replace("{h_q}", labels["quality"])
            .replace("{qrows}", qrows or f"<tr><td colspan=\"2\">{labels['no_data']}</td></tr>")
            .replace("{h_ev}", labels["ev_title"])
            .replace("{evbody}", evbody)
            .replace("{disclaimer}", labels["disclaimer"]))
    return HTMLResponse(_with_legal(page, labels, code))


@router.get("/compare", response_class=HTMLResponse)
def compare_page(request: Request, a: str | None = None,
                 b: str | None = None, c: str | None = None,
                 variable: str = "air_temperature", days: int = 7):
    check_rate(request)
    code, labels = _pick(request)
    nodes = [r["node_id"] for r in query(
        "SELECT node_id FROM nodes ORDER BY node_id")]
    variables = [r["variable"] for r in query(
        "SELECT DISTINCT variable FROM measurements ORDER BY variable")]
    if days not in (1, 7, 30):
        days = 7
    if a not in nodes:
        a = nodes[0] if nodes else None
    if b not in nodes:
        b = nodes[1] if len(nodes) > 1 else a
    sel = [x for x in (a, b, c) if x in nodes]
    seen: list[str] = []
    sel = [x for x in sel if not (x in seen or seen.append(x))]
    if variable not in variables:
        variable = "air_temperature" if "air_temperature" in variables else (
            variables[0] if variables else "air_temperature")

    def opts(items, current):
        return "".join(
            f"<option value=\"{html.escape(i)}\""
            f"{' selected' if i == current else ''}>{html.escape(i)}</option>"
            for i in items)

    results = f"<p class=\"mut\">{labels['no_data']}</p>"
    if sel:
        max_ts = query(
            "SELECT MAX(timestamp_utc_ms) AS m FROM measurements WHERE " +
            f"node_id IN ({','.join(['?'] * len(sel))})",
            tuple(sel))[0]["m"] or 0
        since = max_ts - days * 86400000
        data = {}
        stats = {}
        for nid in sel:
            pts = query(
                "SELECT timestamp_utc_ms, value FROM measurements"
                " WHERE node_id=? AND variable=? AND value IS NOT NULL"
                " AND timestamp_utc_ms>=? ORDER BY sequence", (nid, variable, since))
            step = max(1, len(pts) // 400)
            data[nid] = pts[::step]
            stats[nid] = summary_stats([p["value"] for p in data[nid]])

        def row(label, key):
            return ("<tr><td>" + label + "</td>" + "".join(
                f"<td>{stats[nid][key]}</td>" for nid in sel) + "</tr>")

        stat_rows = [
            row(labels["stat_n"], "count"),
            row(labels["stat_min"], "min"),
            row(labels["stat_max"], "max"),
            row(labels["stat_mean"], "mean"),
        ]
        if len(sel) >= 2:
            first, second = stats[sel[0]], stats[sel[1]]
            diff = (round(first["mean"] - second["mean"], 3)
                    if first["mean"] is not None and second["mean"] is not None
                    else "—")
            stat_rows.append(
                "<tr><td>" + labels["mean_diff"] + "</td>"
                f"<td>{diff}</td>" + "<td></td>" * (len(sel) - 1) + "</tr>")
        series = [
            {"label": nid, "color": _COLOC_COLORS[i % len(_COLOC_COLORS)],
             "pts": [[p["timestamp_utc_ms"], p["value"]] for p in data[nid]]}
            for i, nid in enumerate(sel)
        ]
        results = (
            f"<h2>{html.escape(_human_var(variable))}</h2>"
            "<div class=\"tbl\">"
            f"<table><tr><th></th>" + "".join(
                f"<th>{html.escape(nid)}</th>" for nid in sel) + "</tr>"
            + "\n".join(stat_rows) + "</table></div>"
            "<canvas id=\"chart\" width=\"860\" height=\"220\"></canvas>"
            "<script>var series = " + _json_for_script(series) + """;
(function(){
  var cv = document.getElementById("chart"), ctx = cv.getContext("2d");
  var W = cv.width, H = cv.height, pad = 36;
  var t0 = Infinity, t1 = -Infinity;
  series.forEach(function(s){ s.pts.forEach(function(p){ if (p[0] < t0) t0 = p[0]; if (p[0] > t1) t1 = p[0]; }); });
  if (t1 < t0) return;
  if (t1 === t0) t1 = t0 + 1;
  function x(ts){ return pad + (ts - t0) / (t1 - t0) * (W - 2 * pad); }
  series.forEach(function(s){
    if (!s.pts.length) return;
    var vs = s.pts.map(function(p){ return p[1]; });
    var lo = Math.min.apply(null, vs), hi = Math.max.apply(null, vs);
    if (hi === lo) hi = lo + 1;
    function y(v){ return H - pad - (v - lo) / (hi - lo) * (H - 2 * pad); }
    ctx.strokeStyle = s.color; ctx.lineWidth = 1.6; ctx.beginPath();
    s.pts.forEach(function(p, i){ var px = x(p[0]), py = y(p[1]); if (i) ctx.lineTo(px, py); else ctx.moveTo(px, py); });
    ctx.stroke();
  });
  function hhmm(ms){ var d = new Date(ms); return ("0" + d.getUTCHours()).slice(-2) + ":" + ("0" + d.getUTCMinutes()).slice(-2); }
  ctx.fillStyle = "#8aa0b4"; ctx.font = "11px system-ui";
  ctx.fillText(hhmm(t0), pad, H - 8);
  var e = hhmm(t1);
  ctx.fillText(e, W - pad - ctx.measureText(e).width, H - 8);
})();
</script>""")
    page = (_COMPARE_PAGE.replace("{lang}", code)
            .replace("{title}", labels["title"])
            .replace("{h_compare}", labels["compare"].rstrip(" →"))
            .replace("{back}", labels["back"])
            .replace("{h_a}", labels["node_a"]).replace("{h_b}", labels["node_b"])
            .replace("{h_c}", labels["node_c"])
            .replace("{opts_a}", opts(nodes, a)).replace("{opts_b}", opts(nodes, b))
            .replace("{opts_c}", opts(nodes, c))
            .replace("{h_var}", labels["variable"])
            .replace("{opts_v}", opts(variables, variable))
            .replace("{h_days}", labels["days"])
            .replace("{sel1}", " selected" if days == 1 else "")
            .replace("{sel7}", " selected" if days == 7 else "")
            .replace("{sel30}", " selected" if days == 30 else "")
            .replace("{apply}", labels["apply"])
            .replace("{results}", results)
            .replace("{disclaimer}", labels["disclaimer"]))
    return HTMLResponse(_with_legal(page, labels, code))


_SYSTEM_PAGE = """<!DOCTYPE html>
<html lang="{lang}"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
__REFRESH__<title>{title} — {h_sys}</title>
<style>
body{margin:0;background:#0f1720;color:#e8eef4;font:15px/1.45 system-ui,sans-serif}
main{max-width:900px;margin:0 auto;padding:1rem}
h1{color:#39c2a7;letter-spacing:.12em;font-size:1.2rem}
h2{color:#8aa0b4;font-size:.95rem;margin:1.4rem 0 .5rem}
table{width:100%;border-collapse:collapse;background:#182430;border-radius:12px;overflow:hidden}
.tbl{overflow-x:auto;border-radius:12px}
.tbl table{white-space:nowrap}
th{background:#223140;text-align:left;padding:.55rem .8rem;font-size:.75rem;text-transform:uppercase;color:#8aa0b4}
td{padding:.55rem .8rem;border-top:1px solid #223140}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:.6rem}
.card{background:#182430;border-radius:12px;padding:.7rem .9rem}
.card .v{font-size:1.4rem;font-weight:600}
.card .u{color:#8aa0b4;font-size:.8rem}
a{color:#7cc4ff;text-decoration:none}a:hover{text-decoration:underline}
.mut{color:#8aa0b4;font-size:.85rem}
:focus-visible{outline:2px solid #39c2a7;outline-offset:2px}
.skip{display:none}
@media(max-width:640px){main{padding:.6rem}th,td{padding:.4rem .45rem;font-size:.82rem}}
</style></head><body><main>
<p><a href="/">{back}</a></p>
<h1>{title} — {h_sys}</h1>
<div class="cards">{cards}</div>
<h2>{h_tables}</h2>
<div class="tbl"><table><tr><th>{h_table}</th><th>{h_rows}</th></tr>
{tablerows}
</table></div>
<h2>{h_nodes}</h2>
<p class="mut">{fleet_hint}</p>
<div class="tbl"><table><tr><th>{h_node}</th><th>{h_site}</th><th>{h_last}</th><th>{firmware}</th><th>{flags}</th><th>{visit}</th></tr>
{noderows}
</table></div>
<h2>{coverage}</h2>
<div class="tbl"><table><tr><th>{h_node}</th><th>{expected}</th><th>{received}</th><th>{coverage}</th><th>{longest_gap}</th></tr>
{coveragerows}
</table></div>
<h2>{retention}</h2>
<div class="tbl"><table>{retentionrows}</table></div>
<p class="mut">{disclaimer}</p></main></body></html>"""

_STAT2 = ("<div class=\"stat\"><div class=\"u\">{label}</div>"
          "<div class=\"v\">{value}</div></div>")


@router.get("/system", response_class=HTMLResponse)
def system_page(request: Request):
    check_rate(request)
    code, labels = _pick(request)
    from .coverage import DEFAULT_INTERVAL_MS, node_coverage
    from .db import engine
    from .fleet import fleet_snapshot
    from .retention import read_state
    health_nodes = query("SELECT COUNT(*) AS c FROM nodes")[0]["c"]
    health_meas = query("SELECT COUNT(*) AS c FROM measurements")[0]["c"]
    tables = []
    for name in ("sites", "interventions", "sync_batches", "alert_rules",
                 "alert_log"):
        try:
            tables.append((name, query(f"SELECT COUNT(*) AS c FROM {name}")[0]["c"]))
        except Exception:
            tables.append((name, "—"))
    try:
        db_bytes = engine().execute(
            "SELECT page_count * page_size FROM pragma_page_count(),"
            " pragma_page_size()").fetchone()[0]
        db_txt = f"{db_bytes / 1048576:.1f} MB" if db_bytes else "—"
    except Exception:
        db_txt = "—"
    import time as _time  # noqa: I001 - deferred to avoid circular import
    from . import main as _main  # noqa: I001 - deferred to avoid circular import
    uptime = int(_time.monotonic() - _main._STARTED_MONO)
    cards = [
        _STAT2.format(label=labels["nodes"], value=health_nodes),
        _STAT2.format(label=labels["records"], value=f"{health_meas}"),
        _STAT2.format(label=labels["database"], value=db_txt),
        _STAT2.format(label=labels["uptime"],
                      value=f"{uptime // 3600}h {(uptime % 3600) // 60}m"),
        _STAT2.format(label=labels["version"], value=_main.app.version),
    ]
    tablerows = "".join(
        f"<tr><td>{html.escape(str(name))}</td><td>{val}</td></tr>"
        for name, val in tables)
    noderows = ""
    coveragerows = ""
    now_ms = int(_time.time() * 1000)
    window_from = now_ms - 7 * 86400000
    for r in fleet_snapshot(None)["nodes"]:
        nid = r["node_id"]
        flags = "".join(
            f"<span class=\"q-INVALID\">{html.escape(f)}</span> "
            for f in r["flags"]
        ) or "—"
        visit = ("<strong>" + labels["visit"] + "</strong>"
                 if r["needs_visit"] else labels["no_visit"])
        noderows += (
            f"<tr><td><a href=\"/nodes/{html.escape(nid)}\">"
            f"{html.escape(nid)}</a></td>"
            f"<td>{html.escape(r['site_id'] or labels['none'])}</td>"
            f"<td>{_fmt_utc(r['last_seen_utc_ms'])}</td>"
            f"<td>{html.escape(str(r['firmware_version'] or '—'))}</td>"
            f"<td>{flags}</td><td>{visit}</td></tr>"
        )
        cov = node_coverage(
            nid, "air_temperature", window_from, now_ms, DEFAULT_INTERVAL_MS
        )
        pct = "—" if cov["coverage_pct"] is None else f"{cov['coverage_pct']}%"
        gap_ms = cov["longest_gap_ms"]
        gap_txt = "—" if not gap_ms else f"{gap_ms // 60000} min"
        coveragerows += (
            f"<tr><td><a href=\"/nodes/{html.escape(nid)}\">"
            f"{html.escape(nid)}</a></td>"
            f"<td>{cov['expected_samples']}</td>"
            f"<td>{cov['received_samples']}</td>"
            f"<td>{pct}</td><td>{gap_txt}</td></tr>"
        )
    if not noderows:
        noderows = f"<tr><td colspan=\"6\">{labels['no_data']}</td></tr>"
    if not coveragerows:
        coveragerows = f"<tr><td colspan=\"5\">{labels['no_data']}</td></tr>"

    state = read_state()
    retentionrows = (
        f"<tr><th>{labels['enabled']}</th><td>"
        f"{labels['yes'] if settings.retention_enabled else labels['no']}</td></tr>"
        f"<tr><th>{labels['retention']}</th><td>{settings.retention_days} d</td></tr>"
        f"<tr><th>{labels['last_run']}</th><td>"
        f"{_fmt_utc(state.get('last_run_utc_ms')) if state.get('last_run_utc_ms') else '—'}</td></tr>"
        f"<tr><th>{labels['total']}</th><td>"
        f"{state.get('deleted_measurements', 0)}</td></tr>"
    )
    page = (_SYSTEM_PAGE.replace("{lang}", code)
            .replace("{title}", labels["title"])
            .replace("{h_sys}", labels["sys_status"])
            .replace("{back}", labels["back"])
            .replace("{cards}", "\n".join(cards))
            .replace("{h_tables}", labels["dtable"] + "s")
            .replace("{h_table}", labels["dtable"])
            .replace("{h_rows}", labels["drows"])
            .replace("{tablerows}", tablerows)
            .replace("{h_nodes}", labels["nodes"])
            .replace("{h_node}", labels["node"])
            .replace("{h_site}", labels["site"])
            .replace("{h_last}", labels["last"])
            .replace("{h_total}", labels["total"])
            .replace("{firmware}", labels["firmware"])
            .replace("{flags}", labels["flags"])
            .replace("{visit}", labels["visit"])
            .replace("{fleet_hint}", labels["fleet_hint"])
            .replace("{coverage}", labels["coverage"])
            .replace("{expected}", labels["expected"])
            .replace("{received}", labels["received"])
            .replace("{longest_gap}", labels["longest_gap"])
            .replace("{retention}", labels["retention"])
            .replace("{last_run}", labels["last_run"])
            .replace("{coveragerows}", coveragerows)
            .replace("{retentionrows}", retentionrows)
            .replace("{noderows}", noderows)
            .replace("{disclaimer}", labels["disclaimer"]))
    return HTMLResponse(_with_legal(page, labels, code))

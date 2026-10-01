from __future__ import annotations

import html

from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import HTMLResponse

from .ratelimit import check_rate

router = APIRouter()

LEGAL_VERSION = "2026.09-v2"
LEGAL_EFFECTIVE = "2026-09-25"

FOOTER_LABELS = {
    "en": {"legal": "Legal", "terms": "Terms", "privacy": "Privacy",
           "cookies": "Cookies", "refunds": "Refunds",
           "sys_status": "System status",
           "tagline": "Versioned legal documents. History in git."},
    "es": {"legal": "Legal", "terms": "Términos", "privacy": "Privacidad",
           "cookies": "Cookies", "refunds": "Reembolsos",
           "sys_status": "Estado del sistema",
           "tagline": "Documentos legales versionados. Historial en git."},
    "pt": {"legal": "Legal", "terms": "Termos", "privacy": "Privacidade",
           "cookies": "Cookies", "refunds": "Reembolsos",
           "sys_status": "Estado do sistema",
           "tagline": "Documentos legais versionados. Histórico em git."},
}


def footer_html(code: str) -> str:
    labels = FOOTER_LABELS.get(code, FOOTER_LABELS["es"])
    q = f"?lang={code}" if code in FOOTER_LABELS else ""
    return (
        f"<footer class=\"mut\" style=\"margin-top:1.5rem;border-top:1px solid #223140;"
        f"padding-top:.8rem\">{labels['legal']}: "
        f"<a href=\"/legal/terms{q}\">{labels['terms']}</a> · "
        f"<a href=\"/legal/privacy{q}\">{labels['privacy']}</a> · "
        f"<a href=\"/legal/cookies{q}\">{labels['cookies']}</a> · "
        f"<a href=\"/legal/refunds{q}\">{labels['refunds']}</a> · "
        f"<a href=\"/system\">{labels['sys_status']}</a> · "
        f"{LEGAL_VERSION}</footer>")

DOCS = {
    "terms": {
        "version": LEGAL_VERSION,
        "effective": LEGAL_EFFECTIVE,
        "updated": LEGAL_EFFECTIVE,
        "status": "published",
        "en": {
            "title": "Terms of Use",
            "body": """
<p>CAUCE is a pilot environmental observation network, not a commercial
service. On one side, ESP32 microstations that measure, validate and keep
telemetry locally; on the other, an optional central server that gathers it
together. What follows describes how the thing actually behaves. It isn't
legal advice, and it doesn't pretend to be.</p>
<h2>What the service is</h2>
<p>Each node reads its sensors, grades every sample, stores the records on
its own flash and serves them over Wi-Fi. The central server takes in
batches without duplicating them, runs descriptive analytics and shows a
dashboard. That's the whole offering. There are no user accounts, no
subscriptions, no payments, and nobody promised you uptime — least of all
the solar nodes, which go quiet whenever the weather says so.</p>
<h2>What the data is (and isn't)</h2>
<p>Measurements are comparative environmental telemetry. They are not
certified meteorology, and analytics answers say so on every response
(derived tags plus a non-causality note). CSV and JSON exports can be
reused under the repository's MIT license; just don't present pilot
numbers as official readings.</p>
<h2>Ground rules</h2>
<p>Don't poke admin endpoints without authorization. Don't hammer the sync
API past its published rate limits. The tokens that guard writes are
compared in constant time for a reason — respect it.</p>
<h2>Changes and leaving</h2>
<p>These terms follow the software versions recorded in STATUS.md. Walking
away means decommissioning your node, or asking the operator to purge your
data (see Privacy for how). Which law governs and where disputes land:
REQUIERE REVISIÓN LEGAL — a lawyer needs to look at this before anyone
relies on it.</p>
<h2>Contact</h2>
<p>REQUIERE CONFIRMACIÓN: the operating entity, its address and a contact
channel aren't published in this repository yet. Until they are, this
section can't be completed honestly.</p>""",
        },
        "es": {
            "title": "Términos de uso",
            "body": """
<p>CAUCE es una red piloto de observación ambiental, no un servicio
comercial. De un lado, microestaciones ESP32 que miden, validan y guardan
telemetría localmente; del otro, un servidor central opcional que la junta.
Lo que sigue describe cómo se comporta realmente. No es asesoramiento
jurídico, ni pretende serlo.</p>
<h2>Qué es el servicio</h2>
<p>Cada nodo lee sus sensores, califica cada muestra, guarda los records en
su propia flash y los sirve por Wi-Fi. El servidor central recibe lotes sin
duplicarlos, corre analítica descriptiva y muestra un dashboard. Esa es
toda la oferta. No hay cuentas de usuario, ni suscripciones, ni pagos, y
nadie prometió uptime — menos que nadie los nodos solares, que se callan
cada vez que el clima lo dice.</p>
<h2>Qué son los datos (y qué no)</h2>
<p>Las mediciones son telemetría ambiental comparativa. No son meteorología
certificada, y las respuestas analíticas lo dicen en cada respuesta (tags
derived más nota de no-causalidad). Los exports CSV y JSON pueden
reutilizarse bajo la licencia MIT del repositorio; solo no presentes
números del piloto como lecturas oficiales.</p>
<h2>Reglas de piso</h2>
<p>No toques endpoints admin sin autorización. No martilles la API de sync
más allá de sus rate limits publicados. Los tokens que cuidan las
escrituras se comparan en tiempo constante por algo — respetalo.</p>
<h2>Cambios y salida</h2>
<p>Estos términos siguen las versiones registradas en STATUS.md. Irse
significa decomisionar tu nodo, o pedir al operador la purga de tus datos
(ver Privacidad para cómo). Qué ley rige y dónde caen las disputas:
REQUIERE REVISIÓN LEGAL — un abogado tiene que mirar esto antes de que
alguien se apoye en ello.</p>
<h2>Contacto</h2>
<p>REQUIERE CONFIRMACIÓN: la entidad operadora, su domicilio y un canal de
contacto aún no están publicados en este repositorio. Hasta entonces, esta
sección no puede completarse honestamente.</p>""",
        },
    },
    "privacy": {
        "version": LEGAL_VERSION,
        "effective": LEGAL_EFFECTIVE,
        "updated": LEGAL_EFFECTIVE,
        "status": "published",
        "en": {
            "title": "Privacy Policy",
            "body": """
<p>Below is what this software actually does with data — observed in code,
not assumed. Mentions of Uruguay's Law 18.331 and Decree 414/009 are
context, not a claim: whether and how they apply to your deployment is
something a lawyer has to determine. REQUIERE REVISIÓN LEGAL.</p>
<h2>Who's responsible</h2>
<p>REQUIERE CONFIRMACIÓN: nobody has published the operating entity and a
contact channel in this repository yet (finding BUSINESS INFORMATION REQUIRED).
Until that exists, treat this section as the project's most
important blank space.</p>
<h2>What we hold</h2>
<p><strong>Device telemetry</strong> — node and sensor ids, sequence,
timestamp, variable, value, unit, quality. It's the whole point of the
system, and it stays until the retention job runs.
<strong>Site ids and coordinates</strong> — only so the map has somewhere
to put the dots. <strong>Alert destinations</strong> — a webhook URL or a
Telegram chat id, typed in by whoever creates the rule; without one,
alerts simply have nowhere to go. <strong>Client IPs in the rate
limiter</strong> — abuse prevention with a 60-second memory, pruned on its
own. <strong>Secrets</strong> (API tokens, the Telegram bot token, device
keys) — configuration, never logged, compared in constant time. That's the
complete list: no accounts, no names, no emails, nobody's location, and the
central server never issues a cookie.</p>
<h2>Who else sees it</h2>
<p>Nobody, by default. No analytics, no ads, no trackers, no CDNs, no
fonts, no maps pulled from third parties — a self-hosted deployment sends
exactly nothing anywhere. The two exceptions both point where the
administrator aimed them: alert webhooks and Telegram messages.</p>
<h2>How long it stays, how it's guarded</h2>
<p>Measurements live until <tt>POST /v1/maintenance/retention</tt> purges
them. Rate-limit rows die after about 120 seconds. The alert log only
grows until someone purges it by hand (that policy is still under review).
Traffic is plain HTTP on trusted LAN; exposing this box to an untrusted
network without TLS termination in front would be negligent — the docs say
so, and so do we.</p>
<h2>Your rights</h2>
<p>Law 18.331 gives holders access, rectification, updating and suppression
rights, among others. Here's the honest mapping: read or export everything
through the open API (<tt>/v1/nodes/{id}/measurements</tt>,
<tt>/v1/export-all.csv</tt>) — that part works today, no permission
needed. Rectification or suppression go through REQUIERE CONFIRMACIÓN (we
need a contact channel first). Deleting provisioned-device records runs
through the retention endpoint; copies living on a node need hands on the
device.</p>
<h2>Changes</h2>
<p>Material changes bump the version printed below, and the history lives
in git where anyone can diff it. Keep pointing a node at the central and
that counts as having been told.</p>""",
        },
        "es": {
            "title": "Política de privacidad",
            "body": """
<p>Abajo va lo que este software realmente hace con los datos — observado
en código, no asumido. Las menciones a la Ley 18.331 y el Decreto 414/009
de Uruguay son contexto, no una afirmación: si aplican y cómo a tu
despliegue lo tiene que determinar un abogado. REQUIERE REVISIÓN
LEGAL.</p>
<h2>Quién responde</h2>
<p>REQUIERE CONFIRMACIÓN: nadie publicó aún la entidad operadora ni un
canal de contacto en este repositorio (finding BUSINESS INFORMATION
REQUIRED). Hasta que exista, tratá esta sección como el espacio en blanco
más importante del proyecto.</p>
<h2>Qué guardamos</h2>
<p><strong>Telemetría de dispositivos</strong> — ids de nodo y sensor,
secuencia, timestamp, variable, valor, unidad, calidad. Es todo el punto
del sistema, y queda hasta que corre el job de retención.
<strong>Ids y coordenadas de sitios</strong> — solo para que el mapa tenga
dónde poner los puntos. <strong>Destinos de alertas</strong> — una URL de
webhook o un chat id de Telegram, tipeados por quien crea la regla; sin
uno, las alertas simplemente no tienen adónde ir. <strong>IPs de cliente
en el rate limiter</strong> — prevención de abuso con 60 segundos de
memoria, purgada sola. <strong>Secretos</strong> (tokens API, bot token de
Telegram, device keys) — configuración, nunca logueados, comparados en
tiempo constante. Esa es la lista completa: sin cuentas, sin nombres, sin
emails, sin ubicación de nadie, y el servidor central jamás emite una
cookie.</p>
<h2>Quién más lo ve</h2>
<p>Nadie, por defecto. Sin analytics, sin publicidad, sin trackers, sin
CDNs, sin fuentes, sin mapas traídos de terceros — un despliegue
self-hosted no manda exactamente nada a ningún lado. Las dos excepciones
apuntan adonde el administrador las apuntó: webhooks y mensajes de
Telegram.</p>
<h2>Cuánto dura, cómo se cuida</h2>
<p>Las mediciones viven hasta que <tt>POST /v1/maintenance/retention</tt>
las purga. Las filas de rate-limit mueren a los ~120 segundos. El log de
alertas solo crece hasta que alguien lo purga a mano (esa política sigue
en revisión). El tráfico es HTTP plano en LAN confiable; exponer esta caja
a una red no confiable sin terminación TLS adelante sería negligente — los
docs lo dicen, y nosotros también.</p>
<h2>Tus derechos</h2>
<p>La Ley 18.331 da a los titulares derechos de acceso, rectificación,
actualización y supresión, entre otros. El mapeo honesto: leer o exportar
todo vía la API abierta (<tt>/v1/nodes/{id}/measurements</tt>,
<tt>/v1/export-all.csv</tt>) — eso anda hoy, sin permiso. Rectificación o
supresión van por REQUIERE CONFIRMACIÓN (primero hace falta un canal de
contacto). Borrar records de dispositivos provisionados corre por el
endpoint de retención; las copias que viven en un nodo necesitan manos
sobre el dispositivo.</p>
<h2>Cambios</h2>
<p>Los cambios materiales suben la versión impresa abajo, y el historial
vive en git donde cualquiera puede diffearlo. Seguir apuntando un nodo a
la central cuenta como haber sido notificado.</p>""",
        },
    },
    "cookies": {
        "version": LEGAL_VERSION,
        "effective": LEGAL_EFFECTIVE,
        "updated": LEGAL_EFFECTIVE,
        "status": "published",
        "en": {
            "title": "Cookie Policy",
            "body": """
<p>Full inventory, checked against the code line by line. There is no
banner because there is nothing to agree to — and unlike most cookie
pages, that sentence is covered by a test that would break the build if
it ever stopped being true.</p>
<table><tr><th>Name</th><th>Set by</th><th>Purpose</th><th>Category</th>
<th>Expiry</th><th>Consent</th></tr>
<tr><td>(none)</td><td>central server</td><td>—</td>
<td>strictly necessary</td><td>—</td><td>not applicable: this server has
never emitted a <tt>Set-Cookie</tt> header</td></tr>
<tr><td><tt>cauce-lang</tt></td><td>node dashboard (browser localStorage,
not a cookie at all)</td><td>remembers the UI language on that screen</td>
<td>functional</td><td>until cleared in the browser</td><td>not required:
an on-device preference, no tracking, never sent anywhere</td></tr>
</table>
<p>No analytics, no marketing, no third-party storage of any kind. If a
tracker is ever added, it goes behind explicit opt-in first — shipping
tracking without consent would contradict this versioned policy, and the
tests would say so loudly.</p>""",
        },
        "es": {
            "title": "Política de cookies",
            "body": """
<p>Inventario completo, chequeado contra el código línea por línea. No hay
banner porque no hay nada que aceptar — y a diferencia de la mayoría de
estas páginas, esa frase está cubierta por un test que rompería el build
si alguna vez dejara de ser cierta.</p>
<table><tr><th>Nombre</th><th>Seteada por</th><th>Propósito</th><th>Categoría</th>
<th>Expira</th><th>Consentimiento</th></tr>
<tr><td>(ninguna)</td><td>servidor central</td><td>—</td>
<td>estrictamente necesaria</td><td>—</td><td>no aplica: este servidor jamás
emitió un header <tt>Set-Cookie</tt></td></tr>
<tr><td><tt>cauce-lang</tt></td><td>dashboard del nodo (localStorage del
browser, ni siquiera una cookie)</td><td>recuerda el idioma de la UI en
esa pantalla</td><td>funcional</td><td>hasta borrarla en el browser</td>
<td>no requerido: preferencia on-device, sin tracking, jamás
transmitida</td></tr>
</table>
<p>Cero analytics, marketing o storage de terceros de ningún tipo. Si un
tracker se agregara alguna vez, sale detrás de opt-in explícito primero —
agregar tracking sin consentimiento contradiría esta política versionada,
y los tests lo dirían bien fuerte.</p>""",
        },
    },
    "refunds": {
        "version": LEGAL_VERSION,
        "effective": LEGAL_EFFECTIVE,
        "updated": LEGAL_EFFECTIVE,
        "status": "published",
        "en": {
            "title": "Refunds",
            "body": """
<p>CAUCE has no payments, subscriptions, charges or paid features: there is
nothing to refund, cancel or charge back. This page exists so that absence
is stated instead of implied. If the project ever charges for anything
(hosting, hardware, services), a real refunds policy with withdrawal terms
(Law 17.250 review included) must ship before the first charge:
REQUIERE REVISIÓN LEGAL.</p>""",
        },
        "es": {
            "title": "Reembolsos",
            "body": """
<p>CAUCE no tiene pagos, suscripciones, cargos ni features pagas: no hay
nada que reembolsar, cancelar ni contracargar. Esta página existe para que
la ausencia quede dicha en vez de implicada. Si el proyecto alguna vez
cobra algo (hosting, hardware, servicios), una política real de reembolsos
con términos de retracto (incluyendo revisión Ley 17.250) debe publicarse
antes del primer cobro: REQUIERE REVISIÓN LEGAL.</p>""",
        },
    },
}

_LEGAL_PAGE = """<!DOCTYPE html>
<html lang="{code}"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{doctitle} — CAUCE</title>
<style>
body{margin:0;background:#0f1720;color:#e8eef4;font:15px/1.5 system-ui,sans-serif}
main{max-width:860px;margin:0 auto;padding:1rem}
h1{color:#39c2a7;letter-spacing:.06em;font-size:1.3rem}
h2{color:#8aa0b4;font-size:1rem;margin:1.4rem 0 .4rem}
table{width:100%;border-collapse:collapse;background:#182430;border-radius:12px;overflow:hidden}
.tbl{overflow-x:auto;border-radius:12px}
.tbl table{white-space:nowrap}
th{background:#223140;text-align:left;padding:.55rem .8rem;font-size:.75rem;text-transform:uppercase;color:#8aa0b4}
td{padding:.55rem .8rem;border-top:1px solid #223140}
a{color:#7cc4ff;text-decoration:none}a:hover{text-decoration:underline}
.mut{color:#8aa0b4;font-size:.85rem}
code,tt{background:#223140;border-radius:6px;padding:.05rem .35rem;font-size:.85em}
:focus-visible{outline:2px solid #39c2a7;outline-offset:2px}
@media(max-width:640px){main{padding:.6rem}th,td{padding:.4rem .45rem;font-size:.82rem}}
footer{margin-top:1.5rem;border-top:1px solid #223140;padding-top:.8rem}
</style></head><body><main id="main">
<p><a href="/">{back}</a></p>
<h1>{doctitle}</h1>
<p class="mut">v{version} · {effective_lbl}: {effective} · {updated_lbl}: {updated} · {status}</p>
{body}
<p class="mut">{disclaimer_legal}</p>
{site_footer}
</main></body></html>"""


def _negotiate(request) -> tuple:
    q = (request.query_params.get("lang") or "").lower()
    if q in ("en", "es"):
        return q
    header = (request.headers.get("accept-language", "es") or "es")
    for part in header.split(","):
        base = part.split(";")[0].strip().split("-")[0].lower()
        if base in ("en", "es"):
            return base
    return "es"


@router.get("/legal/{slug}", response_class=HTMLResponse)
def legal_doc(slug: str, request: Request):
    check_rate(request)
    doc = DOCS.get(slug)
    if not doc or doc.get("status") != "published":
        raise HTTPException(status_code=404, detail="unknown_legal_document")
    code = _negotiate(request)
    lang = DOCS[slug][code] if code in DOCS[slug] else DOCS[slug]["es"]
    back = {"en": "← all nodes", "es": "← todos los nodos"}.get(code, "← todos los nodos")
    eff = {"en": "effective", "es": "vigente"}.get(code, "vigente")
    upd = {"en": "updated", "es": "actualizado"}.get(code, "actualizado")
    page = (_LEGAL_PAGE.replace("{code}", code)
            .replace("{doctitle}", html.escape(lang["title"]))
            .replace("{back}", back)
            .replace("{version}", doc["version"])
            .replace("{effective_lbl}", eff).replace("{effective}", doc["effective"])
            .replace("{updated_lbl}", upd).replace("{updated}", doc["updated"])
            .replace("{status}", doc["status"])
            .replace("{body}", lang["body"])
            .replace("{disclaimer_legal}",
                     html.escape(FOOTER_LABELS.get(code, FOOTER_LABELS["es"])["tagline"]))
            .replace("{site_footer}", footer_html(code)))
    return HTMLResponse(page)

# Sistema legal CAUCE — notas de desarrollo

Documentación interna. Los textos jurídicos públicos viven en código
(`backend/cauce_server/legal.py`, dict `DOCS`); este archivo explica la
maquinaria. Nada acá es asesoramiento jurídico.

## Arquitectura

Un módulo es dueño de todo lo legal: documentos versionados,
negociación, inyección de footer. Sin CMS, sin tabla de base, sin fetch
remoto — el texto legal viaja con el release que describe, así nunca
difiere del comportamiento que documenta.

- Documentos: `terms`, `privacy`, `cookies`, `refunds`. Versión actual
  `2026.09-v1` (`LEGAL_VERSION`), vigente 2026-09-25.
- Solo se sirve `status == "published"`. Todo lo demás (borradores,
  notas internas) es 404 por construcción — el allowlist son las keys
  de `DOCS`, no un listado de directorio.
- Negociación de contenido: `Accept-Language` en/es, default es (igual
  que los dashboards). No hay ramificación por usuario, organización o
  país: CAUCE no tiene usuarios ni organizaciones; `user_type` es
  siempre null, explícitamente, por diseño.
- Cada página HTML recibe footer + skip link vía `_with_legal()` en
  `dashboard.py` (punto único de inyección, a nivel string, tras
  renderizar la página).

## Resolución de país e identidad

No hay cascada: CAUCE sirve una jurisdicción piloto (Uruguay,
Canelones). No existe parámetro `organization_id` en ningún lado, así
que no hay superficie IDOR. No existe parámetro `country`, así que no
hay vector de confusión de documentos. Si alguna vez opera
multi-país, esta sección debe reescribirse primero — ver finding F-08.

## Consentimiento

Servidor central: cero cookies, cero analytics, cero recursos de
terceros (afirmado por tests en cada página). Sin nada que consentir,
no hay banner — y un test fija esa afirmación, así un futuro tracker
rompe el build en vez de salir en silencio. Dashboard del nodo: una
key `localStorage` (`cauce-lang`, idioma de UI, on-device, nunca
transmitida).

## Versionado y trazabilidad

Cada documento lleva versión/vigencia/actualización/estado, renderizado
en la página. No existen eventos de consentimiento (no hay
procesamiento gated por consentimiento); si alguna vez se introducen,
la versión aceptada debe registrarse junto — hoy es un no-requisito
documentado, no un descuido.

## Fallback

Sin dependencia remota no hace falta modo degradado: los documentos son
strings estáticos del release. El único fallback del sistema es la
negociación de idioma con default español.

## Testing

`test_legal_pages_render_versioned_no_inventions`,
`test_legal_footer_on_every_page`,
`test_no_cookies_no_third_party_loads`,
`test_forms_have_labels_and_named_buttons`. Los checks de
comportamiento de red (flujos analytics-antes-de-consentimiento) no
aplican: una captura mostraría lo mismo que afirma el test de no-carga —
cero requests de terceros, porque cero código de terceros viaja.

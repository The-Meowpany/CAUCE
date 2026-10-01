# Índice de documentación CAUCE

Espejo en inglés: [docs/en/DOCUMENTATION_INDEX.md](../en/DOCUMENTATION_INDEX.md).

Fuente única de verdad implementado-vs-planificado: [STATUS.md](../../STATUS.md)
(inglés). Manda sobre todo lo de acá cuando discrepan — lo que no debería
pasar, pero el papel aguanta todo.

| Documento | Qué | Estado |
|---|---|---|
| [TESTING.md](TESTING.md) | Cómo correr cada suite; qué significa realmente la cobertura | Estable |
| [DEPLOYMENT.md](DEPLOYMENT.md) | Cero → piloto operativo, en orden, sin saltear pasos | Estable |
| [HARDWARE.md](HARDWARE.md) | BOM borrador, cableado de referencia, reglas de emplazamiento | Borrador v0, sin hardware aún |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Capas, flujos, máquinas de estado, y por qué | Estable |
| [DATA_MODEL.md](DATA_MODEL.md) | Measurement, calidad, storage, exports, ventanas inclusivas | Estable |
| [DOMAIN_GLOSSARY.md](DOMAIN_GLOSSARY.md) | Un término por concepto, exigido en todas partes | Estable |
| [API.md](API.md) | `/api/v1` del nodo: endpoints, auth, streaming, códigos | Estable |
| [SYNC.md](SYNC.md) | Contrato sync nodo↔central, watermark, envelope HMAC | Estable |
| [BACKEND.md](BACKEND.md) | Central: endpoints, garantías de ingesta, límites | Estable |
| [SECURITY.md](SECURITY.md) | Lo que hacemos, lo que abiertamente no | Estable |
| [CALIBRATION.md](CALIBRATION.md) | Modelo offset/scale, límites honestos, plan de co-localización | Modelo definido, runtime pendiente |
| [OTA.md](OTA.md) | FSM de decisión, reglas anti-brick, contrato de manifiesto | Decisión entregada, flasheo pendiente |
| [DASHBOARD.md](DASHBOARD.md) | SPA local + captive portal, y por qué sin dependencias | Estable |
| [BENCH_PLAN.md](BENCH_PLAN.md) | Validación física B1–B5 con criterios de salida | Plan, no ejecutado |
| [ROADMAP.md](ROADMAP.md) | Secuenciación M0–M5, principios, no-objetivos | Estable |
| [PILOT_SPEC.md](PILOT_SPEC.md) | Piloto congelado: nodos, presupuesto, gate de aceptación | Congelado |
| [I18N.md](I18N.md) | Código en inglés; superficies y docs localizados | Estable |

Specs de protocolo en [protocol/v1/](../../protocol/v1/PROTOCOL.md)
(inglés). Archivos raíz operativos: `STATUS.md`, `SECURITY.md` y
`CONTRIBUTING.md`.

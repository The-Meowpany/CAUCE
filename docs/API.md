# API embebida CAUCE — v1

Base: `http://<ip-del-nodo>/api/v1`

Transporte: HTTP servido por el propio nodo (sin dependencias externas).
La lógica vive en `ApiRouter` (portable y testeada en host); la capa
Arduino/`WebServer` es solo transporte (`Esp32ApiServer`, verificado por
compilación — sin placa aún).

## Endpoints

| Método | Ruta | Descripción |
|---|---|---|
| GET | `/node` | Identidad del nodo, versiones firmware/protocolo/hardware, ubicación declarada |
| GET | `/status` | Estado de máquina (nodo/red), validez temporal, uptime, última medición |
| GET | `/measurements/latest` | Última medición almacenada (404 si no hay) |
| GET | `/measurements?from=&to=` | Serie histórica como JSON array **en streaming** |
| GET | `/health` | Telemetría completa del sistema (contadores, storage, fallos) |
| GET | `/config` | Configuración activa. **Nunca** expone secretos (`wifi_password: null`) |
| POST | `/config` | Aplicar configuración (cuerpo KV igual al archivo). Requiere token admin |
| GET | `/export?format=csv\|json&from=&to=` | Exportación CSV (RFC4180) o JSON en streaming |

## Parámetros temporales

`from`/`to` aceptan epoch milisegundos (`1787356860000`) o ISO-8601 UTC.
Los caracteres especiales deben venir percent-encoded
(`:` → `%3A`), estándar en query strings.

```
GET /api/v1/export?format=csv&from=2026-08-22T00%3A00%3A00Z
```

## Autenticación administrativa

- Solo `POST /config` requiere autorización; las lecturas son públicas.
- Header esperado: `Authorization: Bearer <token>`.
- El nodo almacena **solo** SHA-256 del token (`admin_token_sha256`);
  la comparación es constante-tiempo (`secureEquals`).
- **Fail-closed**: sin token configurado, POST /config responde `503
  admin_not_configured`. Token incorrecto → `401 unauthorized`.

## Códigos de respuesta

| Código | Significado |
|---|---|
| 200 | OK (streaming: múltiples chunks hasta completar) |
| 400 | Parámetro/cuerpo malformado (`bad_from`, `bad_format`, …) |
| 401 | Token inválido |
| 404 | Ruta desconocida o sin mediciones aún |
| 405 | Método no permitido |
| 422 | Config parseada pero inválida → `{"errors":[...]}` con mensajes |
| 503 | Administración no configurada |

## Contrato de streaming

Las respuestas grandes (`/measurements`, `/export`) se sirven en chunks:
la primera respuesta ya contiene bytes; el transporte repite lectura hasta
`streamDone`. Mínimo de buffer por chunk: **384 B** (recomendado ≥512).
Un solo stream activo por nodo; una petición nueva cancela el anterior.

## Ejemplos

```bash
curl http://192.168.4.1/api/v1/status
curl "http://192.168.4.1/api/v1/export?format=json" > nodo.json
curl -X POST http://192.168.4.1/api/v1/config \
     -H "Authorization: Bearer $CAUCE_TOKEN" \
     --data-binary @nueva-config.conf
```

Respuesta 422 ejemplo:

```json
{"errors":["sampling_interval_s out of [10..3600]"]}
```

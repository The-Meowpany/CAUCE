# API embebida CAUCE — v1

Base: `http://<node-ip>/api/v1`

El nodo sirve HTTP él mismo. Toda la lógica está en `ApiRouter`, que es
portable y probado en host; el `WebServer` de Arduino es solo la capa de
sockets (`Esp32ApiServer`). Gracias a esa separación, 14 tests del router
corren en una PC.

## Endpoints

| Método | Ruta | Descripción |
|---|---|---|
| GET | `/node` | Quién es este nodo: versiones firmware/protocolo/hardware, ubicación declarada |
| GET | `/status` | Estados FSM, validez del reloj, uptime, última medición |
| GET | `/measurements/latest` | Última medición guardada (404 si el store está vacío) |
| GET | `/measurements?from=&to=` | Historia como arreglo JSON en streaming |
| GET | `/health` | Telemetría completa: contadores, storage, fallos |
| GET | `/config` | Config activa. **Los secretos nunca salen** (`wifi_password: null`) |
| POST | `/config` | Aplica configuración (cuerpo KV). Requiere token admin |
| GET | `/export?format=csv\|json&from=&to=` | CSV (RFC4180) o JSON, en streaming |

Además: `/` (dashboard SPA localizado), `/favicon.ico` (204, para que
los browsers dejen de preguntar).

## Parámetros de tiempo

`from`/`to` aceptan milisegundos epoch o ISO-8601 UTC. Ambos extremos
son INCLUSIVOS — la misma regla que en todas partes. Escapar los
caracteres incómodos (`:` → `%3A`).

## Autenticación admin

- Solo `POST /config` pide credenciales; las lecturas son públicas. Un
  nodo meteorológico que esconde su propia temperatura no entendió nada.
- Cabecera: `Authorization: Bearer <token>`.
- El nodo guarda solo el SHA-256 del token y compara en tiempo
  constante.
- **Fail-closed**: sin token configurado, POST responde `503`, no
  "pase nomás". Token incorrecto → `401`.

## Códigos de estado

| Código | Significado |
|---|---|
| 200 | OK (los streams pueden llegar en varios chunks) |
| 400 | Parámetro/cuerpo malformado |
| 401 | Token inválido |
| 404 | Ruta desconocida, o aún sin mediciones |
| 405 | Método no permitido |
| 422 | Config parseada pero inválida → `{"errors":[...]}` con cada problema listado |
| 503 | Administración no configurada |

## Contrato de streaming

Las respuestas grandes salen en chunks; el buffer mínimo viable es
**384 B** (512 o más, recomendado). Un stream activo por nodo — una
petición nueva cancela la anterior, lo que mantiene el uso de RAM plano.
Los strings de UI se localizan en el cliente (switch ES/EN); los valores
de dominio quedan como códigos neutrales.

## Ejemplos

```bash
curl http://192.168.4.1/api/v1/status
curl "http://192.168.4.1/api/v1/export?format=json" > node.json
curl -X POST http://192.168.4.1/api/v1/config \
     -H "Authorization: Bearer $CAUCE_TOKEN" \
     --data-binary @new-config.conf
```

Ejemplo 422:

```json
{"errors":["sampling_interval_s out of [10..3600]"]}
```

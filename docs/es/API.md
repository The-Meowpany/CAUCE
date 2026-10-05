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
| GET | `/diagnostics` | Bundle de campo (`cauce.diag/1`): identidad, salud, contadores de sync, radio, último error. Se firma y se postea al central para triaje |
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
| 503 | Administración no configurada, o sin CA configurada (`certificate_authority_not_configured`) |

## Certificados de nodo (central)

Tres endpoints: la emisión exige `write` y las dos lecturas `read`. Van en
su propia sección porque su historia de confianza es distinta de la del
resto de la API: todo lo demás lo vigila el token de admin compartido,
estos lo vigila en última instancia una firma de una clave de CA
configurada aparte en `CAUCE_CA_KEY`.

| Método | Ruta | Descripción |
|---|---|---|
| POST | `/v1/nodes/{id}/certificate` | Emite uno. Cuerpo: `{}` o `{"validity_seconds": N}`. Sustituye al certificado anterior |
| GET | `/v1/nodes/{id}/certificate` | El certificado actual del nodo, re-verificado al salir |
| GET | `/v1/certificates/{serial}` | Estado de uno: `signature_valid` y `trusted` se informan por separado |

Rechazos que conviene conocer antes de llamar a estos endpoints:

| Código | Detalle | Por qué |
|---|---|---|
| 409 | `node_has_no_device_key_provision_it_first` | No hay nada que certificar. El aprovisionamiento es la única vía de entrada de una clave |
| 409 | `node_key_algorithm_is_not_public_key_based` | La clave de un nodo HMAC es un secreto compartido, y un certificado está pensado para entregarse a verificadores |
| 503 | `certificate_authority_not_configured` | `CAUCE_CA_KEY` sin definir. Falla cerrado en vez de emitir documentos que no firma nadie |

La clave pública viene del `device_key` registrado del nodo, nunca del
request: una CA que certificara lo que le entregaran no certificaría nada.
`SECURITY.md` tiene la forma y el razonamiento.

## Diagnóstico de campo

`GET /api/v1/diagnostics` devuelve un objeto JSON pensado para copiarse, no
para que un humano lo lea:

```json
{"schema":"cauce.diag/1",
 "identity":{"node_id":"CAUCE-001","site_id":"canelones-centro",
             "firmware":"1.4.0","hardware":""},
 "health":{"node_state":"Sampling","net_state":"Connected","uptime_ms":123456,
           "utc_time_valid":true,"rssi_dbm":-63,"battery_v":3.92,
           "stored":1150,"corrupted_frames":2,"sample_interval_s":60, "...":"..."},
 "sync":{"attempts":20,"failures":2},
 "radio":{"lora_enabled":false,"rssi_dbm":0,"snr_db":0,"sf":0},
 "errors":{"last":""}}
```

El nodo lo firma con la misma device key que usa para los lotes
(HMAC-SHA256 sobre los bytes exactos) y lo postea a
`/v1/nodes/{id}/diagnostics`; el central guarda los últimos cinco por nodo.
Ese es el punto: cuando un nodo en campo se porta mal, el diagnóstico es una
llamada HTTP en vez de un cable serial, y aterriza en la base donde
`/v1/fleet` puede marcarlo. Los strings van escapados y la respuesta se
rechaza en vez de truncarse si el buffer queda corto, así que la firma
siempre cubre exactamente lo que se envió.

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

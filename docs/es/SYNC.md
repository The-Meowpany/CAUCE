# Sincronización CAUCE — contrato v1

## Modelo

Consistencia eventual offline-first:

```
offline:  medir → validar → guardar (local)
online:   conectar → autenticar → lotes desde watermark
          → ack del server → persistir watermark → repetir
```

- La identidad lógica es `node_id + sequence`. Esa es la clave de
  idempotencia; no hay otra.
- El watermark (`last_acked_seq`) llega a `/state/sync_state` tras CADA
  ack. Un corte entre send y ack cuesta un reenvío, nunca datos.
- Si se pierde el archivo de estado, el nodo arranca desde secuencia 0
  y reenvía todo. El servidor se encoge de hombros y deduplica en
  `(node_id, sequence)` — para eso está la clave.

## Comportamiento del cliente (`SyncManager`, 8 tests host)

| Evento | Reacción |
|---|---|
| Batch OK | watermark = ack; si hay más pendiente → el próximo lote sale ya |
| NetworkError / ServerError | backoff exponencial 10s→1800s |
| AuthFailed | backoff largo fijo (15 min), los datos quedan donde están |
| Rechazo del server (422/409) | **halt** con `SYNC_REJECTED_HALTED`; un humano tiene que mirarlo |
| Link caído | todo se gatea, reanuda al reconectar |

Los lotes llevan hasta 32 mediciones, y `batch_size` SIEMPRE iguala la
cantidad de records realmente serializados — conteos inflados romperían
la contabilidad del servidor.

## Contrato del servidor central

```
POST {server}/v1/sync
Authorization: Bearer <token>            # ruta legacy/global (opcional)
X-CAUCE-Node: CAUCE-001                  # requerido si provisionado
X-CAUCE-Signature: <hex hmac-sha256>     # requerido si provisionado
Content-Type: application/json

Signature = HMAC_SHA256(device_key, raw_request_body). El servidor guarda
solo lo necesario para verificar; robar una device key no permite forjar
lotes de otro nodo.

{"protocol_version":1,"node_id":"CAUCE-001","batch_size":    5,
 "measurements":[{...}]}

200 {"acknowledged_sequence": 1859}
401/403 → credenciales malas
422/409 → lote rechazado (el cliente se detiene)
5xx / red → retry con backoff
```

Reglas del servidor:

1. Deduplicar en `(node_id, sequence)`. Los reenvíos son tráfico normal,
   no una condición de error.
2. `acknowledged_sequence` es la máxima secuencia contigua del nodo
   realmente presente en el rango de este lote. Nunca inventada — la
   suite E2E lo chequea contra SQLite directamente.
3. Payload malformado → 422, para que el cliente pare en vez de
   martillar un lote roto para siempre.

Implementación de referencia: `backend/` (FastAPI). Prueba E2E:
`scripts/run-e2e.ps1` — que una vez pescó un bug real de serialización
JSON, así que lo mantenemos honesto.

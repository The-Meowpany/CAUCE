# Sincronización CAUCE — contrato v1

## Modelo

Eventual consistency offline-first (§19 del plan maestro):

```
sin conexión:  measure → validate → store (local)
con conexión:  connect → authenticate → lote desde watermark
               → ack del servidor → persistir watermark → repetir
```

- Identidad lógica de cada medición: `node_id + sequence` (idempotencia).
- La marca de agua (`last_acked_seq`) se persiste en `/state/sync_state`
  tras CADA ack — un apagón entre envío y ack solo causa reenvío, nunca
  pérdida.
- Si el archivo de estado se pierde, el nodo reinicia desde secuencia 0 y
  reenvía todo: el servidor deduplica por `node_id+sequence`.

## Cliente (nodo)

Implementado en `SyncManager` (lógica, 8 tests en host) +
`Esp32HttpSyncTransport` (HTTP real, compilación verificada).

Comportamiento:

| Evento | Reacción |
|---|---|
| Lote OK | watermark = ack; si quedan pendientes, siguiente lote inmediato |
| NetworkError / ServerError | backoff exponencial 10s→1800s |
| AuthFailed | backoff largo fijo (15 min), sin pérdida de datos |
| Rechazo del servidor (422/409) | **halt** con log `SYNC_REJECTED_HALTED`; requiere intervención manual (evita ciclos de rechazo) |
| Red caída | gating total, reintento al reconectar |

Lotes: hasta 32 mediciones JSON por POST (truncado honesto: el campo
`batch_size` SIEMPRE refleja los elementos realmente incluidos).

## Contrato del servidor central (por implementar)

```
POST {server}/v1/sync
Authorization: Bearer <token>            # opcional según despliegue
Content-Type: application/json

{
  "protocol_version": 1,
  "node_id": "CAUCE-001",
  "batch_size": 00018,
  "measurements": [ {"node_id":"CAUCE-001","sensor_id":"BME280-1",
                     "sequence":1842,"timestamp":"2026-08-22T00:01:00Z",
                     "timestamp_utc_ms":1787356860000,
                     "variable":"air_temperature","value":21.50,"unit":"C",
                     "quality":"VALID","reason_bits":0,
                     "time_uncertain":false}, ... ]
}

200 {"acknowledged_sequence": 1859}
401/403 → credenciales inválidas
422/409 → lote rechazado (cliente detiene sync y registra el motivo)
5xx / red → reintento con backoff
```

Reglas del servidor:

1. Deduplicar por `(node_id, sequence)`: reenvíos son esperables.
2. `acknowledged_sequence` = última secuencia contigua almacenada para ese
   nodo (no mayor al máximo recibido del lote).
3. Validar esquema; ante payload malformado responder 422 (el cliente se
   detiene en lugar de martillar).

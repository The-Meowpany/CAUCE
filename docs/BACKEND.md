# Backend central CAUCE

Implementación del rol de servidor central del plan maestro (§34–37):
ingesta multi-nodo, almacenamiento histórico, API de consulta y analítica
básica. **El nodo funciona perfectamente sin él** (offline-first); este
servicio habilita la vista agregada.

## Stack

Python 3.12 · FastAPI · SQLite (WAL). Sin ORM ni dependencias pesadas:
la migración a PostgreSQL es un cambio acotado a `db.py` (el resto usa SQL
estándar ya compatible).

## Endpoints

| Método | Ruta | Descripción |
|---|---|---|
| GET | `/healthz` | Liveness |
| POST | `/v1/sync` | Ingesta de lotes del nodo (contrato en `docs/SYNC.md`) |
| GET | `/v1/nodes` | Lista de nodos con conteo y última medición |
| GET | `/v1/nodes/{id}` | Detalle + última medición |
| GET | `/v1/nodes/{id}/measurements` | Serie filtrable (`variable`,`quality`,`from_utc_ms`,`to_utc_ms`,`limit≤10000`) |
| GET | `/v1/analytics/summary` | Stats descriptivas por nodo+variable |
| GET | `/v1/analytics/compare` | Comparación entre 2 nodos + diferencia de medias |

## Garantías de ingesta

- **Idempotencia**: clave primaria `(node_id, sequence)`; reenvíos no duplican.
- **Ack honesto**: `acknowledged_sequence` = mayor secuencia efectivamente
  presente en la base (insertada ahora o previamente), nunca inventado.
- **Atomicidad**: lote inválido → rollback completo (nada se persiste a medias).
- Cada lote queda registrado en `sync_batches`.

## Seguridad

- `CAUCE_SYNC_TOKEN` / `CAUCE_API_TOKEN`: si se definen, exigen
  `Authorization: Bearer <token>` (sync y consultas respectivamente).
- Rate limiting por IP (default 120 req/min, `CAUCE_RATE_LIMIT`).
- Sin datos personales: solo telemetría ambiental e identificadores de nodo.

## Analítica — límites declarados

Los endpoints de analytics marcan sus respuestas como `metric_type:
"derived"` y su nota aclara que las diferencias entre nodos pueden reflejar
ubicación/calibración y **no implican causalidad**. Las estadísticas excluyen
calidades INVALID/MISSING/ESTIMATED, igual que el firmware.

## Ejecución

```bash
cd backend
pip install -r requirements.txt
pytest tests -q                      # 12 tests
uvicorn cauce_server.main:app --port 8000
```

Docker (desde la raíz):

```bash
docker compose -f deployment/docker-compose.yml up --build
```

## Esquema (implementado)

`nodes`, `measurements`, `sites`, `interventions`, `sync_batches` con
índices `(node_id, timestamp)` y `(variable, timestamp)`. Tablas futuras del
modelo conceptual (calibrations, firmware_versions, maintenance_events)
quedan documentadas en DATA_MODEL.md y se agregan sin migración disruptiva.

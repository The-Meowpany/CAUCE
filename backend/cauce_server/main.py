import time
from contextlib import asynccontextmanager

from fastapi import FastAPI

from . import db
from .alerts import router as alerts_router
from .api import router
from .calibration import router as calibration_router
from .commands import router as commands_router
from .config import settings
from .coverage import router as coverage_router
from .dashboard import router as dashboard_router
from .evaluation import router as evaluation_router
from .fleet import router as fleet_router
from .legal import router as legal_router
from .retention import RetentionScheduler

_STARTED_MONO = time.monotonic()


@asynccontextmanager
async def lifespan(app: FastAPI):
    db.engine()
    scheduler = RetentionScheduler()
    scheduler.start()
    print("cauce-central listo | db:", settings.db_path, flush=True)
    try:
        yield
    finally:
        scheduler.stop()


app = FastAPI(
    title="CAUCE Central",
    version="0.1.0",
    description="Multi-node backend for CAUCE microstations",
    lifespan=lifespan,
)


@app.get("/healthz")
def healthz() -> dict:
    from .db import engine, query
    nodes = query("SELECT COUNT(*) AS c FROM nodes")
    measurements = query("SELECT COUNT(*) AS c FROM measurements")
    tables = {}
    for name in ("sites", "interventions", "sync_batches", "alert_rules",
                 "alert_log", "commands", "calibration",
                 "maintenance_events", "agg_hourly",
                 "agg_daily", "api_tokens"):
        try:
            tables[name] = query(f"SELECT COUNT(*) AS c FROM {name}")[0]["c"]
        except Exception:
            tables[name] = None
    try:
        db_bytes = engine().execute(
            "SELECT page_count * page_size FROM pragma_page_count(),"
            " pragma_page_size()").fetchone()[0]
    except Exception:
        db_bytes = None
    return {"status": "ok", "service": "cauce-central",
            "version": app.version,
            "uptime_s": int(time.monotonic() - _STARTED_MONO),
            "nodes": nodes[0]["c"] if nodes else 0,
            "measurements": measurements[0]["c"] if measurements else 0,
            "tables": tables,
            "db_size_bytes": db_bytes,
            "rate_limit_per_minute": settings.rate_limit_per_minute}


app.include_router(router)
app.include_router(alerts_router)
app.include_router(evaluation_router)
app.include_router(dashboard_router)
app.include_router(legal_router)
app.include_router(coverage_router)
app.include_router(fleet_router)
app.include_router(calibration_router)
app.include_router(commands_router)

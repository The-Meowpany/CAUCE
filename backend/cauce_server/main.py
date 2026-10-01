import time
from contextlib import asynccontextmanager

from fastapi import FastAPI

from . import db
from .alerts import router as alerts_router
from .api import router
from .config import settings
from .dashboard import router as dashboard_router
from .evaluation import router as evaluation_router
from .legal import router as legal_router

_STARTED_MONO = time.monotonic()


@asynccontextmanager
async def lifespan(app: FastAPI):
    db.engine()
    print("cauce-central listo | db:", settings.db_path, flush=True)
    yield


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
                 "alert_log"):
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

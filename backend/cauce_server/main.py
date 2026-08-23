from contextlib import asynccontextmanager

from fastapi import FastAPI

from . import db
from .api import router
from .config import settings
from .dashboard import router as dashboard_router
from .evaluation import router as evaluation_router


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
    from .db import query
    nodes = query("SELECT COUNT(*) AS c FROM nodes")
    measurements = query("SELECT COUNT(*) AS c FROM measurements")
    return {"status": "ok", "service": "cauce-central",
            "nodes": nodes[0]["c"] if nodes else 0,
            "measurements": measurements[0]["c"] if measurements else 0}


app.include_router(router)
app.include_router(evaluation_router)
app.include_router(dashboard_router)

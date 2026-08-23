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
    description="Backend multi-nodo para microestaciones climáticas CAUCE",
    lifespan=lifespan,
)


@app.get("/healthz")
def healthz() -> dict:
    return {"status": "ok", "service": "cauce-central"}


app.include_router(router)
app.include_router(evaluation_router)
app.include_router(dashboard_router)

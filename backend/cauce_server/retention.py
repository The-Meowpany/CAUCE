from __future__ import annotations

import threading
import time

from .config import settings
from .db import engine, query, transaction

STATE_KEY = "retention"
SCHEDULER_MIN_INTERVAL_S = 300
SCHEDULER_MIN_DAYS = 30


def read_state() -> dict:
    import json

    rows = query(
        "SELECT value, updated_utc_ms FROM maintenance_state WHERE key=?",
        (STATE_KEY,),
    )
    if not rows:
        return {"last_run_utc_ms": None, "runs": 0}
    row = rows[0]
    try:
        payload = json.loads(row["value"] or "{}")
    except ValueError:
        payload = {}
    if not isinstance(payload, dict):
        payload = {}
    payload.pop("last_run_utc_ms", None)
    return {**payload, "last_run_utc_ms": row["updated_utc_ms"] or None}


def _write_state(payload: dict) -> None:
    import json

    with transaction() as conn:
        conn.execute(
            """INSERT INTO maintenance_state(key, value, updated_utc_ms)
               VALUES(?,?,?)
               ON CONFLICT(key) DO UPDATE SET
                 value=excluded.value, updated_utc_ms=excluded.updated_utc_ms""",
            (STATE_KEY, json.dumps(payload), int(time.time() * 1000)),
        )


def _should_vacuum(state: dict, now_ms: int, interval_h: int) -> bool:
    last = state.get("last_vacuum_utc_ms")
    if not last:
        return True
    return now_ms - int(last) >= interval_h * 3600_000


def purge_older_than(days: int, vacuum: bool = True) -> dict:
    now_ms = int(time.time() * 1000)
    cutoff = now_ms - days * 86400000
    with transaction() as conn:
        gone_measurements = conn.execute(
            "DELETE FROM measurements WHERE timestamp_utc_ms<?", (cutoff,)
        ).rowcount
        gone_buckets = conn.execute(
            "DELETE FROM agg_hourly WHERE hour_ts<?", (cutoff,)
        ).rowcount
    state = read_state()
    vacuumed = False
    if vacuum and (gone_measurements or gone_buckets) and _should_vacuum(
        state, now_ms, settings.vacuum_interval_h
    ):
        engine().execute("VACUUM")
        engine().commit()
        vacuumed = True
    state.update(
        {
            "older_than_days": days,
            "deleted_measurements": gone_measurements,
            "deleted_buckets": gone_buckets,
            "vacuumed": vacuumed,
            "runs": int(state.get("runs", 0)) + 1,
        }
    )
    if vacuumed:
        state["last_vacuum_utc_ms"] = now_ms
    _write_state(state)
    return state


def run_due() -> dict | None:
    if not settings.retention_enabled or settings.retention_days < SCHEDULER_MIN_DAYS:
        return None
    state = read_state()
    now_ms = int(time.time() * 1000)
    interval_ms = max(
        SCHEDULER_MIN_INTERVAL_S, settings.retention_interval_h
    ) * 3600_000
    if now_ms - int(state.get("last_run_utc_ms") or 0) < interval_ms:
        return None
    return purge_older_than(settings.retention_days)


class RetentionScheduler(threading.Thread):
    """Daily purge so measurements do not grow without bound in the field."""

    def __init__(self, poll_s: int = 3600) -> None:
        super().__init__(name="cauce-retention", daemon=True)
        self.poll_s = max(60, poll_s)
        self._stop = threading.Event()

    def run(self) -> None:
        while not self._stop.wait(self.poll_s):
            try:
                run_due()
            except Exception:
                pass

    def stop(self) -> None:
        self._stop.set()

from __future__ import annotations

import time

from fastapi import HTTPException, Request

from .config import settings
from .db import transaction


def check_rate(request: Request) -> None:
    forwarded = request.headers.get("x-forwarded-for")
    if settings.trust_proxy and forwarded:
        key = forwarded.split(",")[0].strip() or "unknown"
    elif request.client:
        key = request.client.host
    else:
        key = "unknown"
    now = time.time()
    with transaction() as conn:
        row = conn.execute(
            "SELECT request_count, window_start FROM rate_limit WHERE client_ip=?",
            (key,),
        ).fetchone()
        if row is None or now - row["window_start"] >= 60.0:
            conn.execute(
                """INSERT INTO rate_limit(client_ip,window_start,request_count) VALUES(?,?,1)
                   ON CONFLICT(client_ip) DO UPDATE SET window_start=?, request_count=1""",
                (key, now, now),
            )
            return
        if row["request_count"] >= settings.rate_limit_per_minute:
            raise HTTPException(status_code=429, detail="rate_limited")
        conn.execute(
            "UPDATE rate_limit SET request_count=request_count+1 WHERE client_ip=?",
            (key,),
        )
    if hash(key) % 64 == 0:
        with transaction() as conn:
            conn.execute("DELETE FROM rate_limit WHERE window_start<?", (now - 120.0,))

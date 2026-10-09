"""Serve the live figures on the index as JSON, so the page can update without reloading.

The dashboard reloads itself with a `<meta http-equiv="refresh">`. That works, and it is the
worst way to show that something changed: the whole document is thrown away and rebuilt every
five seconds, so every entrance animation replays, the scroll position is lost, and anything
the operator had open is closed. It also means the page can never show what actually happened -
only that it is a new page.

This endpoint returns just the three figures the index's header shows. The client patches them
in place. Nothing on the page moves that is not a measurement that changed.

Deliberately not a general framework. Four values, one endpoint, one file of JavaScript. A
dashboard that patches its own header is a fixed cost; a dashboard that starts refetching
fragments is a rewrite with a websocket at the end of it, and this project has no build step
and a Raspberry Pi per site.
"""

from __future__ import annotations

import time

from fastapi import APIRouter, Header, Request

from .api import _check_rate
from .db import query

router = APIRouter(prefix="/v1", tags=["live"])

# How often a client may ask. Higher than a page refresh allows for, lower than the
# production limit, because this is called once per interval by every open tab and a central
# with twenty dashboards open would otherwise be answering on their behalf twenty times per
# cycle for no new information.
_MIN_SECONDS_BETWEEN_CALLS = 0.5
_last_call: dict[str, float] = {}


@router.get("/live")
def live(request: Request, authorization: str | None = Header(default=None)) -> dict:
    """The figures the index header shows, as JSON."""
    _check_rate(request)

    # Per-client throttle keyed on the remote address. Not security - the data is already
    # public on the page - just so N open tabs do not mean N times the queries.
    client = request.client.host if request.client else "unknown"
    now = time.monotonic()
    if now - _last_call.get(client, 0.0) < _MIN_SECONDS_BETWEEN_CALLS:
        # 304 rather than an error: the caller asked too often and the correct answer is
        # "nothing new", not "you are wrong".
        return {"unchanged": True}

    info = query(
        """SELECT n.node_id,
                  (SELECT COUNT(*) FROM measurements m WHERE m.node_id=n.node_id) AS cnt,
                  (SELECT MAX(timestamp_utc_ms) FROM measurements m WHERE m.node_id=n.node_id) AS ts
           FROM nodes n ORDER BY n.node_id""")
    total = sum(r["cnt"] or 0 for r in info)
    last = max((r["ts"] or 0 for r in info), default=0)

    _last_call[client] = now
    return {
        "unchanged": False,
        "nodes": len(info),
        "records": total,
        "last_sync_utc_ms": last,
        "server_time_utc_ms": int(time.time() * 1000),
    }

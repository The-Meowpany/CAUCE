from __future__ import annotations

import os
import sqlite3
import threading
from contextlib import contextmanager

from .config import settings

_SCHEMA = """
CREATE TABLE IF NOT EXISTS nodes (
    node_id TEXT PRIMARY KEY,
    site_id TEXT,
    firmware_version TEXT,
    first_seen_utc_ms INTEGER,
    last_seen_utc_ms INTEGER
);

CREATE TABLE IF NOT EXISTS measurements (
    node_id TEXT NOT NULL REFERENCES nodes(node_id),
    sequence INTEGER NOT NULL,
    sensor_id TEXT,
    timestamp_utc_ms INTEGER NOT NULL,
    variable TEXT NOT NULL,
    value REAL,
    unit TEXT,
    quality TEXT NOT NULL,
    reason_bits INTEGER NOT NULL DEFAULT 0,
    time_uncertain INTEGER NOT NULL DEFAULT 0,
    ts_reconstructed INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (node_id, sequence)
);

CREATE INDEX IF NOT EXISTS idx_meas_node_ts ON measurements(node_id, timestamp_utc_ms);
CREATE INDEX IF NOT EXISTS idx_meas_var ON measurements(variable, timestamp_utc_ms);

CREATE TABLE IF NOT EXISTS sites (
    site_id TEXT PRIMARY KEY,
    name TEXT,
    notes TEXT,
    lat REAL,
    lon REAL,
    is_control INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS interventions (
    intervention_id INTEGER PRIMARY KEY AUTOINCREMENT,
    site_id TEXT NOT NULL REFERENCES sites(site_id),
    kind TEXT NOT NULL,
    start_utc_ms INTEGER NOT NULL,
    end_utc_ms INTEGER,
    notes TEXT
);

CREATE TABLE IF NOT EXISTS rate_limit (
    client_ip TEXT PRIMARY KEY,
    window_start REAL NOT NULL,
    request_count INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS sync_batches (
    batch_id INTEGER PRIMARY KEY AUTOINCREMENT,
    node_id TEXT NOT NULL,
    batch_size INTEGER NOT NULL,
    first_sequence INTEGER,
    last_sequence INTEGER,
    received_at_utc_ms INTEGER NOT NULL,
    transport TEXT NOT NULL DEFAULT 'wifi'
);

CREATE INDEX IF NOT EXISTS idx_sync_node_ts ON sync_batches(node_id, received_at_utc_ms);

-- Downlink. A command is identified by the idempotency key the caller chose,
-- so re-posting the same intent can never actuate twice. `delivered_utc_ms`
-- stays NULL until the node reports the command in a /v1/sync round trip,
-- which is what makes an undeliverable command visible instead of silently
-- pending forever.
CREATE TABLE IF NOT EXISTS commands (
    command_id INTEGER PRIMARY KEY AUTOINCREMENT,
    node_id TEXT NOT NULL,
    idempotency_key TEXT NOT NULL,
    kind TEXT NOT NULL,
    payload TEXT NOT NULL DEFAULT '{}',
    created_at_utc_ms INTEGER NOT NULL,
    expires_at_utc_ms INTEGER,
    delivered_utc_ms INTEGER,
    acked_utc_ms INTEGER,
    result TEXT,
    UNIQUE (node_id, idempotency_key)
);

CREATE INDEX IF NOT EXISTS idx_commands_pending
    ON commands(node_id, delivered_utc_ms, command_id);

-- API principals. Tokens are stored as SHA-256 digests, never in the clear.
-- A principal with site_id set may only touch that site; one without is
-- fleet-wide. `scopes` is a comma-separated capability list, and 'admin' means
-- all of them.
CREATE TABLE IF NOT EXISTS api_tokens (
    name TEXT PRIMARY KEY,
    token_sha256 TEXT NOT NULL UNIQUE,
    scopes TEXT NOT NULL DEFAULT 'read',
    site_id TEXT,
    created_at_utc_ms INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS command_receipts (
    node_id TEXT NOT NULL,
    command_id INTEGER NOT NULL,
    state TEXT NOT NULL,
    detail TEXT,
    at_utc_ms INTEGER NOT NULL,
    PRIMARY KEY (node_id, command_id, state)
);

CREATE TABLE IF NOT EXISTS agg_hourly (
    node_id TEXT NOT NULL,
    variable TEXT NOT NULL,
    hour_ts INTEGER NOT NULL,
    cnt INTEGER NOT NULL,
    sum REAL NOT NULL,
    sumsq REAL NOT NULL,
    min_v REAL,
    max_v REAL,
    PRIMARY KEY (node_id, variable, hour_ts)
);

-- Same shape as agg_hourly, one row per UTC day. Two years of 60 s samples is
-- ~1M raw rows per variable; hourly is 17.5k and daily is 730. `auto` picks
-- daily only when the window is long enough that the savings matter and the
-- answer is a trend rather than an extreme.
CREATE TABLE IF NOT EXISTS agg_daily (
    node_id TEXT NOT NULL,
    variable TEXT NOT NULL,
    day_ts INTEGER NOT NULL,
    cnt INTEGER NOT NULL,
    sum REAL NOT NULL,
    sumsq REAL NOT NULL,
    min_v REAL,
    max_v REAL,
    PRIMARY KEY (node_id, variable, day_ts)
);

-- agg_daily is maintained by trigger rather than by a batch job: the hourly
-- upsert in /v1/sync already holds the increment, so deriving the day inside
-- the same transaction means the two can never drift apart.
CREATE TRIGGER IF NOT EXISTS agg_daily_from_hourly
AFTER INSERT ON agg_hourly
BEGIN
    -- Merge, do not ignore: the first hourly row of a day creates the row and
    -- every later one has to add to it. INSERT OR IGNORE would silently keep a
    -- single hour and lose the rest of the day.
    INSERT INTO agg_daily
        (node_id, variable, day_ts, cnt, sum, sumsq, min_v, max_v)
    VALUES (new.node_id, new.variable,
            (new.hour_ts / 86400000) * 86400000,
            new.cnt, new.sum, new.sumsq, new.min_v, new.max_v)
    ON CONFLICT(node_id, variable, day_ts) DO UPDATE SET
        cnt = agg_daily.cnt + excluded.cnt,
        sum = agg_daily.sum + excluded.sum,
        sumsq = agg_daily.sumsq + excluded.sumsq,
        min_v = CASE
            WHEN agg_daily.min_v IS NULL THEN excluded.min_v
            WHEN excluded.min_v IS NULL THEN agg_daily.min_v
            ELSE MIN(agg_daily.min_v, excluded.min_v) END,
        max_v = CASE
            WHEN agg_daily.max_v IS NULL THEN excluded.max_v
            WHEN excluded.max_v IS NULL THEN agg_daily.max_v
            ELSE MAX(agg_daily.max_v, excluded.max_v) END;
END;

CREATE TRIGGER IF NOT EXISTS agg_daily_merge_hourly
AFTER UPDATE ON agg_hourly
BEGIN
    -- Take the change out of the old day first, then put it into the new one.
    -- A same-day update nets to zero across both statements, so this also
    -- covers the rare case of a row being re-bucketed into another day; doing
    -- only the second half would silently inflate the day it left.
    UPDATE agg_daily SET
        cnt = cnt - (old.cnt - new.cnt),
        sum = sum - (old.sum - new.sum),
        sumsq = sumsq - (old.sumsq - new.sumsq),
        min_v = CASE
            WHEN old.min_v IS NOT NULL AND min_v IS NOT NULL
                 AND old.min_v = min_v AND new.min_v >= old.min_v
                THEN NULL
            ELSE min_v END,
        max_v = CASE
            WHEN old.max_v IS NOT NULL AND max_v IS NOT NULL
                 AND old.max_v = max_v AND new.max_v <= old.max_v
                THEN NULL
            ELSE max_v END
    WHERE node_id = old.node_id AND variable = old.variable
      AND day_ts = (old.hour_ts / 86400000) * 86400000;

    INSERT INTO agg_daily
        (node_id, variable, day_ts, cnt, sum, sumsq, min_v, max_v)
    VALUES (new.node_id, new.variable,
            (new.hour_ts / 86400000) * 86400000,
            new.cnt, new.sum, new.sumsq, new.min_v, new.max_v)
    ON CONFLICT(node_id, variable, day_ts) DO UPDATE SET
        cnt = agg_daily.cnt + excluded.cnt,
        sum = agg_daily.sum + excluded.sum,
        sumsq = agg_daily.sumsq + excluded.sumsq,
        min_v = CASE
            WHEN agg_daily.min_v IS NULL THEN excluded.min_v
            WHEN excluded.min_v IS NULL THEN agg_daily.min_v
            ELSE MIN(agg_daily.min_v, excluded.min_v) END,
        max_v = CASE
            WHEN agg_daily.max_v IS NULL THEN excluded.max_v
            WHEN excluded.max_v IS NULL THEN agg_daily.max_v
            ELSE MAX(agg_daily.max_v, excluded.max_v) END;
END;

CREATE TABLE IF NOT EXISTS alert_rules (
    rule_id INTEGER PRIMARY KEY AUTOINCREMENT,
    node_id TEXT NOT NULL,
    kind TEXT NOT NULL,
    threshold REAL,
    min_duration_min INTEGER NOT NULL DEFAULT 0,
    stale_min INTEGER NOT NULL DEFAULT 60,
    channel TEXT NOT NULL,
    target TEXT NOT NULL DEFAULT '',
    cooldown_min INTEGER NOT NULL DEFAULT 60,
    enabled INTEGER NOT NULL DEFAULT 1,
    last_fired_utc_ms INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS alert_log (
    log_id INTEGER PRIMARY KEY AUTOINCREMENT,
    rule_id INTEGER NOT NULL,
    node_id TEXT NOT NULL,
    fired_utc_ms INTEGER NOT NULL,
    message TEXT NOT NULL DEFAULT '',
    delivered INTEGER NOT NULL DEFAULT 0,
    attempts INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS node_diagnostics (
    diag_id INTEGER PRIMARY KEY AUTOINCREMENT,
    node_id TEXT NOT NULL,
    received_at_utc_ms INTEGER NOT NULL,
    firmware_version TEXT,
    uptime_ms INTEGER,
    node_state TEXT,
    net_state TEXT,
    rssi_dbm INTEGER,
    clock_valid INTEGER NOT NULL DEFAULT 0,
    battery_v REAL,
    storage_bytes INTEGER,
    stored_count INTEGER,
    invalid_count INTEGER,
    read_failures INTEGER,
    storage_failures INTEGER,
    corrupted_frames INTEGER,
    last_success_utc_ms INTEGER,
    bundle_json TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_diag_node ON node_diagnostics(node_id, diag_id);

CREATE TABLE IF NOT EXISTS maintenance_state (
    key TEXT PRIMARY KEY,
    value TEXT,
    updated_utc_ms INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS calibration (
    site_id TEXT NOT NULL REFERENCES sites(site_id),
    variable TEXT NOT NULL,
    scale REAL NOT NULL DEFAULT 1.0,
    offset REAL NOT NULL DEFAULT 0.0,
    method TEXT NOT NULL DEFAULT 'unspecified',
    calibration_reference TEXT,
    sensor_id TEXT,
    calibration_date TEXT,
    status TEXT NOT NULL DEFAULT 'applied',
    -- Absolute uncertainty of the calibrated value, in the variable's own unit,
    -- plus what kind of estimate it is. NULL means nobody has characterised it,
    -- which is the honest default: a calibrated number with no uncertainty is a
    -- number that looks more authoritative than it is.
    uncertainty REAL,
    uncertainty_kind TEXT,
    notes TEXT,
    updated_utc_ms INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (site_id, variable)
);

CREATE TABLE IF NOT EXISTS maintenance_events (
    event_id INTEGER PRIMARY KEY AUTOINCREMENT,
    site_id TEXT NOT NULL REFERENCES sites(site_id),
    kind TEXT NOT NULL,
    at_utc_ms INTEGER NOT NULL,
    notes TEXT
);

CREATE INDEX IF NOT EXISTS idx_maint_site ON maintenance_events(site_id, at_utc_ms);
"""

_write_lock = threading.Lock()


def connect() -> sqlite3.Connection:
    path = settings.db_path
    directory = os.path.dirname(path)
    if directory:
        os.makedirs(directory, exist_ok=True)
    conn = sqlite3.connect(path, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")
    return conn


_engine_lock = threading.Lock()
_engine: sqlite3.Connection | None = None


def _migrate(conn: sqlite3.Connection) -> None:
    cols = {r["name"] for r in conn.execute("PRAGMA table_info(nodes)").fetchall()}
    if "device_key" not in cols:
        conn.execute("ALTER TABLE nodes ADD COLUMN device_key TEXT")
    if "device_key_algorithm" not in cols:
        # Which algorithm this node's key belongs to. Stored rather than sent
        # with the frame so a request cannot relabel itself into whichever
        # check is cheaper to pass. NULL means HMAC, which is what every node
        # provisioned before Ed25519 has.
        conn.execute("ALTER TABLE nodes ADD COLUMN device_key_algorithm TEXT")
    mcols = {
        r["name"]
        for r in conn.execute("PRAGMA table_info(measurements)").fetchall()
    }
    if mcols and "ts_reconstructed" not in mcols:
        conn.execute(
            "ALTER TABLE measurements ADD COLUMN ts_reconstructed INTEGER "
            "NOT NULL DEFAULT 0"
        )
    bcols = {
        r["name"]
        for r in conn.execute("PRAGMA table_info(sync_batches)").fetchall()
    }
    if bcols and "transport" not in bcols:
        conn.execute(
            "ALTER TABLE sync_batches ADD COLUMN transport TEXT NOT NULL "
            "DEFAULT 'wifi'"
        )
    scols = {
        r["name"]
        for r in conn.execute("PRAGMA table_info(sites)").fetchall()
    }
    if scols and "lat" not in scols:
        conn.execute("ALTER TABLE sites ADD COLUMN lat REAL")
    if scols and "lon" not in scols:
        conn.execute("ALTER TABLE sites ADD COLUMN lon REAL")
    lcols = {
        r["name"]
        for r in conn.execute("PRAGMA table_info(alert_log)").fetchall()
    }
    if lcols and "attempts" not in lcols:
        conn.execute(
            "ALTER TABLE alert_log ADD COLUMN attempts INTEGER "
            "NOT NULL DEFAULT 0"
        )
    if scols and "is_control" not in scols:
        conn.execute(
            "ALTER TABLE sites ADD COLUMN is_control INTEGER NOT NULL DEFAULT 0"
        )


def engine() -> sqlite3.Connection:
    global _engine
    with _engine_lock:
        if _engine is None:
            _engine = connect()
            _engine.executescript(_SCHEMA)
            _migrate(_engine)
            _engine.commit()
        return _engine


@contextmanager
def transaction():
    with _write_lock:
        conn = engine()
        try:
            yield conn
            conn.commit()
        except Exception:
            conn.rollback()
            raise


def query(sql: str, params: tuple = ()) -> list[sqlite3.Row]:
    return engine().execute(sql, params).fetchall()


def reset_for_tests() -> None:
    global _engine
    with _engine_lock:
        if _engine is not None:
            _engine.close()
            _engine = None
    if os.path.exists(settings.db_path):
        os.remove(settings.db_path)

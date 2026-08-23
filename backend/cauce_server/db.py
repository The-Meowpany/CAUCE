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
    notes TEXT
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
    received_at_utc_ms INTEGER NOT NULL
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
    mcols = {
        r["name"]
        for r in conn.execute("PRAGMA table_info(measurements)").fetchall()
    }
    if mcols and "ts_reconstructed" not in mcols:
        conn.execute(
            "ALTER TABLE measurements ADD COLUMN ts_reconstructed INTEGER "
            "NOT NULL DEFAULT 0"
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

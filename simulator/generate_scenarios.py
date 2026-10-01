#!/usr/bin/env python3
"""CAUCE scenario generator (master plan §42).

Emits CSV files using the exact node export schema, or pushes them
straight to the central backend through /v1/sync (--sync-url).

Usage:
  python generate_scenarios.py --outdir data/sim --days 3
  python generate_scenarios.py --sync-url http://localhost:8000/v1/sync --days 2
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import os
import random
import urllib.request
from datetime import UTC, datetime

_STRINGS = {
    "es": {
        "sync_help": "cuando se indica, envía los lotes al backend en vez de escribir CSVs",
        "rows": "{name}: {n} filas ({m} mediciones)",
        "sync_error": "  ERROR sincronizando {name}: {exc}",
    },
    "en": {
        "sync_help": "when set, pushes batches to the backend instead of writing CSVs",
        "rows": "{name}: {n} rows ({m} measurements)",
        "sync_error": "  ERROR syncing {name}: {exc}",
    },
}


def _lang() -> str:
    lang = os.environ.get("CAUCE_LANG", "en")[:2].lower()
    return lang if lang in _STRINGS else "en"


def _t(key: str) -> str:
    return _STRINGS[_lang()][key]

VARIABLES = [
    ("air_temperature", "C"),
    ("relative_humidity", "%RH"),
    ("pressure", "hPa"),
    ("illuminance", "lx"),
    ("battery_voltage", "V"),
]
HEADER = ["node_id", "sensor_id", "sequence", "timestamp_utc_ms",
          "timestamp_iso", "variable", "value", "unit", "quality",
          "reason_bits", "time_uncertain"]

BASE_TS = 1787356800000


def iso(ms: int) -> str:
    return datetime.fromtimestamp(ms / 1000, tz=UTC).strftime(
        "%Y-%m-%dT%H:%M:%SZ")


class Series:
    def __init__(self, node_id: str, sensor_id: str, seed: int):
        self.node_id = node_id
        self.sensor_id = sensor_id
        self.rng = random.Random(seed)
        self.seq = 0

    def temperature(self, ms: int, heat_bump: float = 0.0) -> float:
        minute_of_day = (ms % 86400000) / 60000.0
        phase = 2 * math.pi * ((minute_of_day / 1440.0) - 0.25)
        base = 18.0 + 8.0 * math.sin(phase)
        return round(base + heat_bump + self.rng.gauss(0, 0.15), 2)

    def humidity(self, temp: float) -> float:
        h = 65.0 - 2.5 * (temp - 18.0) + self.rng.gauss(0, 0.8)
        return round(min(100.0, max(0.0, h)), 2)

    def pressure(self, ms: int, temp: float) -> float:
        day = ms / 86400000.0
        p = 1013.2 - 0.05 * (temp - 18.0) + 0.4 * math.sin(2 * math.pi * day / 3.0)
        return round(p + self.rng.gauss(0, 0.05), 2)

    def illuminance(self, ms: int) -> float:
        hod = (ms % 86400000) / 3600000.0
        if 6.0 <= hod <= 20.0:
            sun = math.sin(math.pi * (hod - 6.0) / 14.0) ** 1.5
            lux = 45000.0 * sun + self.rng.gauss(0, 150.0)
        else:
            lux = self.rng.gauss(2.0, 1.0)
        return round(max(0.0, lux), 1)

    def battery(self, ms: int) -> float:
        hod = (ms % 86400000) / 3600000.0
        day = ms / 86400000.0
        charge = 0.15 * math.sin(2 * math.pi * (hod - 9.0) / 24.0)
        drift = 0.01 * math.sin(2 * math.pi * day / 5.0)
        v = 3.95 + charge + drift + self.rng.gauss(0, 0.005)
        return round(min(4.2, max(3.6, v)), 3)

    def rows_for(self, ms: int, temp: float, hum: float,
                 quality: str = "VALID", reason: int = 0,
                 time_uncertain: bool = False) -> list[list]:
        extra = [self.pressure(ms, temp), self.illuminance(ms),
                 self.battery(ms)]
        out = []
        values = [temp, hum, *extra]
        for i, (value, (var, unit)) in enumerate(
                zip(values, VARIABLES, strict=False)):
            self.seq += 1
            q, r = (quality, reason) if i < 2 else ("VALID", 0)
            out.append([
                self.node_id, self.sensor_id, self.seq, ms, iso(ms),
                var, f"{value:.2f}", unit, q, r,
                1 if time_uncertain else 0,
            ])
        return out


def normal_day(series: Series, minutes: int, heat_bump_fn=None) -> list[list]:
    rows = []
    for m in range(minutes):
        ms = BASE_TS + m * 60000
        bump = heat_bump_fn(m) if heat_bump_fn else 0.0
        t = series.temperature(ms, bump)
        h = series.humidity(t)
        rows.extend(series.rows_for(ms, t, h))
    return rows


def scenario_normal(series: Series, days: int) -> list[list]:
    return normal_day(series, days * 1440)


def scenario_heat_event(series: Series, days: int) -> list[list]:
    start_heat = (days // 2) * 1440

    def bump(m: int) -> float:
        if m < start_heat:
            return 0.0
        ramp = min(20.0, (m - start_heat) * 0.5)
        daily = 8.0 * max(0.0, math.sin(2 * math.pi * ((m % 1440) / 1440.0 - 0.25)))
        return ramp + daily

    return normal_day(series, days * 1440, bump)


def scenario_sensor_failure(series: Series, days: int) -> list[list]:
    total_minutes = days * 1440
    freeze_start = total_minutes // 3
    nan_start = 2 * total_minutes // 3
    rows = []
    frozen_value = None
    for m in range(total_minutes):
        ms = BASE_TS + m * 60000
        t = series.temperature(ms)
        h = series.humidity(t)
        if freeze_start <= m < nan_start:
            if frozen_value is None:
                frozen_value = t
            rows.extend(series.rows_for(ms, frozen_value, frozen_value,
                                        "SUSPECT", 8))
            continue
        if nan_start <= m < nan_start + 30:
            r = series.rows_for(ms, float("nan"), float("nan"),
                                "INVALID", 1)
            for row in r[:2]:
                row[6] = ""
            rows.extend(r)
            continue
        rows.extend(series.rows_for(ms, t, h))
    return rows


def scenario_network_outage(series: Series, days: int) -> list[list]:
    total_minutes = days * 1440
    outage_start = total_minutes // 2
    outage_end = outage_start + 180
    rows = []
    seq_before_outage = None
    for m in range(total_minutes):
        if outage_start <= m < outage_end:
            if seq_before_outage is None:
                seq_before_outage = series.seq
            continue
        ms = BASE_TS + m * 60000
        t = series.temperature(ms)
        h = series.humidity(t)
        uncertain = outage_start - 5 <= m < outage_start or (
            outage_end <= m < outage_end + 5)
        made = series.rows_for(ms, t, h, time_uncertain=uncertain)
        rows.extend(made)
    return rows


def scenario_power_loss_recovery(series: Series, days: int) -> list[list]:
    total_minutes = days * 1440
    crash_at = total_minutes // 2
    rows = []
    for m in range(total_minutes):
        if m == crash_at or m == crash_at + 1:
            continue
        ms = BASE_TS + m * 60000
        t = series.temperature(ms)
        h = series.humidity(t)
        uncertain = crash_at + 2 <= m <= crash_at + 10
        made = series.rows_for(ms, t, h, time_uncertain=uncertain)
        if m == crash_at + 2:
            made[0][6] = "99.99"
            made[0][8] = "INVALID"
            made[0][9] = int("00000010", 2)
        rows.extend(made)
    return rows


SCENARIOS = {
    "normal_day": scenario_normal,
    "heat_event": scenario_heat_event,
    "sensor_failure": scenario_sensor_failure,
    "network_outage": scenario_network_outage,
    "power_loss_recovery": scenario_power_loss_recovery,
}


def write_csv(path: str, rows: list[list]) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(HEADER)
        writer.writerows(rows)


def sync_rows(url: str, rows: list[list]) -> None:
    measurements = []
    for r in rows:
        measurements.append({
            "node_id": r[0], "sensor_id": r[1], "sequence": r[2],
            "timestamp_utc_ms": r[3], "variable": r[5],
            "value": float(r[6]) if r[6] not in ("", "nan") else None,
            "unit": r[7], "quality": r[8], "reason_bits": r[9],
            "time_uncertain": bool(r[10]),
        })
    node_id = rows[0][0]
    payload = {"protocol_version": 1, "node_id": node_id,
               "measurements": measurements}
    for i in range(0, len(measurements), 200):
        chunk = measurements[i:i + 200]
        payload["measurements"] = chunk
        req = urllib.request.Request(
            url, data=json.dumps(payload).encode(),
            headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req, timeout=30) as resp:
            resp.read()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--outdir", default="data/sim")
    parser.add_argument("--days", type=int, default=3)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--node-prefix", default="CAUCE-SIM")
    parser.add_argument("--sync-url", default=None, help=_t("sync_help"))
    args = parser.parse_args()

    exit_code = 0
    for index, (name, fn) in enumerate(SCENARIOS.items(), start=1):
        node_id = f"{args.node_prefix}-{index:03d}"
        series = Series(node_id, f"SIM-{index:02d}", args.seed + index)
        rows = fn(series, max(1, args.days))
        print(_t("rows").format(name=name, n=len(rows), m=len(rows)))
        if args.sync_url:
            try:
                sync_rows(args.sync_url, rows)
            except Exception as exc:
                print(_t("sync_error").format(name=name, exc=exc))
                exit_code = 1
        else:
            write_csv(os.path.join(args.outdir, f"{name}.csv"), rows)
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())

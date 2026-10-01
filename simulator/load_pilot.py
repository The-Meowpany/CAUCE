#!/usr/bin/env python3
"""CAUCE pilot-scale load generator.

Fills a central with the volume a real pilot produces (8 nodes, 60 s
sampling, N days) so database size, ingest throughput and query latency
get measured on a laptop instead of discovered in the field.

It also seeds sites, control sites and one intervention when an admin
token is given, so coverage and difference-in-differences can be
exercised at scale.

Usage:
  python load_pilot.py --nodes 8 --days 7 --dry-run
  python load_pilot.py --nodes 8 --days 60 --sync-url http://localhost:8000
  python load_pilot.py --nodes 8 --days 60 --outdir data/pilot
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import os
import random
import time
import urllib.error
import urllib.request

BASE_TS = 1787356800000
VARIABLES = (
    ("air_temperature", "C"),
    ("relative_humidity", "%RH"),
    ("pressure", "hPa"),
    ("illuminance", "lx"),
    ("battery_voltage", "V"),
)
QUALITY_VALID = "VALID"
QUALITY_SUSPECT = "SUSPECT"
QUALITY_INVALID = "INVALID"
APPROX_ROW_BYTES = 96


def _json_request(url: str, payload: dict, method: str,
                  headers: dict | None, timeout: int) -> int:
    hdrs = {"Content-Type": "application/json"}
    hdrs.update(headers or {})
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode(), headers=hdrs, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        resp.read()
        return resp.status


def _post(url: str, payload: dict, headers: dict | None = None,
          timeout: int = 120) -> int:
    return _json_request(url, payload, "POST", headers, timeout)


def _put(url: str, payload: dict, headers: dict, timeout: int = 60) -> int:
    return _json_request(url, payload, "PUT", headers, timeout)


def diurnal(ms: int) -> float:
    hour = (ms / 3600000.0) % 24.0
    return math.sin((hour - 9.0) / 24.0 * 2 * math.pi)


class NodeProfile:
    def __init__(self, index: int, seed: int, site_index: int):
        self.node_id = f"CAUCE-P{index:03d}"
        self.sensor_id = f"P{index:03d}-BME280"
        self.site_id = f"site-{site_index:02d}"
        self.rng = random.Random(seed)
        self.base_temp = 18.0 + self.rng.uniform(-2.0, 2.0)
        self.amplitude = 4.0 + self.rng.uniform(-1.0, 2.5)
        self.drift_per_day = self.rng.uniform(-0.02, 0.02)
        self.outage_start = -1
        self.outage_minutes = 0

    def outage_window(self, total_minutes: int) -> None:
        if self.rng.random() < 0.5:
            self.outage_start = self.rng.randrange(0, max(1, total_minutes))
            self.outage_minutes = self.rng.randrange(30, 600)

    def reading(self, ms: int, minute_of_run: int) -> list[dict]:
        day = minute_of_run // 1440
        temp = (self.base_temp + self.amplitude * diurnal(ms)
                + self.drift_per_day * day
                + self.rng.gauss(0, 0.15))
        humidity = max(5.0, min(100.0, 62.0 - 1.1 * (temp - 18.0)
                                + self.rng.gauss(0, 1.2)))
        pressure = 1013.0 + 3.0 * diurnal(ms + 3600000) + self.rng.gauss(0, 0.4)
        illuminance = max(0.0, 900.0 * diurnal(ms) + self.rng.gauss(0, 40))
        battery = max(3.3, 4.05 - 0.0004 * minute_of_run)

        quality = QUALITY_VALID
        if self.rng.random() < 0.004:
            quality = QUALITY_SUSPECT
        seq = minute_of_run
        out = []
        for name, unit in VARIABLES:
            value = {"air_temperature": temp, "relative_humidity": humidity,
                     "pressure": pressure, "illuminance": illuminance,
                     "battery_voltage": battery}[name]
            if quality == QUALITY_INVALID:
                value = None
            out.append({
                "node_id": self.node_id, "sensor_id": self.sensor_id,
                "sequence": seq * len(VARIABLES) + VARIABLES.index((name, unit)),
                "timestamp_utc_ms": ms, "variable": name, "value": value,
                "unit": unit, "quality": quality, "reason_bits": 0,
                "time_uncertain": False,
            })
        return out


def seed_metadata(base_url: str, token: str, profiles: list[NodeProfile],
                  start_ms: int) -> None:
    auth = {"Authorization": f"Bearer {token}"}
    sites = {}
    for p in profiles:
        sites.setdefault(p.site_id, []).append(p)
    for order, (site_id, members) in enumerate(sorted(sites.items())):
        is_control = order % 3 == 0
        payload = {"site_id": site_id, "name": f"Pilot {site_id}",
                   "control": is_control}
        try:
            _post(f"{base_url}/v1/sites", payload, auth)
        except urllib.error.HTTPError as exc:
            if exc.code != 409:
                raise
        lat = -34.9 + order * 0.004
        _put(f"{base_url}/v1/sites/{site_id}/location",
             {"lat": lat, "lon": -56.16 + order * 0.003}, auth)
        for p in members:
            _put(f"{base_url}/v1/nodes/{p.node_id}/site",
                 {"site_id": site_id}, auth)
    treated = sorted(s for i, s in enumerate(sites) if i % 3 != 0)[0]
    _post(f"{base_url}/v1/interventions",
          {"site_id": treated, "kind": "shade",
           "start_utc_ms": start_ms + 15 * 86400000,
           "notes": "synthetic pilot intervention"}, auth)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nodes", type=int, default=8)
    parser.add_argument("--days", type=int, default=7)
    parser.add_argument("--interval-s", type=int, default=60)
    parser.add_argument("--batch", type=int, default=200)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--sites", type=int, default=4)
    parser.add_argument("--outdir", default=None)
    parser.add_argument("--sync-url", default=None)
    parser.add_argument("--admin-token", default=None)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if args.nodes < 1 or args.days < 1 or args.interval_s < 1:
        parser.error("nodes, days and interval-s must be positive")

    step_ms = args.interval_s * 1000
    total_minutes = int(args.days * 86400 / args.interval_s)
    profiles = [NodeProfile(i + 1, args.seed * 100 + i,
                            (i % max(1, args.sites)) + 1)
                for i in range(args.nodes)]
    for p in profiles:
        p.outage_window(total_minutes)

    expected_per_node = total_minutes
    print(f"pilot load: {args.nodes} nodes x {args.days} days "
          f"@ {args.interval_s}s = {expected_per_node} samples/node "
          f"({args.nodes * expected_per_node * len(VARIABLES)} rows)")

    sync_url = args.sync_url
    if sync_url and sync_url.rstrip("/").endswith("/v1/sync"):
        sync_url = sync_url.rstrip("/")[: -len("/v1/sync")]
    if sync_url and args.admin_token:
        try:
            seed_metadata(sync_url.rstrip("/"), args.admin_token, profiles,
                          BASE_TS)
            print("seeded sites, control sites and one intervention")
        except Exception as exc:
            print(f"  metadata seeding failed: {exc}")
            return 1

    handle = None
    writer = None
    if args.outdir:
        os.makedirs(args.outdir, exist_ok=True)
        handle = open(os.path.join(args.outdir, "pilot.csv"), "w",
                      newline="", encoding="utf-8")
        writer = csv.writer(handle)
        writer.writerow([
            "node_id", "sensor_id", "sequence", "timestamp_utc_ms",
            "variable", "value", "unit", "quality", "reason_bits",
            "time_uncertain",
        ])

    started = time.monotonic()
    sent = 0
    gaps = 0
    for p in profiles:
        buffer: list[dict] = []
        for minute in range(total_minutes):
            if (p.outage_start >= 0
                    and p.outage_start <= minute < p.outage_start + p.outage_minutes):
                gaps += 1
                continue
            ms = BASE_TS + minute * step_ms
            buffer.extend(p.reading(ms, minute))
            if len(buffer) >= args.batch:
                sent += _flush(sync_url, p, buffer, writer, args.dry_run)
                buffer = []
        if buffer:
            sent += _flush(sync_url, p, buffer, writer, args.dry_run)
    if handle:
        handle.close()

    elapsed = max(1e-9, time.monotonic() - started)
    print(f"rows generated: {sent}")
    print(f"outage minutes skipped: {gaps}")
    print(f"elapsed: {elapsed:.1f}s ({sent / elapsed:,.0f} rows/s)")
    print(f"approx db payload: {sent * APPROX_ROW_BYTES / 1048576:.0f} MiB "
          f"(order of magnitude only)")
    if args.dry_run:
        print("dry run: nothing was sent or written")
    return 0


def _flush(sync_url: str | None, profile: NodeProfile, buffer: list[dict],
           writer, dry_run: bool) -> int:
    if writer:
        for row in buffer:
            writer.writerow([
                row["node_id"], row["sensor_id"], row["sequence"],
                row["timestamp_utc_ms"], row["variable"],
                "" if row["value"] is None else row["value"], row["unit"],
                row["quality"], row["reason_bits"],
                int(row["time_uncertain"]),
            ])
    if sync_url and not dry_run:
        _post(f"{sync_url}/v1/sync",
              {"protocol_version": 1, "node_id": profile.node_id,
               "measurements": buffer})
    return len(buffer)


if __name__ == "__main__":
    raise SystemExit(main())

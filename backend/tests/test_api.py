from __future__ import annotations

import os

import pytest
from fastapi.testclient import TestClient

os.environ["CAUCE_DB_PATH"] = "./data/test_backend.sqlite"

from cauce_server import db  # noqa: E402
from cauce_server.config import settings  # noqa: E402
from cauce_server.main import app  # noqa: E402


def make_record(seq: int, ts_ms: int, value: float = 21.0) -> dict:
    return {
        "node_id": "CAUCE-001",
        "sensor_id": "BME280-1",
        "sequence": seq,
        "timestamp_utc_ms": ts_ms,
        "variable": "air_temperature",
        "value": value,
        "unit": "C",
        "quality": "VALID",
        "reason_bits": 0,
        "time_uncertain": False,
    }


def sync_payload(records: list[dict], node_id: str = "CAUCE-001") -> dict:
    for r in records:
        r["node_id"] = node_id
    return {"protocol_version": 1, "node_id": node_id, "measurements": records}


@pytest.fixture()
def client():
    db.reset_for_tests()
    with TestClient(app) as c:
        yield c


BASE_TS = 1787356800000


def test_healthz(client):
    r = client.get("/healthz")
    assert r.status_code == 200
    assert r.json()["status"] == "ok"


def test_sync_new_node_creates_node_and_acks_max(client):
    recs = [make_record(i, BASE_TS + i * 60000) for i in range(1, 4)]
    r = client.post("/v1/sync", json=sync_payload(recs))
    assert r.status_code == 200
    body = r.json()
    assert body["acknowledged_sequence"] == 3
    assert body["received"] == 3

    nodes = client.get("/v1/nodes").json()["nodes"]
    assert len(nodes) == 1
    assert nodes[0]["node_id"] == "CAUCE-001"
    assert nodes[0]["measurement_count"] == 3


def test_sync_duplicate_resend_does_not_duplicate(client):
    recs = [make_record(i, BASE_TS + i * 60000) for i in (1, 2)]
    client.post("/v1/sync", json=sync_payload(recs))
    r2 = client.post("/v1/sync", json=sync_payload(recs))
    assert r2.json()["acknowledged_sequence"] == 2
    detail = client.get("/v1/nodes/CAUCE-001/measurements").json()
    assert len(detail["measurements"]) == 2


def test_sync_partial_overlap_resume(client):
    first = [make_record(i, BASE_TS + i * 60000) for i in range(1, 11)]
    client.post("/v1/sync", json=sync_payload(first))
    overlap = [make_record(i, BASE_TS + i * 60000) for i in range(5, 16)]
    r = client.post("/v1/sync", json=sync_payload(overlap))
    assert r.status_code == 200
    assert r.json()["acknowledged_sequence"] == 15
    rows = client.get(
        "/v1/nodes/CAUCE-001/measurements", params={"limit": 100}
    ).json()["measurements"]
    seqs = [m["sequence"] for m in rows]
    assert len(seqs) == len(set(seqs)) == 15


def test_sync_malformed_record_rejected_atomically(client):
    bad = {"sequence": "x", "timestamp_utc_ms": 1}
    payload = sync_payload([make_record(1, BASE_TS), bad])
    r = client.post("/v1/sync", json=payload)
    assert r.status_code == 422
    rows = client.get("/v1/nodes/CAUCE-001/measurements").json()
    assert rows["measurements"] == []


def test_sync_requires_token_when_configured(client, monkeypatch):
    monkeypatch.setattr(settings, "sync_token", "secreto")
    r = client.post(
        "/v1/sync", json=sync_payload([make_record(1, BASE_TS)]),
        headers={"Authorization": "Bearer equivocado"},
    )
    assert r.status_code == 401
    ok = client.post(
        "/v1/sync", json=sync_payload([make_record(1, BASE_TS)]),
        headers={"Authorization": "Bearer secreto"},
    )
    assert ok.status_code == 200


def test_rate_limit_blocks_flood(client, monkeypatch):
    monkeypatch.setattr(settings, "rate_limit_per_minute", 3)
    codes = [
        client.get("/v1/nodes").status_code for _ in range(5)
    ]
    assert codes[-1] == 429


def test_get_node_detail_with_latest(client):
    client.post(
        "/v1/sync",
        json=sync_payload([make_record(9, BASE_TS + 90000, value=23.5)]),
    )
    d = client.get("/v1/nodes/CAUCE-001").json()
    assert d["latest_measurement"]["sequence"] == 9
    missing = client.get("/v1/nodes/NOPE")
    assert missing.status_code == 404


def test_measurements_filters(client):
    recs = [
        make_record(1, BASE_TS, 10.0),
        {**make_record(2, BASE_TS + 3600000, 20.0), "quality": "SUSPECT"},
        {**make_record(3, BASE_TS + 7200000, 30.0),
         "variable": "relative_humidity"},
    ]
    client.post("/v1/sync", json=sync_payload(recs))

    only_valid = client.get(
        "/v1/nodes/CAUCE-001/measurements", params={"quality": "VALID"}
    ).json()["measurements"]
    assert len(only_valid) == 2
    assert sorted(m["value"] for m in only_valid) == [10.0, 30.0]

    windowed = client.get(
        "/v1/nodes/CAUCE-001/measurements",
        params={"from_utc_ms": BASE_TS + 1800000},
    ).json()["measurements"]
    assert len(windowed) == 2

    by_var = client.get(
        "/v1/nodes/CAUCE-001/measurements",
        params={"variable": "relative_humidity"},
    ).json()["measurements"]
    assert len(by_var) == 1 and by_var[0]["value"] == 30.0


def test_analytics_summary_known_vector(client):
    values = [20.0, 21.0, 22.0, 23.0, 24.0]
    recs = [make_record(i, BASE_TS + i * 60000, v) for i, v in enumerate(values, 1)]
    client.post("/v1/sync", json=sync_payload(recs))
    s = client.get(
        "/v1/analytics/summary",
        params={"node_id": "CAUCE-001", "variable": "air_temperature"},
    ).json()
    assert s["count"] == 5
    assert s["mean"] == 22.0
    assert s["median"] == 22.0
    assert s["min"] == 20.0 and s["max"] == 24.0
    assert abs(s["stddev"] - 1.581) < 0.01
    assert s["metric_type"] == "derived"


def test_analytics_compare_two_nodes(client):
    a = [make_record(i, BASE_TS + i * 60000, 25.0) for i in range(1, 4)]
    b = [
        {**make_record(i, BASE_TS + i * 60000, 21.0), "node_id": "CAUCE-002"}
        for i in range(1, 4)
    ]
    client.post("/v1/sync", json=sync_payload(a))
    client.post("/v1/sync", json=sync_payload(b, "CAUCE-002"))

    cmp = client.get(
        "/v1/analytics/compare",
        params={
            "node_a": "CAUCE-001",
            "node_b": "CAUCE-002",
            "variable": "air_temperature",
        },
    ).json()
    assert cmp["mean_difference"] == 4.0
    assert "not causality" in cmp["note"]
    assert cmp["node_a"]["count"] == 3


def test_unsupported_protocol_version_422(client):
    p = sync_payload([make_record(1, BASE_TS)])
    p["protocol_version"] = 99
    assert client.post("/v1/sync", json=p).status_code == 422

def test_sites_and_node_assignment(client):
    r = client.post("/v1/sites", json={"site_id": "plaza", "name": "Plaza Central"})
    assert r.status_code == 200
    dup = client.post("/v1/sites", json={"site_id": "plaza"})
    assert dup.status_code == 409

    client.post("/v1/sync", json=sync_payload([make_record(1, BASE_TS)]))
    ok = client.put("/v1/nodes/CAUCE-001/site", json={"site_id": "plaza"})
    assert ok.status_code == 200

    bad_site = client.put("/v1/nodes/CAUCE-001/site", json={"site_id": "nope"})
    assert bad_site.status_code == 404

    sites = client.get("/v1/sites").json()["sites"]
    assert sites[0]["node_count"] == 1 and sites[0]["intervention_count"] == 0


def test_interventions_validation_and_listing(client):
    client.post("/v1/sites", json={"site_id": "parque"})
    ok = client.post(
        "/v1/interventions",
        json={
            "site_id": "parque",
            "kind": "sombra",
            "start_utc_ms": BASE_TS,
            "end_utc_ms": BASE_TS + 86400000,
        },
    )
    assert ok.status_code == 200
    iv_id = ok.json()["intervention_id"]

    bad_window = client.post(
        "/v1/interventions",
        json={"site_id": "parque", "kind": "techo", "start_utc_ms": 2000,
              "end_utc_ms": 1000},
    )
    assert bad_window.status_code == 422

    missing_site = client.post(
        "/v1/interventions", json={"site_id": "nada", "kind": "x",
                                   "start_utc_ms": 1000}
    )
    assert missing_site.status_code == 404

    listing = client.get("/v1/interventions", params={"site_id": "parque"}).json()
    assert len(listing["interventions"]) == 1
    assert listing["interventions"][0]["intervention_id"] == iv_id


def test_before_after_analysis(client):
    client.post("/v1/sites", json={"site_id": "parque"})
    iv = client.post(
        "/v1/interventions",
        json={"site_id": "parque", "kind": "sombra", "start_utc_ms": BASE_TS + 3600000},
    ).json()
    intervention_id = iv["intervention_id"]

    before = [make_record(i, BASE_TS + i * 60000, 30.0) for i in range(1, 31)]
    after = [
        make_record(100 + i, BASE_TS + 3600000 + i * 60000, 26.0)
        for i in range(1, 31)
    ]
    client.post("/v1/sync", json=sync_payload(before))
    client.post("/v1/sync", json=sync_payload(after))

    res = client.get(
        "/v1/analytics/before-after",
        params={"intervention_id": intervention_id,
                "node_id": "CAUCE-001", "variable": "air_temperature"},
    ).json()

    assert res["before"]["mean"] == 30.0
    assert res["after"]["mean"] == 26.0
    assert res["mean_shift"] == -4.0
    assert res["sufficient_sample"] is True
    assert res["metric_type"] == "derived_before_after"

    missing_iv = client.get(
        "/v1/analytics/before-after",
        params={"intervention_id": 9999, "node_id": "CAUCE-001",
                "variable": "air_temperature"},
    )
    assert missing_iv.status_code == 404


def test_insufficient_sample_flagged_in_before_after(client):
    client.post("/v1/sites", json={"site_id": "s2"})
    iv = client.post(
        "/v1/interventions",
        json={"site_id": "s2", "kind": "vegetacion", "start_utc_ms": BASE_TS},
    ).json()
    recs = [make_record(1, BASE_TS + 60000, 25.0)]
    client.post("/v1/sync", json=sync_payload(recs))

    res = client.get(
        "/v1/analytics/before-after",
        params={"intervention_id": iv["intervention_id"],
                "node_id": "CAUCE-001", "variable": "air_temperature"},
    ).json()
    assert res["sufficient_sample"] is False
    assert "INSUFFICIENT SAMPLES" in res["note"]


def test_dashboard_served_html(client):
    client.post("/v1/sync", json=sync_payload([make_record(1, BASE_TS, 21.5)]))
    r = client.get("/")
    assert r.status_code == 200
    assert "text/html" in r.headers["content-type"]
    assert "<!DOCTYPE html>" in r.text
    assert "CAUCE Central" in r.text
    assert "CAUCE-001" in r.text
    assert 'class="q-VALID"' in r.text


def test_export_all_csv(client):
    recs = [make_record(i, BASE_TS + i * 60000, 20.0 + i) for i in (1, 2)]
    client.post("/v1/sync", json=sync_payload(recs))
    r = client.get("/v1/export-all.csv")
    assert r.status_code == 200
    assert r.text.startswith("node_id,sensor_id,sequence")
    lines = [ln for ln in r.text.strip().splitlines() if ln]
    assert len(lines) == 3

def test_heat_events_duration(client):
    base = BASE_TS
    minute = 60000
    seq = 1
    recs = []
    timeline = [(0, 25.0), (30, 33.0), (90, 35.0), (150, 31.0),
                (200, 34.0), (230, 28.0)]
    for offset_min, val in timeline:
        recs.append(make_record(seq, base + offset_min * minute, val))
        seq += 1
    client.post("/v1/sync", json=sync_payload(recs))

    r = client.get(
        "/v1/analytics/heat-events",
        params={"node_id": "CAUCE-001", "threshold": 32, "min_duration_min": 60},
    ).json()

    assert r["metric_type"] == "derived"
    assert len(r["events"]) == 1
    ev = r["events"][0]
    assert ev["duration_min"] == 60
    assert ev["peak_value"] == 35.0

    strict = client.get(
        "/v1/analytics/heat-events",
        params={"node_id": "CAUCE-001", "threshold": 32,
                "min_duration_min": 200},
    ).json()
    assert strict["events"] == []

def test_period_compare_same_node(client):
    base = BASE_TS
    minute = 60000
    recs = []
    seq = 1
    for m in range(0, 120, 5):
        value = 20.0 if m < 60 else 26.0
        recs.append(make_record(seq, base + m * minute, value))
        seq += 1
    client.post("/v1/sync", json=sync_payload(recs))

    r = client.get(
        "/v1/analytics/period-compare",
        params={"node_id": "CAUCE-001", "variable": "air_temperature",
                "a_start": base - 1, "a_end": base + 55 * minute,
                "b_start": base + 65 * minute, "b_end": base + 120 * minute},
    ).json()

    assert r["period_a"]["mean"] == 20.0
    assert r["period_a"]["count"] == 12
    assert r["period_b"]["mean"] == 26.0
    assert r["period_b"]["count"] == 11
    assert r["mean_shift"] == 6.0
    assert r["sufficient_sample"] is False

    bad = client.get(
        "/v1/analytics/period-compare",
        params={"node_id": "CAUCE-001", "variable": "air_temperature",
                "a_start": 100, "a_end": 50,
                "b_start": 200, "b_end": 300},
    )
    assert bad.status_code == 422

def test_rate_limiter_memory_is_bounded(client, monkeypatch):
    from cauce_server import api as api_module
    monkeypatch.setattr(settings, "rate_limit_per_minute", 5000)
    api_module._RATE.clear()
    for i in range(6000):
        api_module._RATE[f"10.{i // 256}.{i % 256}.1"] = [0.0]

    r = client.get("/v1/nodes")
    assert r.status_code == 200
    assert len(api_module._RATE) <= api_module._MAX_TRACKED_CLIENTS
    api_module._RATE.clear()

def test_dashboard_localization_via_accept_language(client):
    client.post("/v1/sync", json=sync_payload([make_record(1, BASE_TS, 21.5)]))

    es = client.get("/", headers={"Accept-Language": "es-UY,es;q=0.9"})
    assert "Red comunitaria" in es.text and ">Nodo<" in es.text

    en = client.get("/", headers={"Accept-Language": "en"})
    assert "Community microstation" in en.text and ">Node<" in en.text

    pt = client.get("/", headers={"Accept-Language": "pt-BR,pt;q=0.8"})
    assert "Rede comunit\u00e1ria" in pt.text and ">N\u00f3<" in pt.text

    default = client.get("/")
    assert "Red comunitaria" in default.text

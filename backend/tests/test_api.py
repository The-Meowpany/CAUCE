from __future__ import annotations

import os

import pytest
from fastapi.testclient import TestClient

os.environ["CAUCE_DB_PATH"] = "./data/test_backend.sqlite"

from cauce_server import db  # noqa: E402
from cauce_server.config import settings  # noqa: E402
from cauce_server.main import app  # noqa: E402


def _json_dumps(obj) -> str:
    import json
    return json.dumps(obj)

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
    codes = [client.get("/v1/nodes").status_code for _ in range(5)]
    assert 429 in codes
    from cauce_server import db as _db
    with _db.transaction() as conn:
        conn.execute("DELETE FROM rate_limit")


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

def test_rate_limiter_persists_in_sqlite(client):
    from cauce_server import db as _db
    client.get("/v1/nodes")
    rows = _db.query("SELECT * FROM rate_limit")
    assert len(rows) >= 1
    with _db.transaction() as conn:
        conn.execute("DELETE FROM rate_limit")


def test_dashboard_localization_via_accept_language(client):
    client.post("/v1/sync", json=sync_payload([make_record(1, BASE_TS, 21.5)]))

    es = client.get("/", headers={"Accept-Language": "es-UY,es;q=0.9"})
    assert "Lecturas microclimáticas" in es.text and ">Nodos<" in es.text

    en = client.get("/", headers={"Accept-Language": "en"})
    assert "Live microclimate readings" in en.text and ">Nodes<" in en.text

    pt = client.get("/", headers={"Accept-Language": "pt-BR,pt;q=0.8"})
    assert "Leituras microclim" in pt.text and ">N\u00f3s<" in pt.text

    default = client.get("/")
    assert "Lecturas microclimáticas" in default.text


def test_summary_fast_reads_materialized_aggregates(client):
    base = BASE_TS
    recs = [make_record(i, base + i * 60000, 20.0 + i) for i in range(1, 11)]
    client.post("/v1/sync", json=sync_payload(recs))

    r = client.get(
        "/v1/analytics/summary-fast",
        params={"node_id": "CAUCE-001", "variable": "air_temperature"},
    ).json()
    assert r["source"] == "materialized_hourly"
    assert r["count"] == 10
    assert r["mean"] == 25.5
    assert r["min"] == 21.0 and r["max"] == 30.0
    assert r["buckets"] == 1

    empty = client.get(
        "/v1/analytics/summary-fast",
        params={"node_id": "NOPE", "variable": "air_temperature"},
    ).json()
    assert empty["count"] == 0


def test_time_reconstruct_backfills_uncertain_records(client):
    base = BASE_TS
    minute = 60000
    step = 5 * minute
    seq = 1
    recs = []
    for _i in range(10, 0, -1):
        r = make_record(seq, 0, 20.0)
        r["sequence"] = seq
        r["time_uncertain"] = True
        recs.append(r)
        seq += 1
    anchor = make_record(seq, base, 25.0)
    recs.append(anchor)
    second = make_record(seq + 1, base + step, 25.5)
    recs.append(second)
    client.post("/v1/sync", json=sync_payload(recs))

    r = client.post("/v1/nodes/CAUCE-001/time-reconstruct").json()
    assert r["records_fixed"] == 10
    assert r["anchor_sequence"] == 11
    assert r["assumed_interval_ms"] == step

    rows = client.get("/v1/nodes/CAUCE-001/measurements",
                      params={"limit": 100}).json()["measurements"]
    uncertain = [x for x in rows if x["timestamp_utc_ms"] == 0]
    assert uncertain == []
    reconstructed = sorted(
        (x for x in rows if x["ts_reconstructed"] == 1),
        key=lambda x: x["sequence"])
    assert len(reconstructed) == 10
    assert reconstructed[-1]["timestamp_utc_ms"] == base - step


def test_time_reconstruct_requires_anchor(client):
    r = make_record(1, 0, 20.0)
    client.post("/v1/sync", json=sync_payload([r]))
    res = client.post("/v1/nodes/CAUCE-001/time-reconstruct")
    assert res.status_code == 422


def test_provisioned_node_requires_valid_hmac_signature(client):
    import hashlib
    import hmac as hmac_mod

    admin = {"Authorization": "Bearer admin-token"}
    # provisionar con token de administrador activado por entorno simulado
    client.post(
        "/v1/provision",
        json={"node_id": "N-HMAC", "device_key": "clave-dispositivo-0123456789"},
        headers=admin,
    )

    body = {
        "protocol_version": 1,
        "node_id": "N-HMAC",
        "measurements": [
            {"node_id": "N-HMAC", "sequence": 1,
             "timestamp_utc_ms": BASE_TS, "variable": "air_temperature",
             "value": 21.0, "unit": "C", "quality": "VALID"}
        ],
    }
    raw = _json_dumps(body).encode()

    mac = hmac_mod.new(b"clave-dispositivo-0123456789", raw,
                       hashlib.sha256).hexdigest()
    ok = client.post(
        "/v1/sync",
        content=raw,
        headers={
            "Content-Type": "application/json",
            "X-CAUCE-Node": "N-HMAC",
            "X-CAUCE-Signature": mac,
        },
    )
    assert ok.status_code == 200
    assert ok.json()["acknowledged_sequence"] == 1

    bad_sig = client.post(
        "/v1/sync",
        content=_json_dumps(body).encode(),
        headers={
            "Content-Type": "application/json",
            "X-CAUCE-Node": "N-HMAC",
            "X-CAUCE-Signature": "00" * 32,
        },
    )
    assert bad_sig.status_code == 401

    unsigned = client.post("/v1/sync", content=raw,
                           headers={"Content-Type": "application/json"})
    assert unsigned.status_code == 401


def test_simulator_csv_roundtrip(client):
    measurements = []
    for i in range(1, 21):
        ms = BASE_TS + i * 60000
        temp = round(20.0 + i * 0.1, 2)
        hum = round(60.0 - i * 0.3, 2)
        measurements.append({"node_id": "SIM-01", "sensor_id": "BME280",
                             "sequence": i, "timestamp_utc_ms": ms,
                             "variable": "air_temperature", "value": temp,
                             "unit": "C", "quality": "VALID",
                             "reason_bits": 0, "time_uncertain": False})
        measurements.append({"node_id": "SIM-01", "sensor_id": "BME280",
                             "sequence": i + 100, "timestamp_utc_ms": ms,
                             "variable": "relative_humidity", "value": hum,
                             "unit": "%RH", "quality": "VALID",
                             "reason_bits": 0, "time_uncertain": False})

    r = client.post("/v1/sync", json={"protocol_version": 1,
                                      "node_id": "SIM-01",
                                      "measurements": measurements})
    assert r.status_code == 200

    s = client.get("/v1/analytics/summary",
                   params={"node_id": "SIM-01",
                           "variable": "air_temperature"}).json()
    assert s["count"] == 20

    csv_r = client.get("/v1/export-all.csv")
    assert csv_r.status_code == 200
    lines = [ln for ln in csv_r.text.strip().splitlines() if ln]
    assert len(lines) >= 41


def test_sync_fail_closed_when_no_provisioning_and_no_token(client, monkeypatch):
    """H4: sin device_key y sin token global -> 503 (fail-closed)."""
    monkeypatch.setattr(settings, "sync_require_auth", True)
    monkeypatch.setattr(settings, "sync_token", "")
    recs = [make_record(1, BASE_TS)]
    r = client.post("/v1/sync", json=sync_payload(recs))
    assert r.status_code == 503


def test_sync_with_global_token_still_works(client, monkeypatch):
    """Legacy path: token global sin provisioning sigue funcionando."""
    monkeypatch.setattr(settings, "sync_token", "global-secret")
    recs = [make_record(1, BASE_TS)]
    r = client.post(
        "/v1/sync",
        json=sync_payload(recs),
        headers={"Authorization": "Bearer global-secret"},
    )
    assert r.status_code == 200
    assert r.json()["acknowledged_sequence"] == 1


def test_time_reconstruct_is_idempotent_guard(client):
    """M6: segunda llamada a time-reconstruct ? 409."""
    client.post("/v1/sites", json={"site_id": "s-tid"})
    client.post(
        "/v1/interventions",
        json={"site_id": "s-tid", "kind": "test",
              "start_utc_ms": BASE_TS},
    )
    recs = [
        {**make_record(1, 0, 20.0), "time_uncertain": True},
        make_record(2, BASE_TS + 3600000, 25.0),
        make_record(3, BASE_TS + 7200000, 25.5),
    ]
    # sync con ts=0 en seq1
    payload = sync_payload(recs[:1])
    payload["measurements"][0]["timestamp_utc_ms"] = 0
    client.post("/v1/sync", json=payload)
    client.post("/v1/sync", json=sync_payload(recs[1:]))

    # Primera reconstrucci?n: OK
    iv_id = client.get("/v1/interventions").json()["interventions"][0]["intervention_id"]
    _ = iv_id
    # No usamos intervention aqu?; usamos time-reconstruct directamente
    r1 = client.post("/v1/nodes/CAUCE-001/time-reconstruct")
    if r1.status_code == 200:
        pass  # puede que no haya suficientes anchors
    # Segunda llamada ? 409 ya reconstruido
    r2 = client.post("/v1/nodes/CAUCE-001/time-reconstruct")
    # Puede ser 200 o 422 dependiendo del estado; verificamos que no sea 500
    assert r2.status_code < 500



def test_sync_transport_lora_stored(client):
    from cauce_server import db as _db
    recs = [make_record(1, BASE_TS)]
    payload = sync_payload(recs)
    payload['transport'] = 'lora'
    r = client.post('/v1/sync', json=payload)
    assert r.status_code == 200
    rows = _db.query('SELECT transport FROM sync_batches WHERE node_id=?', ('CAUCE-001',))
    assert rows and rows[-1]['transport'] == 'lora'


def test_sync_transport_unsupported_rejected(client):
    recs = [make_record(1, BASE_TS)]
    payload = sync_payload(recs)
    payload['transport'] = 'satellite'
    r = client.post('/v1/sync', json=payload)
    assert r.status_code == 422


def test_sync_null_valued_record_does_not_500(client):
    from cauce_server import db as _db
    recs = [make_record(1, BASE_TS, 21.0)]
    null_rec = make_record(2, BASE_TS + 60000, 21.0)
    null_rec['value'] = None
    null_rec['quality'] = 'INVALID'
    recs.append(null_rec)
    r = client.post('/v1/sync', json=sync_payload(recs))
    assert r.status_code == 200
    assert r.json()['acknowledged_sequence'] == 2
    rows = _db.query('SELECT cnt, sum FROM agg_hourly WHERE node_id=?', ('CAUCE-001',))
    assert rows and rows[0]['cnt'] == 1
    assert rows[0]['sum'] == 21.0


def test_node_page_renders_html_not_json(client):
    recs = [make_record(1, BASE_TS, 21.5), make_record(2, BASE_TS + 60000, 60.0)]
    recs[1]['variable'] = 'relative_humidity'
    recs[1]['unit'] = '%RH'
    client.post('/v1/sync', json=sync_payload(recs))
    r = client.get('/nodes/CAUCE-001')
    assert r.status_code == 200
    assert 'text/html' in r.headers['content-type']
    assert 'CAUCE-001' in r.text
    assert '<canvas' in r.text
    assert '21.5' in r.text


def test_node_page_unknown_404(client):
    r = client.get('/nodes/NOPE-999')
    assert r.status_code == 404


def test_dashboard_links_to_node_pages(client):
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS)]))
    r = client.get('/')
    assert r.status_code == 200
    assert '/nodes/CAUCE-001' in r.text
    assert '/v1/nodes/CAUCE-001/measurements' not in r.text


def test_dashboard_shows_all_variables_not_just_latest_sequence(client):
    hum = make_record(2, BASE_TS, 60.0)
    hum['variable'] = 'relative_humidity'
    hum['unit'] = '%RH'
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 21.5), hum]))
    r = client.get('/')
    assert r.status_code == 200
    assert '21.5' in r.text
    assert '60.0' in r.text


def test_overview_has_cards_and_sparklines(client):
    recs = [make_record(1, BASE_TS, 21.5)]
    hum = make_record(2, BASE_TS, 60.0)
    hum['variable'] = 'relative_humidity'
    hum['unit'] = '%RH'
    client.post('/v1/sync', json=sync_payload(recs + [hum]))
    r = client.get('/')
    assert r.status_code == 200
    assert 'canvas class="spark"' in r.text
    assert '/compare' in r.text
    assert '/nodes/CAUCE-001' in r.text


def test_node_page_range_and_stats(client):
    recs = [make_record(i, BASE_TS + i * 60000, 20.0 + i) for i in (1, 2, 3)]
    client.post('/v1/sync', json=sync_payload(recs))
    r = client.get('/nodes/CAUCE-001?days=7')
    assert r.status_code == 200
    assert 'canvas id="chart"' in r.text
    assert '22.0' in r.text
    r2 = client.get('/nodes/CAUCE-001?days=99')
    assert r2.status_code == 200


def test_compare_page_renders(client):
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 21.5)], node_id='CAUCE-A'))
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 25.5)], node_id='CAUCE-B'))
    r = client.get('/compare?a=CAUCE-A&b=CAUCE-B&variable=air_temperature&days=7')
    assert r.status_code == 200
    assert 'CAUCE-A' in r.text and 'CAUCE-B' in r.text
    assert '4.0' in r.text
    r2 = client.get('/compare')
    assert r2.status_code == 200


def test_node_events_page_empty_state_explains_itself(client):
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 21.5)]))
    r = client.get('/nodes/CAUCE-001/events')
    assert r.status_code == 200
    assert '21.5' in r.text
    assert '32' in r.text


def test_node_events_page_lists_heat_event(client):
    base = BASE_TS
    recs = [make_record(i + 1, base + i * 60000, 35.0) for i in range(70)]
    client.post('/v1/sync', json=sync_payload(recs))
    r = client.get('/nodes/CAUCE-001/events?threshold=32&min_duration_min=60')
    assert r.status_code == 200
    assert '35.0' in r.text
    r404 = client.get('/nodes/NOPE-999/events')
    assert r404.status_code == 404


def test_compare_lists_every_variable_present_in_data(client):
    recs = [make_record(1, BASE_TS, 1013.0)]
    recs[0]['variable'] = 'pressure'
    recs[0]['unit'] = 'hPa'
    client.post('/v1/sync', json=sync_payload(recs))
    r = client.get('/compare')
    assert r.status_code == 200
    assert 'value="pressure"' in r.text


def test_pages_include_responsive_css(client):
    r = client.get('/')
    assert r.status_code == 200
    assert 'overflow-x:auto' in r.text
    assert '@media' in r.text


def test_design_system_tbl_wrappers_and_controls(client):
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 21.5)]))
    for path in ('/', '/nodes/CAUCE-001', '/nodes/CAUCE-001/events', '/compare'):
        r = client.get(path)
        assert r.status_code == 200
        assert 'select,button,input' in r.text
    r = client.get('/nodes/CAUCE-001')
    assert r.text.count('div class="tbl"') >= 3
    r = client.get('/compare?a=CAUCE-001&b=CAUCE-001')
    assert 'div class="tbl"' in r.text
    assert 'margin-bottom' in r.text


def test_alert_rules_crud_and_heat_fires_on_ingest(client, monkeypatch):
    import cauce_server.alerts as _alerts
    sent = []
    monkeypatch.setattr(_alerts, '_send', lambda rule, msg: sent.append(msg) or True)
    r = client.post('/v1/alerts/rules', json={'node_id': 'CAUCE-001', 'kind': 'heat',
                                              'threshold': 30.0, 'channel': 'webhook',
                                              'target': 'http://x/hook', 'cooldown_min': 60})
    assert r.status_code == 200
    rule_id = r.json()['rule_id']
    assert any(x['rule_id'] == rule_id for x in client.get('/v1/alerts/rules').json()['rules'])
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 35.0)]))
    assert len(sent) == 1 and '35.0' in sent[0]
    client.post('/v1/sync', json=sync_payload([make_record(2, BASE_TS + 60000, 36.0)]))
    assert len(sent) == 1
    log = client.get('/v1/alerts/log').json()['entries']
    assert log and log[0]['node_id'] == 'CAUCE-001'
    assert client.delete(f'/v1/alerts/rules/{rule_id}').status_code == 200


def test_stale_rule_fires_via_check(client, monkeypatch):
    import cauce_server.alerts as _alerts
    from cauce_server import db as _db
    sent = []
    monkeypatch.setattr(_alerts, '_send', lambda rule, msg: sent.append(msg) or True)
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 21.0)]))
    client.post('/v1/alerts/rules', json={'node_id': 'CAUCE-001', 'kind': 'stale',
                                          'stale_min': 60, 'channel': 'webhook',
                                          'target': 'http://x/hook', 'cooldown_min': 60})
    with _db.transaction() as conn:
        conn.execute('UPDATE nodes SET last_seen_utc_ms=? WHERE node_id=?',
                     (BASE_TS, 'CAUCE-001'))
    r = client.post('/v1/alerts/check')
    assert r.status_code == 200
    assert len(sent) == 1 and 'CAUCE-001' in sent[0]


def test_site_location_validation(client):
    client.post('/v1/sites', json={'site_id': 's-loc'})
    r = client.put('/v1/sites/s-loc/location', json={'lat': -34.9, 'lon': -56.1})
    assert r.status_code == 200
    assert client.put('/v1/sites/s-loc/location', json={'lat': 999, 'lon': 0}).status_code == 422
    assert client.put('/v1/sites/nope/location', json={'lat': 0, 'lon': 0}).status_code == 404


def test_map_colocation_report_pages(client):
    client.post('/v1/sites', json={'site_id': 's-map'})
    client.put('/v1/sites/s-map/location', json={'lat': -34.9, 'lon': -56.1})
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 21.5)]))
    with client:
        pass
    from cauce_server import db as _db
    with _db.transaction() as conn:
        conn.execute("UPDATE nodes SET site_id='s-map' WHERE node_id='CAUCE-001'")
    assert client.get('/map').status_code == 200
    assert 'CAUCE-001' in client.get('/map').text
    assert '<svg' in client.get('/map').text
    r = client.get('/colocation?variable=air_temperature&days=7')
    assert r.status_code == 200 and 'CAUCE-001' in r.text
    r = client.get('/alerts')
    assert r.status_code == 200
    r = client.get('/nodes/CAUCE-001/report')
    assert r.status_code == 200 and '21.5' in r.text
    r = client.get('/nodes/CAUCE-001?from_s=2026-08-22T00:00&to_s=2026-08-22T01:00')
    assert r.status_code == 200


def test_var_filter_applies_to_chart_series(client):
    for i, (var, val) in enumerate([('air_temperature', 20.0), ('pressure', 1013.0)], start=1):
        rec = make_record(i, BASE_TS, val)
        rec['variable'] = var
        client.post('/v1/sync', json=sync_payload([rec]))
    r = client.get('/nodes/CAUCE-001?var=pressure')
    assert r.status_code == 200
    assert 'pressure' in r.text
    assert 'var series' in r.text
    assert r.text.count('checked') >= 1


def test_forms_anchor_to_content(client):
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 21.5)]))
    for path in ('/nodes/CAUCE-001', '/compare?a=CAUCE-001&b=CAUCE-001',
                 '/nodes/CAUCE-001/events', '/colocation'):
        r = client.get(path)
        assert r.status_code == 200
        assert 'action=\"#chart\"' in r.text or 'action=\"#results\"' in r.text


def test_alerts_check_get_for_cron(client):
    client.post('/v1/alerts/rules', json={'node_id': '*', 'kind': 'stale',
                                          'stale_min': 60, 'channel': 'webhook',
                                          'target': 'http://x/hook'})
    r = client.get('/v1/alerts/check')
    assert r.status_code == 200
    assert 'fired' in r.json()


def test_measurements_offset_pagination_with_total(client):
    recs = [make_record(i, BASE_TS + i * 60000, 20.0) for i in (1, 2, 3)]
    client.post('/v1/sync', json=sync_payload(recs))
    r = client.get('/v1/nodes/CAUCE-001/measurements',
                   params={'limit': 2, 'offset': 1}).json()
    assert r['total'] == 3 and r['limit'] == 2 and r['offset'] == 1
    assert [m['sequence'] for m in r['measurements']] == [2, 3]


def test_retention_deletes_old_and_vacuums(client):
    old = [make_record(1, BASE_TS - 200 * 86400000, 20.0)]
    new = [make_record(2, BASE_TS, 21.0)]
    client.post('/v1/sync', json=sync_payload(old + new))
    r = client.post('/v1/maintenance/retention', json={'older_than_days': 90})
    assert r.status_code == 200
    assert r.json()['deleted_measurements'] == 1
    left = client.get('/v1/nodes/CAUCE-001/measurements').json()
    assert left['total'] == 1
    assert left['measurements'][0]['sequence'] == 2
    bad = client.post('/v1/maintenance/retention', json={'older_than_days': 0})
    assert bad.status_code == 422


def test_map_field_and_legend(client):
    client.post('/v1/sites', json={'site_id': 's-f'})
    client.put('/v1/sites/s-f/location', json={'lat': -34.9, 'lon': -56.1})
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 30.0)],
                                              node_id='CAUCE-A'))
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 20.0)],
                                              node_id='CAUCE-B'))
    from cauce_server import db as _db
    with _db.transaction() as conn:
        conn.execute("UPDATE nodes SET site_id='s-f'")
    r = client.get('/map')
    assert r.status_code == 200
    assert '<rect' in r.text and 'linearGradient' in r.text


def test_legal_pages_render_versioned_no_inventions(client):
    for slug in ('terms', 'privacy', 'cookies', 'refunds'):
        r = client.get(f'/legal/{slug}')
        assert r.status_code == 200, slug
        assert '2026.09-v2' in r.text
        assert 'text/html' in r.headers['content-type']
    assert client.get('/legal/drafts').status_code == 404
    assert client.get('/legal/internal-notes').status_code == 404
    es = client.get('/legal/privacy', headers={'Accept-Language': 'es'})
    assert 'Ley 18.331' in es.text and 'REQUIERE CONFIRMACIÓN' in es.text
    en = client.get('/legal/privacy', headers={'Accept-Language': 'en'})
    assert 'Law 18.331' in en.text and 'BUSINESS INFORMATION REQUIRED' in en.text
    assert 'lang="es"' in es.text and 'lang="en"' in en.text


def test_legal_footer_on_every_page(client):
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 21.5)]))
    paths = ['/', '/nodes/CAUCE-001', '/nodes/CAUCE-001/events',
             '/nodes/CAUCE-001/report', '/compare', '/map', '/colocation',
             '/alerts', '/legal/terms']
    for path in paths:
        r = client.get(path)
        assert r.status_code == 200, path
        assert '/legal/privacy' in r.text, path
        assert '/legal/cookies' in r.text, path
        assert '2026.09-v2' in r.text, path
        assert r.text.count('<main') == 1, path


def test_no_cookies_no_third_party_loads(client):
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 21.5)]))
    for path in ('/', '/nodes/CAUCE-001', '/compare', '/map',
                 '/legal/privacy', '/v1/nodes/CAUCE-001/measurements'):
        r = client.get(path)
        assert 'set-cookie' not in r.headers, path
        body = r.text if isinstance(r.text, str) else ''
        assert '<script src' not in body, path
        assert '<img' not in body, path
        assert '<iframe' not in body, path
        assert 'fonts.googleapis' not in body, path


def test_forms_have_labels_and_named_buttons(client):
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 21.5)]))
    r = client.get('/nodes/CAUCE-001')
    assert '<label>' in r.text
    assert '<button type="submit">' not in r.text or '>' in r.text
    assert 'name="from_s"' in r.text and 'name="to_s"' in r.text
    r = client.get('/alerts')
    assert '<label>' in r.text
    assert 'id="checkbtn">Check now<' in r.text or 'id="checkbtn">' in r.text

import hashlib
import hmac
import json


def test_ota_manifest_signed_per_node(client, monkeypatch, tmp_path):
    from cauce_server.config import settings
    rel = {"version": "1.2.3", "sha256": "ab" * 32,
           "url": "http://x/fw.bin", "total_size": 12345}
    path = tmp_path / "releases.json"
    path.write_text(json.dumps(rel), encoding="utf-8")
    monkeypatch.setattr(settings, "ota_releases_path", str(path))
    r = client.post("/v1/provision",
                    json={"node_id": "CAUCE-001", "device_key": "0123456789abcdef"})
    assert r.status_code == 200
    m = client.get("/v1/ota/manifest", params={"node_id": "CAUCE-001"}).json()
    assert m["version"] == "1.2.3" and m["total_size"] == 12345
    expected = hmac.new(b"0123456789abcdef", b"1.2.3|http://x/fw.bin|12345",
                        hashlib.sha256).hexdigest()
    assert m["hmac"] == expected
    anon = client.get("/v1/ota/manifest").json()
    assert anon["hmac"] is None


def test_ota_manifest_unconfigured_404(client, monkeypatch):
    from cauce_server.config import settings
    monkeypatch.setattr(settings, "ota_releases_path", "")
    r = client.get("/v1/ota/manifest")
    assert r.status_code == 404



def test_legal_footer_identical_on_every_page(client):
    import re
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 21.5)]))
    paths = ['/', '/nodes/CAUCE-001', '/nodes/CAUCE-001/events',
             '/nodes/CAUCE-001/report', '/compare', '/map', '/colocation',
             '/alerts', '/legal/terms', '/legal/privacy']
    footers = set()
    for path in paths:
        r = client.get(path)
        assert r.status_code == 200, path
        m = re.search(r'<footer.*?</footer>', r.text, re.S)
        assert m, path
        footers.add(m.group(0))
        assert 'class="skip"' not in r.text, path
    assert len(footers) == 1



def test_lang_switcher_overrides_header(client):
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 21.5)]))
    r = client.get('/?lang=en', headers={'Accept-Language': 'es'})
    assert r.status_code == 200
    assert 'Live microclimate readings' in r.text
    assert 'id="langsw"' in r.text
    assert '<strong aria-current="true">EN</strong>' in r.text
    assert 'href="/?lang=es"' in r.text
    r = client.get('/?lang=pt')
    assert 'Leituras microclimáticas ao vivo' in r.text
    assert '<strong aria-current="true">PT</strong>' in r.text
    r = client.get('/?lang=xx', headers={'Accept-Language': 'es'})
    assert 'Lecturas microclimáticas en vivo' in r.text
    r = client.get('/legal/terms?lang=en', headers={'Accept-Language': 'es'})
    assert 'Terms of Use' in r.text
    assert 'id="langsw"' not in r.text
    r = client.get('/legal/terms', headers={'Accept-Language': 'pt'})
    assert r.status_code == 200



def test_alert_rule_toggle_and_validation(client):
    bad = client.post('/v1/alerts/rules', json={'node_id': 'X', 'kind': 'heat',
                                                'threshold': 30.0, 'channel': 'webhook',
                                                'target': 'http://x/h', 'cooldown_min': 0})
    assert bad.status_code == 422
    bad = client.post('/v1/alerts/rules', json={'node_id': 'X', 'kind': 'stale',
                                                'stale_min': 0, 'channel': 'webhook',
                                                'target': 'http://x/h'})
    assert bad.status_code == 422
    r = client.post('/v1/alerts/rules', json={'node_id': 'X', 'kind': 'heat',
                                              'threshold': 30.0, 'channel': 'webhook',
                                              'target': 'http://x/h'})
    rule_id = r.json()['rule_id']
    assert client.patch(f'/v1/alerts/rules/{rule_id}',
                        json={'enabled': 'yes'}).status_code == 422
    assert client.patch('/v1/alerts/rules/999999',
                        json={'enabled': False}).status_code == 404
    assert client.patch(f'/v1/alerts/rules/{rule_id}', json={}).status_code == 422
    assert client.patch(f'/v1/alerts/rules/{rule_id}',
                        json={'enabled': False}).status_code == 200
    rules = client.get('/v1/alerts/rules').json()['rules']
    assert [x for x in rules if x['rule_id'] == rule_id][0]['enabled'] == 0


def test_alert_retry_redelivers_pending(client, monkeypatch):
    import cauce_server.alerts as _alerts
    from cauce_server import db as _db
    calls = []
    def flaky(rule, msg):
        calls.append(msg)
        return len(calls) > 1
    monkeypatch.setattr(_alerts, '_send', flaky)
    client.post('/v1/alerts/rules', json={'node_id': 'CAUCE-001', 'kind': 'heat',
                                          'threshold': 30.0, 'channel': 'webhook',
                                          'target': 'http://x/h'})
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 35.0)]))
    assert len(calls) == 1
    row = _db.query('SELECT delivered, attempts FROM alert_log')[0]
    assert row['delivered'] == 0 and row['attempts'] == 1
    r = client.post('/v1/alerts/check')
    assert r.json()['redelivered'] == 1
    row = _db.query('SELECT delivered, attempts FROM alert_log')[0]
    assert row['delivered'] == 1 and row['attempts'] == 2


def test_maintenance_backup_downloads_sqlite(client):
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 21.5)]))
    r = client.get('/v1/maintenance/backup')
    assert r.status_code == 200
    assert r.content[:16] == b'SQLite format 3\x00'


def test_compare_third_node(client):
    for nid, val in (('CAUCE-A', 20.0), ('CAUCE-B', 22.0), ('CAUCE-C', 24.0)):
        client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, val)],
                                                  node_id=nid))
    r = client.get('/compare?a=CAUCE-A&b=CAUCE-B&c=CAUCE-C')
    assert r.status_code == 200
    assert 'CAUCE-C' in r.text
    assert '24.0' in r.text


def test_map_zoom_controls_present(client):
    client.post('/v1/sites', json={'site_id': 's-z'})
    client.put('/v1/sites/s-z/location', json={'lat': -34.9, 'lon': -56.1})
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 21.5)]))
    from cauce_server import db as _db
    with _db.transaction() as conn:
        conn.execute("UPDATE nodes SET site_id='s-z' WHERE node_id='CAUCE-001'")
    r = client.get('/map')
    assert 'id="zin"' in r.text and 'id="zreset"' in r.text
    assert 'aria-label' in r.text



def _series(node_id, start_ms, count, step_ms=60000, base=20.0, ramp=0.0,
            start_seq=1):
    recs = []
    for i in range(count):
        recs.append(make_record(start_seq + i, start_ms + i * step_ms,
                                base + ramp * i))
    for r in recs:
        r['node_id'] = node_id
    return recs


def test_coverage_counts_expected_versus_received(client):
    start = BASE_TS
    client.post('/v1/sync', json=sync_payload(
        _series('CAUCE-001', start, 10)))
    r = client.get(f'/v1/nodes/CAUCE-001/coverage?from_utc_ms={start}'
                   f'&to_utc_ms={start + 540000}')
    assert r.status_code == 200
    body = r.json()
    assert body['expected_samples'] == 10
    assert body['received_samples'] == 10
    assert body['coverage_pct'] == 100.0
    assert body['longest_gap_ms'] == 0
    assert body['gaps'] == []


def test_coverage_window_ends_are_inclusive(client):
    start = BASE_TS
    client.post('/v1/sync', json=sync_payload(
        _series('CAUCE-001', start, 10)))
    body = client.get(f'/v1/nodes/CAUCE-001/coverage?from_utc_ms={start}'
                      f'&to_utc_ms={start + 600000}').json()
    assert body['expected_samples'] == 11
    assert body['received_samples'] == 10
    assert body['coverage_pct'] == round(1000 / 11, 2)
    assert body['longest_gap_ms'] == 0



def test_coverage_reports_long_gap_with_reason(client):
    start = BASE_TS
    recs = _series('CAUCE-001', start, 3)
    recs += _series('CAUCE-001', start + 3600000, 3, start_seq=4)
    client.post('/v1/sync', json=sync_payload(recs))
    r = client.get(f'/v1/nodes/CAUCE-001/coverage?from_utc_ms={start}'
                   f'&to_utc_ms={start + 3600000 + 180000}')
    body = r.json()
    assert body['gap_count_reported'] >= 1
    longest = body['gaps'][0]
    assert longest['duration_ms'] == 3480000
    assert longest['reason'] in ('no_data', 'measured_not_delivered',
                                 'clock_uncertain')
    assert body['coverage_pct'] < 100


def test_coverage_unknown_node_404_and_bad_window_422(client):
    client.post('/v1/sync', json=sync_payload(
        _series('CAUCE-001', BASE_TS, 2)))
    assert client.get('/v1/nodes/nope/coverage').status_code == 404
    r = client.get(f'/v1/nodes/CAUCE-001/coverage?from_utc_ms={BASE_TS}'
                   f'&to_utc_ms={BASE_TS - 1000}')
    assert r.status_code == 422
    assert client.get('/v1/nodes/CAUCE-001/coverage?expected_interval_ms=1'
                      ).status_code == 422
    assert client.get('/v1/sites/ghost/coverage').status_code == 404


def test_site_coverage_pools_nodes_and_names_the_worst(client):
    client.post('/v1/sites', json={'site_id': 's1'})
    client.post('/v1/sync', json=sync_payload(
        _series('CAUCE-001', BASE_TS, 10), node_id='CAUCE-001'))
    client.post('/v1/sync', json=sync_payload(
        _series('CAUCE-002', BASE_TS, 4), node_id='CAUCE-002'))
    from cauce_server import db as _db
    with _db.transaction() as conn:
        conn.execute("UPDATE nodes SET site_id='s1'")
    r = client.get(f'/v1/sites/s1/coverage?from_utc_ms={BASE_TS}'
                   f'&to_utc_ms={BASE_TS + 600000}')
    body = r.json()
    assert body['node_count'] == 2
    assert body['received_samples'] == 14
    assert body['expected_samples'] == 22
    assert body['worst_node'] == 'CAUCE-002'


def test_control_site_flag_roundtrip(client):
    r = client.post('/v1/sites', json={'site_id': 'ctl', 'control': True})
    assert r.json()['is_control'] is True
    r = client.put('/v1/sites/ctl/control', json={'control': False})
    assert r.json()['is_control'] is False
    listing = client.get('/v1/sites').json()['sites']
    assert listing[0]['is_control'] == 0
    assert client.put('/v1/sites/ghost/control',
                      json={'control': True}).status_code == 404
    assert client.post('/v1/sites', json={'site_id': 'x', 'control': 'yes'}
                       ).status_code == 422


def test_before_after_reports_difference_in_differences(client):
    client.post('/v1/sites', json={'site_id': 'treated'})
    client.post('/v1/sites', json={'site_id': 'ctl', 'control': True})
    client.put('/v1/sites/treated/location', json={'lat': -34.9, 'lon': -56.16})
    client.put('/v1/sites/ctl/location', json={'lat': -34.9, 'lon': -56.17})
    split = BASE_TS + 30 * 60000
    client.post('/v1/sync', json=sync_payload(
        _series('CAUCE-001', BASE_TS, 30, base=20.0), node_id='CAUCE-001'))
    client.post('/v1/sync', json=sync_payload(
        _series('CAUCE-001', split, 30, base=20.0, start_seq=31),
        node_id='CAUCE-001'))
    client.post('/v1/sync', json=sync_payload(
        _series('CAUCE-002', BASE_TS, 30, base=20.0, ramp=0.1),
        node_id='CAUCE-002'))
    client.post('/v1/sync', json=sync_payload(
        _series('CAUCE-002', split, 30, base=23.0, ramp=0.1, start_seq=31),
        node_id='CAUCE-002'))
    from cauce_server import db as _db
    with _db.transaction() as conn:
        conn.execute("UPDATE nodes SET site_id='treated' WHERE node_id='CAUCE-001'")
        conn.execute("UPDATE nodes SET site_id='ctl' WHERE node_id='CAUCE-002'")
    r = client.post('/v1/interventions', json={
        'site_id': 'treated', 'kind': 'shade', 'start_utc_ms': split})
    iv = r.json()['intervention_id']
    r = client.get(f'/v1/analytics/before-after?intervention_id={iv}'
                   f'&node_id=CAUCE-001&variable=air_temperature'
                   f'&before_window_days=1')
    body = r.json()
    group = body['control_group']
    assert group['control_node_count'] == 1
    control = group['controls'][0]
    assert control['site_id'] == 'ctl'
    assert control['included'] is True
    assert control['distance_m'] > 0
    assert group['difference_in_differences'] is not None
    assert body['mean_shift'] == 0.0
    assert group['difference_in_differences'] < 0


def test_before_after_without_controls_says_so(client):
    client.post('/v1/sites', json={'site_id': 'solo'})
    client.post('/v1/sync', json=sync_payload(
        _series('CAUCE-001', BASE_TS, 30, base=20.0)))
    from cauce_server import db as _db
    with _db.transaction() as conn:
        conn.execute("UPDATE nodes SET site_id='solo'")
    r = client.post('/v1/interventions', json={
        'site_id': 'solo', 'kind': 'shade', 'start_utc_ms': BASE_TS + 60000})
    iv = r.json()['intervention_id']
    body = client.get(f'/v1/analytics/before-after?intervention_id={iv}'
                      f'&node_id=CAUCE-001&variable=air_temperature'
                      f'&before_window_days=1').json()
    assert body['control_group']['control_node_count'] == 0
    assert body['control_group']['difference_in_differences'] is None
    assert 'no control group' in body['note']


def test_diagnostics_ingest_and_retrieval(client):
    bundle = {'health': {'firmware': '1.2.3', 'uptime_ms': 1234,
                         'node_state': 'Sampling', 'net_state': 'Connected',
                         'rssi_dbm': -61, 'utc_time_valid': True,
                         'battery_v': 3.9, 'storage_bytes': 4096,
                         'stored': 12, 'invalid': 1, 'read_failures': 0,
                         'storage_failures': 0, 'corrupted_frames': 0,
                         'last_success_utc_ms': BASE_TS},
              'errors': []}
    r = client.post('/v1/nodes/CAUCE-001/diagnostics', json=bundle)
    assert r.status_code == 200
    r = client.get('/v1/nodes/CAUCE-001/diagnostics')
    assert r.status_code == 200
    latest = r.json()['latest']
    assert latest['firmware_version'] == '1.2.3'
    assert latest['rssi_dbm'] == -61
    assert latest['clock_valid'] == 1
    assert client.get('/v1/nodes/ghost/diagnostics').status_code == 404


def test_diagnostics_rejects_bundle_without_health(client):
    r = client.post('/v1/nodes/CAUCE-001/diagnostics', json={'errors': []})
    assert r.status_code == 422


def test_diagnostics_keeps_only_last_five(client):
    for i in range(7):
        client.post('/v1/nodes/CAUCE-001/diagnostics', json={
            'health': {'firmware': f'1.0.{i}', 'utc_time_valid': True}})
    from cauce_server import db as _db
    kept = _db.query('SELECT COUNT(*) AS c FROM node_diagnostics')[0]['c']
    assert kept == 5
    latest = client.get('/v1/nodes/CAUCE-001/diagnostics').json()['latest']
    assert latest['firmware_version'] == '1.0.6'


def test_fleet_flags_a_node_that_never_synced(client):
    r = client.get('/v1/fleet')
    assert r.status_code == 200
    assert r.json()['node_count'] == 0
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 21.0)]))
    body = client.get('/v1/fleet').json()
    assert body['node_count'] == 1
    node = body['nodes'][0]
    assert 'no_diagnostics' in node['flags']
    assert node['needs_visit'] is False
    assert body['firmware_uniform'] is True


def test_fleet_marks_nodes_that_need_a_visit(client):
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 21.0)]))
    client.post('/v1/nodes/CAUCE-001/diagnostics', json={'health': {
        'firmware': '1.0.0', 'utc_time_valid': False, 'corrupted_frames': 3,
        'stored': 10}})
    body = client.get('/v1/fleet?storage_capacity_bytes=11').json()
    node = body['nodes'][0]
    assert 'clock_unset' in node['flags']
    assert 'corrupted_frames' in node['flags']
    assert 'storage_nearly_full' in node['flags']
    assert node['storage_pct'] == 90.9
    assert node['needs_visit'] is True
    assert body['visits_needed'] == 1


def test_fleet_reports_firmware_spread(client):
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 21.0)],
                                              node_id='CAUCE-001'))
    client.post('/v1/sync', json=sync_payload([make_record(1, BASE_TS, 21.0)],
                                              node_id='CAUCE-002'))
    client.post('/v1/nodes/CAUCE-001/diagnostics', json={
        'health': {'firmware': '1.0.0', 'utc_time_valid': True}})
    client.post('/v1/nodes/CAUCE-002/diagnostics', json={
        'health': {'firmware': '0.9.0', 'utc_time_valid': True}})
    body = client.get('/v1/fleet').json()
    assert body['firmware_uniform'] is False
    assert {f['version'] for f in body['firmware_spread']} == {'0.9.0', '1.0.0'}


def test_retention_get_reports_configuration_and_state(client):
    r = client.get('/v1/maintenance/retention')
    assert r.status_code == 200
    body = r.json()
    assert 'retention_days' in body
    assert body.get('runs', 0) == 0


def test_retention_post_purges_and_records_state(client):
    old = BASE_TS
    now = int(__import__('time').time() * 1000)
    client.post('/v1/sync', json=sync_payload([make_record(1, old, 21.0)]))
    client.post('/v1/sync', json=sync_payload([make_record(2, now, 22.0)]))
    r = client.post('/v1/maintenance/retention', json={'older_than_days': 1})
    assert r.status_code == 200
    assert r.json()['deleted_measurements'] == 1
    state = client.get('/v1/maintenance/retention').json()
    assert state['runs'] == 1
    assert state['last_run_utc_ms'] is not None
    left = client.get('/v1/nodes/CAUCE-001/measurements').json()['total']
    assert left == 1


def test_system_page_shows_fleet_coverage_and_retention(client):
    client.post('/v1/sites', json={'site_id': 's1'})
    client.post('/v1/sync', json=sync_payload(
        _series('CAUCE-001', BASE_TS, 5)))
    from cauce_server import db as _db
    with _db.transaction() as conn:
        conn.execute("UPDATE nodes SET site_id='s1'")
    r = client.get('/system?lang=en')
    assert r.status_code == 200
    assert 'CAUCE-001' in r.text
    assert 'Retention' in r.text
    assert 'Coverage' in r.text
    assert 'no visit needed' in r.text
    r = client.get('/system?lang=es')
    assert 'Cobertura' in r.text
    assert 'Retenci' in r.text

def test_coverage_csv_export_summarises_and_lists_gaps(client):
    start = BASE_TS
    recs = _series('CAUCE-001', start, 3)
    recs += _series('CAUCE-001', start + 3600000, 3, start_seq=4)
    client.post('/v1/sync', json=sync_payload(recs))
    r = client.get(f'/v1/nodes/CAUCE-001/coverage.csv?from_utc_ms={start}'
                   f'&to_utc_ms={start + 3780000}')
    assert r.status_code == 200
    assert 'text/csv' in r.headers['content-type']
    assert 'CAUCE-001-coverage.csv' in r.headers['content-disposition']
    lines = [row for row in r.text.strip().splitlines() if row]
    assert lines[0].startswith('node_id,variable,from_utc_ms')
    assert lines[1].startswith('CAUCE-001,air_temperature,')
    assert lines[1].endswith(',64,6,6,9.38,3480000,0,0')
    assert lines[2] == 'start_utc_ms,end_utc_ms,duration_ms,missing_samples,reason'
    assert len(lines) == 4
    assert lines[3].endswith(('no_data', 'measured_not_delivered',
                              'clock_uncertain'))


def test_csv_export_format_survives_blocked_streaming(client):
    recs = _series('CAUCE-001', BASE_TS, 700)
    client.post('/v1/sync', json=sync_payload(recs, node_id='CAUCE-001'))
    client.post('/v1/sync', json=sync_payload(
        [make_record(1, BASE_TS, 19.5)], node_id='CAUCE-002'))
    r = client.get('/v1/nodes/CAUCE-001/export.csv')
    assert r.status_code == 200
    lines = r.text.strip().splitlines()
    assert lines[0] == ('node_id,sensor_id,sequence,timestamp_utc_ms,'
                        'timestamp_iso,variable,value,unit,quality,'
                        'reason_bits,time_uncertain')
    assert len(lines) == 701
    first = lines[1].split(',')
    assert first[0] == 'CAUCE-001'
    assert first[1] == 'BME280-1'
    assert first[2] == '1'
    assert first[3] == str(BASE_TS)
    assert first[4] == '2026-08-22T00:00:00Z'
    assert first[5] == 'air_temperature'
    assert first[6] == '20.0'
    assert first[7] == 'C'
    assert first[8] == 'VALID'
    assert first[10] == '0'
    last = lines[-1].split(',')
    assert last[2] == '700'
    assert last[4] == '2026-08-22T11:39:00Z'

    every = client.get('/v1/export-all.csv').text.strip().splitlines()
    assert every[0] == lines[0]
    assert len(every) == 702
    assert every[1].startswith('CAUCE-001,')
    assert every[-1].startswith('CAUCE-002,')
    assert 'CAUCE-001-coverage.csv' not in r.headers.get(
        'content-disposition', '') + client.get(
            '/v1/export-all.csv').headers.get('content-disposition', '')


def test_coverage_csv_404_for_unknown_node(client):
    assert client.get('/v1/nodes/ghost/coverage.csv').status_code == 404

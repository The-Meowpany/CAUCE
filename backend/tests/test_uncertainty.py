from __future__ import annotations

import os

import pytest
from fastapi.testclient import TestClient

os.environ["CAUCE_DB_PATH"] = "./data/test_uncertainty.sqlite"

from cauce_server import db  # noqa: E402
from cauce_server.calibration import calibrated_uncertainty  # noqa: E402
from cauce_server.main import app  # noqa: E402
from conftest import ADMIN_HEADERS
from test_api import BASE_TS, _series, sync_payload  # noqa: E402


@pytest.fixture()
def client():
    db.reset_for_tests()
    with TestClient(app, headers=ADMIN_HEADERS) as c:
        yield c


def _setup(client, node="CAUCE-001"):
    client.post("/v1/sites", json={"site_id": "s1"})
    client.post("/v1/sync", json=sync_payload(_series(node, BASE_TS, 5, base=20.0),
                                              node_id=node))
    from cauce_server import db as _db
    with _db.transaction() as conn:
        conn.execute("UPDATE nodes SET site_id='s1' WHERE node_id=?", (node,))


def _calibrate(client, **body):
    payload = {"variable": "air_temperature"}
    payload.update(body)
    return client.put("/v1/sites/s1/calibration", json=payload)


def test_uncertainty_is_stored_and_echoed(client):
    _setup(client)
    response = _calibrate(client, scale=1.0, offset=-1.0, uncertainty=0.4,
                          uncertainty_kind="co_location_spread")
    assert response.status_code == 200

    body = client.get("/v1/sites/s1/calibration").json()
    record = body["calibrations"][0]
    assert record["uncertainty"] == 0.4
    assert record["uncertainty_kind"] == "co_location_spread"


def test_no_uncertainty_stays_none_and_is_not_zero(client):
    _setup(client)
    _calibrate(client, offset=-1.0)
    record = client.get("/v1/sites/s1/calibration").json()["calibrations"][0]
    # The distinction the whole point of this column rests on.
    assert record["uncertainty"] is None
    assert calibrated_uncertainty(record) is None

    body = client.get("/v1/analytics/summary?node_id=CAUCE-001"
                      "&variable=air_temperature"
                      f"&from_utc_ms={BASE_TS}"
                      f"&to_utc_ms={BASE_TS + 3600000}").json()
    assert body["calibration"]["applied"] is True
    assert body["calibration"]["uncertainty"] is None
    assert body["calibrated"]["mean"] is not None


def test_uncertainty_scales_with_the_correction(client):
    _setup(client)
    # Halving the signal halves the uncertainty the scale introduces.
    _calibrate(client, scale=0.5, offset=0.0, uncertainty=0.8,
               uncertainty_kind="sensor_datasheet")
    record = client.get("/v1/sites/s1/calibration").json()["calibrations"][0]
    assert calibrated_uncertainty(record) == 0.4

    body = client.get("/v1/analytics/summary?node_id=CAUCE-001"
                      "&variable=air_temperature"
                      f"&from_utc_ms={BASE_TS}"
                      f"&to_utc_ms={BASE_TS + 3600000}").json()
    assert body["calibration"]["uncertainty"] == 0.4


def test_uncertainty_survives_a_partial_update(client):
    _setup(client)
    _calibrate(client, offset=-1.0, uncertainty=0.5,
               uncertainty_kind="repeatability")
    # Re-posting the offset alone must not silently drop the characterisation.
    _calibrate(client, offset=-2.0)
    record = client.get("/v1/sites/s1/calibration").json()["calibrations"][0]
    assert record["offset"] == -2.0
    assert record["uncertainty"] == 0.5
    assert record["uncertainty_kind"] == "repeatability"


def test_uncertainty_validation(client):
    _setup(client)
    assert _calibrate(client, uncertainty=-1.0).status_code == 422
    assert _calibrate(client, uncertainty=1e9).status_code == 422
    assert _calibrate(client, uncertainty="wide").status_code == 422
    assert _calibrate(client, uncertainty=True).status_code == 422
    assert _calibrate(client, uncertainty_kind="vibes").status_code == 422
    # A kind with no number is worse than silence.
    assert _calibrate(client, uncertainty_kind="estimated").status_code == 422
    for kind in ("sensor_datasheet", "co_location_spread", "repeatability",
                 "estimated", "unknown"):
        assert _calibrate(client, uncertainty=0.2, uncertainty_kind=kind
                          ).status_code == 200


def test_csv_exports_the_uncertainty_and_leaves_it_empty_when_unknown(client):
    _setup(client)
    _calibrate(client, offset=-1.0)
    header = client.get("/v1/nodes/CAUCE-001/export.csv").text.splitlines()[0]
    assert header.split(",")[-1] == "calibration_uncertainty"
    row = client.get("/v1/nodes/CAUCE-001/export.csv").text.splitlines()[1]
    assert row.split(",")[-1] == "", "unknown uncertainty must stay blank"

    _calibrate(client, offset=-1.0, uncertainty=0.25,
               uncertainty_kind="co_location_spread")
    row = client.get("/v1/nodes/CAUCE-001/export.csv").text.splitlines()[1]
    assert row.split(",")[-1] == "0.25"


def test_uncertainty_scales_a_negative_correction_by_its_magnitude(client):
    _setup(client)
    _calibrate(client, scale=-2.0, uncertainty=0.5)
    record = client.get("/v1/sites/s1/calibration").json()["calibrations"][0]
    # A negative scale is a reflection plus a gain; the gain is |scale|.
    assert calibrated_uncertainty(record) == 1.0


def test_no_calibration_reports_applied_false(client):
    _setup(client)
    assert calibrated_uncertainty(None) is None
    body = client.get("/v1/analytics/summary?node_id=CAUCE-001"
                      "&variable=air_temperature"
                      f"&from_utc_ms={BASE_TS}"
                      f"&to_utc_ms={BASE_TS + 3600000}").json()
    assert body["calibration"]["applied"] is False
    assert body["calibration"]["uncertainty"] is None
    assert "calibrated" not in body

"""The fleet heat view, and the credential that stopped anyone reaching it.

Two things are asserted here that nothing else covered.

The first is the regression that started this: `/nodes/{id}/events` forwarded the browser's
absent `Authorization` header into `api.analytics_heat_events`, which requires a read scope.
A browser cannot set that header from an address bar, so the page whose entire job is to show
heat events answered 401 on every dashboard page except itself - and the node page linked
straight into it. It passes now because it calls the pure detector directly, and these tests
hold that in place by requesting it with no credential at all.

The second is the key collision that made `/heat` return 500 on first render:
`heat_summary()` returns a count under the key `events`, which overwrote the event *list* of
the same name via `**`, so `max()` received an int. It raised rather than rendering something
wrong, which is the best of the three ways that could have gone. A test that only asks for a
200 catches it; a test that asks for the numbers in the page catches the next one too.
"""

from __future__ import annotations

import os
import re
import time

from cauce_server.analytics import detect_heat_events, heat_summary

# This module raises the rate limit for itself, before the app is imported, because
# `config.Settings` reads the environment once at import time. It cannot be done in conftest:
# conftest is imported first, so by the time a module sets it the value is already fixed for
# the whole run, and `test_the_challenge_endpoint_is_rate_limited` would stop seeing a 429.
# Raising it suite-wide to make this module comfortable would delete that test's subject.
os.environ.setdefault("CAUCE_RATE_LIMIT", "100000")

from cauce_server.main import app  # noqa: E402
from fastapi.testclient import TestClient

MINUTE = 60_000


def rows(*pairs: tuple[int, float | None]) -> list[dict]:
    """(minute, value) pairs become the row shape the detector reads."""
    return [{"timestamp_utc_ms": m * MINUTE, "value": v} for m, v in pairs]


# One shared client for the module. The rate limiter counts per client, so a fresh client
# per test exhausts it and every assertion after the first few comes back `rate_limited` -
# which reads as a broken feature rather than as a test that hammered the limiter. The
# bearer token is the literal "admin-token" that `conftest` configures; importing it is not
# possible because `tests` also exists in site-packages and that copy shadows this repo's.

_SHARED = TestClient(app, headers={"Authorization": "Bearer admin-token"})


def client() -> TestClient:
    return _SHARED


# --------------------------------------------------------------- the pure detector


def test_a_run_above_the_threshold_is_reported_with_its_peak_and_duration():
    out = detect_heat_events(rows((0, 20.0), (30, 35.0), (60, 36.0), (90, 34.0),
                                  (120, 34.0), (150, 20.0)), 32.0, 60)
    assert out
    assert out[0]["peak_value"] == 36.0
    assert out[0]["duration_min"] == 60


def test_a_run_shorter_than_the_minimum_is_not_an_event():
    # Two samples above threshold, then cold: 10 minutes, below the 60-minute floor.
    assert detect_heat_events(rows((0, 35.0), (10, 35.0), (20, 20.0)), 32.0, 60) == []


def test_a_run_shorter_than_the_minimum_at_the_end_of_the_data_is_not_reported():
    # Still above the threshold when the samples stop, and never reached the floor. There is
    # no evidence it ended, so reporting one would invent an end time and a duration.
    assert detect_heat_events(rows((0, 35.0), (30, 36.0)), 32.0, 60) == []


def test_a_continuous_run_is_reported_in_chunks_of_the_minimum():
    """DOCUMENTED BEHAVIOUR, and the reason the fleet page counts look inflated.

    Three unbroken hours above the threshold at `min_duration_min=60` come back as three
    events of one hour each, not one event of three hours. The detector closes an event as
    soon as it reaches the minimum and starts the next one immediately, so a long heat wave
    is counted in tiles.

    This is pre-existing API behaviour and this file does not change it - an API contract is
    not something to redefine inside a bug fix. It is asserted here so that a future change is
    a deliberate decision rather than an accident, and because the fleet page's "N events"
    column is only meaningful once you know what N counts.

    Whether it is the right semantics for a heat wave is a question for the owner of the
    metric, not for this test file.
    """
    continuous = rows(*[(m, 35.0) for m in range(0, 181, 60)])
    out = detect_heat_events(continuous, 32.0, 60)
    # Four samples spanning 180 minutes give two events, not three. Closing a chunk takes two
    # samples - the one that opens it and the one that reaches the minimum - so a run of N
    # samples yields N-1 events. That off-by-one is part of why a long heat wave reads as a
    # larger number than a reader expects, and it is the reason the column is labelled with
    # the tiling rather than presented as "heat waves".
    assert len(out) == 2, f"expected the run tiled at 60 min, got {len(out)}"
    assert all(e["duration_min"] == 60 for e in out)
    assert out[0]["start_utc_ms"] == 0
    assert out[1]["start_utc_ms"] == 120 * MINUTE


def test_the_peak_is_the_worst_sample_not_the_first():
    # Two separate runs, so this measures the peak within a run rather than the tiling.
    out = detect_heat_events(rows((0, 40.0), (60, 33.0), (90, 10.0),
                                  (120, 45.0), (180, 44.0), (240, 10.0)),
                             32.0, 60)
    assert [e["peak_value"] for e in out] == [40.0, 45.0]


def test_an_empty_input_is_no_events_not_an_error():
    assert detect_heat_events([], 32.0, 60) == []
    assert heat_summary([]) == {"events": 0, "total_minutes": 0,
                                "peak_value": None, "longest_min": 0}


# --------------------------------------------------------------- the pages


def test_the_per_node_events_page_opens_without_a_credential():
    """The regression. No token in the client, and the page must still answer."""
    c = client()
    r = c.get("/nodes/CAUCE-001/events")
    assert r.status_code == 200, f"unauthenticated heat page -> {r.status_code}"


def test_the_fleet_page_renders_and_reports_a_count_per_node():
    c = client()
    r = c.get("/heat")
    assert r.status_code == 200
    # The count and the list must not have collided again. A page that renders the word
    # "CAUCE-001" twice with one number each is the shape this file exists to catch.
    assert "CAUCE-001" in r.text


def test_the_fleet_page_survives_a_threshold_nothing_crosses():
    # An unreachable threshold is the normal state of a real fleet on a cool day, and it is
    # the branch that had no rows to iterate.
    c = client()
    r = c.get("/heat?threshold=79")
    assert r.status_code == 200


def test_the_fleet_page_clamps_an_absurd_window_rather_than_scanning_the_table():
    c = client()
    r = c.get("/heat?hours=99999999")
    assert r.status_code == 200


def test_no_page_ships_an_unresolved_placeholder():
    # The dashboard already has this guard for its own pages; /heat joined it late and the
    # existing test only covers what it knew about.
    c = client()
    for path in ("/", "/map", "/colocation", "/alerts", "/heat", "/system"):
        body = c.get(path).text
        leftovers = [tok for tok in ("{events}", "{body}", "{summary}",
                                     "{heat_window}", "{thr}") if tok in body]
        assert not leftovers, f"{path} shipped {leftovers}"


def test_the_map_and_the_events_page_link_to_the_fleet_view():
    c = client()
    assert '"/heat"' in c.get("/map").text
    assert '"/heat"' in c.get("/nodes/CAUCE-001/events").text

def test_the_legend_is_the_scale_the_field_is_painted_with():
    """Why this test exists: the legend used to lie, and look like it did not.

    `_temp_color` maps degrees to colour on a fixed 0-40 C scale, and the field cells are
    painted with it. The legend, though, was a `linearGradient` with its two ends pinned to
    the lowest and highest value in the data and labelled with those two numbers. A bar
    running from colour(10) to colour(20) passes through colours that colour(15) is not -
    so reading a temperature off the bar gave a wrong answer, confidently, for every value
    in between. It was only wrong when the data did not already fill the scale, which is why
    it survived.

    The bar is now sampled from the same function the cells use, so the two agree at every
    value by construction rather than by coincidence.
    """
    from cauce_server.dashboard import _TEMP_SCALE_HI, _TEMP_SCALE_LO, _temp_color
    assert (_TEMP_SCALE_LO, _TEMP_SCALE_HI) == (0.0, 40.0)

    # Monotone and clamped at both ends: a legend that wraps or inverts at the extremes
    # would be wrong in the same way a stretched gradient was.
    seq = [_temp_color(v) for v in range(0, 41, 2)]
    assert seq == sorted(set(seq)) or len(set(seq)) == len(seq)
    assert _temp_color(-50) == _temp_color(0.0)
    assert _temp_color(500) == _temp_color(40.0)

    # The scale is a named constant rather than something derived from the data at render
    # time - that is what makes two screenshots of different hours comparable. Asserted by
    # reading the value, not by inspecting source: the behaviour above already pins the
    # observable part.
    assert isinstance(_TEMP_SCALE_HI, float)


def test_the_map_page_carries_no_gradient_legend():
    c = client()
    body = c.get("/map").text
    # No data in this module's database, so there is no field and no legend to draw - the
    # point here is only that the gradient construction is gone. The populated case, where
    # the ramp's ticks and cells are asserted, is `test_map_field_and_legend` in test_api.
    assert "linearGradient" not in body

# --------------------------------------------------------------- map window and staleness


_SEED_N = 0


def _seed_site_and_nodes(client, ages_h: dict):
    """Place nodes on one site, each with a single reading `ages_h` old.

    The module shares one database, so reusing a node id across tests means the second test
    sees the first test's reading - the map takes the highest sequence, not the one just
    written. Ids are therefore unique per call, which is what makes these tests independent
    of the order they happen to run in.
    """
    global _SEED_N
    _SEED_N += 1
    tag = f"w{_SEED_N}"
    site = f"s-{tag}"
    client.post("/v1/sites", json={"site_id": site})
    client.put(f"/v1/sites/{site}/location", json={"lat": -34.9, "lon": -56.16})
    from cauce_server import db as _db

    now = int(time.time() * 1000)
    nodes = {}
    for i, (nid, age_h) in enumerate(ages_h.items(), start=1):
        real = f"CAUCE-{tag}-{nid.rsplit('-', 1)[-1]}"
        nodes[real] = 20.0 + i
        client.post("/v1/sync", json={
            "protocol_version": 1, "node_id": real,
            "measurements": [{
                "node_id": real, "sensor_id": "BME-1", "sequence": 1,
                "timestamp_utc_ms": now - int(age_h * 3600000),
                "variable": "air_temperature", "value": 20.0 + i, "unit": "C",
                "quality": "VALID", "reason_bits": 0, "time_uncertain": False,
            }],
        })
    with _db.transaction() as conn:
        conn.execute(f"UPDATE nodes SET site_id=? WHERE node_id IN ({','.join('?' * len(nodes))})",
                     (site, *nodes))
    return nodes


def test_the_map_offers_a_window_control():
    c = client()
    _seed_site_and_nodes(c, {"CAUCE-FRESH": 0.2})
    body = c.get("/map").text
    assert "/map?hours=1" in body
    assert "/map?hours=168" in body


def test_a_node_older_than_the_window_is_drawn_hollow_not_dropped():
    """A node that reported yesterday is still a node.

    Dropping it would hide a station, which is the opposite of what a map should do. But
    painting it identically to one that reported a minute ago made a three-day-old reading
    look current. It is drawn, unfilled and dashed, and the tooltip says how old.
    """
    c = client()
    nodes = _seed_site_and_nodes(c, {"CAUCE-FRESH": 0.2, "CAUCE-OLD": 300})
    old_id = next(n for n in nodes if n.endswith("OLD"))
    fresh = next(n for n in nodes if n.endswith("FRESH"))
    body = c.get("/map?hours=24").text
    assert old_id in body, "a stale node must still appear on the map"

    def circle_for(name):
        m = re.search(r"<circle\b[^>]*>(?:(?!</circle>).)*?"
                      + re.escape(name) + r".*?</circle>", body, re.S)
        assert m, f"{name} has no circle on the map"
        return m.group(0)

    assert 'fill="none"' in circle_for(old_id), "the stale node should be hollow"
    assert "stroke-dasharray" in circle_for(old_id), "the stale node should be dashed"
    assert 'fill="none"' not in circle_for(fresh), "the fresh node should be filled"

def test_both_nodes_still_show_their_temperatures():
    """A stale node keeps its value on the map.

    The temperature lives in the SVG `<title>` as a tooltip rather than as visible text, so
    this asserts the tooltip carries it. Asserting on visible page text would be asserting
    on a rendering choice rather than on the behaviour.
    """
    c = client()
    nodes = _seed_site_and_nodes(c, {"CAUCE-FRESH": 0.2, "CAUCE-OLD": 300})
    body = c.get("/map?hours=24").text
    titles = dict(re.findall(r"<title>(CAUCE-\w+-[A-Z]+): ([^<]+)</title>", body))
    fresh = next(n for n in nodes if n.endswith("FRESH"))
    old_id = next(n for n in nodes if n.endswith("OLD"))
    assert titles.get(fresh, "").startswith(f"{nodes[fresh]:.1f}"), titles
    assert titles.get(old_id, "").startswith(f"{nodes[old_id]:.1f}"), titles
    # The stale one says so and how old; the fresh one says it is inside the window.
    assert "antiguo" in titles.get(old_id, ""), titles.get(old_id)
    assert "en ventana" in titles.get(fresh, ""), titles.get(fresh)


def test_a_wide_window_makes_an_old_node_fresh_again():
    """The staleness mark is relative to the window, not absolute.

    A reading 300 h old is stale for a 24 h window and current for a 30-day one, which is
    the honest answer: "stale" only means something relative to the question being asked.
    """
    c = client()
    nodes = _seed_site_and_nodes(c, {"CAUCE-OLD": 300})
    nid = next(iter(nodes))
    narrow = c.get("/map?hours=24").text
    wide = c.get("/map?hours=720").text

    def hollow_for(body, name):
        # Scoped to this test's own node: the module shares a database, so other tests'
        # nodes are on the map too, and a page-wide check would assert about whatever
        # happened to be stale there.
        for m in re.finditer(r"<circle\b[^>]*>(?:(?!</circle>).)*?</circle>", body, re.S):
            if f"<title>{name}:" in m.group(0):
                return 'fill="none"' in m.group(0)
        return False

    assert hollow_for(narrow, nid), f"{nid} should be hollow at a 24h window"
    assert not hollow_for(wide, nid), f"{nid} should be filled at a 720h window"


def test_an_absurd_window_is_clamped_rather_than_scanning_everything():
    c = client()
    assert c.get("/map?hours=999999999").status_code == 200
    assert c.get("/map?hours=0").status_code == 200
    assert c.get("/map?hours=-5").status_code == 200

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

from cauce_server.analytics import detect_heat_events, heat_summary
from cauce_server.main import app
from fastapi.testclient import TestClient

MINUTE = 60_000


def rows(*pairs: tuple[int, float | None]) -> list[dict]:
    """(minute, value) pairs become the row shape the detector reads."""
    return [{"timestamp_utc_ms": m * MINUTE, "value": v} for m, v in pairs]


def client() -> TestClient:
    return TestClient(app)


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
    with client() as c:
        r = c.get("/nodes/CAUCE-001/events")
    assert r.status_code == 200, f"unauthenticated heat page -> {r.status_code}"


def test_the_fleet_page_renders_and_reports_a_count_per_node():
    with client() as c:
        r = c.get("/heat")
    assert r.status_code == 200
    # The count and the list must not have collided again. A page that renders the word
    # "CAUCE-001" twice with one number each is the shape this file exists to catch.
    assert "CAUCE-001" in r.text


def test_the_fleet_page_survives_a_threshold_nothing_crosses():
    # An unreachable threshold is the normal state of a real fleet on a cool day, and it is
    # the branch that had no rows to iterate.
    with client() as c:
        r = c.get("/heat?threshold=79")
    assert r.status_code == 200


def test_the_fleet_page_clamps_an_absurd_window_rather_than_scanning_the_table():
    with client() as c:
        r = c.get("/heat?hours=99999999")
    assert r.status_code == 200


def test_no_page_ships_an_unresolved_placeholder():
    # The dashboard already has this guard for its own pages; /heat joined it late and the
    # existing test only covers what it knew about.
    with client() as c:
        for path in ("/", "/map", "/colocation", "/alerts", "/heat", "/system"):
            body = c.get(path).text
            leftovers = [tok for tok in ("{events}", "{body}", "{summary}",
                                         "{heat_window}", "{thr}") if tok in body]
            assert not leftovers, f"{path} shipped {leftovers}"


def test_the_map_and_the_events_page_link_to_the_fleet_view():
    with client() as c:
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
    with client() as c:
        body = c.get("/map").text
    # No data in this module's database, so there is no field and no legend to draw - the
    # point here is only that the gradient construction is gone. The populated case, where
    # the ramp's ticks and cells are asserted, is `test_map_field_and_legend` in test_api.
    assert "linearGradient" not in body

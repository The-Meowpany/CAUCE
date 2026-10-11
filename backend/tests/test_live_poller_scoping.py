"""The live poller must touch only the figures it owns, and only where it owns them.

This file exists because of a regression the tests passed straight through.

The poller removed the `<meta http-equiv="refresh">` from every template, and then updated
`.stat .v` elements by position - `stats[0]`, `stats[1]`, `stats[2]`. Those are the first
three matching elements on the page, which is only the right answer on the index.

Two consequences, both invisible to the suite:

1. Five of seven pages - the map, heat events, node report, co-location, compare - had their
   refresh disarmed and had nothing to replace it. They went from updating themselves every
   interval to being static, with no change to justify it.

2. `/system` renders five tiles in the same class, and the third is the **database size**. A
   poll there overwrote "Base 0.2 MB" with a timestamp and left it there - a page displaying
   confidently wrong numbers, which is worse than a stale one.

Neither was caught, because both need the rendered page and the script compared against each
other. So they are asserted here: each figure names itself with `data-live`, only the index
carries such tiles, and every other page keeps the fallback it needs to stay current.
"""

from __future__ import annotations

import re

import pytest
from cauce_server import db
from cauce_server.main import app
from conftest import ADMIN_HEADERS
from fastapi.testclient import TestClient

BASE_TS = 1787356860000


@pytest.fixture()
def client():
    db.reset_for_tests()
    with TestClient(app, headers=ADMIN_HEADERS) as c:
        c.post("/v1/sites", json={"site_id": "s1"})
        c.put("/v1/sites/s1/location", json={"lat": -34.9, "lon": -56.1})
        c.post("/v1/sync", json={
            "protocol_version": 1, "node_id": "CAUCE-A",
            "measurements": [{
                "node_id": "CAUCE-A", "sensor_id": "S", "sequence": 1,
                "timestamp_utc_ms": BASE_TS, "variable": "air_temperature",
                "value": 21.0, "unit": "C", "quality": "VALID",
                "reason_bits": 0, "time_uncertain": False,
            }],
        })
        yield c


# Every page that carries the shared script.
PAGES = ["/", "/system", "/map", "/heat", "/nodes/CAUCE-A", "/colocation", "/compare"]

LIVE_TILES = re.compile(r'<div class="stat" data-live="([a-z_]+)"')


def _poller_source(page_html: str) -> str:
    """Just the body of the function that talks to /v1/live, comments removed.

    Isolating it matters: the shared template carries several scripts, and one of them - the
    entrance animation - legitimately reads `.stat .v` to animate the tiles in. Asserting over
    the whole document flags that correct code, which is how a check stops being worth having.
    """
    # Comments go first, then the lookup. The poller's own docstring explains that it "asks
    # /v1/live", and that sentence sits *above* the IIFE - so searching the raw text for
    # "/v1/live" finds the comment, and the nearest preceding IIFE is the chart renderer rather
    # than the poller. Searching for the call instead of the word is what makes this stable.
    script = re.sub(r"/\*.*?\*/", "", page_html, flags=re.S)
    script = re.sub(r"//[^\n]*", "", script)
    at = script.find("fetch('/v1/live'")
    if at < 0:
        at = script.find('fetch("/v1/live"')
    if at < 0:
        return ""
    # The outer IIFE's exact spelling, not `(function`: `.forEach(function (k) {` also starts
    # with that, so a looser match lands on the poller's own inner callback and runs on into
    # the entrance animation that follows it.
    start = script.rfind("(function () {", 0, at)
    if start < 0:
        return ""
    # Bounded at both ends. Taking everything from the opening to the end of the document
    # sweeps in the entrance animation that follows, which reads `.stat .v` correctly and is
    # not what this file is about.
    end = script.find("</script>", at)
    return script[start:end] if end > start else script[start:]


def test_only_the_index_carries_live_tiles(client):
    """A tile with `data-live` on another page would be claimed by the poller.

    It is harmless in itself - the poller writes the quantity named by the attribute - but the
    point of the attribute is that it marks a figure the poller owns. A page growing one by
    accident is the `/system` defect arriving again by another route.
    """
    for page in PAGES:
        r = client.get(page)
        assert r.status_code == 200, page
        tiles = LIVE_TILES.findall(r.text)
        if page == "/":
            assert set(tiles) == {"nodes", "records", "last_sync"}, (
                f"the index is the page the poller updates; it carries {tiles}")
        else:
            assert tiles == [], (
                f"{page} carries live tiles {tiles}; only the index should, because only it "
                f"has figures the poller knows how to update")


def test_every_page_that_cannot_be_polled_keeps_its_refresh(client):
    """The regression, asserted where it actually happens.

    The first version of this test asserted the fallback tag was present in the HTML. It is
    *always* present - the script removes it at runtime, after the document has been served -
    so the assertion passed whether or not the tag was being removed, which is exactly the
    defect. Checking the served HTML cannot see a decision made by JavaScript.

    So this checks the decision itself: the poller may only drop the fallback when it has
    figures to keep current in its place.
    """
    script = _poller_source(client.get("/map").text)
    assert script, "the map runs no poller, so there is nothing to guard"
    removal = re.search(
        r"if\s*\(([^)]*)\)\s*\{\s*\n?\s*fallback\.parentNode\.removeChild", script)
    assert removal, (
        "the poller no longer removes the fallback at all, or it does so in a shape this "
        "check cannot recognise - either way it needs a human")
    condition = removal.group(1)
    assert "haveTargets" in condition or "targets" in condition, (
        f"the fallback is removed under the condition {condition!r}, which does not depend on "
        f"whether this page has anything to poll. Every page runs this script, so five of "
        f"them lose their refresh and have nothing to replace it.")


def test_the_poller_reports_whether_it_has_anything_to_update(client):
    """`haveTargets` has to be derived from the named attributes, not hardcoded.

    If it were a constant, the guard above would compile, pass, and mean nothing.
    """
    script = _poller_source(client.get("/").text)
    derived = re.search(
        r"haveTargets\s*=\s*([^;]+);", script)
    assert derived, "the poller does not compute whether it has targets"
    expression = derived.group(1)
    assert "live" in expression and "||" in expression, (
        f"haveTargets is computed as {expression!r}, which does not read the named figures")


def test_the_poller_does_not_address_tiles_by_position(client):
    """The mechanism, not just the outcome.

    `stats[0]` means "the first matching element on whatever page this is", which is a
    different thing on every page. The names have to be in the script for the outcome above
    to mean anything.
    """
    for page in PAGES:
        text = client.get(page).text
        # Only the poller is in scope. Another script on the same page does read `.stat .v` -
        # the entrance animation collects those elements to animate them in, and it is
        # read-only, so asserting over the whole document would flag correct code. The defect
        # was positional *writes*, and the poller is the only thing that writes.
        script = _poller_source(text)
        assert script, f"{page} runs no live poller"
        assert not re.search(r"stats\s*\[\s*0\s*\]", script), (
            f"{page} still addresses live figures by position; the third .stat tile on "
            f"/system is the database size")
        assert not re.search("querySelectorAll\\(\\s*['\"]\\.stat", script), (
            f"{page}'s poller still collects the tiles as an undifferentiated list")
        # And it must write through the named attribute rather than by class.
        assert "data-live" in script, (
            f"{page}'s poller does not address figures by name")
        assert 'data-live="' in text, (
            f"{page} runs the poller but nothing names the figures it may update")


def test_the_system_page_keeps_its_database_size(client):
    """The corruption itself, at the level a reader would see it."""
    text = client.get("/system").text
    assert 'data-live="database"' not in text, (
        "the database tile is not one the poller updates and must never be given its name")
    # The five system tiles are still all rendered, each with a value of its own.
    values = re.findall(r'<div class="stat"><div class="u">([^<]*)</div>'
                        r'<div class="v">([^<]*)</div></div>', text)
    assert len(values) == 5, f"/system renders {len(values)} tiles, expected 5"
    labels = [label for label, _ in values]
    assert len(set(labels)) == 5, f"/system tiles are not distinct: {labels}"


def test_the_index_still_shows_the_three_figures(client):
    """The fix must not cost the index its live update."""
    text = client.get("/").text
    assert set(LIVE_TILES.findall(text)) == {"nodes", "records", "last_sync"}


def test_only_changed_figures_are_marked_as_changed(client):
    """Flashing all three every poll is the flicker the diff check exists to prevent.

    The first version removed and re-added the class on every `.stat .v` regardless of
    whether the text had changed. It now collects what moved and marks only that.
    """
    text = client.get("/").text
    assert "moved.forEach" in text or "moved.forEach(function" in text, (
        "the poller has to mark only the figures whose text actually changed")
    assert "stats.forEach" not in text, (
        "the poller is still flashing every tile on every poll")

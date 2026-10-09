"""The dashboard's self-refresh interval is a deployment decision, not a template constant.

It used to be the literal 60 in three page templates and nowhere else. That is a reasonable
default and the wrong shape: a central in a village with one Wi-Fi link wants a slow reload,
anything watching a node come back wants a fast one, and wanting no reload at all meant
editing a template. It is now `settings.dashboard_refresh_s`, resolved in one place.

These tests exist because the value is easy to get subtly wrong rather than obviously wrong.
A tag of `content="0"` still parses as valid HTML and still reloads - browsers treat it as
"as fast as possible", which is the exact opposite of turning the refresh off. That failure
would not show up in a smoke test that only asks for HTTP 200.
"""

from __future__ import annotations

import pytest
from cauce_server.config import settings
from cauce_server.dashboard import _refresh_meta
from cauce_server.main import app


@pytest.fixture
def fast_restore():
    """Set the interval and put it back, so a test cannot leak into the next one."""
    original = settings.dashboard_refresh_s
    yield
    settings.dashboard_refresh_s = original


def _refresh_tags(html: str) -> list[str]:
    import re

    return re.findall(r'<meta http-equiv="refresh"[^>]*>', html)


def test_the_default_is_the_sixty_seconds_that_was_hardcoded():
    # Not a preference assertion. Sixty is what shipped, so a default that silently changed
    # would change what every existing deployment reloads at without anyone asking.
    assert settings.dashboard_refresh_s == 60


def test_the_interval_reaches_the_rendered_page(fast_restore):
    from fastapi.testclient import TestClient

    settings.dashboard_refresh_s = 5
    with TestClient(app) as client:
        body = client.get("/").text
    # The tag is the no-JavaScript fallback and carries the honest interval. It is NOT set to
    # zero: `content="0"` with no URL reloads immediately and forever, and `url=self` is not a
    # URL keyword but a link *type*, so the browser resolved it as a relative path and every
    # page 404'd on `/self`. The script removes the tag before it can fire.
    assert 'id="cauce-fallback-refresh"' in body
    assert 'content="5"' in body
    assert "url=self" not in body


def test_the_fallback_names_no_url_at_all(fast_restore):
    """A bare `content="N"` reloads the page you are on.

    Any URL at all is worse: the tag cannot express "reload me in N seconds, but not if I have
    JavaScript", and the attempt to say so with `url=self` sent every page to `/self`.
    """
    from fastapi.testclient import TestClient

    settings.dashboard_refresh_s = 5
    with TestClient(app) as client:
        body = client.get("/?lang=es&from=x").text
    tag = body.split('id="cauce-fallback-refresh"')[1].split(">")[0]
    assert "url" not in tag, f"the refresh names a URL: {tag!r}"


def test_every_page_carries_the_token_so_no_page_is_frozen(fast_restore):
    """Six of the ten templates never had the token, so they never refreshed at all.

    `_with_legal` has always resolved `__REFRESH__`; six templates simply did not carry it, so
    `/map`, `/colocation`, `/alerts`, `/report`, `/events` and `/compare` were static and no
    test noticed, because the tests only asked about `/`.
    """
    import re
    from pathlib import Path

    from cauce_server import dashboard as D
    from fastapi.testclient import TestClient

    src = Path(D.__file__).read_text(encoding="utf-8")
    templates = re.findall(r'(_[A-Z_]+) = """<!DOCTYPE html>(.*?)"""', src, re.S)
    assert len(templates) == 10
    for name, body in templates:
        assert "__REFRESH__" in body, f"{name} has no refresh token and would never update"

    # And with the interval off, no page may emit a tag at all.
    settings.dashboard_refresh_s = 0
    with TestClient(app) as client:
        for path in ("/", "/map", "/alerts", "/colocation"):
            body = client.get(path).text
            # The string appears in the script, which always tries to remove the tag. What
            # must be absent is the tag itself.
            assert 'id="cauce-fallback-refresh"' not in body, path
            assert "http-equiv=\"refresh\"" not in body, path


def test_every_page_carries_the_token_and_none_is_left_unresolved(fast_restore):
    from fastapi.testclient import TestClient

    settings.dashboard_refresh_s = 7
    with TestClient(app) as client:
        for path in ("/", "/nodes/does-not-exist", "/alerts"):
            body = client.get(path).text
            # The token is resolved in `_with_legal`, which every render path funnels
            # through. A template that skipped it would render a literal `__REFRESH__` into
            # the page and reload every 60 seconds by falling back to nothing at all.
            assert "__REFRESH__" not in body, f"{path} shipped the token unresolved"


def test_zero_removes_the_tag_instead_of_asking_for_an_instant_reload(fast_restore):
    from fastapi.testclient import TestClient

    settings.dashboard_refresh_s = 0
    with TestClient(app) as client:
        body = client.get("/").text
    tags = _refresh_tags(body)
    assert tags == [], f"0 must emit no refresh tag, emitted {tags}"


def test_a_negative_interval_is_also_off_rather_than_a_browser_error():
    # Negative is not a meaningful cadence. Browsers ignore a malformed value, so emitting
    # one would leave the caller believing it had configured something.
    original = settings.dashboard_refresh_s
    try:
        settings.dashboard_refresh_s = -5
        assert _refresh_meta() == ""
    finally:
        settings.dashboard_refresh_s = original


def test_the_tag_is_valid_html_and_carries_no_stray_braces():
    # The token sits inside a `.format()`-ed template, so an unescaped brace here would
    # either break formatting or survive into the output as a literal.
    tag = _refresh_meta()
    assert tag.startswith('<meta http-equiv="refresh" id="cauce-fallback-refresh" content="')
    assert "{" not in tag and "}" not in tag

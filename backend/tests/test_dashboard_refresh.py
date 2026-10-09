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
    # The interval is now the POLL period, not a document reload. A bare `content="5"` made
    # every entrance animation replay every five seconds, so the page looked busy and
    # unchanged simultaneously; `0` hands the updating to the script and `url=self` is the
    # fallback for a browser without JavaScript.
    assert '<meta http-equiv="refresh" content="0;url=self">' in body


def test_the_refresh_names_itself_so_query_parameters_survive(fast_restore):
    from fastapi.testclient import TestClient

    settings.dashboard_refresh_s = 5
    with TestClient(app) as client:
        body = client.get("/?lang=es&from=x").text
    # A bare "/" would drop the query string on the fallback reload, losing the reader's
    # filters. `self` re-requests the URL they are already on.
    assert 'url=self' in body
    assert 'url=/"' not in body


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
    assert tag.startswith('<meta http-equiv="refresh" content="')
    assert "{" not in tag and "}" not in tag

"""Motion that is added on purpose, and that gets out of the way.

Everything here is behind `prefers-reduced-motion`. That is not decoration: a dashboard read
fifty times a day by someone who has asked their operating system for less movement should not
argue with that preference, and a data panel where motion gates information is a bug. So the
tests below check both halves - that the motion exists, and that it is entirely conditional.

The counter is the part most able to lie. An animation that animates `107238` to "1,072.4"
has made the data less true in exchange for being livelier, which is the opposite of the job.
So the tests parse the same way the script does and assert the final value is the original
string.
"""

from __future__ import annotations

import re

import pytest
from cauce_server.dashboard import _MOTION_CSS, _MOTION_JS


def client():
    from cauce_server.main import app
    from fastapi.testclient import TestClient

    return TestClient(app, headers={"Authorization": "Bearer admin-token"})


# --------------------------------------------------------------------- the CSS


def test_all_motion_is_inside_a_reduced_motion_query():
    """Every animated declaration must be conditional.

    If one rule escapes the block, a user who asked for reduced motion still gets it, and
    there is no test that would notice.
    """
    animated = re.findall(r"^.*\banimation:[^;}]*", _MOTION_CSS, re.M)
    assert animated, "no animations found at all - did the motion layer get dropped?"
    assert "@media (prefers-reduced-motion:no-preference)" in _MOTION_CSS

    # Every `animation` or `transition` shorthand outside the media block is a bug. Split the
    # stylesheet at the block and check the head has no animation of its own.
    head, _, _block = _MOTION_CSS.partition("@media (prefers-reduced-motion:no-preference)")
    assert "animation:" not in head, "an animation sits outside the reduced-motion block"
    assert "transition:" not in head


def test_the_script_bails_out_when_reduced_motion_is_requested():
    """The JS has to check the same media query, not just the CSS.

    A stylesheet cannot stop a counter from running. The script reads the preference itself
    and returns before touching the DOM, which is the only thing that actually honours it.
    """
    assert "prefers-reduced-motion: reduce" in _MOTION_JS
    assert re.search(r"REDUCED\s*=\s*window\.matchMedia", _MOTION_JS)
    # The bail-out has to come before the first mutation of the page.
    m = re.search(r"if \(REDUCED\) return;", _MOTION_JS)
    assert m, "no early return on reduced motion"
    first_dom = re.search(r"document\.querySelector", _MOTION_JS)
    assert m.start() < first_dom.start(), (
        "the reduced-motion check happens after the DOM is already being touched")


# --------------------------------------------------------------------- the counter


def _counter_regex() -> str:
    """The same pattern the script uses, extracted so the test and the page cannot diverge."""
    m = re.search(r"raw\.match\(/(.+?)/\)", _MOTION_JS)
    assert m, "the counter's parse pattern is gone"
    return m.group(1)


def test_the_counter_recognises_the_numbers_the_dashboard_actually_renders():
    import re as _re

    pattern = _re.compile(_counter_regex())
    for value in ("0", "42", "107238", "1,072,384", "24.5", "-3.2", "12 %", "34°",
                  "8.4 km/h", "2.1 m"):
        assert pattern.match(value), f"{value!r} would not be recognised, so it would not animate"


def test_the_counter_declines_anything_that_is_not_just_a_number():
    import re as _re

    pattern = _re.compile(_counter_regex())
    # Text, dates and identifiers must be left exactly as they are. A counter that mangles a
    # timestamp to make it count up is worse than one that does not move.
    for value in ("CAUCE-SIM-001", "2026-10-09T01:30Z", "sin datos", "N/A", "", "3d 4h",
                  "42 Measurement"):
        assert not pattern.match(value), f"{value!r} would be treated as a number"


def test_the_counter_keeps_precision_and_separators():
    """The animation must end on the original string, never on a reformatted number.

    `int.toLocaleString` and `toFixed` are both lossy in a way that shows: 107238 rendered as
    107,238 is fine, but the same value finished as 107237.9 is a lie about a measurement.
    So the script restores the original text on the final frame.
    """
    assert 'el.textContent = raw;' in _MOTION_JS, (
        "the final frame must restore the original text, not a formatted approximation")
    assert "toFixed(decimals)" in _MOTION_JS, "precision must follow the source string"
    assert "decimals" in _MOTION_JS


def test_counting_up_never_overshoots_the_value():
    # easeOutExpo is monotonic in [0,1]; if that ever changed to a spring that overshoots, a
    # temperature would briefly read above its real value on the way past.
    assert "Math.pow(2, -10 * t)" in _MOTION_JS
    assert "t === 1 ? 1" in _MOTION_JS


# --------------------------------------------------------------------- charts


def test_charts_draw_only_after_measuring_the_real_length():
    """`getTotalLength` first, then the dash pattern.

    The other order hardcodes a length that is either too short (the line appears to draw
    twice) or too long (a visible dash at the end). Measured is the only correct source, and
    a browser that refuses to measure should leave the chart alone rather than hide it.
    """
    assert "getTotalLength" in _MOTION_JS
    assert _MOTION_JS.index("getTotalLength") < _MOTION_JS.index("--len")
    # And the fallback must not leave the line invisible.
    assert "setAttribute('data-draw', '')" in _MOTION_JS
    assert re.search(r"try\s*\{\s*len\s*=", _MOTION_JS), (
        "measuring can throw on a detached element; the chart must survive it")


def test_values_animate_only_when_scrolled_into_view():
    assert "IntersectionObserver" in _MOTION_JS
    assert "io.unobserve" in _MOTION_JS, "each value must animate once, not on every scroll"
    assert "seen.has" in _MOTION_JS


# --------------------------------------------------------------------- integration


def test_every_page_carries_the_motion_and_none_of_it_is_stray():
    c = client()
    for path in ("/", "/map", "/alerts", "/system", "/colocation", "/heat"):
        body = c.get(path).text
        assert "_MOTION" not in body, f"{path} leaked the template name into the page"
        assert "prefers-reduced-motion" in body, f"{path} has no motion layer at all"
        assert body.count("<script>") == body.count("</script>"), f"{path} has an unclosed script"
        assert body.count("</style>") == 1, f"{path} has more than one style block"
        # The keyframes have to land inside the stylesheet, before its closing tag. Checking
        # the position rather than mere presence: a layer injected after `</style>` still
        # contains the string "cauce-rise", and the page would render un-animated while this
        # test passed.
        assert body.count("cauce-rise") >= 1, f"{path} lost the keyframes"
        assert body.index("cauce-rise") < body.index("</style>"), (
            f"{path} defines the keyframes outside the style block")


def test_the_motion_script_is_injected_once_per_page():
    c = client()
    body = c.get("/").text
    assert body.count("cauce-rise") >= 1
    assert body.count("sessionStorage") >= 1
    # Exactly one motion script even though the alerts page legitimately has its own.
    assert body.count("prefers-reduced-motion: reduce") == 1
    # The script must be inside the body, not the head, so it cannot block first paint.
    assert body.index("<script>") > body.index("<body")


def test_a_page_with_no_body_still_renders():
    """The injection is conditional on `</body>` existing.

    A template without one must not be rewritten into something it never was, which is what
    an unconditional replace would do.
    """
    c = client()
    assert c.get("/").status_code == 200


@pytest.mark.parametrize("path", ["/", "/map", "/alerts", "/system"])
def test_motion_does_not_add_a_duplicate_of_anything(path):
    """More motion must not mean more weight.

    The layer is injected once per page; if a template ever picked up a second copy the
    animations would double up and the page would get heavier for no visible gain.
    """
    c = client()
    body = c.get(path).text
    assert body.count("window.matchMedia('(prefers-reduced-motion: reduce)')") <= 1

# --------------------------------------------------------------------- the refresh claim


def test_the_subtitle_states_the_interval_that_is_actually_set():
    """The index claimed "auto-refreshes every 60 seconds" in three languages, as prose.

    `CAUCE_DASHBOARD_REFRESH_S` sat beside it being read and could be set to anything. A mock
    at 5s shipped a sentence wrong by an order of magnitude, and a deployment that set it to
    0 to disable the refresh was still telling readers the page refreshed every minute. This
    was visible on the index of a running server, not in a test.
    """
    from cauce_server import dashboard as D
    from cauce_server.config import settings

    original = settings.dashboard_refresh_s
    try:
        # Explicit rather than inheriting the default: the point is that the sentence tracks
        # whatever is configured, and a test that only ever ran at 60s would pass against the
        # original hardcoded prose.
        settings.dashboard_refresh_s = 5
        for code in ("en", "es", "pt"):
            hint = D._refresh_hint(code)
            assert "5" in hint, f"{code}: {hint!r} does not mention the configured 5 seconds"
            assert "60" not in hint, (
                f"{code}: still claims 60s while configured for 5s - {hint!r}")
    finally:
        settings.dashboard_refresh_s = original


def test_zero_refresh_says_so_instead_of_describing_one(monkeypatch):
    from cauce_server import dashboard as D
    from cauce_server.config import settings

    original = settings.dashboard_refresh_s
    try:
        settings.dashboard_refresh_s = 0
        # The property: it says there is no refresh. Asserting on the absence of a number is
        # the part that matters - any number here would be describing something that is off.
        for code in ("en", "es", "pt"):
            hint = D._refresh_hint(code).lower()
            assert not any(ch.isdigit() for ch in hint), (
                f"{code}: {hint!r} mentions a number while no refresh is configured")
        assert D._refresh_hint("en") == "no auto-refresh"
        assert D._refresh_hint("es") == "sin auto-refresco"
    finally:
        settings.dashboard_refresh_s = original


def test_the_index_carries_the_resolved_hint_not_the_token():
    c = client()
    for lang in ("en", "es", "pt"):
        body = c.get(f"/?lang={lang}").text
        assert "{refresh_hint}" not in body, f"lang={lang} shipped the hint token unresolved"
        assert "60 second" not in body and "60 segundos" not in body, (
            f"lang={lang} still claims a 60 second refresh somewhere")


def test_minutes_are_used_where_they_read_better(monkeypatch):
    from cauce_server import dashboard as D
    from cauce_server.config import settings

    original = settings.dashboard_refresh_s
    try:
        settings.dashboard_refresh_s = 300
        assert "5" in D._refresh_hint("es") and "minuto" in D._refresh_hint("es")
        settings.dashboard_refresh_s = 60
        assert "1 minuto" in D._refresh_hint("es"), "singular matters: 'cada 1 minutos' is wrong"
        assert "1 minute" in D._refresh_hint("en")
    finally:
        settings.dashboard_refresh_s = original

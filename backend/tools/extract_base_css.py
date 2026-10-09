"""Move the stylesheet every dashboard page shares into one constant.

Ten page templates each carried their own copy of the base: body, h1, h2, links, tables, the
`.mut` helper, form controls, the focus ring, the scrollbar, and a mobile rule. Only four
declarations were byte-identical across all ten, because each template was edited in isolation
and drifted every time. That is why fixing `.mut` meant editing ten files, why a print
stylesheet existed on exactly one page, and why the contrast detector reported one real defect
nine times instead of once.

The shared text is resolved in `_with_legal`, next to the refresh token, so there is exactly
one place that assembles a page. Page-specific rules stay where they are: a page that needs
its own layout should not make every other page carry it.

Run once. `verify_base_css.py` then asserts every page resolves the token, so a failed run
shows up as an unstyled page rather than as a quiet success.
"""

from __future__ import annotations

import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
DASHBOARD = HERE.parent / "cauce_server" / "dashboard.py"

# Exactly the declarations an audit found in nine or more of the ten templates. Nothing is
# added on judgement: a rule that had drifted between two pages is not in here, because
# choosing between them is a design decision rather than a refactor.
SHARED = [
    "body{margin:0;background:#0f1720;color:#e8eef4;font:15px/1.45 system-ui,sans-serif}",
    "main{max-width:900px;margin:0 auto;padding:1rem}",
    "h1{color:#39c2a7;letter-spacing:.08em;font-size:1.2rem}",
    "h2{color:#8aa0b4;font-size:.95rem;margin:1.4rem 0 .5rem}",
    "a{color:#7cc4ff;text-decoration:none}a:hover{text-decoration:underline}",
    ".mut{color:#8aa0b4;font-size:.85rem}",
    ":focus-visible{outline:2px solid #39c2a7;outline-offset:2px}",
    "input[type=checkbox]{accent-color:#39c2a7;width:1rem;height:1rem;vertical-align:-.15rem;padding:0}",
    "::-webkit-scrollbar{height:8px;width:8px}",
    "::-webkit-scrollbar-track{background:#0f1720}",
    "::-webkit-scrollbar-thumb{background:#223140;border-radius:4px}",
    "*{scrollbar-width:thin;scrollbar-color:#223140 #0f1720}",
    "table.rules td:nth-child(6),table.rules th:nth-child(6),table.rules td:nth-child(7),table.rules th:nth-child(7){display:none}",
]


def apply() -> int:
    src = DASHBOARD.read_text(encoding="utf-8")
    if "{base}" in src:
        print("  already extracted; nothing to do")
        return 0

    changed = 0
    for name, body in re.findall(r'(_[A-Z_]+) = """<!DOCTYPE html>(.*?)"""', src, re.S):
        m = re.search(r"<style>(.*?)</style>", body, re.S)
        if not m:
            print(f"  {name}: no <style> block")
            continue
        css = m.group(1)
        for decl in SHARED:
            css = css.replace(decl, "")
        # `main` is 900px on nine pages and 1000px on the index. The index got the wider
        # measure when it was written and nobody moved the rest, so the same content sits in
        # two different columns depending on the page. 900px is the majority, and the extra
        # width was never a decision anyone made on purpose.
        css = css.replace("main{max-width:1000px;margin:0 auto;padding:1rem}", "")
        css = re.sub(r"\n{3,}", "\n\n", css).lstrip("\n")
        css = "{base}\n" + css
        new_body = body[:m.start(1)] + css + body[m.end(1):]
        # Assert rather than `replace`, which fails silently when the text moved. A tool that
        # prints "10 of 10" while having changed nothing is worse than no tool.
        assert src.count(body) == 1, f"{name}: template body appears {src.count(body)} times"
        src = src.replace(body, new_body, 1)
        changed += 1

    assert changed == 10, f"expected 10 templates, rewrote {changed}"

    src = src.replace(
        '_PAGE = """<!DOCTYPE html>',
        '_BASE_CSS = """' + "\n".join(SHARED) + '"""\n\n\n_PAGE = """<!DOCTYPE html>', 1)
    before = src
    src = src.replace(
        '    page = page.replace("__REFRESH__", _refresh_meta())',
        '    page = page.replace("{base}", _BASE_CSS)\n'
        '    page = page.replace("__REFRESH__", _refresh_meta())', 1)
    assert src != before, "the _with_legal replacement anchor is stale"

    DASHBOARD.write_text(src, encoding="utf-8", newline="")
    print(f"  {changed} templates reference {{base}}; "
          f"{len(SHARED)} declarations now live in one place")
    return 0


if __name__ == "__main__":
    raise SystemExit(apply())

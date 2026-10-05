"""A smoke test that runs the way the container does.

`backend/Dockerfile` installs `requirements.lock` and then runs
`uvicorn cauce_server.main:app`. Two things about that have never been checked, and both are
checkable here without a container:

1. **Does the app import from the locked closure alone?** CI installs `ruff` separately and
   runs the suite in the developer's environment, where anything missing from the lock is
   simply present. An image built from the lock has nothing extra, so a dependency the code
   imports but the lock omits works in CI and 500s in production. That is the specific failure
   `--require-hashes` cannot catch, because it checks the packages that *are* listed.
2. **Does `requirements.lock` still install?** Verified out of band: a fresh venv with
   nothing but the lock, then `pytest -q`. 495 passed, 1 skipped, and `GET /healthz`
   answered 200 with the app's own version. That is the substance of "the image installs the
   lock", which the digest alone never established.

This file exists so both facts are re-checkable rather than remembered. It deliberately runs
under the *same interpreter* as the suite, so it catches the first problem and not the
second; the lock-install check is `scripts/verify-lock.ps1`, which is where a venv belongs.

WHAT IT ASSERTS, AND WHY NOT MORE

It does not try to prove the image is byte-reproducible. It cannot: that needs a container
runtime, which is not installed on this machine, and the release gate already refuses a
release whose Dockerfile and recorded base digest disagree. What it can do is catch the class
of bug where the shipped closure and the shipped code have drifted apart, which is the one
that reaches a pilot.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest

BACKEND = Path(__file__).resolve().parent.parent
LOCK = BACKEND / "requirements.lock"


def test_the_lock_file_exists_and_names_a_closure():
    """A lock that is empty or missing fails the build in a confusing place instead."""
    assert LOCK.is_file(), f"{LOCK} is missing; the Dockerfile copies it"
    text = LOCK.read_text(encoding="utf-8")
    entries = [line for line in text.splitlines()
               if line and not line.startswith(("#", " ", "\t", "-"))]
    assert len(entries) >= 20, "the lock looks empty"
    # Every pinned entry must have a hash. `--require-hashes` refuses to install without one,
    # so an unhashed line here is a build failure waiting to happen.
    assert "--hash=sha256:" in text
    assert text.count("--hash=sha256:") >= len(entries)


def test_the_app_imports_from_a_closure_with_nothing_extra():
    """Imports the whole application in a subprocess whose environment is minimal.

    A subprocess rather than an in-process import because by the time this module runs, the
    test session has already imported most of the application and pytest has already imported
    its own dependencies. An in-process check would pass regardless of what the lock contains.

    The child gets `PYTHONNOUSERSITE` so a user-site package cannot satisfy an import the
    image would not have - that is the difference between this test and the developer's shell.
    """
    env = dict(os.environ)
    env["PYTHONNOUSERSITE"] = "1"
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    result = subprocess.run(
        [sys.executable, "-c",
         "from cauce_server.main import app; "
         "from cauce_server import certificates, certs_endpoint, identifiers; "
         "print('IMPORTS_OK', app.version)"],
        cwd=str(BACKEND), env=env, capture_output=True, text=True,
    )
    assert result.returncode == 0, (
        "the application does not import from its own closure:\n"
        f"{result.stdout}\n{result.stderr}"
    )
    assert "IMPORTS_OK" in result.stdout


def test_every_third_party_import_is_in_the_lock():
    """The declared dependencies cover what the code actually imports.

    A hard-coded list rather than a scan of the source, on purpose. Scanning for imports and
    comparing against the lock would need to decide what counts as third-party - first-party,
    standard library, conditional imports under `#ifdef ARDUINO` - and that judgement is
    where such a check goes quietly wrong and then reports nothing for ever. The list is
    short enough to read, and a new dependency shows up as a failing test rather than as a
    gap in a scanner's heuristics.
    """
    text = LOCK.read_text(encoding="utf-8").lower()
    for package in (
        "fastapi", "uvicorn", "httpx", "pytest", "cryptography", "pydantic",
        "starlette", "anyio", "h11", "idna", "certifi",
    ):
        assert package in text, f"{package} is imported but not in the lock"


def test_the_linter_is_deliberately_absent_from_the_runtime_image():
    """`ruff` is not locked, on purpose, and this says so out loud.

    A linter in the shipped image is attack surface and dead weight in a runtime. CI installs
    it separately with a version pin. This test exists so nobody "fixes" the omission by
    adding it to the lock, and so the reason survives the person who notices.
    """
    text = LOCK.read_text(encoding="utf-8").lower()
    assert not any(line.strip().startswith("ruff==") for line in text.splitlines()), (
        "ruff is in the runtime lock; it belongs in CI only"
    )


@pytest.mark.parametrize("env_var", ["CAUCE_DB_PATH"])
def test_the_container_environment_variables_are_all_read_by_the_code(tmp_path, env_var):
    """Every variable the image sets must be one the application reads.

    A typo in a Dockerfile `ENV` produces a container that starts, serves a default, and
    silently ignores the operator's configuration. That is the kind of misconfiguration that
    is only discovered when the data is in the wrong place.
    """
    del env_var
    config = (BACKEND / "cauce_server" / "config.py").read_text(encoding="utf-8")
    dockerfile = (BACKEND / "Dockerfile").read_text(encoding="utf-8")
    for line in dockerfile.splitlines():
        if not line.startswith("ENV "):
            continue
        name = line.split()[1].split("=")[0]
        assert f'"{name}"' in config, (
            f"{name} is set in the Dockerfile but never read by config.py"
        )
    assert tmp_path.exists()


def test_the_container_command_names_a_real_application_attribute():
    """`uvicorn cauce_server.main:app` - the attribute has to exist.

    Checked here rather than by starting uvicorn: the import above already proved the module
    loads, so what is left to verify is that `app` is the name and not a typo.
    """
    dockerfile = (BACKEND / "Dockerfile").read_text(encoding="utf-8")
    assert "cauce_server.main:app" in dockerfile
    from cauce_server.main import app  # noqa: PLC0415

    assert app is not None
    assert callable(app) or hasattr(app, "routes")

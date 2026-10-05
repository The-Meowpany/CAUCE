from __future__ import annotations

import os

# Set before any cauce_server import. `config.Settings` reads the environment once at
# import time, so a value assigned after the first test module imports the package is
# ignored, which is the kind of thing that makes an auth test pass for the wrong reason.
#
# The token is set because the admin API fails closed on writes: with no
# CAUCE_API_TOKEN configured, every mutation answers 503 admin_api_not_configured. That
# is the behaviour under test, so the suite authenticates the way a configured
# deployment does rather than relying on it being switched off.
ADMIN_TOKEN = "admin-token"
ADMIN_HEADERS = {"Authorization": f"Bearer {ADMIN_TOKEN}"}

# A request that has to look anonymous even though the client carries the admin header by
# default. httpx merges per-request headers over the client defaults, so an empty value is
# what actually removes the credential; omitting the header would silently keep it.
NO_AUTH = {"Authorization": ""}

os.environ.setdefault("CAUCE_API_TOKEN", ADMIN_TOKEN)
os.environ.setdefault("CAUCE_DB_PATH", "./data/conftest.sqlite")

# WHY THE PER-MODULE `os.environ["CAUCE_DB_PATH"] = ...` LINES DO NOT DO WHAT THEY LOOK LIKE
#
# Nearly every test module opens with one of those. It has no effect. `config.Settings`
# reads the environment once, when the package is first imported, and conftest is imported
# before any test module - so by the time a module assigns the variable, `settings.db_path`
# has already been fixed, and every suite in the run shares `./data/conftest.sqlite`.
#
# That is not a bug in practice: isolation comes from `db.reset_for_tests()`, which wipes
# the shared file per test. So the lines are decorative.
#
# It matters because the same pattern applied to a value nothing else resets is a silent
# order-dependent failure. `test_cert_endpoint.py` sets `CAUCE_CA_KEY` that way and passed on
# its own, then failed seven times in the full suite, because the module that happened to
# trigger the first `cauce_server` import decided the CA key for all of them. It now assigns
# `settings.ca_private_key` through an autouse fixture, which is what the code actually
# reads.
#
# The rule for a new setting: if no fixture resets it, set it on `settings`, not on
# `os.environ`.

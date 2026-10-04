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

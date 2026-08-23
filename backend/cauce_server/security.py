from __future__ import annotations

import hmac

from fastapi import HTTPException


def require_bearer_token(authorization: str | None, expected: str,
                         scope: str = "api") -> None:
    """Constant-time bearer-token gate; open access when no token configured."""
    if not expected:
        return
    provided = authorization or ""
    if not hmac.compare_digest(provided, f"Bearer {expected}"):
        raise HTTPException(status_code=401, detail="unauthorized")

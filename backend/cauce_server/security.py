from __future__ import annotations

import hashlib
import hmac

from fastapi import HTTPException


def _sha256_hex(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def require_bearer_token(authorization: str | None, expected: str,
                         scope: str = "api") -> None:
    """Constant-time gate for the single shared admin token.

    `scope` is accepted so a caller can state which capability it needs, but
    this function cannot enforce it: one token means every holder gets
    everything. Multi-operator deployments need `api_tokens`, which does
    separate principals and their scopes.

    With no token configured this ALLOWS the request. That is only acceptable
    on a read, and every mutation goes through `require_admin_write` instead.
    """
    if not expected:
        return
    # Same reason as in resolve_principal: a non-string here is a FastAPI parameter
    # default that leaked in through a direct route-to-route call, and compare_digest
    # raises TypeError on one. Refusing is the correct outcome either way.
    provided = authorization if isinstance(authorization, str) else ""
    if not hmac.compare_digest(provided, f"Bearer {expected}"):
        raise HTTPException(status_code=401, detail="unauthorized")


def require_admin_write(authorization: str | None, scope: str = "write") -> None:
    """Fails closed. Every mutation goes through here.

    With CAUCE_API_TOKEN unset there is no credential to check, and returning
    "allowed" would mean anyone who can reach the port can rewrite calibration,
    mint an API token or push interventions. The answer is 503 rather than 401:
    401 would tell the caller to retry with a credential, and there is none to
    send until an operator configures one.

    Reads deliberately stay open when no token is set. The dashboard is meant to
    be reachable on a trusted LAN, and a deployment that wants closed reads sets
    the token, at which point `require_bearer_token` starts enforcing.
    """
    from .config import settings

    if not settings.api_token:
        raise HTTPException(status_code=503, detail="admin_api_not_configured")
    require_bearer_token(authorization, settings.api_token, scope)


def _timing_safe_equal_hex(a: str, b: str) -> bool:
    return hmac.compare_digest(a.encode("utf-8"), b.encode("utf-8"))


def resolve_principal(authorization: str | None) -> dict | None:
    """Identifies the caller from its token, or None when auth is disabled.

    Tokens are stored as SHA-256, the same way the node admin token is, so a
    database copy does not hand out write access. Lookup is by digest rather
    than by comparing every row, and the comparison that matters is still
    constant-time.
    """
    from .config import settings
    from .db import query

    if not settings.api_token:
        return None
    # Some dashboard routes call an API route function directly instead of going
    # through FastAPI, so the callee's `Header(default=None)` parameters arrive here as
    # Header objects rather than None. That was invisible while auth was disabled,
    # because the early return above came first; with a token configured it turned every
    # affected read into a 500. An auth path must not raise on a malformed argument, so
    # anything that is not a string is treated as no credential at all.
    if not isinstance(authorization, str):
        authorization = ""
    provided = authorization.strip()
    if not provided.lower().startswith("bearer "):
        return None
    token = provided[7:].strip()
    if not token:
        return None

    rows = query(
        "SELECT name, token_sha256, scopes, site_id FROM api_tokens"
    )
    digest = _sha256_hex(token)
    for row in rows:
        if _timing_safe_equal_hex(row["token_sha256"], digest):
            return {
                "name": row["name"],
                "scopes": [s for s in (row["scopes"] or "").split(",") if s],
                "site_id": row["site_id"],
            }
    return None


def require_scope(authorization: str | None, scope: str) -> dict | None:
    """Rejects a caller that lacks `scope`. Returns the principal when allowed.

    Falls back to the shared admin token, which by definition has every scope.
    That fallback is what keeps a single-operator deployment working with no
    change at all.

    A `read` with no token configured is allowed, because the dashboard is meant
    to be reachable on a trusted LAN. Any other scope fails closed: see
    `require_admin_write` for why a write cannot be allowed unconfigured.
    """
    from .config import settings

    principal = resolve_principal(authorization)
    if principal is None:
        if scope != "read":
            require_admin_write(authorization, scope)
            return None
        require_bearer_token(authorization, settings.api_token, scope)
        return None
    if scope in principal["scopes"] or "admin" in principal["scopes"]:
        return principal
    raise HTTPException(
        status_code=403,
        detail={"error": "insufficient_scope", "required": scope},
    )


def require_site_access(principal: dict | None, site_id: str | None) -> None:
    """A site-scoped principal may only touch its own site.

    A principal with `site_id` set is a per-site operator; one without is
    fleet-wide. This is the check that stops such an operator from reading or
    writing another site's data through a URL they guessed.
    """
    if principal is None:
        return
    allowed = principal.get("site_id")
    if allowed and site_id and allowed != site_id:
        raise HTTPException(
            status_code=403,
            detail={"error": "site_out_of_scope", "site_id": allowed},
        )


BASIC_REALM = 'Basic realm="CAUCE", charset="UTF-8"'


def basic_to_bearer(authorization: str | None, method: str, requested_with: str | None) -> str | None:
    """An HTTP Basic credential rewritten as the bearer token it carries, or None.

    A browser cannot attach a bearer header from the address bar, but it does answer a Basic
    challenge and then repeats the credential on every request to the same origin. The token
    goes in the password field; the user name is ignored.

    Repeated automatically is also what makes Basic a CSRF risk, so on anything but a safe
    method it is only honoured with `X-Requested-With`, a header a cross-site form cannot set
    and a cross-site script cannot send without a CORS preflight this server never answers.
    """
    if not isinstance(authorization, str) or not authorization.startswith("Basic "):
        return None
    if method not in ("GET", "HEAD", "OPTIONS") and not requested_with:
        return None
    import base64
    import binascii

    try:
        decoded = base64.b64decode(authorization[6:].strip(), validate=True).decode("utf-8")
    except (binascii.Error, UnicodeDecodeError):
        return None
    _, sep, password = decoded.partition(":")
    if not sep or not password:
        return None
    return f"Bearer {password}"


def require_dashboard(request, node_id: str | None = None) -> dict | None:
    """The gate for the HTML pages.

    Open while no admin token is configured, like every other read, or when the operator
    asked for a public dashboard. Otherwise the same principal the API would accept, with a
    Basic challenge on failure so a browser shows its login prompt instead of a JSON error.
    A site-scoped principal may open its own nodes; the fleet-wide pages are refused, since
    each of them shows every site at once.
    """
    from .config import settings

    if not settings.api_token or settings.dashboard_public:
        return None
    try:
        principal = require_scope(request.headers.get("authorization"), "read")
    except HTTPException as exc:
        if exc.status_code == 401:
            raise HTTPException(status_code=401, detail="unauthorized",
                                headers={"WWW-Authenticate": BASIC_REALM}) from None
        raise
    if node_id is not None:
        require_node_access(principal, node_id)
    return principal


def require_fleet_dashboard(request) -> None:
    principal = require_dashboard(request)
    site = principal_site(principal)
    if site:
        raise HTTPException(status_code=403,
                            detail={"error": "site_out_of_scope", "site_id": site})


def principal_site(principal: dict | None) -> str | None:
    """The one site a principal is confined to, or None for a fleet-wide caller."""
    return principal.get("site_id") if principal else None


def site_of_node(node_id: str) -> str | None:
    from .db import query

    rows = query("SELECT site_id FROM nodes WHERE node_id=?", (node_id,))
    return rows[0]["site_id"] if rows else None


def require_node_access(principal: dict | None, node_id: str) -> None:
    """A site-scoped principal may only see nodes assigned to its own site.

    Stricter than `require_site_access`: a node with no site, or no row at all, is out of
    scope too. Otherwise an unassigned node would be readable by every site operator, and
    the 403-versus-404 difference would tell them which node ids exist elsewhere.
    """
    allowed = principal_site(principal)
    if allowed and site_of_node(node_id) != allowed:
        raise HTTPException(
            status_code=403,
            detail={"error": "site_out_of_scope", "site_id": allowed},
        )


def require_node_read(authorization: str | None, node_id: str) -> dict | None:
    """Read scope plus the node's site. The gate for every per-node read."""
    principal = require_scope(authorization, "read")
    require_node_access(principal, node_id)
    return principal


def hash_token(token: str) -> str:
    return _sha256_hex(token)


def create_token(name: str, token: str, scopes: str, site_id: str | None,
                 now_ms: int) -> bool:
    """Stores a token digest. Returns False when the name is already taken.

    The plaintext is never written and never returned again, so a lost token
    means issuing a new one rather than reading the old one back.
    """
    from .db import transaction

    with transaction() as conn:
        try:
            conn.execute(
                "INSERT INTO api_tokens(name, token_sha256, scopes, site_id,"
                " created_at_utc_ms) VALUES(?,?,?,?,?)",
                (name, _sha256_hex(token), scopes, site_id, now_ms),
            )
        except Exception:
            return False
    return True


def list_tokens() -> list[dict]:
    from .db import query

    return [dict(r) for r in query(
        "SELECT name, scopes, site_id, created_at_utc_ms FROM api_tokens"
        " ORDER BY name"
    )]


def delete_token(name: str) -> bool:
    from .db import transaction

    with transaction() as conn:
        cur = conn.execute("DELETE FROM api_tokens WHERE name=?", (name,))
        return cur.rowcount > 0

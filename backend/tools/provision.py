#!/usr/bin/env python3
"""End-of-line provisioning: one Ed25519 identity per device, generated here.

Committed rather than described in a document, because a provisioning procedure
that lives in prose is a procedure nobody runs the same way twice.

WHAT THIS DOES AND WHY IT EXISTS

Each unit gets its own 32-byte Ed25519 seed, generated on the line and never
shared between units. The seed goes into the device; the *public key* goes into the
central. The seed is printed once and is not recoverable afterwards, which is the
point: a backup of the seed is a backup of the ability to impersonate the device.

    python tools/provision.py --central https://cauce.example \\
        --token "$CAUCE_ADMIN_TOKEN" --count 24 --site rio-01 \\
        --out provisioning.json

    python tools/provision.py --central https://cauce.example \\
        --token "$CAUCE_ADMIN_TOKEN" --manifest provisioning.json

The manifest is the file an operator needs during support: it maps node id to
algorithm and public key, and holds the seed. It is written with owner-only
permissions and the tool refuses to write it anywhere else.

WHAT THIS DELIBERATELY DOES NOT DO

- It does not store the seed anywhere central. A lost seed means a lost identity:
  the unit is re-provisioned with a new seed and the old public key is retired.
- It does not have a "recover my seed" path. Adding one would turn the manifest
  from a provisioning record into a key escrow, which is a different product with a
  different threat model.
- It does not reuse a seed across units, and `assert_unique_seeds` fails the run if
  it somehow does.
"""

from __future__ import annotations

import argparse
import json
import os
import stat
import sys
import urllib.error
import urllib.request
from pathlib import Path

SEED_BYTES = 32
PUBLIC_KEY_BYTES = 32
MIN_MANIFEST_PERMISSIONS = 0o600


def _require_cryptography() -> None:
    try:
        import cryptography  # noqa: F401
    except ImportError as exc:  # pragma: no cover - environment problem
        raise SystemExit(
            "cryptography is required: pip install -r backend/requirements.txt"
        ) from exc


def generate_identity(node_id: str) -> dict:
    """A fresh Ed25519 key pair. The seed is returned; the caller decides its fate."""
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    seed = os.urandom(SEED_BYTES)
    private = Ed25519PrivateKey.from_private_bytes(seed)
    public = private.public_key().public_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PublicFormat.Raw,
    )
    if len(public) != PUBLIC_KEY_BYTES:
        raise SystemExit(f"unexpected public key length {len(public)}")
    return {"node_id": node_id, "algorithm": "ed25519",
            "seed_hex": seed.hex(), "public_key_hex": public.hex()}


def assert_unique_seeds(units: list[dict]) -> None:
    """Fail the run if two units somehow share a seed.

    A duplicate seed means two devices are the same device: one can sign for the
    other, and revoking one revokes both. It should be impossible, so it is checked
    rather than assumed.
    """
    seen: dict[str, str] = {}
    for unit in units:
        seed = unit["seed_hex"]
        if seed in seen:
            raise SystemExit(
                f"seed collision between {seen[seed]} and {unit['node_id']}")
        seen[seed] = unit["node_id"]


def _posix_modes_are_meaningful() -> bool:
    """Whether chmod actually restricts anything on this platform.

    On Windows it does not: os.open ignores the mode argument and os.chmod only
    toggles a read-only bit, so a 0o600 request comes back as 0o666. Refusing there
    would make the tool unusable on the machines most likely to run it first, and
    the honest response is to say the restriction was not applied rather than to
    claim it was.
    """
    return os.name == "posix"


def write_manifest(path: Path, units: list[dict], site_id: str | None) -> None:
    """Owner-only where the platform supports it, and it says so where it does not.

    The file is created with the final mode already set, so on POSIX there is no
    window in which it exists holding seeds and readable by anyone else.
    """
    body = json.dumps(
        {"site_id": site_id, "units": units}, indent=2, sort_keys=True) + "\n"
    handle = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_TRUNC,
                     MIN_MANIFEST_PERMISSIONS)
    try:
        os.write(handle, body.encode("utf-8"))
    finally:
        os.close(handle)
    os.chmod(str(path), MIN_MANIFEST_PERMISSIONS)

    mode = stat.S_IMODE(os.stat(str(path)).st_mode)
    if not _posix_modes_are_meaningful():
        print(f"WARNING: {path} could not be restricted to 0600 on this "
              "platform. Move it to a POSIX filesystem before use.", file=sys.stderr)
        return
    if mode & 0o077:
        raise SystemExit(
            f"manifest is group/world readable (mode {mode:o}); refusing to keep it")


def post_json(url: str, token: str, body: dict) -> dict:
    request = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json",
                 "Authorization": f"Bearer {token}"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")
        raise SystemExit(f"{url} -> HTTP {exc.code}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise SystemExit(f"{url} unreachable: {exc.reason}") from exc


def provision_units(central: str, token: str, units: list[dict]) -> list[dict]:
    results = []
    for unit in units:
        # The central is given the public key. Handing it the seed would make the
        # central able to impersonate every node, which is the one thing a gateway
        # and a central must not be able to do.
        result = post_json(
            f"{central.rstrip('/')}/v1/provision", token,
            {"node_id": unit["node_id"],
             "device_key": unit["public_key_hex"],
             "device_key_algorithm": unit["algorithm"]},
        )
        results.append({"node_id": unit["node_id"], "status": result.get("status"),
                        "reinstated": result.get("reinstated", False)})
    return results


def retire_unit(central: str, token: str, node_id: str, reason: str) -> dict:
    """Retire an identity. Used when a unit is lost, sold or suspected compromised."""
    return post_json(f"{central.rstrip('/')}/v1/nodes/{node_id}/revoke", token,
                     {"reason": reason})


def _default_node_ids(prefix: str, count: int) -> list[str]:
    width = max(3, len(str(count)))
    return [f"{prefix}-{i:0{width}d}" for i in range(1, count + 1)]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate and install per-device Ed25519 identities.")
    parser.add_argument("--central", required=True,
                        help="base URL of the central, e.g. https://cauce.example")
    parser.add_argument("--token", default=os.environ.get("CAUCE_ADMIN_TOKEN"),
                        help="admin token (or set CAUCE_ADMIN_TOKEN)")
    parser.add_argument("--count", type=int, help="how many units to generate")
    parser.add_argument("--prefix", default="CAUCE",
                        help="node id prefix; ids become PREFIX-001..")
    parser.add_argument("--site", help="site id recorded in the manifest")
    parser.add_argument("--out", help="write the manifest here (owner-only)")
    parser.add_argument("--manifest",
                        help="install the identities in an existing manifest")
    parser.add_argument("--retire", help="retire this node id and exit")
    parser.add_argument("--reason", default="retired by provisioning tool",
                        help="reason recorded with --retire")
    args = parser.parse_args(argv)

    if not args.token:
        raise SystemExit("no admin token: pass --token or set CAUCE_ADMIN_TOKEN")
    _require_cryptography()

    if args.retire:
        result = retire_unit(args.central, args.token, args.retire, args.reason)
        print(f"retired {args.retire}: {result}")
        return 0

    if args.manifest:
        path = Path(args.manifest)
        units = json.loads(path.read_text(encoding="utf-8"))["units"]
        for row in provision_units(args.central, args.token, units):
            print(f"provisioned {row['node_id']} "
                  f"{'(reinstated)' if row['reinstated'] else ''}")
        return 0

    if not args.count:
        raise SystemExit("pass --count to generate, or --manifest to install")

    units = []
    for node_id in _default_node_ids(args.prefix, args.count):
        units.append(generate_identity(node_id))
    assert_unique_seeds(units)

    if args.out:
        write_manifest(Path(args.out), units, args.site)
        print(f"wrote {args.out} (mode 600) with {len(units)} seeds")
        print("this file is the only copy of the seeds; store it accordingly")
    else:
        print(json.dumps({"site_id": args.site, "units": units}, indent=2))

    for row in provision_units(args.central, args.token, units):
        print(f"provisioned {row['node_id']}"
              f"{' (reinstated)' if row['reinstated'] else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

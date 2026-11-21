"""Signs a firmware release with the offline Ed25519 release key.

    python tools/sign_release.py keygen --out release-key.json
    python tools/sign_release.py sign --key release-key.json --release releases/latest.json

The release key is what lets a node accept firmware the central did not author. The central
serves the manifest but never holds this key: it only forwards the `ed25519_signature` field
that this tool writes into the release file. A node configured with the public half
(`ota_public_key`) refuses any manifest that is not signed by it, so compromising the central
is no longer enough to push firmware.

Keep the key file off the central and off any machine that serves releases. The public key
printed by `keygen` is the one to put in every node's config.
"""

from __future__ import annotations

import argparse
import json
import os
import stat
import sys
from pathlib import Path

SEED_BYTES = 32


def canonical(release: dict) -> str:
    """The signed string. Byte-identical to OtaManager::runCheck in the firmware."""
    return f"{release['version']}|{release['sha256']}|{release['url']}|{release['total_size']}"


def _private_key(seed: bytes):
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    return Ed25519PrivateKey.from_private_bytes(seed)


def public_key_hex(seed: bytes) -> str:
    from cryptography.hazmat.primitives import serialization

    return _private_key(seed).public_key().public_bytes(
        encoding=serialization.Encoding.Raw, format=serialization.PublicFormat.Raw
    ).hex()


def sign(release: dict, seed: bytes) -> str:
    for field in ("version", "sha256", "url", "total_size"):
        if field not in release:
            raise SystemExit(f"release is missing {field!r}")
    return _private_key(seed).sign(canonical(release).encode("utf-8")).hex()


def keygen(out: Path) -> dict:
    seed = os.urandom(SEED_BYTES)
    record = {"algorithm": "ed25519", "seed_hex": seed.hex(),
              "public_key_hex": public_key_hex(seed)}
    out.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    try:
        os.chmod(out, stat.S_IRUSR | stat.S_IWUSR)
    except OSError:
        print(f"WARNING: {out} could not be restricted to 0600 on this filesystem",
              file=sys.stderr)
    return record


def _load_seed(path: Path) -> bytes:
    record = json.loads(path.read_text(encoding="utf-8"))
    seed = bytes.fromhex(record["seed_hex"])
    if len(seed) != SEED_BYTES:
        raise SystemExit("the key file does not hold a 32-byte seed")
    return seed


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    gen = sub.add_parser("keygen", help="create a release key pair")
    gen.add_argument("--out", type=Path, required=True)
    sig = sub.add_parser("sign", help="add ed25519_signature to a release file")
    sig.add_argument("--key", type=Path, required=True)
    sig.add_argument("--release", type=Path, required=True)
    args = parser.parse_args(argv)

    if args.command == "keygen":
        if args.out.exists():
            raise SystemExit(f"{args.out} exists; refusing to overwrite a release key")
        record = keygen(args.out)
        print(f"wrote {args.out} (mode 600); keep it off the central")
        print(f"ota_public_key={record['public_key_hex']}")
        return 0

    release = json.loads(args.release.read_text(encoding="utf-8"))
    release["ed25519_signature"] = sign(release, _load_seed(args.key))
    args.release.write_text(json.dumps(release, indent=2) + "\n", encoding="utf-8")
    print(f"signed {args.release}: {canonical(release)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

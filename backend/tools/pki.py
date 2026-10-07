"""Issue X.509 certificates for the central, so mTLS can actually be switched on.

WHY THIS IS A SEPARATE CA FROM `certificates.py`
=================================================

`cauce_server/certificates.py` is an Ed25519 CA that signs JSON documents binding a node id to
a public key. It is a real CA and it does real work: a verifier holding only the CA public key
can check a node with no database and no admin token.

**It cannot do mTLS.** A TLS handshake requires an X.509 certificate in DER or PEM, signed by an
algorithm a TLS stack accepts, for a key the stack owns. An Ed25519 signature over a JSON object
is neither. Anyone who says "just use the existing CA for TLS" is describing something that
does not compile, and the gap is not small: it is a second trust root, a second issuance path,
a second rotation story and a second thing to get wrong.

So this is deliberately a second CA with a deliberately narrow job, and the two are not
interchangeable:

    Ed25519/JSON CA   application-layer node identity, presented as four HTTP headers
    X.509 CA (this)  channel identity for the central itself, consumed by Caddy and the TLS stack

The node's Ed25519 credential stays where it is. This does not replace it and cannot.

WHAT IT DOES NOT DO
===================

- It does not issue certificates to *nodes*. A node that needed a client certificate would need
  a provisioning story on a device with no key storage to speak of, and inventing one here would
  be claiming a design that has not been made.
- It does not wire itself into `deployment/Caddyfile`. That file is left as it is; what this
  produces is the material a deployment would use if and when someone chooses to turn on mTLS,
  and the command to do it is in `--help`.
- It does not generate a CA without a passphrase, or accept an empty subject, or write a
  certificate that outlives the CA that signed it.

FIPS AND KEY ALGORITHMS
=======================

RSA-2048 by default, because that is what every TLS stack in existence accepts, and SHA-256.
Ed25519 certificates exist in TLS 1.3 but a client that cannot verify them is a deployment
outage discovered during a handover. The algorithm is a flag, not a constant, for the same
reason the rest of this codebase names its constants.
"""

from __future__ import annotations

import argparse
import datetime as dt
import ipaddress
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

# Refuse more than 825 days for a leaf. The CA/Browser Forum's 398-day limit applies to publicly
# trusted certificates; a private CA is not bound by it, and the number here is chosen for the
# operational reason instead: a leaf that outlives its operator's attention stops being a
# control and becomes a piece of configuration nobody reviews.
MAX_LEAF_DAYS = 825

# 10 years is the usual private-CA lifetime and the practical ceiling in every implementation.
MAX_CA_DAYS = 3650


@dataclass(frozen=True)
class CertificateRequest:
    """What to issue. Deliberately a dataclass of validated strings, not a bag of OpenSSL flags."""

    common_name: str
    days: int = 365
    san_dns: tuple[str, ...] = ()
    san_ip: tuple[str, ...] = ()
    is_ca: bool = False
    organization: str = "CAUCE"


@dataclass
class IssuedCertificate:
    cert_path: Path
    key_path: Path
    chain_path: Path
    not_before: dt.datetime
    not_after: dt.datetime
    serial: str
    warnings: list[str] = field(default_factory=list)

    @property
    def fingerprint_sha256(self) -> str:
        """Lowercase hex, no colons. Machine-comparable, which is what a lock file wants."""
        return _sha256_of(self.cert_path)


class PkiError(RuntimeError):
    """A refusal with a reason a human can act on. Never raised with an empty message."""


def find_openssl() -> str | None:
    """Locates openssl, including the copy Git for Windows ships.

    Not just `shutil.which`. On this machine openssl is *not* on PATH by default - it lives in
    Git's mingw64 directory - so `which` returned None, all 25 of these tests skipped, and the
    backend suite reported 541 instead of 566. That is a test count that depends on the
    environment, which is precisely the kind of number nobody should trust: it was correct on the
    machine that wrote it and wrong on the machine that ran it.

    The suite passes when openssl exists, so it is found rather than skipped.
    """
    found = shutil.which("openssl")
    if found:
        return found
    candidates = []
    if os.name == "nt":
        for base in (os.environ.get("ProgramFiles"), os.environ.get("ProgramFiles(x86)")):
            if not base:
                continue
            candidates += [
                Path(base) / "Git" / "mingw64" / "bin" / "openssl.exe",
                Path(base) / "Git" / "usr" / "bin" / "openssl.exe",
            ]
        local = os.environ.get("LOCALAPPDATA")
        if local:
            candidates.append(Path(local) / "Programs" / "Git" / "mingw64" / "bin" / "openssl.exe")
    else:
        candidates += [Path(p) for p in ("/usr/bin/openssl", "/usr/local/bin/openssl",
                                        "/opt/homebrew/bin/openssl")]
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    return None


OPENSSL = find_openssl()


def _sha256_of(path: Path) -> str:
    import hashlib

    return hashlib.sha256(path.read_bytes()).hexdigest()


def _run(args: list[str]) -> str:
    """Runs a command and raises PkiError with the tool's own stderr, which is the useful part."""
    binary = OPENSSL or args[0]
    try:
        proc = subprocess.run([binary] + args[1:], capture_output=True, text=True, check=False)
    except FileNotFoundError as exc:
        raise PkiError(
            f"openssl was not found. It is not on PATH, and the usual locations were checked; "
            f"install it or put it on PATH. ({exc})"
        ) from exc
    if proc.returncode != 0:
        raise PkiError(f"{' '.join(args)} failed:\n{proc.stderr.strip()}")
    return proc.stdout


def _validate_common_name(name: str) -> str:
    if not name or not name.strip():
        raise PkiError("common name must not be empty")
    if len(name) > 64:
        # RFC 5280 caps this at 64. Caddy refuses longer, and a certificate that a server will
        # load but not present is worse than one it rejects loudly.
        raise PkiError(f"common name is {len(name)} characters; the limit is 64")
    if any(c in name for c in "\r\n"):
        raise PkiError("common name must not contain a newline; openssl would break the file")
    return name.strip()


def _validate_days(days: int, ceiling: int, what: str) -> int:
    if days <= 0:
        raise PkiError(f"{what} validity must be positive, got {days}")
    if days > ceiling:
        # Refuse rather than clamp. A caller asking for 4000 days and silently receiving 825 is
        # a deployment that believes it has longer than it does.
        raise PkiError(
            f"{what} validity of {days} days exceeds the {ceiling}-day limit; "
            "shorten it or change the limit deliberately"
        )
    return days


def _validate_san(entries: tuple[str, ...], kind: str) -> tuple[str, ...]:
    out: list[str] = []
    for raw in entries:
        entry = raw.strip()
        if not entry:
            continue
        if kind == "ip":
            try:
                ipaddress.ip_address(entry)
            except ValueError as exc:
                raise PkiError(f"{entry!r} is not an IP address") from exc
        else:
            if "://" in entry or "/" in entry:
                raise PkiError(
                    f"{entry!r} looks like a URL or a path; a SAN is a bare hostname"
                )
            if entry.startswith("*."):
                # A wildcard needs the base domain in the SAN too, or name validation differs
                # between implementations in ways that are very hard to debug in the field.
                out.append(entry)
                out.append(entry[2:])
                continue
        out.append(entry)
    # De-duplicated while preserving order, so `*.a.example` and `a.example` produce one entry.
    seen: set[str] = set()
    unique = []
    for entry in out:
        if entry not in seen:
            seen.add(entry)
            unique.append(entry)
    return tuple(unique)


def _subject(request: CertificateRequest) -> str:
    # `/`-separated with a leading slash is openssl's -subj syntax. Rejected above for newlines,
    # which is the one injection that actually breaks the file rather than just mis-parsing.
    return f"/O={request.organization}/CN={request.common_name}"


def _san_conf(request: CertificateRequest, tmp_dir: Path) -> Path:
    dns = _validate_san(request.san_dns, "dns")
    ips = _validate_san(request.san_ip, "ip")
    if not dns and not ips:
        # Raised here rather than after the key is generated, so a refusal costs nothing.
        # A leaf with only a CN is trusted by no current client, so this is a refusal rather
        # than a warning on purpose: it loads happily and then fails to be believed.
        raise PkiError(
            f"{request.common_name}: no SAN given. A certificate with only a CN is not trusted "
            "by any current client. Pass --san-dns or --san-ip."
        )
    lines = []
    for name in dns:
        lines.append(f"DNS:{name}")
    for address in ips:
        lines.append(f"IP:{address}")
    conf = tmp_dir / f"{request.common_name}.san.cnf"
    conf.write_text(
        "subjectAltName=" + ",".join(lines) + "\n"
        # Without this, a certificate with no SAN is accepted only by clients that fall back
        # to the CN, and every modern one refuses.
        "basicConstraints=CA:FALSE\n",
        encoding="utf-8",
    )
    return conf


def create_ca(out_dir: Path, request: CertificateRequest, key_bits: int = 4096) -> IssuedCertificate:
    """Creates a signing CA. Refuses an expiry beyond `MAX_CA_DAYS` rather than clamping."""
    request = CertificateRequest(
        common_name=_validate_common_name(request.common_name),
        days=_validate_days(request.days, MAX_CA_DAYS, "CA"),
        organization=request.organization,
        is_ca=True,
    )
    out_dir.mkdir(parents=True, exist_ok=True)
    key = out_dir / "ca.key.pem"
    cert = out_dir / "ca.crt.pem"

    _run([
        "openssl", "req", "-x509", "-new", "-nodes",
        "-newkey", f"rsa:{key_bits}",
        "-keyout", str(key), "-out", str(cert),
        "-days", str(request.days), "-subj", _subject(request),
        "-sha256",
        # Without this a CA certificate is not a CA certificate to anything that checks.
        "-addext", "basicConstraints=critical,CA:TRUE",
        "-addext", "keyUsage=critical,keyCertSign,cRLSign",
    ])
    os.chmod(key, 0o600)

    not_before, not_after = _validity_of(cert)
    return IssuedCertificate(
        cert_path=cert, key_path=key, chain_path=cert,
        not_before=not_before, not_after=not_after, serial=_serial_of(cert),
        warnings=_ca_warnings(key, cert, not_after),
    )


def _ca_warnings(key: Path, cert: Path, not_after: dt.datetime) -> list[str]:
    """Checks the two things that make a CA unusable and that `openssl req` will not tell you."""
    warnings: list[str] = []
    mode = key.stat().st_mode & 0o777
    if mode & 0o077:
        warnings.append(
            f"the CA private key is mode {mode:o}; anything that can read it can issue certificates"
        )
    remaining = (not_after - dt.datetime.now(dt.UTC)).days
    if remaining < 90:
        warnings.append(f"the CA expires in {remaining} days; plan the rotation now, not later")
    return warnings


def issue_leaf(
    ca: IssuedCertificate, out_dir: Path, request: CertificateRequest,
    key_bits: int = 2048,
) -> IssuedCertificate:
    """Issues a server or client certificate signed by `ca`."""
    request = CertificateRequest(
        common_name=_validate_common_name(request.common_name),
        days=_validate_days(request.days, MAX_LEAF_DAYS, "leaf"),
        san_dns=request.san_dns, san_ip=request.san_ip,
        organization=request.organization,
    )
    # The directory is created first because `_san_conf` writes its extension file into it,
    # and creating an empty directory is not a side effect worth contorting the order for.
    # Validated before anything is *generated*: the first version generated the key and the
    # CSR and only then discovered a missing SAN, so every refusal left a private key and a
    # CSR on disk. Three refusals in a test run produced three orphaned key pairs that nothing
    # would ever clean up, and an orphaned key is worse than no key at all.
    out_dir.mkdir(parents=True, exist_ok=True)
    san = _san_conf(request, out_dir)
    key = out_dir / f"{request.common_name}.key.pem"
    csr = out_dir / f"{request.common_name}.csr.pem"
    cert = out_dir / f"{request.common_name}.crt.pem"
    # The chain is the leaf plus the CA, in that order. Caddy wants both; a deployment that
    # serves only the leaf fails in the browser, not in the server, which is the worst place.
    chain = out_dir / f"{request.common_name}.chain.pem"

    _run([
        "openssl", "req", "-new", "-nodes", "-newkey", f"rsa:{key_bits}",
        "-keyout", str(key), "-out", str(csr), "-subj", _subject(request),
    ])
    os.chmod(key, 0o600)

    args = [
        "openssl", "x509", "-req", "-in", str(csr),
        "-CA", str(ca.cert_path), "-CAkey", str(ca.key_path),
        "-days", str(request.days), "-sha256", "-out", str(cert),
    ]
    args += ["-extfile", str(san)]
    args += ["-CAcreateserial"]
    _run(args)
    # `-CAcreateserial` writes `<ca>.srl` next to the CA certificate, outside out_dir and
    # therefore outside anything the caller was told about. It is deleted rather than left:
    # a serial file next to the CA is state a rotation procedure has to know about, and the
    # next issuance recreates it.
    serial_file = ca.cert_path.with_suffix(".srl")
    serial_file.unlink(missing_ok=True)

    cert.read_bytes()
    chain.write_bytes(cert.read_bytes() + ca.cert_path.read_bytes())
    csr.unlink(missing_ok=True)

    not_before, not_after = _validity_of(cert)
    warnings = []
    if not_after > ca.not_after:
        # Should be impossible given the ceilings, but a CA older than the current constant
        # would do it, and an expiring leaf is exactly the failure this is here to prevent.
        warnings.append(
            f"the leaf outlives the CA ({not_after.date()} vs {ca.not_after.date()}); "
            "rotate the CA"
        )
    return IssuedCertificate(
        cert_path=cert, key_path=key, chain_path=chain,
        not_before=not_before, not_after=not_after, serial=_serial_of(cert),
        warnings=warnings,
    )


def _validity_of(cert: Path) -> tuple[dt.datetime, dt.datetime]:
    text = _run(["openssl", "x509", "-in", str(cert), "-noout", "-dates"]).strip()
    start = end = None
    for line in text.splitlines():
        if line.startswith("notBefore="):
            start = _parse_openssl_date(line.split("=", 1)[1])
        elif line.startswith("notAfter="):
            end = _parse_openssl_date(line.split("=", 1)[1])
    if start is None or end is None:
        raise PkiError(f"could not read the validity of {cert}")
    return start, end


def _parse_openssl_date(text: str) -> dt.datetime:
    # `Jun  7 12:00:00 2026 GMT` - the double space is openssl's, not a typo here.
    return dt.datetime.strptime(" ".join(text.split()), "%b %d %H:%M:%S %Y %Z").replace(
        tzinfo=dt.UTC
    )


def _serial_of(cert: Path) -> str:
    return _run(["openssl", "x509", "-in", str(cert), "-noout", "-serial"]).strip().split("=")[-1]


def verify_chain(leaf: IssuedCertificate, ca: IssuedCertificate) -> tuple[bool, str]:
    """Actually verifies, rather than asserting the file exists. Returns (ok, output)."""
    # Through `_run`, for the same reason as every other call: this one was left calling
    # `openssl` bare, so it was the last place in the file that still assumed PATH.
    output = ""
    try:
        output = _run(["openssl", "verify", "-CAfile", str(ca.cert_path), str(leaf.cert_path)])
        return True, output
    except PkiError as exc:
        # A verification failure is not a tool failure; it is the answer.
        return False, str(exc)


def _cmd_create_ca(args: argparse.Namespace) -> int:
    ca = create_ca(
        args.out_dir,
        CertificateRequest(common_name=args.common_name, days=args.days,
                           organization=args.organization),
    )
    _print_issued(ca)
    return 0


def _cmd_issue(args: argparse.Namespace) -> int:
    ca_cert = args.ca_cert
    ca_key = args.ca_key or ca_cert.with_name("ca.key.pem")
    if not ca_cert.is_file():
        raise PkiError(f"CA certificate not found: {ca_cert}")
    if not Path(ca_key).is_file():
        raise PkiError(f"CA private key not found: {ca_key}")
    not_before, not_after = _validity_of(ca_cert)

    ca = IssuedCertificate(
        cert_path=ca_cert, key_path=Path(ca_key), chain_path=ca_cert,
        not_before=not_before, not_after=not_after, serial=_serial_of(ca_cert),
    )
    leaf = issue_leaf(
        ca, args.out_dir,
        CertificateRequest(
            common_name=args.common_name, days=args.days,
            san_dns=tuple(args.san_dns or ()), san_ip=tuple(args.san_ip or ()),
            organization=args.organization,
        ),
    )
    _print_issued(leaf)

    ok, output = verify_chain(leaf, ca)
    print(f"chain verification: {'OK' if ok else 'FAILED'} - {output}")
    return 0 if ok else 1


def _cmd_inventory(args: argparse.Namespace) -> int:
    """Lists what is in a directory and whether it is still valid.

    A rotation is a procedure, and a procedure nobody can run to find out what is about to
    expire is a procedure that gets run after an outage.
    """
    rows = []
    now = dt.datetime.now(dt.UTC)
    for cert in sorted(args.out_dir.glob("*.crt.pem")):
        try:
            start, end = _validity_of(cert)
        except PkiError as exc:
            rows.append((cert.name, "unreadable", str(exc)))
            continue
        days = (end - now).days
        state = "expired" if days < 0 else ("expiring" if days < 30 else "ok")
        rows.append((cert.name, state, f"{start.date()} .. {end.date()} ({days} days)"))
    if not rows:
        print(f"no certificates in {args.out_dir}")
        return 1
    width = max(len(r[0]) for r in rows)
    for name, state, detail in rows:
        print(f"{name:<{width}}  {state:<9}  {detail}")
    return 0


def _print_issued(cert: IssuedCertificate) -> None:
    print(f"certificate : {cert.cert_path}")
    print(f"chain       : {cert.chain_path}")
    print(f"private key : {cert.key_path}")
    print(f"serial      : {cert.serial}")
    print(f"validity    : {cert.not_before.isoformat()} .. {cert.not_after.isoformat()}")
    print(f"sha256      : {cert.fingerprint_sha256}")
    for warning in cert.warnings:
        print(f"WARNING: {warning}", file=sys.stderr)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="pki.py",
        description="X.509 material for the central's own TLS. Not for node identity - that is "
                    "the Ed25519 CA in cauce_server/certificates.py.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = parser.add_subparsers(dest="command", required=True)

    ca = sub.add_parser("create-ca", help="create a signing CA")
    ca.add_argument("--common-name", default="CAUCE Central CA")
    ca.add_argument("--days", type=int, default=3650)
    ca.add_argument("--organization", default="CAUCE")
    ca.add_argument("--out-dir", type=Path, default=Path("deployment/pki"))
    ca.set_defaults(func=_cmd_create_ca)

    leaf = sub.add_parser("issue", help="issue a leaf certificate")
    leaf.add_argument("--common-name", required=True)
    leaf.add_argument("--days", type=int, default=365)
    leaf.add_argument("--san-dns", action="append", help="repeatable")
    leaf.add_argument("--san-ip", action="append", help="repeatable")
    leaf.add_argument("--organization", default="CAUCE")
    leaf.add_argument("--ca-cert", type=Path, required=True)
    leaf.add_argument("--ca-key", type=Path)
    leaf.add_argument("--out-dir", type=Path, default=Path("deployment/pki"))
    leaf.set_defaults(func=_cmd_issue)

    inv = sub.add_parser("inventory", help="list certificates and their remaining validity")
    inv.add_argument("--out-dir", type=Path, default=Path("deployment/pki"))
    inv.set_defaults(func=_cmd_inventory)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except PkiError as exc:
        print(f"refused: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

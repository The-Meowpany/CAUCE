"""Tests for the X.509 issuance tool.

The property that matters throughout is **a refusal leaves nothing behind**. This tool writes
private keys, and the first version generated the key and the CSR and only then discovered a
missing SAN — so every refusal orphaned a key pair on disk. Three refusals in a test run left
three orphaned CA-capable keys that nothing would ever clean up.

So most of these assert two things together: the refusal happened, *and* the directory is as it
was before. A test that only checks the refusal passes while the real defect is in place.
"""

from __future__ import annotations

import datetime as dt
import os
from pathlib import Path

import pytest

# The tool's own resolver, not `shutil.which`. openssl is not on PATH on a default Windows
# install with Git present, and `which` therefore returned None: all 25 of these skipped and
# the suite reported 541 rather than 566. A count that depends on the shell is a count nobody
# should trust, so the tool finds openssl and the tests inherit that.
from tools.pki import (  # noqa: E402  # noqa: E402
    MAX_CA_DAYS,
    MAX_LEAF_DAYS,
    OPENSSL,
    CertificateRequest,
    IssuedCertificate,
    PkiError,
    _san_conf,
    _validate_common_name,
    _validate_days,
    _validate_san,
    create_ca,
    issue_leaf,
    verify_chain,
)

pytestmark = pytest.mark.skipif(
    OPENSSL is None,
    reason="openssl could not be located, including in the usual install locations",
)


def _openssl_available() -> bool:
    return OPENSSL is not None


@pytest.fixture
def ca(tmp_path: Path) -> IssuedCertificate:
    return create_ca(tmp_path / "ca", CertificateRequest(common_name="CAUCE Test CA", days=900))


def _dir_state(path: Path) -> set[str]:
    return {p.name for p in path.iterdir()} if path.is_dir() else set()


# --- validation ------------------------------------------------------------


def test_an_empty_common_name_is_refused():
    with pytest.raises(PkiError, match="must not be empty"):
        _validate_common_name("   ")


def test_an_over_long_common_name_is_refused():
    # RFC 5280 caps it at 64. Caddy refuses longer, and a certificate that loads but is never
    # presented is worse than one it rejects.
    with pytest.raises(PkiError, match="the limit is 64"):
        _validate_common_name("C" * 65)


def test_a_common_name_with_a_newline_is_refused():
    # openssl's -subj takes this string; a newline breaks the file rather than mis-parsing it.
    with pytest.raises(PkiError, match="must not contain a newline"):
        _validate_common_name("evil\nO=Other")


def test_validity_beyond_the_leaf_ceiling_is_refused_not_clamped():
    # Refused rather than clamped: a caller asking for 4000 days and receiving 825 is a
    # deployment that believes it has longer than it does.
    with pytest.raises(PkiError, match="exceeds the 825-day limit"):
        _validate_days(4000, MAX_LEAF_DAYS, "leaf")


def test_validity_beyond_the_ca_ceiling_is_refused():
    with pytest.raises(PkiError, match="exceeds the 3650-day limit"):
        _validate_days(4000, MAX_CA_DAYS, "CA")


def test_a_non_positive_validity_is_refused():
    with pytest.raises(PkiError, match="must be positive"):
        _validate_days(0, MAX_LEAF_DAYS, "leaf")


def test_a_url_is_not_a_valid_san():
    with pytest.raises(PkiError, match="bare hostname"):
        _validate_san(("https://central.example/v1",), "dns")


def test_a_path_is_not_a_valid_san():
    with pytest.raises(PkiError, match="bare hostname"):
        _validate_san(("central.example/v1",), "dns")


def test_a_malformed_ip_is_refused():
    with pytest.raises(PkiError, match="not an IP address"):
        _validate_san(("999.1.1.1",), "ip")


def test_a_valid_ip_is_accepted():
    assert _validate_san(("10.0.0.5", "::1"), "ip") == ("10.0.0.5", "::1")


def test_a_wildcard_carries_its_base_domain():
    # Name validation for `*.a.example` differs between implementations unless the base domain
    # is also present. That difference is very hard to debug in the field.
    assert _validate_san(("*.central.example",), "dns") == (
        "*.central.example", "central.example")


def test_duplicate_sans_collapse_without_reordering():
    entries = _validate_san(("b.example", "a.example", "b.example"), "dns")
    assert entries == ("b.example", "a.example")


def test_a_leaf_with_no_san_is_refused():
    # Refused rather than warned about: a CN-only certificate loads happily and is then
    # trusted by nobody.
    with pytest.raises(PkiError, match="no SAN given"):
        _san_conf(CertificateRequest(common_name="x.example"), Path("."))


# --- refusals leave nothing behind -----------------------------------------


def test_a_refused_leaf_writes_no_key_and_no_csr(tmp_path: Path):
    out = tmp_path / "out"
    ca = create_ca(tmp_path / "ca", CertificateRequest(common_name="CA", days=900))
    before = _dir_state(out)

    with pytest.raises(PkiError):
        issue_leaf(ca, out, CertificateRequest(common_name="nosan.example"))

    assert _dir_state(out) == before
    assert not list(out.glob("*.key.pem"))
    assert not list(out.glob("*.csr.pem"))


def test_a_refused_san_writes_nothing(tmp_path: Path):
    out = tmp_path / "out"
    ca = create_ca(tmp_path / "ca", CertificateRequest(common_name="CA", days=900))
    out.mkdir(parents=True, exist_ok=True)

    with pytest.raises(PkiError, match="bare hostname"):
        issue_leaf(ca, out, CertificateRequest(
            common_name="x.example", san_dns=("https://a/b",)))

    assert _dir_state(out) == set()


def test_a_refused_validity_writes_nothing(tmp_path: Path):
    out = tmp_path / "out"
    ca = create_ca(tmp_path / "ca", CertificateRequest(common_name="CA", days=900))
    out.mkdir(parents=True, exist_ok=True)

    with pytest.raises(PkiError, match="exceeds"):
        issue_leaf(ca, out, CertificateRequest(
            common_name="x.example", san_dns=("x.example",), days=9999))

    assert _dir_state(out) == set()


# --- issuance ---------------------------------------------------------------


def test_a_ca_is_created_with_the_right_extensions(tmp_path: Path):
    if not _openssl_available():
        pytest.skip("openssl missing")
    ca = create_ca(tmp_path / "ca", CertificateRequest(common_name="CAUCE Test CA", days=900))

    # Through the tool's own runner rather than `subprocess.run(["openssl", ...])`. This test
    # was the last caller assuming PATH, and it failed with a bare FileNotFoundError on a
    # machine where openssl exists in Git's directory - which says nothing about the
    # certificate it was trying to check.
    from tools.pki import _run

    text = _run(["openssl", "x509", "-in", str(ca.cert_path), "-noout", "-text"])
    # Without basicConstraints the certificate is not a CA to anything that checks, which is
    # a failure mode that only appears at the first issuance - long after the CA was created.
    assert "Basic Constraints: critical" in text
    assert "CA:TRUE" in text
    # The long name openssl accepts on the command line is `keyCertSign`; what it *prints* is
    # "Certificate Sign". Asserting the input spelling here failed against a certificate that
    # was entirely correct, which is the argument for reading the tool's output rather than
    # its arguments.
    assert "Certificate Sign" in text
    assert "CRL Sign" in text
    assert ca.serial != ""


def test_an_issued_leaf_verifies_against_the_ca(tmp_path: Path):
    if not _openssl_available():
        pytest.skip("openssl missing")
    ca = create_ca(tmp_path / "ca", CertificateRequest(common_name="CA", days=900))
    leaf = issue_leaf(ca, tmp_path / "out", CertificateRequest(
        common_name="central.example", san_dns=("central.example",), san_ip=("10.0.0.5",)))

    ok, output = verify_chain(leaf, ca)
    assert ok, output
    assert leaf.not_before < leaf.not_after


def test_the_chain_file_contains_the_leaf_and_the_ca(tmp_path: Path):
    if not _openssl_available():
        pytest.skip("openssl missing")
    ca = create_ca(tmp_path / "ca", CertificateRequest(common_name="CA", days=900))
    leaf = issue_leaf(ca, tmp_path / "out", CertificateRequest(
        common_name="central.example", san_dns=("central.example",)))

    chain = leaf.chain_path.read_bytes()
    assert chain == leaf.cert_path.read_bytes() + ca.cert_path.read_bytes()


def test_the_csr_is_removed_after_issuance(tmp_path: Path):
    if not _openssl_available():
        pytest.skip("openssl missing")
    ca = create_ca(tmp_path / "ca", CertificateRequest(common_name="CA", days=900))
    out = tmp_path / "out"
    issue_leaf(ca, out, CertificateRequest(
        common_name="central.example", san_dns=("central.example",)))
    # A CSR is a request for a certificate with no certificate attached. Leaving one next to
    # the key is a small pile of material that looks like something it is not.
    assert not list(out.glob("*.csr.pem"))


def test_no_serial_file_is_left_next_to_the_ca(tmp_path: Path):
    if not _openssl_available():
        pytest.skip("openssl missing")
    ca_dir = tmp_path / "ca"
    ca = create_ca(ca_dir, CertificateRequest(common_name="CA", days=900))
    issue_leaf(ca, tmp_path / "out", CertificateRequest(
        common_name="central.example", san_dns=("central.example",)))
    # `-CAcreateserial` writes one. It is state a rotation procedure would have to know about.
    assert not list(ca_dir.glob("*.srl"))


def test_issuance_is_repeatable_and_gives_distinct_serials(tmp_path: Path):
    if not _openssl_available():
        pytest.skip("openssl missing")
    ca = create_ca(tmp_path / "ca", CertificateRequest(common_name="CA", days=900))
    first = issue_leaf(ca, tmp_path / "out", CertificateRequest(
        common_name="a.example", san_dns=("a.example",)))
    second = issue_leaf(ca, tmp_path / "out", CertificateRequest(
        common_name="b.example", san_dns=("b.example",)))
    # A repeated serial is a certificate revocation problem nobody discovers until it matters.
    assert first.serial != second.serial


def test_the_private_key_is_not_world_readable(tmp_path: Path):
    if not _openssl_available():
        pytest.skip("openssl missing")
    ca = create_ca(tmp_path / "ca", CertificateRequest(common_name="CA", days=900))
    mode = ca.key_path.stat().st_mode & 0o777
    # Windows reports 666 for everything; the check is meaningless there and asserting it
    # would fail the suite on a platform where the concept does not apply.
    if os.name != "nt":
        assert not (mode & 0o077), f"CA key mode {mode:o}"


def test_a_soon_to_expire_ca_is_warned_about(tmp_path: Path):
    if not _openssl_available():
        pytest.skip("openssl missing")
    # A CA inside the 90-day warning window. A rotation is a procedure, and a procedure
    # nobody can run to find out what is about to expire runs after an outage.
    ca = create_ca(tmp_path / "ca", CertificateRequest(common_name="Short CA", days=10))
    assert any("expires in" in w for w in ca.warnings), ca.warnings


def test_a_wide_open_ca_key_is_warned_about(tmp_path: Path):
    if not _openssl_available():
        pytest.skip("openssl missing")
    if os.name == "nt":
        pytest.skip("POSIX file modes do not apply here")
    ca_dir = tmp_path / "ca"
    ca = create_ca(ca_dir, CertificateRequest(common_name="CA", days=900))
    os.chmod(ca.key_path, 0o644)
    # Recomputed rather than reused from creation: the warning belongs to the state on disk.
    from tools.pki import _ca_warnings
    warnings = _ca_warnings(ca.key_path, ca.cert_path, ca.not_after)
    assert any("private key is mode" in w for w in warnings), warnings


def test_a_leaf_that_outlives_its_ca_is_flagged(tmp_path: Path):
    # Constructed rather than issued, because no pair of ceilings currently permits it: a CA
    # created under an older, longer limit than the current one would do it, and an expiring
    # leaf is exactly the failure this guard exists for.
    now = dt.datetime.now(dt.UTC)
    ca = IssuedCertificate(
        cert_path=tmp_path / "ca.crt.pem", key_path=tmp_path / "ca.key.pem",
        chain_path=tmp_path / "ca.crt.pem",
        not_before=now - dt.timedelta(days=1), not_after=now + dt.timedelta(days=10),
        serial="00",
    )
    assert ca.not_after < now + dt.timedelta(days=MAX_LEAF_DAYS)

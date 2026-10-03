"""Signature algorithms, checked against published vectors rather than itself.

The HMAC cases pin RFC 4231. The Ed25519 cases pin RFC 8032 section 7.1, which
is the point of having them: a signature scheme whose only test is that it
accepts what it just produced proves nothing, because a verifier that always
returns true passes it too.
"""

from __future__ import annotations

import pytest
from cauce_server.signing import (
    ALGORITHMS,
    DEFAULT_ALGORITHM,
    ED25519,
    HMAC_SHA256,
    SignatureError,
    decode_key_material,
    encode_key_material,
    get_algorithm,
    sign_frame,
    signature_bytes,
    verify_frame,
)

# RFC 4231 test case 1.
RFC4231_KEY = bytes.fromhex("0b" * 20)
RFC4231_MESSAGE = b"Hi There"
RFC4231_HMAC = bytes.fromhex(
    "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7")

# RFC 8032 section 7.1, TEST 1: empty message.
RFC8032_1_SECRET = (
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
RFC8032_1_PUBLIC = (
    "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
RFC8032_1_SIGNATURE = (
    "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8"
    "821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b")

# RFC 8032 section 7.1, TEST 2: one byte message.
RFC8032_2_SECRET = (
    "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb")
RFC8032_2_PUBLIC = (
    "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c")
RFC8032_2_SIGNATURE = (
    "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085a"
    "c1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00")


class TestHmacVectors:
    """RFC 4231 pins the primitive; these pin how this module feeds it.

    A device key is a passphrase, so the key is interpreted as UTF-8 text and
    RFC 4231's all-binary key cannot be expressed here. The vector is therefore
    checked against the primitive directly, and the wrapper is checked against
    an independent computation over the encoded key. Both halves matter: the
    first proves the vector, the second proves the encoding contract that a
    later refactor could otherwise change silently.
    """

    def test_the_rfc4231_vector_is_what_the_primitive_produces(self):
        import hashlib
        import hmac

        produced = hmac.new(RFC4231_KEY, RFC4231_MESSAGE,
                            hashlib.sha256).digest()
        assert produced == RFC4231_HMAC

    def test_signing_uses_the_utf8_encoding_of_the_key(self):
        import hashlib
        import hmac

        algorithm = get_algorithm(HMAC_SHA256)
        key = "clave-del-dispositivo"
        expected = hmac.new(key.encode("utf-8"), RFC4231_MESSAGE,
                            hashlib.sha256).digest()
        assert algorithm.sign(RFC4231_MESSAGE, key) == expected

    def test_a_non_ascii_passphrase_is_encoded_as_utf8(self):
        import hashlib
        import hmac

        algorithm = get_algorithm(HMAC_SHA256)
        key = "clave-café-ñ"
        expected = hmac.new(key.encode("utf-8"), b"body", hashlib.sha256).digest()
        assert algorithm.sign(b"body", key) == expected
        assert algorithm.verify(b"body", expected, key)
        # The latin-1 reading of the same characters must not verify, which is
        # what pins the encoding rather than leaving it to chance.
        assert not algorithm.verify(b"body", expected, key.encode("latin-1")
                                    .decode("latin-1").encode("utf-8")
                                    .decode("latin-1"))

    def test_verify_matches_an_independent_computation(self):
        import hashlib
        import hmac

        algorithm = get_algorithm(HMAC_SHA256)
        signature = hmac.new(b"k", b"body", hashlib.sha256).digest()
        assert algorithm.verify(b"body", signature, "k")
        assert not algorithm.verify(b"body2", signature, "k")
        assert not algorithm.verify(b"body", signature, "k2")

    def test_a_flipped_bit_fails(self):
        algorithm = get_algorithm(HMAC_SHA256)
        signature = bytearray(algorithm.sign(RFC4231_MESSAGE, "clave"))
        signature[0] ^= 0x01
        assert not algorithm.verify(RFC4231_MESSAGE, bytes(signature), "clave")

    def test_a_signature_of_the_wrong_length_is_refused(self):
        algorithm = get_algorithm(HMAC_SHA256)
        signature = algorithm.sign(b"body", "clave")
        assert not algorithm.verify(b"body", signature[:-1], "clave")
        assert not algorithm.verify(b"body", signature + b"\x00", "clave")


class TestEd25519Vectors:
    @pytest.mark.parametrize(
        "secret,public,message,signature",
        [
            (RFC8032_1_SECRET, RFC8032_1_PUBLIC, b"", RFC8032_1_SIGNATURE),
            (RFC8032_2_SECRET, RFC8032_2_PUBLIC, b"r", RFC8032_2_SIGNATURE),
        ],
    )
    def test_signs_rfc8032_vectors(self, secret, public, message, signature):
        produced = get_algorithm(ED25519).sign(message, secret)
        assert produced.hex() == signature
        # The vector's own public key is what the derivation must agree with,
        # checked against the library rather than against ourselves.
        assert get_algorithm(ED25519).verify(message, produced, public)

    def test_the_wrong_public_key_is_refused(self):
        signature = get_algorithm(ED25519).sign(b"r", RFC8032_2_SECRET)
        assert not get_algorithm(ED25519).verify(b"r", signature,
                                                 RFC8032_1_PUBLIC)

    def test_a_modified_message_is_refused(self):
        signature = get_algorithm(ED25519).sign(b"r", RFC8032_2_SECRET)
        assert not get_algorithm(ED25519).verify(b"s", signature,
                                                 RFC8032_2_PUBLIC)

    def test_a_malformed_signature_is_refused_without_raising(self):
        algorithm = get_algorithm(ED25519)
        assert not algorithm.verify(b"r", b"\x00" * 63, RFC8032_2_PUBLIC)
        assert not algorithm.verify(b"r", b"", RFC8032_2_PUBLIC)
        # A scalar at or beyond the group order must be refused, not reduced.
        assert not algorithm.verify(b"r", b"\xff" * 64, RFC8032_2_PUBLIC)

    def test_a_malformed_key_is_refused_without_raising(self):
        algorithm = get_algorithm(ED25519)
        signature = algorithm.sign(b"r", RFC8032_2_SECRET)
        for bad_key in ("", "not-a-key", "00", RFC8032_2_SECRET):
            assert not algorithm.verify(b"r", signature, bad_key)


class TestAlgorithmSelection:
    def test_a_node_without_an_algorithm_gets_hmac(self):
        assert get_algorithm(None).name == DEFAULT_ALGORITHM
        assert get_algorithm("").name == HMAC_SHA256

    def test_an_unknown_algorithm_is_refused(self):
        with pytest.raises(SignatureError):
            get_algorithm("md5")
        with pytest.raises(SignatureError):
            signature_bytes("rot13")

    def test_every_advertised_algorithm_is_resolvable(self):
        for name in ALGORITHMS:
            assert get_algorithm(name).signature_bytes == signature_bytes(name)

    def test_the_two_algorithms_have_different_trailer_lengths(self):
        assert signature_bytes(HMAC_SHA256) == 32
        assert signature_bytes(ED25519) == 64


class TestSignedFrames:
    def test_hmac_frames_round_trip(self):
        frame = b"\xca\x01\x00\x00body-and-crc"
        signed = sign_frame(frame, "clave-secreta")
        assert len(signed) == len(frame) + 32
        assert signed.startswith(frame)
        assert verify_frame(signed, "clave-secreta")

    def test_ed25519_frames_round_trip(self):
        frame = b"\xca\x01\x00\x00body-and-crc"
        signed = sign_frame(frame, RFC8032_2_SECRET, ED25519)
        assert len(signed) == len(frame) + 64
        assert signed.startswith(frame)
        assert verify_frame(signed, RFC8032_2_PUBLIC, ED25519)

    def test_a_private_key_cannot_be_used_to_verify(self):
        """Signing with the seed must not verify against it.

        Otherwise a node holding only its seed could impersonate itself to the
        central, and the central would be storing a secret it can replay.
        """
        frame = b"payload"
        signed = sign_frame(frame, RFC8032_2_SECRET, ED25519)
        assert not verify_frame(signed, RFC8032_2_SECRET, ED25519)

    def test_a_hmac_signature_is_refused_when_ed25519_is_expected(self):
        """The algorithm comes from provisioning, not from the wire.

        An attacker cannot downgrade a frame to the cheaper check by relabelling
        it, because the node's algorithm is already fixed server-side.
        """
        frame = b"payload"
        signed = sign_frame(frame, "clave-secreta", HMAC_SHA256)
        assert not verify_frame(signed, "clave-secreta", ED25519)

    def test_an_ed25519_signature_is_refused_when_hmac_is_expected(self):
        frame = b"payload"
        signed = sign_frame(frame, RFC8032_2_SECRET, ED25519)
        assert not verify_frame(signed, RFC8032_2_PUBLIC, HMAC_SHA256)

    def test_tampering_anywhere_breaks_the_frame(self):
        frame = b"\xca\x01\x00\x00body-and-crc"
        for algorithm, key in ((HMAC_SHA256, "clave-secreta"),
                               (ED25519, RFC8032_2_SECRET)):
            signed = sign_frame(frame, key, algorithm)
            for index in range(len(frame)):
                altered = bytearray(signed)
                altered[index] ^= 0x01
                assert not verify_frame(bytes(altered), key, algorithm), (
                    f"{algorithm}: byte {index} was not covered")

    def test_a_frame_too_short_to_carry_a_signature_is_refused(self):
        assert not verify_frame(b"tiny", "clave-secreta")
        assert not verify_frame(b"", "clave-secreta", ED25519)

    def test_verifying_without_a_key_is_refused(self):
        signed = sign_frame(b"payload", "clave-secreta")
        assert not verify_frame(signed, "")
        assert not verify_frame(b"payload", "")

    def test_signing_without_a_key_is_an_error(self):
        with pytest.raises(SignatureError):
            sign_frame(b"payload", "", HMAC_SHA256)


class TestKeyMaterial:
    def test_hex_and_base64_of_the_same_bytes_agree(self):
        import base64

        raw = bytes(range(32))
        as_hex = encode_key_material(raw)
        as_b64 = base64.b64encode(raw).decode()
        assert decode_key_material(as_hex, 32) == raw
        assert decode_key_material(as_b64, 32) == raw

    def test_a_wrong_length_is_refused(self):
        with pytest.raises(SignatureError):
            decode_key_material("0011", 32)

    def test_empty_key_material_is_refused(self):
        with pytest.raises(SignatureError):
            decode_key_material("   ", 32)

    def test_a_non_string_key_is_refused(self):
        with pytest.raises(SignatureError):
            decode_key_material(None, 32)  # type: ignore[arg-type]

    def test_ambiguous_material_that_is_both_hex_and_base64_is_read_as_hex(self):
        """A 64-character string is hex first, so encoding never changes a key."""
        text = "ab" * 32
        assert decode_key_material(text, 32) == bytes.fromhex(text)

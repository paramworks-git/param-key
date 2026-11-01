"""Check components/param_key/pk_crypto.h and the IR codec against a Python reference.

The reference uses the `cryptography` package, as the Home Assistant
integration does, so a pass here means the firmware and the integration agree
on pairing, keys, codes and frames. docs/test-vectors.json pins one fixed run
of each for the integration's own tests; regenerate it with --write (only when
the protocol changes on purpose).

Run with ./tests/run.sh (builds crypto_cli in a container with mbedtls 3).
"""

import hashlib
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import AESCCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

CLI = os.environ.get("CRYPTO_CLI", "/tmp/crypto_cli")
VECTORS = Path(__file__).resolve().parents[2] / "docs" / "test-vectors.json"

MAGIC, VERSION = 0x50, 2
PAIR_SALT = b"param-key pair v2"
failures: list[str] = []


def check(name: str, cond: bool, detail: object = "") -> None:
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else f"  -> {detail}"))
    if not cond:
        failures.append(name)


def cli(*args: str) -> str:
    return subprocess.run([CLI, *args], capture_output=True, text=True).stdout.strip()


# --- Reference implementation (what the integration does) -------------------

def pub_of(priv: bytes) -> bytes:
    return X25519PrivateKey.from_private_bytes(priv).public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def shared_of(priv: bytes, pub: bytes) -> bytes:
    return X25519PrivateKey.from_private_bytes(priv).exchange(X25519PublicKey.from_public_bytes(pub))


def derive(shared: bytes, remote_pub: bytes, ha_pub: bytes, mac: bytes) -> tuple[bytes, int]:
    okm = HKDF(algorithm=hashes.SHA256(), length=20, salt=PAIR_SALT,
               info=remote_pub + ha_pub + mac).derive(shared)
    return okm[:16], struct.unpack(">I", okm[16:20])[0] % 1_000_000


def seq_bytes(epoch: int, counter: int) -> bytes:
    return epoch.to_bytes(3, "little") + struct.pack("<I", counter)


def nonce(mac: bytes, epoch: int, counter: int) -> bytes:
    return mac + seq_bytes(epoch, counter)


def seal(key: bytes, mac: bytes, epoch: int, counter: int, plain: bytes) -> bytes:
    aad = plain[:3] + seq_bytes(epoch, counter)
    return aad + AESCCM(key, tag_length=8).encrypt(nonce(mac, epoch, counter), plain[3:], aad)


def ir_packet(carrier_hz: int, timings: list[int]) -> bytes:
    out = bytes([MAGIC, VERSION, ord("I")]) + struct.pack("<H", (carrier_hz + 50) // 100)
    for t in timings:
        units = min(max((abs(t) + 2) // 4, 1), 32767)
        out += struct.pack("<h", -units if t < 0 else units)
    return out


def fixed(label: str, n: int = 32) -> bytes:
    return hashlib.sha256(label.encode()).digest()[:n]


def vectors() -> dict:
    r_priv, h_priv = fixed("remote"), fixed("ha")
    r_pub, h_pub = pub_of(r_priv), pub_of(h_priv)
    mac, bridge = bytes.fromhex("020000000001"), bytes.fromhex("020000000002")
    shared = shared_of(r_priv, h_pub)
    key, code = derive(shared, r_pub, h_pub, mac)
    key_plain = bytes([MAGIC, VERSION, ord("K"), 7, 0]) + b"vol_up"
    screen_plain = bytes([MAGIC, VERSION, ord("S")]) + b"\x1f".join([b"Media", b"Shield", b"Netflix", b"40", b""])
    nec = [9000, -4500] + [560, -560] * 32 + [560]
    return {
        "remote_mac": mac.hex(), "bridge_mac": bridge.hex(),
        "remote_private": r_priv.hex(), "remote_public": r_pub.hex(),
        "ha_private": h_priv.hex(), "ha_public": h_pub.hex(),
        "shared": shared.hex(), "key": key.hex(), "code": f"{code:06d}",
        "frames": [
            {"sender": "remote", "epoch": 0, "counter": 4097, "plain": key_plain.hex(),
             "frame": seal(key, mac, 0, 4097, key_plain).hex()},
            {"sender": "bridge", "epoch": 449280, "counter": 12, "plain": screen_plain.hex(),
             "frame": seal(key, bridge, 449280, 12, screen_plain).hex()},
        ],
        "ir": {"carrier": 38000, "timings": nec, "packet": ir_packet(38000, nec).hex()},
    }


def main() -> None:
    if "--write" in sys.argv:
        VECTORS.write_text(json.dumps(vectors(), indent=2) + "\n")
        print(f"wrote {VECTORS}")
        return

    check("counter and replay self-test", cli("selftest") == "OK")

    v = json.loads(VECTORS.read_text())
    check("vectors unchanged", v == vectors(), "regenerate with --write only for a protocol change")

    # Fixed vectors through the firmware code.
    check("vector: public key", cli("pub", v["remote_private"]) == v["remote_public"])
    check("vector: shared", cli("shared", v["remote_private"], v["ha_public"]) == v["shared"])
    check("vector: key and code", cli("derive", v["shared"], v["remote_public"], v["ha_public"], v["remote_mac"])
          == f'{v["key"]} {v["code"]}')
    for f in v["frames"]:
        mac = v["remote_mac"] if f["sender"] == "remote" else v["bridge_mac"]
        check(f"vector: seal {f['sender']}",
              cli("seal", v["key"], mac, str(f["epoch"]), str(f["counter"]), f["plain"]) == f["frame"])
        check(f"vector: open {f['sender']}",
              cli("open", v["key"], mac, f["frame"]) == f'{f["epoch"]} {f["counter"]} {f["plain"]}')
    check("vector: IR packet", cli("ir", "38000", *map(str, v["ir"]["timings"])) == v["ir"]["packet"])

    # Random pairings: firmware keypair, Python peer, both sides derive the same.
    for i in range(20):
        r_priv, r_pub = (bytes.fromhex(x) for x in cli("keypair").split())
        h_priv = os.urandom(32)
        h_pub = pub_of(h_priv)
        mac = os.urandom(6)
        ok_pub = pub_of(r_priv) == r_pub
        fw_shared = bytes.fromhex(cli("shared", r_priv.hex(), h_pub.hex()))
        ok_shared = fw_shared == shared_of(h_priv, r_pub)
        key, code = derive(fw_shared, r_pub, h_pub, mac)
        ok_derive = cli("derive", fw_shared.hex(), r_pub.hex(), h_pub.hex(), mac.hex()) == f"{key.hex()} {code:06d}"
        check(f"random pairing {i}", ok_pub and ok_shared and ok_derive)

    # Random frames both ways, including an empty body (the hello ack 'Y').
    for i, body_len in enumerate([0, 1, 5, 64, 247, 1452]):
        key, mac = os.urandom(16), os.urandom(6)
        counter = int.from_bytes(os.urandom(4), "little") % (2**32 - 1)
        epoch = int.from_bytes(os.urandom(3), "little")
        plain = bytes([MAGIC, VERSION, ord("S")]) + os.urandom(body_len)
        frame = cli("seal", key.hex(), mac.hex(), str(epoch), str(counter), plain.hex())
        check(f"seal matches reference ({body_len} B)", frame == seal(key, mac, epoch, counter, plain).hex(),
              frame[:40])
        check(f"reference frame opens ({body_len} B)",
              cli("open", key.hex(), mac.hex(), seal(key, mac, epoch, counter, plain).hex())
              == f"{epoch} {counter} {plain.hex()}")

    # Tampering and misuse are rejected.
    key, mac = os.urandom(16), os.urandom(6)
    plain = bytes([MAGIC, VERSION, ord("K"), 1, 0]) + b"power"
    frame = seal(key, mac, 5, 42, plain)
    for name, bad in [
        ("flipped body bit", frame[:11] + bytes([frame[11] ^ 1]) + frame[12:]),
        ("flipped tag bit", frame[:-1] + bytes([frame[-1] ^ 1])),
        ("changed counter", frame[:6] + struct.pack("<I", 43) + frame[10:]),
        ("changed epoch", frame[:3] + (6).to_bytes(3, "little") + frame[6:]),
        ("changed type", frame[:2] + b"H" + frame[3:]),
        ("truncated", frame[:-1]),
        ("v1 header", frame[:1] + b"\x01" + frame[2:]),
    ]:
        check(f"rejects {name}", cli("open", key.hex(), mac.hex(), bad.hex()) == "FAIL")
    check("rejects other sender MAC", cli("open", key.hex(), os.urandom(6).hex(), frame.hex()) == "FAIL")
    check("rejects other key", cli("open", os.urandom(16).hex(), mac.hex(), frame.hex()) == "FAIL")
    check("rejects low-order peer key", cli("shared", os.urandom(32).hex(), "00" * 32) == "FAIL")

    # IR codec: round trip at 4 us resolution, sign kept, clamped, size limit.
    timings = [9000, -4500, 560, -1690, 1, -2, 200000, -200000]
    packet = cli("ir", "38000", *map(str, timings))
    check("IR packet matches reference", packet == ir_packet(38000, timings).hex(), packet)
    parsed = cli("parseir", packet).split()
    check("IR round trip", parsed == ["38000", "9000", "-4500", "560", "-1692", "4", "-4", "131068", "-131068"], parsed)
    check("IR at size limit", cli("ir", "38000", *(["500", "-500"] * 362 + ["500"])) != "FAIL")
    check("IR over size limit", cli("ir", "38000", *(["500", "-500"] * 363)) == "FAIL")

    print(f"\n{len(failures)} failure(s)" if failures else "\nall passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()

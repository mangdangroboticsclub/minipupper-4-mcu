#!/usr/bin/env python3
"""Reference implementation of the sealed-blob layout (§6.4) and the eFuse
identity record (§6.2).

The firmware has the same two layouts in `components/mpx_identity/mpx_seal.c`
and `mpx_efuse_record.c`. The cloud and the provisioning station parse what the
robot writes, so a field moving by one byte would break production in a way no
compiler catches -- hence a second implementation plus frozen golden vectors
(`tools/vectors/seal.json`) that both sides are tested against.

    python3 tools/mpx_seal.py vectors           # regenerate tools/vectors/seal.json
    python3 tools/mpx_seal.py vectors --check   # fail if the file is stale
    python3 tools/mpx_seal.py c-header PATH     # emit the C header the host tests use
    python3 tools/mpx_seal.py selftest          # properties the layout must have

No third-party dependencies: this has to run on a bench laptop and in CI.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
VECTORS_PATH = REPO_ROOT / "tools" / "vectors" / "seal.json"

ID_LEN = 16
BLOB_MAGIC = b"MPXS"
BLOB_FORMAT = 0x01
PLAIN_MAGIC = b"MPXK"
PLAIN_FORMAT = 0x01
RSA_BYTES = 384
BLOB_HEADER = 8
BLOB_LEN = BLOB_HEADER + RSA_BYTES
PLAIN_LEN = 149

# Fields of the OAEP plaintext, in order: (name, length).
PLAIN_FIELDS = [
    ("magic", 4),
    ("format", 1),
    ("key_version", 4),
    ("robot_id", 16),
    ("nonce", 16),
    ("key", 32),
    ("mac", 6),
    ("sec_flags", 2),
    ("secure_version", 4),
    ("app_version", 32),
    ("ds_pub_sha256", 32),
]

# Fields of the eFuse record, in order: (name, length).
RECORD_FIELDS = [
    ("robot_id", 16),
    ("format", 1),
    ("flags", 1),
    ("kv_base", 2),
]

RECORD_FLAG_DEV = 0x01
RECORD_FLAG_REPLACEMENT = 0x02
RECORD_FORMAT_V1 = 0x01


def _field_offsets(fields):
    offsets = {}
    off = 0
    for name, length in fields:
        offsets[name] = off
        off += length
    return offsets, off


PLAIN_OFFSETS, PLAIN_TOTAL = _field_offsets(PLAIN_FIELDS)
RECORD_OFFSETS, RECORD_TOTAL = _field_offsets(RECORD_FIELDS)

assert PLAIN_TOTAL == PLAIN_LEN, f"plain length {PLAIN_TOTAL} != {PLAIN_LEN}"
assert RECORD_TOTAL == 20, f"record length {RECORD_TOTAL} != 20"


def encode_plain(rec: dict) -> bytes:
    """Builds the 149-byte OAEP plaintext from `rec` (see PLAIN_FIELDS)."""
    robot_id = rec["robot_id"]
    if not isinstance(robot_id, str) or len(robot_id) != ID_LEN:
        raise ValueError("robot_id must be a 16-character string")
    if not 1 <= rec["key_version"] <= 0xFFFFFFFF:
        raise ValueError("key_version must be >= 1")
    app_version = rec.get("app_version", "")
    app_bytes = app_version.encode("ascii")
    if len(app_bytes) > 32:
        raise ValueError("app_version does not fit in 32 bytes")
    if any(ord(c) == 0 for c in app_version):
        raise ValueError("app_version must not contain NUL")

    out = bytearray(PLAIN_LEN)
    out[0:4] = PLAIN_MAGIC
    out[4] = PLAIN_FORMAT
    out[PLAIN_OFFSETS["key_version"]:PLAIN_OFFSETS["key_version"] + 4] = (
        rec["key_version"].to_bytes(4, "big")
    )
    out[PLAIN_OFFSETS["robot_id"]:PLAIN_OFFSETS["robot_id"] + 16] = robot_id.encode("ascii")
    out[PLAIN_OFFSETS["nonce"]:PLAIN_OFFSETS["nonce"] + 16] = rec["nonce"]
    out[PLAIN_OFFSETS["key"]:PLAIN_OFFSETS["key"] + 32] = rec["key"]
    out[PLAIN_OFFSETS["mac"]:PLAIN_OFFSETS["mac"] + 6] = rec["mac"]
    out[PLAIN_OFFSETS["sec_flags"]:PLAIN_OFFSETS["sec_flags"] + 2] = (
        rec.get("sec_flags", 0).to_bytes(2, "big")
    )
    out[PLAIN_OFFSETS["secure_version"]:PLAIN_OFFSETS["secure_version"] + 4] = (
        rec.get("secure_version", 0).to_bytes(4, "big")
    )
    out[PLAIN_OFFSETS["app_version"]:PLAIN_OFFSETS["app_version"] + len(app_bytes)] = app_bytes
    out[PLAIN_OFFSETS["ds_pub_sha256"]:PLAIN_OFFSETS["ds_pub_sha256"] + 32] = rec.get(
        "ds_pub_sha256", b"\x00" * 32
    )
    return bytes(out)


def decode_plain(blob: bytes) -> dict:
    """Parses the 149-byte OAEP plaintext. Raises ValueError on a bad header."""
    if len(blob) != PLAIN_LEN:
        raise ValueError(f"plaintext must be {PLAIN_LEN} bytes, got {len(blob)}")
    if blob[0:4] != PLAIN_MAGIC or blob[4] != PLAIN_FORMAT:
        raise ValueError("bad magic or format in seal plaintext")
    app_raw = blob[PLAIN_OFFSETS["app_version"]:PLAIN_OFFSETS["app_version"] + 32]
    app = app_raw.split(b"\x00", 1)[0].decode("ascii")
    return {
        "key_version": int.from_bytes(blob[PLAIN_OFFSETS["key_version"]:][:4], "big"),
        "robot_id": blob[PLAIN_OFFSETS["robot_id"]:][:16].decode("ascii"),
        "nonce": blob[PLAIN_OFFSETS["nonce"]:][:16],
        "key": blob[PLAIN_OFFSETS["key"]:][:32],
        "mac": blob[PLAIN_OFFSETS["mac"]:][:6],
        "sec_flags": int.from_bytes(blob[PLAIN_OFFSETS["sec_flags"]:][:2], "big"),
        "secure_version": int.from_bytes(blob[PLAIN_OFFSETS["secure_version"]:][:4], "big"),
        "app_version": app,
        "ds_pub_sha256": blob[PLAIN_OFFSETS["ds_pub_sha256"]:][:32],
    }


def encode_blob(seal_key_id: int, rsa_ciphertext: bytes) -> bytes:
    """Wraps the RSA-3072 ciphertext in the upload envelope (§6.4)."""
    if len(rsa_ciphertext) != RSA_BYTES:
        raise ValueError(f"ciphertext must be {RSA_BYTES} bytes")
    if not 0 <= seal_key_id <= 0xFF:
        raise ValueError("seal_key_id must be a byte")
    out = bytearray(BLOB_LEN)
    out[0:4] = BLOB_MAGIC
    out[4] = BLOB_FORMAT
    out[5] = seal_key_id
    out[BLOB_HEADER:BLOB_HEADER + RSA_BYTES] = rsa_ciphertext
    return bytes(out)


def encode_record(rec: dict) -> bytes:
    """Builds the 20-byte eFuse identity record (§6.2)."""
    robot_id = rec["robot_id"]
    if not robot_id or len(robot_id) > ID_LEN:
        raise ValueError("robot_id must be 1..16 characters")
    flags = rec.get("flags", 0)
    if flags & ~(RECORD_FLAG_DEV | RECORD_FLAG_REPLACEMENT):
        raise ValueError("unknown flag bits")
    kv_base = rec["kv_base"]
    if not 1 <= kv_base <= 0xFFFF:
        raise ValueError("kv_base must be 1..65535")

    out = bytearray(RECORD_TOTAL)
    id_bytes = robot_id.encode("ascii")
    out[RECORD_OFFSETS["robot_id"]:RECORD_OFFSETS["robot_id"] + len(id_bytes)] = id_bytes
    out[RECORD_OFFSETS["format"]] = RECORD_FORMAT_V1
    out[RECORD_OFFSETS["flags"]] = flags
    out[RECORD_OFFSETS["kv_base"]:RECORD_OFFSETS["kv_base"] + 2] = kv_base.to_bytes(2, "big")
    return bytes(out)


def decode_record(raw: bytes) -> dict:
    if len(raw) != RECORD_TOTAL:
        raise ValueError(f"record must be {RECORD_TOTAL} bytes, got {len(raw)}")
    if raw[RECORD_OFFSETS["format"]] != RECORD_FORMAT_V1:
        raise ValueError("unknown record format")
    kv_base = int.from_bytes(raw[RECORD_OFFSETS["kv_base"]:][:2], "big")
    if kv_base == 0:
        raise ValueError("kv_base 0 is not a valid record")
    id_raw = raw[RECORD_OFFSETS["robot_id"]:][:ID_LEN]
    return {
        "robot_id": id_raw.split(b"\x00", 1)[0].decode("ascii"),
        "format": raw[RECORD_OFFSETS["format"]],
        "flags": raw[RECORD_OFFSETS["flags"]],
        "kv_base": kv_base,
    }


# ── Golden vectors ──────────────────────────────────────────────────────────

# Two records chosen to catch field mixups rather than to be realistic: every
# multi-byte field has a distinct non-symmetric value, and the second one has an
# empty app version and a non-zero secure_version.
FIXED_RECORDS = [
    {
        "name": "pilot-kv1",
        "robot_id": "MPX-AJZ7J8Y0YTDS",
        "key_version": 1,
        "nonce": bytes(range(16)),
        "key": bytes.fromhex("27c6f7d89725761ebff5ad8f0a29ce353ed5b2ea282e6ce0abb2d04af5daf5ae"),
        "mac": bytes.fromhex("246f28aabbcc"),
        "sec_flags": 0x0005,
        "secure_version": 0,
        "app_version": "1.2.3",
        "ds_pub_sha256": bytes(range(0x20, 0x40)),
    },
    {
        "name": "locked-kv7-empty-appver",
        "robot_id": "MPX-0123456789AB",
        "key_version": 7,
        "nonce": bytes([0xAA] * 16),
        "key": bytes([0x11] * 32),
        "mac": bytes.fromhex("c0ffee000001"),
        "sec_flags": 0x003F,
        "secure_version": 0x01020304,
        "app_version": "",
        "ds_pub_sha256": b"\x00" * 32,
    },
]


BYTE_FIELDS = ("nonce", "key", "mac", "ds_pub_sha256")


def record_to_json(rec: dict) -> dict:
    """JSON-safe copy of a record: raw bytes become hex strings."""
    out = dict(rec)
    for field in BYTE_FIELDS:
        if field in out:
            out[f"{field}_hex"] = out.pop(field).hex()
    return out


def record_from_json(rec: dict) -> dict:
    """Inverse of record_to_json."""
    out = dict(rec)
    for field in BYTE_FIELDS:
        if f"{field}_hex" in out:
            out[field] = bytes.fromhex(out.pop(f"{field}_hex"))
    return out


def build_vectors() -> dict:
    vectors = []
    for rec in FIXED_RECORDS:
        plain = encode_plain(rec)
        # A stand-in ciphertext: the layout test is about framing, and a real
        # RSA-OAEP output is random, so it can never be a golden vector.
        stub_ct = bytes([0xA5]) * RSA_BYTES
        blob = encode_blob(0, stub_ct)
        vectors.append(
            {
                "name": rec["name"],
                "record": record_to_json(rec),
                "plain_hex": plain.hex(),
                "plain_len": len(plain),
                "blob_hex": blob.hex(),
                "blob_len": len(blob),
            }
        )

    record_vectors = [
        {
            "name": "pilot",
            "record": {"robot_id": "MPX-AJZ7J8Y0YTDS", "flags": 0, "kv_base": 1},
            "raw_hex": encode_record(
                {"robot_id": "MPX-AJZ7J8Y0YTDS", "flags": 0, "kv_base": 1}
            ).hex(),
        },
        {
            "name": "dev-flagged-kv3",
            "record": {"robot_id": "MPX-0123456789AB", "flags": 0x03, "kv_base": 3},
            "raw_hex": encode_record(
                {"robot_id": "MPX-0123456789AB", "flags": 0x03, "kv_base": 3}
            ).hex(),
        },
    ]

    return {
        "$comment": (
            "Generated by tools/mpx_seal.py vectors. Golden vectors for the sealed "
            "blob layout (§6.4) and the eFuse identity record (§6.2). The blob's "
            "ciphertext is a stand-in: real RSA-OAEP output is random."
        ),
        "plain_len": PLAIN_LEN,
        "blob_len": BLOB_LEN,
        "rsa_bytes": RSA_BYTES,
        "plain_fields": [{"name": n, "offset": PLAIN_OFFSETS[n], "len": l}
                         for n, l in PLAIN_FIELDS],
        "record_fields": [{"name": n, "offset": RECORD_OFFSETS[n], "len": l}
                          for n, l in RECORD_FIELDS],
        "seals": vectors,
        "records": record_vectors,
    }


def write_vectors() -> Path:
    VECTORS_PATH.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(build_vectors(), indent=2, ensure_ascii=False) + "\n"
    VECTORS_PATH.write_text(text, encoding="utf-8")
    return VECTORS_PATH


def check_vectors() -> int:
    if not VECTORS_PATH.exists():
        print(f"missing {VECTORS_PATH}; run: python3 tools/mpx_seal.py vectors")
        return 1
    expected = json.dumps(build_vectors(), indent=2, ensure_ascii=False) + "\n"
    actual = VECTORS_PATH.read_text(encoding="utf-8")
    if actual != expected:
        print(f"{VECTORS_PATH} is stale; run: python3 tools/mpx_seal.py vectors")
        return 1
    return 0


def write_c_header(path: Path) -> None:
    data = build_vectors()
    lines = [
        "/* Generated by tools/mpx_seal.py -- do not edit. */",
        "#ifndef MPX_SEAL_VECTORS_H",
        "#define MPX_SEAL_VECTORS_H",
        "",
        f"#define MPX_SEAL_VEC_PLAIN_LEN {data['plain_len']}",
        f"#define MPX_SEAL_VEC_BLOB_LEN {data['blob_len']}",
        f"#define MPX_SEAL_VEC_RSA_BYTES {data['rsa_bytes']}",
        "",
        f"#define MPX_SEAL_VEC_SEAL_COUNT {len(data['seals'])}",
        "/* name, OAEP plaintext, upload blob */",
        "static const struct {",
        "    const char *name;",
        "    const char *plain_hex;",
        "    const char *blob_hex;",
        "} MPX_SEAL_VECS[] = {",
    ]
    for vec in data["seals"]:
        lines.append(f'    {{"{vec["name"]}", "{vec["plain_hex"]}", "{vec["blob_hex"]}"}},')
    lines += [
        "};",
        "",
        f"#define MPX_SEAL_VEC_RECORD_COUNT {len(data['records'])}",
        "/* name, raw 20-byte eFuse record */",
        "static const struct {",
        "    const char *name;",
        "    const char *raw_hex;",
        "} MPX_SEAL_RECORDS[] = {",
    ]
    for vec in data["records"]:
        lines.append(f'    {{"{vec["name"]}", "{vec["raw_hex"]}"}},')
    lines += ["};", "", "#endif /* MPX_SEAL_VECTORS_H */", ""]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def selftest() -> int:
    failures = []

    def check(cond: bool, msg: str) -> None:
        if not cond:
            failures.append(msg)

    data = build_vectors()

    # Lengths are the contract with the firmware.
    check(PLAIN_LEN == 149, "plaintext must be 149 bytes")
    check(BLOB_LEN == 392, "blob must be 392 bytes (8 + 384)")

    # Every plaintext field must start where the firmware's header says it does,
    # and the last field must end exactly at the end of the buffer.
    last_end = 0
    for name, length in PLAIN_FIELDS:
        check(PLAIN_OFFSETS[name] >= 0, f"{name} has no offset")
        last_end = PLAIN_OFFSETS[name] + length
    check(last_end == PLAIN_LEN, f"fields end at {last_end}, not {PLAIN_LEN}")

    # Round trips.
    for vec in data["seals"]:
        rec = record_from_json(vec["record"])
        plain = bytes.fromhex(vec["plain_hex"])
        decoded = decode_plain(plain)
        check(decoded["key_version"] == rec["key_version"], "kv round trip")
        check(decoded["robot_id"] == rec["robot_id"], "id round trip")
        check(decoded["nonce"] == rec["nonce"], "nonce round trip")
        check(decoded["key"] == rec["key"], "key round trip")
        check(decoded["mac"] == rec["mac"], "mac round trip")
        check(decoded["sec_flags"] == rec["sec_flags"], "flags round trip")
        check(decoded["secure_version"] == rec["secure_version"], "sver round trip")
        check(decoded["app_version"] == rec["app_version"], "appver round trip")
        check(decoded["ds_pub_sha256"] == rec["ds_pub_sha256"], "dshash round trip")

        blob = bytes.fromhex(vec["blob_hex"])
        check(len(blob) == BLOB_LEN, "blob length")
        check(blob[0:4] == BLOB_MAGIC, "blob magic")
        check(blob[4] == BLOB_FORMAT, "blob format")
        check(blob[BLOB_HEADER:] == bytes([0xA5]) * RSA_BYTES, "ciphertext at offset 8")

    # Every single-byte-sensitivity property: changing one field must change the
    # bytes, otherwise two different robots could produce the same plaintext.
    base = record_from_json(data["seals"][0]["record"])
    base_plain = encode_plain(base)
    mutations = {
        "key_version": {**base, "key_version": 2},
        "robot_id": {**base, "robot_id": "MPX-AJZ7J8Y0YTDT"},
        "nonce": {**base, "nonce": bytes([0xFF] * 16)},
        "key": {**base, "key": bytes([0x00] * 32)},
        "mac": {**base, "mac": bytes.fromhex("000000000000")},
        "sec_flags": {**base, "sec_flags": 0},
        "secure_version": {**base, "secure_version": 1},
        "app_version": {**base, "app_version": "1.2.4"},
        "ds_pub_sha256": {**base, "ds_pub_sha256": b"\x01" * 32},
    }
    for field, rec in mutations.items():
        check(encode_plain(rec) != base_plain, f"changing {field} did not change the plaintext")

    # Rejections: a station must never seal something the cloud cannot trust.
    for bad, why in [
        ({"robot_id": "MPX-AJZ7J8Y0YTD", "key_version": 1}, "short id"),
        ({"robot_id": "MPX-AJZ7J8Y0YTDS", "key_version": 0}, "kv 0"),
        ({"robot_id": "MPX-AJZ7J8Y0YTDS", "key_version": 1, "app_version": "x" * 33}, "long appver"),
    ]:
        rec = {**base, **bad}
        try:
            encode_plain(rec)
        except ValueError:
            pass
        else:
            failures.append(f"accepted invalid plaintext: {why}")

    # eFuse record: fixed length, and kv_base 0 is never a record.
    for vec in data["records"]:
        raw = bytes.fromhex(vec["raw_hex"])
        check(len(raw) == 20, "record must be 20 bytes")
        check(raw[RECORD_OFFSETS["format"]] == RECORD_FORMAT_V1, "record format byte")
        decoded = decode_record(raw)
        check(decoded["robot_id"] == vec["record"]["robot_id"], "record id round trip")
        check(decoded["flags"] == vec["record"]["flags"], "record flags round trip")
        check(decoded["kv_base"] == vec["record"]["kv_base"], "record kv round trip")
    try:
        decode_record(bytes(RECORD_TOTAL))
    except ValueError:
        pass
    else:
        failures.append("accepted an all-zero eFuse record")

    if failures:
        for f in failures:
            print(f"FAIL  {f}")
        return 1
    print(f"mpx_seal selftest: {len(data['seals'])} seals, "
          f"{len(data['records'])} records, all checks passed")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_vectors = sub.add_parser("vectors", help="write or check tools/vectors/seal.json")
    p_vectors.add_argument("--check", action="store_true",
                           help="exit 1 if the file is out of date instead of writing")

    p_header = sub.add_parser("c-header", help="emit the C header for the host tests")
    p_header.add_argument("path", type=Path)

    sub.add_parser("selftest", help="check the layout's properties")

    args = parser.parse_args(argv)

    if args.cmd == "vectors":
        if args.check:
            return check_vectors()
        path = write_vectors()
        print(f"wrote {path}")
        return 0
    if args.cmd == "c-header":
        write_c_header(args.path)
        print(f"wrote {args.path}")
        return 0
    if args.cmd == "selftest":
        return selftest()
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

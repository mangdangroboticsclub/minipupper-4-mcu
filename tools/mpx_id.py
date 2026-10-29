#!/usr/bin/env python3
"""Reference implementation of the MPX robot ID and key derivation.

This is the single source of truth for two things the firmware and the cloud
must agree on byte for byte (docs/ROBOT_IDENTITY_AND_KEYS.md §6.1, §6.3):

  ID        "MPX-" + 11 Crockford-Base32 symbols + 1 Luhn-mod-32 check symbol
  K_v       HMAC-SHA256(S, "mpx/v1/robot-key" 0x00 robot_id[16] uint32_be(v))

`S` is the per-chip secret burned into the HMAC_UP eFuse block. In production
it never leaves the chip; this tool exists to

  * generate IDs and validate ones typed by a human,
  * reproduce the golden vectors the firmware asserts against,
  * emit those vectors as a C header so the host tests need no JSON parser.

The vectors live in tools/vectors/identity.json and `vectors --check` fails if
the file is stale, so CI catches an algorithm change that was not propagated
to the firmware tests.

Usage:
    ./mpx_id.py new [--prefix MPX] [--count N]
    ./mpx_id.py check ID [ID ...]
    ./mpx_id.py derive --robot-id ID --kv 1 [--secret-hex HEX]
    ./mpx_id.py vectors [--check]
    ./mpx_id.py c-header [PATH]
    ./mpx_id.py selftest
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import re
import secrets
import struct
import sys
from pathlib import Path

# ── constants (frozen: §6.1, §6.3, §12.1 #1 and #3) ──────────────────────

ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"  # Crockford Base32, no I L O U
ID_RE = re.compile(r"^(MPX|DEV)-[0-9A-HJKMNP-TV-Z]{12}$")
BODY_LEN = 11
ID_LEN = 16  # "MPX-" + 12 == 16 ASCII bytes == the frame's uuid field

DERIVE_LABEL = b"mpx/v1/robot-key"
KEYFP_LABEL = b"mpx/v1/key-fp"
TOKEN_LABEL = b"mpx-ai-v1:"

# Deterministic secret for the golden vectors only. NEVER provision this on a
# board: it is published, so a robot holding it is unprotected.
TEST_SECRET_HEX = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"

ROOT = Path(__file__).resolve().parent.parent
VECTORS = ROOT / "tools" / "vectors" / "identity.json"


# ── §6.1 robot ID ────────────────────────────────────────────────────────

def luhn32_check(body: str) -> str:
    """The Luhn-mod-32 check symbol for an 11-symbol body."""
    factor, total = 2, 0
    for ch in reversed(body):
        a = factor * ALPHABET.index(ch)
        factor = 1 if factor == 2 else 2
        total += a // 32 + a % 32
    return ALPHABET[(32 - total % 32) % 32]


def new_robot_id(prefix: str = "MPX") -> str:
    body = "".join(secrets.choice(ALPHABET) for _ in range(BODY_LEN))
    return f"{prefix}-{body}{luhn32_check(body)}"


def normalise(raw: str) -> str:
    """Repair what a human types. Only for human input: the gateway compares
    the 16 bytes exactly, so a normalised ID that differs from the burned one
    is simply a different (unknown) robot."""
    return (
        raw.strip()
        .upper()
        .replace("O", "0")
        .replace("I", "1")
        .replace("L", "1")
    )


def is_valid_robot_id(raw: str) -> bool:
    s = normalise(raw)
    return bool(ID_RE.match(s)) and luhn32_check(s[4:15]) == s[15]


# ── §6.3 key derivation ──────────────────────────────────────────────────

def derive_message(robot_id: str, kv: int) -> bytes:
    rid = robot_id.encode("ascii")
    if len(rid) != ID_LEN:
        raise ValueError(f"robot_id must be exactly {ID_LEN} ASCII bytes, got {len(rid)}")
    if not 0 <= kv <= 0xFFFFFFFF:
        raise ValueError(f"key version out of range: {kv}")
    return DERIVE_LABEL + b"\x00" + rid + struct.pack(">I", kv)


def derive_key(secret: bytes, robot_id: str, kv: int) -> bytes:
    """K_v. On device this is esp_hmac_calculate() over the same bytes."""
    return hmac.new(secret, derive_message(robot_id, kv), hashlib.sha256).digest()


def key_fp(key: bytes) -> str:
    return hmac.new(key, KEYFP_LABEL, hashlib.sha256).hexdigest()[:16]


def robot_token(robot_id: str, key: bytes) -> str:
    """What the robot sends to the voice/LLM gateway. Unchanged in v2."""
    sig = hmac.new(key, TOKEN_LABEL + robot_id.encode("ascii"), hashlib.sha256).hexdigest()
    return f"{robot_id}.{sig[:32]}"


# ── golden vectors ───────────────────────────────────────────────────────

# Bodies on the edges of the value range: an ordinary ID, all-zero symbols, and
# all-maximum symbols. The check symbol is computed, never typed -- a
# hand-written one is how this file first shipped "MPX-ZZZZZZZZZZZZ", whose last
# symbol did not match its own body, and nothing noticed until the C tests
# started validating the vectors instead of only carrying them.
VECTOR_BODIES = ("AJZ7J8Y0YTD", "00000000000", "ZZZZZZZZZZZ")
VECTOR_IDS = tuple(f"MPX-{b}{luhn32_check(b)}" for b in VECTOR_BODIES)

# Inputs that must be rejected. The check symbol is what catches a typo before
# it becomes a support call.
INVALID_IDS = (
    "MPX-DOG-01",          # the pre-identity placeholder
    "MPX-A1Z7J8Y0YTDS",    # one symbol changed, check symbol left alone
    "MPX-AJZ7J8Y0YTDSX",   # 13 symbols
    "MPX-AJZ7J8Y0YTD",     # 11 symbols, no check symbol
    "DEV-AJZ7J8Y0YTD",     # missing check symbol
    "XPX-AJZ7J8Y0YTDS",    # wrong prefix
    "MPX-AJZ7J8Y0YTDU",    # U is not in the alphabet
    "MPX-AJZ7J8Y0YTDI",    # I normalises to 1, so this is 1 mismatch away
)


def build_vectors() -> dict:
    secret = bytes.fromhex(TEST_SECRET_HEX)
    entries = []
    for rid in VECTOR_IDS:
        for kv in (1, 2):
            k = derive_key(secret, rid, kv)
            entries.append(
                {
                    "robot_id": rid,
                    "kv": kv,
                    "secret_hex": secret.hex(),
                    "msg_hex": derive_message(rid, kv).hex(),
                    "key_hex": k.hex(),
                    "key_fp": key_fp(k),
                    "robot_token": robot_token(rid, k),
                }
            )

    return {
        "_comment": "Generated by tools/mpx_id.py. Do not edit; run `tools/mpx_id.py vectors`.",
        "version": 1,
        "source": "docs/ROBOT_IDENTITY_AND_KEYS.md §6.1, §6.3",
        "alphabet": ALPHABET,
        "id_regex": ID_RE.pattern,
        "id_len": ID_LEN,
        "derive_label": DERIVE_LABEL.decode(),
        "key_fp_label": KEYFP_LABEL.decode(),
        "token_label": TOKEN_LABEL.decode(),
        "test_secret_hex": TEST_SECRET_HEX,
        "test_secret_warning": "published test key; never provision it on a board",
        "vectors": entries,
        "valid_ids": list(VECTOR_IDS),
        "invalid_ids": [i for i in INVALID_IDS if not is_valid_robot_id(i)],
    }


def vectors_text(data: dict | None = None) -> str:
    # ensure_ascii=False keeps the § of the doc reference readable in the file.
    return json.dumps(data or build_vectors(), indent=2, ensure_ascii=False) + "\n"


def write_vectors(path: Path = VECTORS) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(vectors_text(), encoding="utf-8")
    return path


# ── C header, so the host tests need no JSON parser ──────────────────────

def c_bytes(b: bytes) -> str:
    return ", ".join(f"0x{x:02x}" for x in b)


def c_header(data: dict | None = None) -> str:
    data = data or build_vectors()
    out = [
        "/* GENERATED by tools/mpx_id.py -- do not edit, run `tools/mpx_id.py c-header`. */",
        "#ifndef MPX_VECTORS_H",
        "#define MPX_VECTORS_H",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        f'#define MPX_VEC_ALPHABET     "{data["alphabet"]}"',
        f'#define MPX_VEC_ID_REGEX     "{data["id_regex"]}"',
        f"#define MPX_VEC_ID_LEN       {data['id_len']}",
        f'#define MPX_VEC_DERIVE_LABEL "{data["derive_label"]}"',
        f'#define MPX_VEC_KEYFP_LABEL  "{data["key_fp_label"]}"',
        f'#define MPX_VEC_TOKEN_LABEL  "{data["token_label"]}"',
        "",
        "typedef struct {",
        "    const char    *robot_id;",
        "    uint32_t       kv;",
        "    const uint8_t *secret;      /* 32 bytes on device: the HMAC_UP block */",
        "    size_t         secret_len;",
        "    const uint8_t *msg;         /* 37 bytes */",
        "    size_t         msg_len;",
        "    const uint8_t *key;         /* 32 bytes */",
        "    size_t         key_len;",
        "    const char    *key_fp;      /* 16 hex chars */",
        "    const char    *robot_token;",
        "} mpx_vector_t;",
        "",
    ]

    names = []
    for i, v in enumerate(data["vectors"]):
        slug = f"v{i}_kv{v['kv']}"
        out += [
            f"static const uint8_t mpx_vec_{slug}_secret[] = {{ {c_bytes(bytes.fromhex(v['secret_hex']))} }};",
            f"static const uint8_t mpx_vec_{slug}_msg[] = {{ {c_bytes(bytes.fromhex(v['msg_hex']))} }};",
            f"static const uint8_t mpx_vec_{slug}_key[] = {{ {c_bytes(bytes.fromhex(v['key_hex']))} }};",
            (
                f'static const mpx_vector_t mpx_vec_{slug} = {{ "{v["robot_id"]}", {v["kv"]}u, '
                f"mpx_vec_{slug}_secret, sizeof mpx_vec_{slug}_secret, "
                f"mpx_vec_{slug}_msg, sizeof mpx_vec_{slug}_msg, "
                f"mpx_vec_{slug}_key, sizeof mpx_vec_{slug}_key, "
                f'"{v["key_fp"]}", "{v["robot_token"]}" }};'
            ),
            "",
        ]
        names.append(f"&mpx_vec_{slug}")

    out += [
        f"#define MPX_VEC_COUNT {len(names)}",
        "static const mpx_vector_t *const mpx_vectors[MPX_VEC_COUNT] = {",
        "    " + ", ".join(names),
        "};",
        "",
        f"#define MPX_VALID_ID_COUNT {len(data['valid_ids'])}",
        "static const char *const mpx_valid_ids[MPX_VALID_ID_COUNT] = {",
        "    " + ", ".join(f'"{i}"' for i in data["valid_ids"]),
        "};",
        "",
        f"#define MPX_INVALID_ID_COUNT {len(data['invalid_ids'])}",
        "static const char *const mpx_invalid_ids[MPX_INVALID_ID_COUNT] = {",
        "    " + ", ".join(f'"{i}"' for i in data["invalid_ids"]),
        "};",
        "",
        "#endif /* MPX_VECTORS_H */",
        "",
    ]
    return "\n".join(out)


# ── §11 test 1: the ID properties, as a self-check ───────────────────────

def selftest() -> int:
    fails = 0

    def check(ok: bool, what: str) -> None:
        nonlocal fails
        if not ok:
            fails += 1
            print(f"FAIL  {what}", file=sys.stderr)

    # Test vector from §6.1, and the negative case.
    check(luhn32_check("AJZ7J8Y0YTD") == "S", "luhn32 of AJZ7J8Y0YTD is S")
    check(is_valid_robot_id("MPX-AJZ7J8Y0YTDS"), "MPX-AJZ7J8Y0YTDS validates")
    # The vectors the firmware asserts against have to be valid under the very
    # algorithm they exercise, or the C tests fail on a vector bug instead of a
    # code bug.
    check(
        all(is_valid_robot_id(i) for i in VECTOR_IDS),
        "every golden vector ID validates",
    )
    check(not is_valid_robot_id("MPX-A1Z7J8Y0YTDS"), "MPX-A1Z7J8Y0YTDS rejected")
    check(not is_valid_robot_id("MPX-DOG-01"), "MPX-DOG-01 rejected")
    check(is_valid_robot_id("mpx-ajz7j8y0ytds"), "lower case normalises and validates")

    # Generated IDs: well formed, right length, unique.
    generated = [new_robot_id() for _ in range(20_000)]
    check(all(ID_RE.match(i) for i in generated), "every generated ID matches ID_RE")
    check(all(len(i.encode()) == ID_LEN for i in generated), "every generated ID is 16 bytes")
    check(len(set(generated)) == len(generated), "generated IDs are unique")

    # Every single-symbol substitution in the body must fail the check. That is
    # the entire point of the check symbol.
    bad = 0
    for rid in generated[:200]:
        body = rid[4:15]
        for pos in range(BODY_LEN):
            for ch in ALPHABET:
                if ch == body[pos]:
                    continue
                mutated = body[:pos] + ch + body[pos + 1:]
                if is_valid_robot_id(f"MPX-{mutated}{rid[15]}"):
                    bad += 1
    check(bad == 0, f"single-symbol substitutions all rejected ({bad} accepted)")

    # Vectors reproduce the values published in §6.3.
    secret = bytes.fromhex(TEST_SECRET_HEX)
    k = derive_key(secret, "MPX-AJZ7J8Y0YTDS", 1)
    check(
        derive_message("MPX-AJZ7J8Y0YTDS", 1).hex()
        == "6d70782f76312f726f626f742d6b6579004d50582d414a5a374a3859305954445300000001",
        "§6.3 msg vector",
    )
    check(
        k.hex() == "27c6f7d89725761ebff5ad8f0a29ce353ed5b2ea282e6ce0abb2d04af5daf5ae",
        "§6.3 K_1 vector",
    )
    check(key_fp(k) == "6d5f50ad94184e23", "§6.3 key_fp vector")
    check(
        robot_token("MPX-AJZ7J8Y0YTDS", k) == "MPX-AJZ7J8Y0YTDS.460a4a84a2e2e36cefade46a38a89c93",
        "§6.3 robot token vector",
    )

    # kv changes the key and nothing else about the derivation.
    check(derive_key(secret, "MPX-AJZ7J8Y0YTDS", 2) != k, "kv=2 derives a different key")
    check(len(derive_message("MPX-AJZ7J8Y0YTDS", 1)) == 37, "msg is 37 bytes")

    # The checked-in vectors file must match what this code produces.
    try:
        on_disk = json.loads(VECTORS.read_text(encoding="utf-8"))
        check(on_disk == json.loads(vectors_text()), f"{VECTORS.name} is up to date")
    except FileNotFoundError:
        check(False, f"{VECTORS} is missing (run `tools/mpx_id.py vectors`)")

    print("selftest: FAIL" if fails else "selftest: ok", file=sys.stderr if fails else sys.stdout)
    return 1 if fails else 0


# ── CLI ──────────────────────────────────────────────────────────────────

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_new = sub.add_parser("new", help="generate new robot IDs")
    p_new.add_argument("--prefix", default="MPX", choices=("MPX", "DEV"))
    p_new.add_argument("--count", type=int, default=1)

    p_check = sub.add_parser("check", help="validate IDs typed by a human")
    p_check.add_argument("ids", nargs="+")

    p_derive = sub.add_parser("derive", help="derive a key and its fingerprints")
    p_derive.add_argument("--robot-id", required=True)
    p_derive.add_argument("--kv", type=int, default=1)
    p_derive.add_argument("--secret-hex", default=TEST_SECRET_HEX)

    p_vec = sub.add_parser("vectors", help="regenerate tools/vectors/identity.json")
    p_vec.add_argument("--check", action="store_true", help="fail if the file is stale")

    p_hdr = sub.add_parser("c-header", help="write the vectors as a C header")
    p_hdr.add_argument("path", nargs="?", help="output path (default: stdout)")

    sub.add_parser("selftest", help="run the §11 test-1 properties")

    args = ap.parse_args(argv)

    if args.cmd == "new":
        for _ in range(args.count):
            print(new_robot_id(args.prefix))
        return 0

    if args.cmd == "check":
        bad = 0
        for raw in args.ids:
            norm = normalise(raw)
            if is_valid_robot_id(raw):
                note = "" if norm == raw.strip() else f"  (normalised from {raw!r})"
                print(f"ok      {norm}{note}")
            else:
                bad += 1
                print(f"INVALID {raw!r}", file=sys.stderr)
        return 1 if bad else 0

    if args.cmd == "derive":
        secret = bytes.fromhex(args.secret_hex)
        if len(secret) != 32:
            print("secret must be 32 bytes", file=sys.stderr)
            return 2
        if not is_valid_robot_id(args.robot_id):
            print(f"warning: {args.robot_id!r} is not a valid robot ID", file=sys.stderr)
        msg = derive_message(args.robot_id, args.kv)
        k = derive_key(secret, args.robot_id, args.kv)
        print(f"robot_id  {args.robot_id}")
        print(f"kv        {args.kv}")
        print(f"msg       {msg.hex()}  ({len(msg)} bytes)")
        print(f"K_v       {k.hex()}")
        print(f"key_fp    {key_fp(k)}")
        print(f"token     {robot_token(args.robot_id, k)}")
        return 0

    if args.cmd == "vectors":
        if args.check:
            try:
                have = VECTORS.read_text(encoding="utf-8")
            except FileNotFoundError:
                print(f"{VECTORS} is missing", file=sys.stderr)
                return 1
            if have != vectors_text():
                print(f"{VECTORS} is stale; run `tools/mpx_id.py vectors`", file=sys.stderr)
                return 1
            print(f"{VECTORS.name} is up to date")
            return 0
        print(f"wrote {write_vectors()}")
        return 0

    if args.cmd == "c-header":
        text = c_header()
        if args.path:
            path = Path(args.path)
            path.parent.mkdir(parents=True, exist_ok=True)
            # Only touch the file when it changed, so the build stays warm.
            if path.exists() and path.read_text(encoding="utf-8") == text:
                return 0
            path.write_text(text, encoding="utf-8")
        else:
            sys.stdout.write(text)
        return 0

    if args.cmd == "selftest":
        return selftest()

    return 2


if __name__ == "__main__":
    sys.exit(main())

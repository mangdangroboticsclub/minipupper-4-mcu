#!/usr/bin/env python3
"""Bench station for provisioning a robot identity (docs/ROBOT_IDENTITY_AND_KEYS.md §6.6).

The device side of this conversation is `components/mpx_identity/mpx_provision.c`:
a listener on UART0 at 115200 8N1 that reads `@MPXPROV <json>` lines and answers
`@MPXPROVR <json>`. This script is the other half. It does four things:

  * finds the port by pinging (`@MPXPROV` with no body) instead of guessing,
  * walks a unit through `provision` -> `lock`, resuming a half-burned board,
  * decrypts the returned sealed blob with the station's private key, which is
    the only proof that the unit sealed to the key the cloud actually holds,
  * writes the resulting registration record for the cloud to import.

The private key never touches the device: the device seals K_v to the public
half, and K_v leaves the seal only here, on the station, where it is immediately
handed to the cloud's key wrap.

Usage:
    ./mpx_provision.py scan
    ./mpx_provision.py status [--port /dev/ttyACM0]
    ./mpx_provision.py provision [--id MPX-xxxx] [--kv N] [--replace] [--out reg.json]
                                 [--secret-hex HEX | --secret-random]
    ./mpx_provision.py reseal [--out reg.json]
    ./mpx_provision.py lock
    ./mpx_provision.py unseal BLOB_HEX [--out reg.json]
    ./mpx_provision.py verify --registration reg.json

`provision` prints the sealed blob and writes the registration file; `unseal`
takes a blob from an older log or a device that has one but no station run.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path

import serial
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))

import mpx_id  # noqa: E402  the ID generator; one implementation, not two
import mpx_seal  # noqa: E402  the blob layout, shared with the firmware

DEFAULT_BAUD = 115200
DEBUG = bool(os.environ.get("MPX_PROV_DEBUG"))
DEFAULT_KEY = REPO_ROOT / "tools" / "secrets" / "prov_seal_priv.pem"
PREFIX = "@MPXPROV"
REPLY = "@MPXPROVR"
# The blob reply is ~800 characters and the reply line is terminated by a
# newline; the device's log can interleave anything before it, so the read
# window is generous.
REPLY_TIMEOUT = 12.0


class StationError(RuntimeError):
    """A reply the station cannot act on."""


def open_port(port: str | None, baud: int = DEFAULT_BAUD) -> serial.Serial:
    """Open the port, or find it by pinging when none was given."""
    if port is not None:
        try:
            return serial.Serial(port, baud, timeout=0.2)
        except serial.SerialException as exc:
            raise StationError(f"cannot open {port}: {exc}") from exc

    candidates = []
    try:
        from serial.tools import list_ports
    except ImportError:                                  # pragma: no cover
        list_ports = None
    if list_ports is not None:
        # ESP32-S3 devkits enumerate as USB CDC; a UART bridge shows up as
        # ttyUSB. Both can be the provisioning port, so try both families.
        candidates = [p.device for p in list_ports.comports()
                      if "ttyACM" in p.device or "ttyUSB" in p.device]

    for device in candidates:
        try:
            conn = serial.Serial(device, baud, timeout=0.2)
        except OSError:
            continue
        try:
            if ping(conn):
                print(f"found provisioning listener on {device}")
                return conn
        finally:
            if not conn.is_open:                         # pragma: no cover
                conn = None
        conn.close()

    raise StationError(
        "no provisioning listener found; pass --port, or check that the image was "
        "built with CONFIG_APP_PROVISIONING_ENABLE=y")


def ping(conn: serial.Serial) -> bool:
    """A bare prefix is the ping: any reply means a listener is there."""
    conn.reset_input_buffer()
    conn.write(b"@MPXPROV\n")
    conn.flush()
    deadline = time.monotonic() + 1.5
    while time.monotonic() < deadline:
        line = conn.readline().decode("utf-8", "replace").strip()
        if line.startswith(REPLY):
            return True
    return False


def send(conn: serial.Serial, command: dict) -> dict:
    """Send one command and return the parsed reply."""
    conn.reset_input_buffer()
    line = f"{PREFIX} {json.dumps(command, separators=(',', ':'))}\n"
    conn.write(line.encode())
    conn.flush()
    return read_reply(conn)


def read_reply(conn: serial.Serial, timeout: float = REPLY_TIMEOUT) -> dict:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        raw = conn.readline().decode("utf-8", "replace").strip()
        if not raw.startswith(REPLY):
            # The device's own log shares this UART, and its error lines are the
            # only diagnosis for a refusal. Silent by default (they are noise
            # during a normal run), echoed with MPX_PROV_DEBUG=1.
            if DEBUG and raw:
                print(f"  device: {raw}", file=sys.stderr)
            continue
        body = raw[len(REPLY):].strip()
        try:
            reply = json.loads(body)
        except json.JSONDecodeError as exc:
            raise StationError(f"unparsable reply: {body!r}") from exc
        if not reply.get("ok", False):
            # A refusal is information, not a crash: the station needs to know
            # which state the unit is in to decide what to do next.
            raise StationError(
                f"{reply.get('cmd', '?')} refused: {reply.get('error', '?')} "
                f"(state={reply.get('state', '?')})")
        return reply
    raise StationError("no reply from the device; is the port the right one?")


def load_private_key(path: Path):
    if not path.exists():
        raise StationError(
            f"missing seal private key at {path}\n"
            "the station needs the private half of the key embedded in the image; "
            "see components/mpx_identity/certs/README.md")
    return serialization.load_pem_private_key(path.read_bytes(), password=None)


def unseal(blob_hex: str, private_key) -> tuple[dict, int]:
    """Decrypt a sealed blob and return (plaintext, seal_key_id).

    OAEP is the integrity check: a single flipped bit anywhere in the
    ciphertext or in the plaintext's own bytes makes the decrypt fail rather
    than return something plausible, which is why the plaintext's 6-byte MAC is
    not re-checked here -- it is there for the device to recognise its own blob,
    not for the station.
    """
    blob = bytes.fromhex(blob_hex)
    if len(blob) != mpx_seal.BLOB_LEN:
        raise StationError(f"blob is {len(blob)} bytes, expected {mpx_seal.BLOB_LEN}")
    if blob[:4] != mpx_seal.BLOB_MAGIC or blob[4] != mpx_seal.BLOB_FORMAT:
        raise StationError("blob header is not an MPX seal envelope")
    seal_key_id = blob[5]

    try:
        plaintext = private_key.decrypt(
            blob[mpx_seal.BLOB_HEADER:],
            padding.OAEP(mgf=padding.MGF1(algorithm=hashes.SHA256()),
                         algorithm=hashes.SHA256(), label=None))
    except ValueError as exc:
        # Wrong key, or the unit sealed to a slot this station does not hold.
        raise StationError(
            f"the blob does not decrypt under {private_key} slot {seal_key_id}: {exc}")
    try:
        plain = mpx_seal.decode_plain(plaintext)
    except ValueError as exc:
        raise StationError(f"sealed plaintext is malformed: {exc}") from exc
    return plain, seal_key_id


def registration(plain: dict, seal_key_id: int) -> dict:
    """The record the cloud imports: the key, and enough to audit where it came from."""
    key = plain["key"]
    return {
        "robot_uuid": plain["robot_id"],
        "kv": plain["key_version"],
        "aes_key_hex": key.hex(),
        "key_fp": mpx_id.key_fp(key),
        "seal_key_id": seal_key_id,
        "app_version": plain["app_version"],
        "secure_version": plain["secure_version"],
        "sec_flags": plain["sec_flags"],
        "provisioned_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }


def write_registration(path: Path, record: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(record, indent=2) + "\n")
    # The file holds a robot's key in the clear until the cloud wraps it, so it
    # must not be readable by anything else on the station.
    os.chmod(path, 0o600)
    print(f"registration written to {path} (mode 600)")
    print(f"  robot_uuid {record['robot_uuid']}  kv {record['kv']}  "
          f"key_fp {record['key_fp']}")
    print("  import it with the cloud's provisioning endpoint before deleting it")


def cmd_scan(args) -> int:
    with open_port(args.port, args.baud) as conn:
        port = conn.port
    print(f"port {port}")
    return 0


def cmd_status(args) -> int:
    with open_port(args.port, args.baud) as conn:
        reply = send(conn, {"cmd": "status"})
    for key in ("state", "id", "kv", "key_block", "secure_version", "sec_flags"):
        if key in reply:
            print(f"{key:14s} {reply[key]}")
    return 0


def cmd_provision(args) -> int:
    secret: bytes | None = None
    if args.secret_hex is not None:
        try:
            secret = bytes.fromhex(args.secret_hex)
        except ValueError as exc:
            raise StationError(f"--secret-hex is not hex: {exc}") from exc
        if len(secret) != 32:
            raise StationError(
                f"--secret-hex is {len(secret)} bytes, expected 32 (64 hex chars)")
    elif args.secret_random:
        # The HSM case stood in for: a factory that generates S itself and keeps
        # it, instead of letting each chip generate its own.
        secret = os.urandom(32)

    robot_id = args.id or mpx_id.new_robot_id()
    if not mpx_id.is_valid_robot_id(robot_id):
        raise StationError(f"{robot_id!r} is not a valid robot ID (run mpx_id.py check)")

    with open_port(args.port, args.baud) as conn:
        before = send(conn, {"cmd": "status"})
        existing = before.get("id", "")

        # A half-burned unit already holds its ID, and burning a second, different
        # one over it would strand the unit: the record is the identity, so the
        # device refuses rather than silently replacing it. Carrying the burned
        # ID forward makes "run it again" the right move after a lost power
        # cable, which is the normal way this command fails.
        if not args.id and existing and existing != "unprovisioned":
            if mpx_id.is_valid_robot_id(existing):
                print(f"reusing the ID already burned on this unit: {existing}")
                robot_id = existing

        print(f"provisioning {robot_id} kv={args.kv}")

        if existing.startswith("MPX-") and existing != robot_id:
            if not args.replace:
                raise StationError(
                    f"unit already provisioned as {before['id']}; "
                    "re-run with --replace to burn a replacement identity")
            print(f"replacing identity {before['id']}")

        command = {"cmd": "provision", "id": robot_id, "kv": args.kv}
        if secret is not None:
            command["s"] = secret.hex()
        try:
            reply = send(conn, command)
        except StationError as exc:
            # A partial burn is the expected failure of a bench run that lost
            # power; the device tells us which half is present, so retry once.
            if "bad_request" not in str(exc):
                raise
            print(f"{exc}; retrying after status")
            conn.reset_input_buffer()
            reply = send(conn, command)

    if reply.get("state") != "sealed":
        raise StationError(f"device reported state {reply.get('state')!r} after provisioning")

    print(f"sealed: id {reply['id']} kv {reply['kv']} key_block {reply['key_block']}")

    if secret is not None and reply.get("key_source") != "supplied":
        # The unit burned its own S instead of the one we sent. Nothing is
        # recoverable from it: only the sealed blob holds the key.
        print("warning: the device generated its own secret, not the supplied one; "
              "the key below cannot be re-derived from the factory's S")

    plain, seal_key_id = unseal(reply["blob"], load_private_key(args.key))
    if plain["robot_id"] != robot_id:
        raise StationError(f"seal round-trip returned {plain['robot_id']}, expected {robot_id}")

    if secret is not None:
        # The whole point of supplying S is that K_v is reproducible from it, so
        # this is checked against the seal rather than trusted.
        derived = mpx_id.derive_key(secret, robot_id, args.kv)
        if derived != plain["key"]:
            raise StationError(
                "the sealed key does not derive from the supplied secret "
                f"(expected {derived.hex()[:16]}, sealed {plain['key'].hex()[:16]})")
        print(f"key reproduces from the supplied secret (source "
              f"{reply.get('key_source', '?')})")

    record = registration(plain, seal_key_id)
    if secret is not None:
        # Printed, not stored: the factory keeps S in its HSM, and a copy on the
        # station's disk would be the same key material with worse handling.
        print(f"secret S (32 bytes hex, keep in the HSM, delete from logs): {secret.hex()}")
    if args.out is not None:
        write_registration(args.out, record)
    else:
        print("unsealed K_v, fingerprint " + record["key_fp"])
        print("  pass --out REGISTRATION.json to write it for the cloud")

    # The eFuse is burned, but the firmware that was running when it happened
    # read its identity at boot and found nothing: the HMAC peripheral only sees
    # a newly burned key after a reset. Without this the unit looks provisioned
    # to the station and unprovisioned to itself until someone power-cycles it
    # and wonders why enrolment never happened.
    print("now reset the board (power-cycle or `idf.py -p PORT monitor` then Ctrl-T "
          "Ctrl-R) so it reads its new identity and enrols")
    return 0


def cmd_reseal(args) -> int:
    with open_port(args.port, args.baud) as conn:
        # The device requires the identity in every seal request, including a
        # reseal: it compares the ID against the burned record, and a reseal
        # that could name a different ID would be a second way to provision.
        status = send(conn, {"cmd": "status"})
        if status.get("state") != "sealed":
            raise StationError(f"unit is {status.get('state', '?')}, not sealed; "
                               "reseal only re-seals an already provisioned unit")
        reply = send(conn, {"cmd": "reseal", "id": status["id"], "kv": status["kv"]})
    plain, seal_key_id = unseal(reply["blob"], load_private_key(args.key))
    record = registration(plain, seal_key_id)
    print(f"resealed {record['robot_uuid']} kv {record['kv']} key_fp {record['key_fp']}")
    if args.out is not None:
        write_registration(args.out, record)
    return 0


def cmd_lock(args) -> int:
    """Write-protect the identity block. Irreversible, so it is asked twice."""
    if not args.yes:
        answer = input(
            f"lock the identity block on {args.port or 'the first listener found'}?\n"
            "this is permanent and cannot be undone. type LOCK to continue: ")
        if answer.strip() != "LOCK":
            print("not locked")
            return 1

    with open_port(args.port, args.baud) as conn:
        # The device wants the same word: a command this expensive should be
        # impossible to send by reflex from a script.
        reply = send(conn, {"cmd": "lock", "confirm": "LOCK"})
    print(f"locked: {reply.get('state')}")
    print("  the identity block is now permanently write-disabled on this unit")
    return 0


def cmd_unseal(args) -> int:
    plain, seal_key_id = unseal(args.blob, load_private_key(args.key))
    record = registration(plain, seal_key_id)
    if args.out is not None:
        write_registration(args.out, record)
    else:
        print(json.dumps(record, indent=2))
    return 0


def cmd_verify(args) -> int:
    """Check a registration file against the blob and the key, without the device."""
    record = json.loads(Path(args.registration).read_text())
    key = bytes.fromhex(record["aes_key_hex"])
    if len(key) != 32:
        raise StationError(f"{args.registration}: aes_key_hex is not 32 bytes")
    if mpx_id.key_fp(key) != record["key_fp"]:
        raise StationError(f"{args.registration}: key_fp does not match the key")
    # The derived key is a pure function of the ID and the per-chip secret, so
    # with S in hand the registration can be reproduced end to end.
    if args.secret_hex is not None:
        secret = bytes.fromhex(args.secret_hex)
        derived = mpx_id.derive_key(secret, record["robot_uuid"], record["kv"])
        if derived != key:
            raise StationError(
                f"{args.registration}: key does not derive from that secret "
                f"(got {derived.hex()[:16]}, file has {key.hex()[:16]})")
        print("key matches the derivation from the supplied secret")
    print(f"ok {record['robot_uuid']} kv {record['kv']} key_fp {record['key_fp']}")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    def add_common(p, with_key: bool = True):
        p.add_argument("--port", help="serial port; omitted means scan")
        p.add_argument("--baud", type=int, default=DEFAULT_BAUD)
        if with_key:
            p.add_argument("--key", type=Path, default=DEFAULT_KEY,
                           help="station seal private key (default: tools/secrets/prov_seal_priv.pem)")

    add_common(sub.add_parser("scan", help="find the provisioning port"), with_key=False)
    add_common(sub.add_parser("status", help="report the unit's provisioning state"), with_key=False)
    p = sub.add_parser("lock", help="write-disable the identity block (permanent)")
    add_common(p, with_key=False)
    p.add_argument("--yes", action="store_true", help="skip the interactive confirmation")

    p = sub.add_parser("provision", help="burn an identity and collect its key")
    add_common(p)
    p.add_argument("--id", help="robot ID to burn; omitted means generate one")
    p.add_argument("--kv", type=int, default=1, help="key version (default: 1)")
    p.add_argument("--replace", action="store_true",
                   help="burn a replacement identity over an existing one")
    p.add_argument("--out", type=Path, help="write the registration JSON here")
    group = p.add_mutually_exclusive_group()
    group.add_argument("--secret-hex", metavar="HEX",
                       help="factory-supplied per-chip secret S (64 hex chars) to burn "
                            "instead of letting the chip generate one")
    group.add_argument("--secret-random", action="store_true",
                       help="generate S on the station (stand-in for the factory HSM) "
                            "and burn that")

    p = sub.add_parser("reseal", help="re-seal an already provisioned unit")
    add_common(p)
    p.add_argument("--out", type=Path, help="write the registration JSON here")

    p = sub.add_parser("unseal", help="decrypt a blob hex string")
    p.add_argument("blob", help="blob_hex as printed at provisioning time")
    p.add_argument("--key", type=Path, default=DEFAULT_KEY)
    p.add_argument("--out", type=Path)

    p = sub.add_parser("verify", help="check a registration file")
    p.add_argument("--registration", type=Path, required=True)
    p.add_argument("--secret-hex", help="per-chip secret S, to re-derive the key")

    args = parser.parse_args(argv)
    handlers = {
        "scan": cmd_scan, "status": cmd_status, "provision": cmd_provision,
        "reseal": cmd_reseal, "lock": cmd_lock, "unseal": cmd_unseal,
        "verify": cmd_verify,
    }
    try:
        return handlers[args.command](args)
    except StationError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except serial.SerialException as exc:
        # Unplugged mid-run is an operator event, not a bug to trace.
        print(f"error: serial port failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

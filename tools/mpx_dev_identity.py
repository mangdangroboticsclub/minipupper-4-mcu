#!/usr/bin/env python3
"""Give this dev board its own identity in the local sdkconfig (D11).

Every board in the office currently boots as `MPX-DOG-01` with the published
AES key `00..01`, so two robots on the same gateway are the same robot and the
key in the binary is in `docs/`. This writes a unique `DEV-` ID and a random
key into the developer's own `sdkconfig`, which is not in version control.

    ./mpx_dev_identity.py show          # what this checkout currently boots as
    ./mpx_dev_identity.py apply         # new DEV- id + random key, this board only

It never touches `sdkconfig.defaults`: a default with a random key in it would
give every clone the same "unique" identity, which is the bug it exists to fix.

The ID format and the check symbol come from tools/mpx_id.py, so a DEV- ID
generated here is valid under the same validator the firmware runs.
"""

from __future__ import annotations

import argparse
import re
import secrets
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import mpx_id  # noqa: E402  (sibling module, path set above)

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SDKCONFIG = ROOT / "sdkconfig"

UUID_SYM = "CONFIG_APP_ROBOT_UUID"
KEY_SYM = "CONFIG_APP_CHAT_AES_KEY_HEX"

# The values that mean "nobody has provisioned this checkout" -- the Kconfig
# defaults. Replacing them is the whole job; replacing anything else needs --force
# so a real identity cannot be wiped by a stray invocation.
PLACEHOLDER_UUID = "MPX-DOG-01"
PLACEHOLDER_KEY = "00" * 31 + "01"


def read_setting(path: Path, symbol: str) -> str | None:
    """The value of a string symbol in sdkconfig, or None."""
    if not path.is_file():
        return None
    pattern = re.compile(rf"^{symbol}=(.*)$", re.MULTILINE)
    match = pattern.search(path.read_text(encoding="utf-8", errors="replace"))
    if match is None:
        return None
    return match.group(1).strip().strip('"')


def write_setting(path: Path, symbol: str, value: str) -> None:
    """Set a string symbol, replacing the existing line or appending one."""
    line = f'{symbol}="{value}"'
    text = path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""
    pattern = re.compile(rf"^{symbol}=.*$", re.MULTILINE)
    if pattern.search(text):
        text = pattern.sub(line, text, count=1)
    else:
        if text and not text.endswith("\n"):
            text += "\n"
        text += line + "\n"
    path.write_text(text, encoding="utf-8")


def cmd_show(args: argparse.Namespace) -> int:
    path = Path(args.sdkconfig)
    uuid = read_setting(path, UUID_SYM)
    key_hex = read_setting(path, KEY_SYM)
    if not path.is_file():
        print(f"{path}: not found -- run `idf.py set-target esp32s3` first")
        return 1

    print(f"sdkconfig      {path}")
    print(f"robot id       {uuid or '(unset, Kconfig default: %s)' % PLACEHOLDER_UUID}")
    if key_hex:
        try:
            fp = mpx_id.key_fp(bytes.fromhex(key_hex))
        except ValueError:
            fp = "(not 64 hex digits)"
        tag = "  <-- the published default key" if key_hex == PLACEHOLDER_KEY else ""
        print(f"key fingerprint {fp}{tag}")
    else:
        print("key fingerprint (unset, Kconfig default)")
    return 0


def cmd_apply(args: argparse.Namespace) -> int:
    path = Path(args.sdkconfig)
    if not path.is_file():
        print(f"{path}: not found -- run `idf.py set-target esp32s3` first",
              file=sys.stderr)
        return 1

    current_uuid = read_setting(path, UUID_SYM)
    current_key = read_setting(path, KEY_SYM)

    if not args.force:
        for symbol, current, placeholder in (
            (UUID_SYM, current_uuid, PLACEHOLDER_UUID),
            (KEY_SYM, current_key, PLACEHOLDER_KEY),
        ):
            if current is not None and current != placeholder:
                print(
                    f"{symbol} is already set to a non-default value; refusing to "
                    f"overwrite it (pass --force if that is really what you want)",
                    file=sys.stderr,
                )
                return 1

    robot_id = args.id or mpx_id.new_robot_id(prefix="DEV")
    if not mpx_id.is_valid_robot_id(robot_id):
        print(f"{robot_id}: not a valid robot ID", file=sys.stderr)
        return 1
    if not robot_id.startswith("DEV-"):
        print(f"{robot_id}: warning -- dev identities should use the DEV- prefix",
              file=sys.stderr)

    key_hex = args.key_hex or secrets.token_hex(32)  # 32-byte AES key
    if len(key_hex) != 64:
        print("key must be 64 hex digits (32 bytes)", file=sys.stderr)
        return 1
    try:
        key = bytes.fromhex(key_hex)
    except ValueError:
        print("key is not valid hex", file=sys.stderr)
        return 1
    if key == bytes(32):
        print("all-zero key is refused: the firmware treats it as unprovisioned",
              file=sys.stderr)
        return 1

    write_setting(path, UUID_SYM, robot_id)
    write_setting(path, KEY_SYM, key_hex)

    print(f"robot id        {robot_id}")
    print(f"key fingerprint {mpx_id.key_fp(key)}")
    print(f"written to      {path}  (not in version control)")
    print("rebuild with `idf.py build` -- a running robot keeps the old identity")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--sdkconfig",
        default=str(DEFAULT_SDKCONFIG),
        help="sdkconfig to read/write (default: the repo root's)",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    show = sub.add_parser("show", help="print the identity this checkout boots as")
    show.set_defaults(func=cmd_show)

    apply = sub.add_parser("apply", help="write a new unique dev identity")
    apply.add_argument("--id", help="use this robot ID instead of generating one")
    apply.add_argument("--key-hex", help="use this 64-hex-digit key")
    apply.add_argument("--force", action="store_true",
                       help="overwrite an identity that is already set")
    apply.set_defaults(func=cmd_apply)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

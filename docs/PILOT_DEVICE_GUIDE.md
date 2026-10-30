# Pilot device guide

For anyone building firmware, flashing a board, or handling a pilot unit. The
reference for how identity works is [IDENTITY_AND_KEYS.md](./IDENTITY_AND_KEYS.md);
this is the procedure.

## The one rule

**A board's identity lives in its fuses, the firmware is disposable.** You can
flash a pilot unit as often as you like — but only with an image that reads the
fuses. Flashing the default development image onto a provisioned board makes it
run as `MPX-DOG-01` with the public dev key, which looks fine until two boards do
it at once. The firmware now logs an error when it detects this
(`identity: this board IS provisioned ... but the image uses the Kconfig identity`),
but it still boots — do not ignore that line.

## 1. One-time setup

Firmware commands need ESP-IDF v5.5.4 with the ESP32-S3 toolchain. Everything
else is plain Python. Where each kind of command runs:

| Command | Needs | Runs on |
|---|---|---|
| `idf.py …` | ESP-IDF v5.5.4 + toolchain | wherever ESP-IDF is installed — a container or a native install |
| `python3 tools/mpx_provision.py …` | Python 3.10+, `pyserial`, `cryptography` | the machine the board's USB cable is plugged into |
| `psql …` | database credentials | the gateway host |

Pick whichever of the two setups below matches your machine. They are
interchangeable; nothing in this guide depends on one of them.

### Option A — dev container

If you use the project's devcontainer, ESP-IDF is already installed inside it and
the repo is mounted at the container's workspace path. The container name,
mounts and USB passthrough are described in [../DEV_CONTAINER.md](../DEV_CONTAINER.md).

```bash
# from the host: find the running container, then get a shell inside it
CID=$(docker ps --filter ancestor=vsc-md-mpx-dog --format '{{.Names}}' | head -1)
docker exec -it "$CID" bash

cd <repo-root>                      # e.g. /workspaces/md-mpx-dog
source /opt/esp/idf/export.sh       # wherever ESP-IDF lives in that image; per shell
idf.py --version                    # expect ESP-IDF v5.5.4
```

Or run one command at a time from the host, without entering:

```bash
CID=$(docker ps --filter ancestor=vsc-md-mpx-dog --format '{{.Names}}' | head -1)
docker exec "$CID" bash -lc 'cd <repo-root> && source /opt/esp/idf/export.sh >/dev/null 2>&1 && idf.py build'
```

**USB inside a container** only works if the device was passed through
(`--device=/dev/ttyUSB0`, `--privileged`, or the devcontainer equivalent). If
`ls /dev/ttyUSB*` is empty in there, run the flashing, monitoring and station
commands on the host instead: `esptool`/`idf.py` are the only things that need
the container, and the station is plain Python.

### Option B — native install

```bash
git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf && ./install.sh esp32s3
. ~/esp/esp-idf/export.sh          # per shell; add to your shell profile if you like
```

Then in the repo root. Same commands as above, no `docker exec`:

```bash
idf.py set-target esp32s3
idf.py host-test
```

### Option C — Windows (PowerShell)

Two ways in, and the shortcuts the installer makes are the easy one:

**C1. ESP-IDF Tools Installer** (no compiler toolchain wrangling). Download it
from <https://dl.espressif.com/dl/esp-idf/>, pick v5.5.4 and the `esp32s3`
target. It creates *ESP-IDF 5.5 PowerShell* and *ESP-IDF 5.5 CMD* shortcuts in
the Start menu; opening one sources the environment for you. Skip to "Then, in
the repo root" below.

**C2. Git clone + scripts** (what you do on Linux, in PowerShell):

```powershell
git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf.git $HOME\esp\esp-idf
cd $HOME\esp\esp-idf
.\install.bat esp32s3
. .\export.ps1                 # per shell; run again in every new window
idf.py --version               # expect ESP-IDF v5.5.4
```

If PowerShell refuses the script, allow it for that window only (this does not
change machine policy):

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
. .\export.ps1
```

Then, in the repo root:

```powershell
idf.py set-target esp32s3
idf.py host-test
```

**Windows specifics that bite:**

- **Port names are `COM` ports**, not `/dev/tty*`. List them:

  ```powershell
  [System.IO.Ports.SerialPort]::GetPortNames()
  ```

  If that comes back empty (PowerShell 7 without the serial assembly, or an
  old machine), this always works:

  ```powershell
  Get-CimInstance Win32_PnPEntity | Where-Object Name -match 'COM\d+' | Select-Object Name
  ```

  Then pass the port explicitly: `idf.py -p COM5 flash`. If the board does not
  appear, install the USB bridge driver — Silicon Labs **CP210x** for most
  boards, **WCH CH343/CH340** for the newer ones. Device Manager shows the
  board under *Ports (COM & LPT)* and flags the driver problem there.
- **No line-continuation backslash.** Long commands use a backtick at end of
  line, and the argument stays quoted:

  ```powershell
  idf.py -B build\pilot `
         -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.pilot" `
         build flash
  ```

  The `;` inside the quotes is safe in PowerShell — it is part of the value, not
  a command separator.
- **Station commands are plain Python** and do not need the IDF environment.
  Any 3.10+ interpreter works:

  ```powershell
  py -3 -m pip install pyserial cryptography
  py -3 tools\mpx_provision.py status --port COM5
  py -3 tools\mpx_provision.py provision --port COM5 --out bench\label.json
  ```

  Close `idf.py monitor` before either station command: one process per COM port.
- **Deep build paths break the build.** Windows still enforces a 260-character
  limit unless long paths are enabled, and ESP-IDF build trees are deep. Keep
  the checkout near the drive root (`C:\work\md-mpx-dog`), or enable it once
  from an **administrator** shell:

  ```powershell
  Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem' `
                   -Name LongPathsEnabled -Value 1 -Type DWord
  ```

- **WSL2 also works** for building, but USB serial needs `usbipd-win` to forward
  the port into the distro. If you only need to flash, run PowerShell natively
  and skip the forwarding.

### Any of the three

python3, cmake and ninja come with the IDF installer. On Linux, serial access
usually needs group membership — without it every port operation fails with
`Permission denied`:

```bash
sudo usermod -aG dialout "$USER"   # then log out and back in
```

`sdkconfig` is **not in git**. The committed configuration is
`sdkconfig.defaults`, so a fresh clone has to pick the target once:

```bash
idf.py set-target esp32s3        # or pass -DIDF_TARGET=esp32s3 to every command
idf.py host-test                 # 6/6 green; proves your checkout is sane
```

`.gitignore` also ignores `sdkconfig`, so anything you change in menuconfig stays
on your machine. If a change must be shared, it belongs in `sdkconfig.defaults`.

## 2. Menuconfig

`idf.py menuconfig` → top entry **MPX Dog App Configuration**. These are the
entries that matter; everything else can stay at its default.

### Identity (the important one)

| Menu entry | Symbol | Bench / dev board | Pilot unit | Why |
|---|---|---|---|---|
| Robot identity source | `APP_IDENTITY_SOURCE` | *Kconfig strings (development only)* | **eFuse HMAC_UP key + USR_DATA robot ID** | the two must agree; a Kconfig ID with an eFuse key is a robot the gateway cannot authenticate |
| Unique robot identifier | `APP_ROBOT_UUID` | `MPX-DOG-01`, or `32 tools/mpx_id.py new` | *irrelevant* (dimmed) | only read in Kconfig mode |
| AES-256-GCM key | `APP_CHAT_AES_KEY_HEX` | dev key or one from `tools/mpx_id.py derive` | *irrelevant* (dimmed) | only read in Kconfig mode; the default is public and logs an error |
| Accept `@MPXPROV` provisioning commands on UART0 | `APP_PROVISIONING_ENABLE` | **off** | **on** while it is on the bench | irreversible burn commands; a shipped robot must not accept them |
| Seal public key slot | `APP_PROV_SEAL_KEY_ID` | 0 | 0 (1 is reserved for a KMS rotation) | which embedded public key the sealed blob uses |
| Announce the identity to the cloud on first connect | `APP_ENROLL_ENABLE` | *off* (dimmed) | **on** | retries every 30 s until the gateway accepts the envelope |
| Enrolment endpoint path | `APP_ENROLL_PATH` | `/v1/provisioning/enroll` | same | path on the gateway |

Note the dependency: the bottom five are dimmed unless the identity source is
**eFuse**. That is not a bug — a Kconfig build has nothing to provision and
nothing to announce.

### Network

| Menu entry | Symbol | Value |
|---|---|---|
| Wi-Fi AP SSID | `APP_AP_SSID` | `MPX-Dog` unless you need a different one per bench |
| Wi-Fi AP password | `APP_AP_PASSWORD` | empty = open AP (default); set one for a shared lab |
| Cloud chat backend server IP or hostname | `APP_CHAT_SERVER_IP` | `echo`, or your gateway's address |
| Cloud chat backend server port | `APP_CHAT_SERVER_PORT` | `8080` |
| Cloud chat WebSocket path | `APP_CHAT_WS_PATH` | `/v1/chat/ingress` |

### Bench comfort

| Menu entry | Symbol | Note |
|---|---|---|
| Enable periodic IMU data logging | `APP_IMU_LOG_ENABLE` | default on = spam at 200 ms; turn off unless you are debugging the IMU |
| RL housing temperature guard | `APP_RL_GUARD_TEMP_DC` | default `600` = 60 °C. `1500` disables it — a bench workaround, never ship it |
| System monitor | `APP_SYSMON_ENABLE` | default on; CPU/RAM/task load in the log and the PWA |

### Do not touch

- **eFuse custom table** (`CONFIG_EFUSE_CUSTOM_TABLE`, in `sdkconfig.defaults`).
  Always on. The dev and factory images must agree on the fuse map or the same
  physical block means different things to different images.
- **Partition table** (`CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=partitions.csv`).

## 3. The two profiles

You should almost never hand-edit menuconfig. The profiles are files:

| | `sdkconfig.defaults` | `+ sdkconfig.defaults.pilot` |
|---|---|---|
| identity | Kconfig strings | eFuse record + `HMAC_UP` key |
| `@MPXPROV` listener | off | **on** |
| enrolment task | not compiled | on |
| use it for | dev boards with no fuses, CI, unit work | pilot units, bench provisioning |

```bash
# development image (the default)
idf.py build flash monitor

# pilot image — separate build directory, or it fights with sdkconfig
idf.py -B build/pilot \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.pilot" \
       build flash
```

Keep the pilot build in `build/pilot` so switching back and forth does not thrash
`sdkconfig`. Both profiles set `CONFIG_EFUSE_CUSTOM_TABLE`, so the fuse map is
identical either way.

## 4. Flash a board

```bash
# find the port
ls /dev/ttyUSB* /dev/ttyACM*    # Linux: CP210x/CH340 → ttyUSB*, native USB → ttyACM*
ls /dev/tty.usbserial-*         # macOS
# Windows: COM3 and up, in Device Manager

idf.py -p /dev/ttyUSB1 flash    # substitute your port; dev image
idf.py -p /dev/ttyUSB1 monitor  # 115200 8N1, Ctrl-] to quit
```

`flash` writes images and nothing else. It does **not** touch fuses, and it cannot
— provisioning is a runtime action over UART (§6). Flashing a provisioned board
with the pilot image keeps its identity as long as you do not erase the `nvs`
partition: `idf.py erase-flash` **does** erase NVS, which loses the enrolment
marker and the stored seal (the unit re-enrols on next boot, so this is
recoverable — but do not do it on a unit that is `locked` and offline).

## 5. Join it to Wi-Fi

A pilot unit needs STA connectivity to reach the gateway. Two ways:

**PWA (usual):** connect to `MPX-Dog` (open by default), open
`http://192.168.2.1`, go to the Wi-Fi page, scan, enter credentials. Enterprise
networks (eduroam etc.) additionally need the `identity` field.

**API:**

```bash
curl -X POST http://192.168.2.1/v1/wifi/connect \
     -d '{"ssid":"lab-iot","password":"..."}'
curl http://192.168.2.1/v1/wifi/status
```

Credentials land in NVS and survive reflashing. `POST /v1/wifi/forget` clears
them.

## 6. Provision a new board

Only once per board, and only for a board whose fuses are blank.

`--port` is `/dev/ttyUSB1` on Linux, `/dev/tty.usbserial-*` on macOS and `COM5`
on Windows — substitute yours throughout. On Windows the station runs as
`py -3 tools\mpx_provision.py ...` (see §1, Option C).

```bash
# 0. station dependencies — plain Python, anywhere the board is plugged in
python3 -c "import serial, cryptography" || pip install pyserial cryptography

# 1. flash the pilot image (listener on) and stop the monitor — it holds the port
idf.py -B build/pilot -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.pilot" \
       -p /dev/ttyUSB1 flash

# 2. confirm the board is blank and the listener answers
python3 tools/mpx_provision.py status --port /dev/ttyUSB1
#    state          unprovisioned

# 3. burn + seal in one session; prints the label
python3 tools/mpx_provision.py provision --port /dev/ttyUSB1 --out bench/label-MPX-XXXX.json

# 4. RESET the board — power-cycle, or Ctrl-T Ctrl-R in monitor.
#    Until reset the running firmware has no identity (the HMAC peripheral does
#    not see a freshly burned key) and will not enrol.

# 5. watch it enrol
idf.py -p /dev/ttyUSB1 monitor
```

Expected output of step 3:

```
provisioning MPX-1ZV8ZT4W3HPZ kv=1
sealed: id MPX-1ZV8ZT4W3HPZ kv 1 key_block 0
unsealed K_v, fingerprint 67fe53e910bb325f
  pass --out REGISTRATION.json to write it for the cloud
now reset the board (...) so it reads its new identity and enrols
```

Record the ID and `key_fp` on the physical label. That is the whole provenance
chain: the station never keeps `K_v`, and the cloud learns it only from the
envelope.

**Do not run `lock` on a pilot unit.** It permanently write-disables the identity
block. It is for units leaving the building and nothing else.

## 7. After every flash — checklist

- [ ] Boot log has the identity line, and `state` is what you expect:
      `identity: state=efuse id="MPX-…" kv=1 key_fp=67fe53e910bb325f`
      (`state=kconfig` + a warning = you flashed the dev image onto a pilot unit)
- [ ] `key_fp` matches the label on the board
- [ ] No `identity: no eFuse record -- this unit is not provisioned` (if it was)
- [ ] Enrolment: `identity not yet announced; enrolment task started` then
      `enrolled MPX-… kv=1 (generated key)` — once. On later boots it is silent.
- [ ] Wi-Fi up, and `GET /v1/wifi/status` shows an IP
- [ ] Cloud has the row (§8)
- [ ] Chat works: send something from the PWA and confirm a reply

First boot after provisioning may need 30 s: enrolment retries on a timer, so a
robot that boots before its AP is up still enrols once the link returns.

## 8. Verify from the cloud side

```bash
# on the gateway host
psql "$DATABASE_URL" -c \
  "SELECT robot_uuid, left(aes_key_hex,8) || '…' AS key, created_at, last_seen_at
     FROM robots ORDER BY created_at DESC LIMIT 5;"
```

What you should see: the ID that is on the label, and a non-zero `created_at`.
The gateway also logs it:

```
enrolled MPX-… kv=1 key_fp=67fe53e910bb325f app=1.0.0-pilot
```

`key_fp` on both sides is the cheap way to prove "right robot, right key" without
moving the key. If the fingerprint on the label and in the cloud differ, the row
belongs to a different burn — see the conflict row in §9.

## 9. Log messages, decoded

| Message | Means | Do |
|---|---|---|
| `identity: no eFuse record -- this unit is not provisioned` | fuses blank | provision it (§6), or use the dev image |
| `identity: record "MPX-…" but no HMAC_UP key` | half-burned board | re-run `provision`; it burns only the missing half |
| `identity: this board IS provisioned … but the image uses the Kconfig identity` | dev image on a provisioned unit | rebuild with `sdkconfig.defaults.pilot` |
| `identity: this is the PUBLIC development key` | dev key in `APP_CHAT_AES_KEY_HEX` | fine on a bench, never against a real gateway |
| `identity: legacy development ID "MPX-DOG-01"` | Kconfig ID is not 16 chars | harmless; it is zero-padded on the wire |
| `cannot seal the key, enrolment stays pending` | no working `HMAC_UP` block / bad key slot | check `APP_PROV_SEAL_KEY_ID`, re-provision |
| `cloud refused enrolment with status 409` | that ID already holds a different key | the row belongs to another burn; see below |
| `cloud refused enrolment with status 422` | ID not production format, junk envelope, or envelope bound to a different robot | inspect with `mpx_provision.py unseal <blob>` |
| `enrolment POST failed: <err>` | gateway unreachable | check `APP_CHAT_SERVER_IP`/port, Wi-Fi, and that the gateway is up |
| `provisioned MPX-… kv=1 -- reset to load the new identity` | burn succeeded | reset the board |
| `error: no provisioning listener found` (station) | wrong port, monitor holding it, or dev image flashed | close `monitor`, flash the pilot image |

**409 in detail.** Enrolment is first-claim-wins: whoever claims an ID first owns
it. A 409 means the database already has that exact ID with a different key. If
the board was re-provisioned (`--replace`), delete the stale row first:

```bash
psql "$DATABASE_URL" -c "DELETE FROM robots WHERE robot_uuid = 'MPX-…';"
```

Then reset the board and it re-enrols.

## 10. Recovery playbook

| Situation | What to do |
|---|---|
| Power cut mid-provision | `status` shows `partial`. Re-run `provision` — with no `--id` it adopts the ID already burned and burns only the missing half (a wasted key block is the cost). `--id` with a *different* ID is refused by design. |
| Sealed, but the blob was lost | `mpx_provision.py reseal` re-derives from fuses and prints a fresh blob; the cloud row is unchanged because `K_v` is unchanged |
| Board works, cloud row deleted | just reset the board — it re-enrols and re-creates the row |
| Cloud row exists with a different key | `DELETE` the row, reset the board (§9) |
| `lock`ed and the row is lost | the key is still derivable from the fuses, so `reseal` recovers it — but no new identity can ever be burned |
| Board replaced under warranty | new ID, new row; delete the old row so a returned unit cannot come back |
| Reflashed by mistake with the dev image | rebuild with the pilot profile; the fuses are untouched |

## 11. Never do this

- `idf.py erase-flash` on a `locked` pilot unit that has no cloud row.
- `mpx_provision.py lock` on anything that is not leaving the building.
- Ship `sdkconfig.defaults.pilot` (the listener burns fuses) or the dev image.
- Commit `sdkconfig`, `tools/secrets/*.pem`, or a station `label-*.json`.
- Hand-edit `APP_CHAT_AES_KEY_HEX` to a real key and call it production: it is
  compiled into the image, so everyone with the binary has the key.
- Assume `idf.py flash` provisions anything. It never does.

## 12. Reading further

| Topic | Document |
|---|---|
| how the ID and key are generated, byte by byte | [IDENTITY_AND_KEYS.md](./IDENTITY_AND_KEYS.md) |
| the first-connect flow and why design B | [ENROLLMENT.md](./ENROLLMENT.md) |
| design rationale and the original spec | [ROBOT_IDENTITY_AND_KEYS.md](./ROBOT_IDENTITY_AND_KEYS.md) |
| milestone status, deviations | [FIRMWARE_IMPLEMENTATION_PLAN.md](./FIRMWARE_IMPLEMENTATION_PLAN.md) |
| container, build, flash, debug recipes | [../DEV_CONTAINER.md](../DEV_CONTAINER.md) |
| board bring-up | [NEW_BOARD_BRINGUP.md](./NEW_BOARD_BRINGUP.md) |

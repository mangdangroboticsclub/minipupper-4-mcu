# Robot identity and key generation

How a robot gets its ID, where its secret lives, how the AES key is derived, and
where that key goes. Reference for the mechanics; the reasoning behind the
choices is in [ROBOT_IDENTITY_AND_KEYS.md](./ROBOT_IDENTITY_AND_KEYS.md), and the
first-connect flow is in [ENROLLMENT.md](./ENROLLMENT.md).

The contract in one paragraph: a robot is identified by a 16-byte string
`MPX-XXXXXXXXXXXX`, and its AES-256 key is derived on the device from a 32-byte
secret burned into eFuse that no software can read and no server ever sees. The
cloud learns the key only from a sealed envelope that only it can open.

```
factory                          the dog                        cloud
tools/mpx_provision.py           eFuse: S (HMAC_UP, unreadable)  robots table
  ID + S generated                 K_v = HMAC_UP(S, label‖ID‖kv)  opens envelope
  burns ID + S, prints label       seals K_v to RSA-3072 pubkey   stores K_v wrapped
```

## 1. The ID

`MPX-` followed by 11 symbols of Crockford base32 and one check symbol — 16
characters total, which is exactly the 16 bytes the wire frame has carried all
along (`[16B uuid][12B IV][ciphertext][16B tag]`), so nothing on the wire changed.

```
M P X - A J Z 7 J 8 Y 0 Y T D S
└─┬─┘ └────────┬────────┘ └─┬─┘
prefix     11 body symbols   check
```

- **Alphabet** `0123456789ABCDEFGHJKMNPQRSTVWXYZ` — Crockford: no `I`, `L`, `O`,
  `U`. They are *invalid*, not aliased, because this runs on wire bytes; aliasing
  is a job for typed input, not for a parser.
- **Check symbol** is Luhn mod 32 over the 11 body symbols, right to left, with
  the doubled value folded as `a / 32 + a % 32`. The fold matters: plain Luhn
  mod 32 misses a substitution of `delta 16` at the doubling position. With the
  fold, **every single-symbol substitution is detectable** —
  `python3 tools/mpx_id.py selftest` brute-forces 20,000 generated IDs × every
  position × every wrong symbol to prove it.
- **Dev IDs** are `DEV-` with the same shape, always with
  `MPX_EFUSE_FLAG_DEV` set and never accepted by the cloud.
- **Legacy IDs** (`MPX-DOG-01`, `MPX-DEV-Robot-UUID`) still parse through
  `mpx_id_legacy_valid()` and `mpx_id_to_frame()` zero-pads them into 16 bytes,
  so a bench board that predates this work keeps booting. New IDs never look like
  that.

Reference implementation: `tools/mpx_id.py` (Python) and
`components/mpx_identity/mpx_id.c` (device). They are kept honest by
`tools/vectors/identity.json`, which both sides are tested against.

## 2. Where the secret lives

Two 32-byte things, both in eFuse on the ESP32-S3:

| | eFuse | Purpose | Read | Write |
|---|---|---|---|---|
| `S` | one `BLOCK_KEY0..5` | HMAC-SHA256 key for derivation | `RD_DIS` | `WR_DIS` |
| ID, format, flags, `kv_base` | `BLOCK_USR_DATA` (`BLOCK3`) | the record | readable | `WR_DIS` |

`S` is used **only** through the HMAC peripheral (`esp_hmac_calculate()`): the
key is fed to the hardware, the output is a MAC, and the key itself never enters
the address space. That is why it is a `HMAC_UP` key rather than something we
read and hash in software — there is no code path, ours or an attacker's, that
can exfiltrate it after `RD_DIS` is burned.

The 20-byte record at `BLOCK_USR_DATA` bits 0..159
(`components/mpx_identity/mpx_efuse_record.h`):

```
offset  size  field
0       16    robot ID, zero-padded (legacy short IDs allowed)
16      1     format, 0x01
17      1     flags: 0x01 DEV, 0x02 REPLACEMENT; unknown bits rejected
18      2     kv_base, big endian; 0 rejected
```

Field names in `main/esp_efuse_custom_table.csv`:
`MPX_ROBOT_ID`, `MPX_ID_FORMAT`, `MPX_ID_FLAGS`, `MPX_KV_BASE`. They sit at bits
0..159; `USER_DATA.MAC_CUSTOM` at bits 200..247 is the neighbouring built-in
field and is left alone.

## 3. Deriving the key

```
K_v = HMAC-SHA256(S, "mpx/v1/robot-key" ‖ 0x00 ‖ ID[16] ‖ u32be(kv))
```

The message is 37 bytes, and every part is load-bearing:

| bytes | content | why |
|---|---|---|
| 0..15 | `mpx/v1/robot-key` | domain separation: `S` may later key other things |
| 16 | `0x00` | terminates the label, so no two (label, ID) pairs can collide |
| 17..32 | ID as 16 frame bytes | binds the key to the ID; a swapped record yields a different key |
| 33..36 | `kv`, big endian | lets a key be rotated without touching `S` |

`mpx_derive_key()` takes an `hmac_sha256` **callback** rather than calling the
ESP-IDF API directly (`components/mpx_identity/mpx_derive.h`). On the device the
callback is `mpx_efuse_hmac_sha256()` (which ignores its key arguments — the
peripheral owns the key); in the host tests it is `mbedtls_md_hmac`. That is what
makes the derivation testable without a board, and it is why a firmware change to
the label or layout shows up as a failing host test instead of a fleet of robots
that cannot talk to the cloud.

Frozen vectors live in `tools/vectors/identity.json` and are compiled into the
tests. For `MPX-AJZ7J8Y0YTDS`, `kv=1`, `S = 000102…1e1f`:

```
msg = 6d70782f76312f726f626f742d6b6579004d50582d414a5a374a3859305954445300000001
K_1 = 27c6f7d89725761ebff5ad8f0a29ce353ed5b2ea282e6ce0abb2d04af5daf5ae
```

**Key version.** `kv` is per-robot and lives in the eFuse record (`kv_base`), and
a successful rotation increments it in NVS (`mpx_id` namespace). The frame does
*not* yet carry it — see §11.

## 4. Fingerprint

`key_fp = first 8 bytes of HMAC-SHA256(K_v, "mpx/v1/key-fp")`, hex, 16 chars.

A short, non-secret identifier that two sides can compare without moving the key:
the station prints it on the label, the cloud logs it at enrolment, and the
console shows it. Enough to tell "right robot, wrong key" from "wrong robot".
16 hex chars is also what fits the frame's existing MAC field, which §11 uses.

## 5. Which source, and what boot does

`CONFIG_APP_IDENTITY_SOURCE` picks one (`main/Kconfig.projbuild:204`):

| source | ID from | key from | used for |
|---|---|---|---|
| `KCONFIG` (default) | `APP_ROBOT_UUID` | `APP_CHAT_AES_KEY_HEX`, or derived from `APP_IDENTITY_SECRET_HEX` | development, CI, bench boards with no fuses |
| `EFUSE` | `BLOCK_USR_DATA` record | `HMAC_UP` block + record | anything that ships |

`mpx_identity_init()` runs in `app_main()` right after `init_nvs()`
(`main/main.cc:82`) — before WiFi, before the chat socket, before anything can
ask for a key. It:

1. reads the record and finds the `HMAC_UP` block;
2. derives `K_v` and feeds it into the same code path the Kconfig source uses;
3. computes the fingerprint;
4. never fails the boot on a bad eFuse read: a partially provisioned board comes
   up, logs `unprovisioned`, and enrolment stays pending.

There is no fallback from `EFUSE` to `KCONFIG`. A mis-provisioned board fails
loudly rather than silently talking to the cloud with a development key.

## 6. API and call sites

Everything that needs an ID or a key goes through
`components/mpx_identity/include/mpx_identity.h`:

| call | returns |
|---|---|
| `mpx_identity_init()` | `ESP_OK`, or an error if the selected source is unusable |
| `mpx_identity_state()` | `DISABLED`, `UNPROVISIONED`, `READY` |
| `mpx_identity_robot_id()` | NUL-terminated string, never `NULL` |
| `mpx_identity_robot_key(uint8_t out[32])` | `false` when there is no key yet |
| `mpx_identity_frame_id(uint8_t out[16])` | the 16 wire bytes, zero-padded |
| `mpx_identity_key_version()` | `uint32_t` |
| `mpx_identity_key_fp()` | 16-char fingerprint |
| `mpx_identity_sec_flags()` | eFuse security state, for the envelope |

Consumers, all switched over — no module keeps its own copy of the ID any more:

| where | uses |
|---|---|
| `main/network/chat_ws.cc:593,601,631` | frame header ID (memcpy exactly 16 bytes, no `strncpy`, no NUL) |
| `main/wasm/wasm_decrypt.cc:92,101` | skill-blob decryption key |
| `main/agent/agent_config.cc:211,215` | agent identity |
| `main/network/http_server.cc:2565,2583,2694` | `/api/...` identity fields |
| `main/pi/pi_link.cc:187`, `main/pi/pi_protocol.h:184` | Pi link handshake |
| `main/voice/mpx_voice_ota.c:255` | ⚠️ still MAC-derived `Device-Id` — see §11 |
| `components/mpx_identity/mpx_enroll.c` | enrolment |

## 7. Sealing and first-connect enrolment

The device never sends `K_v` in the clear, and the cloud never sends `S`
anywhere. On first connect the device seals the key to a station public key
compiled into the firmware (`components/mpx_identity/certs/prov_seal_pub.pem`,
RSA-3072) and posts it:

```
POST /v1/provisioning/enroll
{"robot_uuid":"MPX-…","kv":1,"seal_key_id":0,"blob":"<hex>"}
```

Envelope (392 bytes, `mpx_seal.h`):

```
"MPXS" | format 1 | seal_key_id | 2 reserved | RSA-3072-OAEP-SHA256(plaintext) = 384 B
```

Plaintext (149 bytes) — the inner `robot_id` must equal the outer one:

```
"MPXK"|fmt|kv u32be|robot_id 16|nonce 16|key 32|mac 6|sec_flags u16|secure_ver u32|app_version 32|sha256(DS pub) 32
```

`keywrap.py` opens it: `FileCipher` on a development box, a KMS key in
production (the factory refuses the file wrapper when `MPX_ENV=prod`). Then
`provisioning.py` verifies, in order — envelope layout, plaintext MAC, the inner
ID against the outer ID, the key version — and only then writes
`robots (robot_uuid, aes_key_hex)`. Repeat enrol with the same key is `200`;
a *different* key for an existing ID is `409` and leaves the row untouched.

Interception is worthless: the envelope is public-key encrypted, and replaying it
gets you the same key the robot already has. Nothing on this path is a shared
secret.

## 8. Cloud storage

One row per robot in `robots`, and the gateway keeps an in-process cache keyed by
the 16 wire bytes (`core_gateway/crypto.py`, `_KEY_STORE`) so the frame path does
not hit PostgreSQL per packet. `voice_gateway/vg/auth.py` reads the same table
with a 300 s TTL.

`aes_key_hex` is the plain key, wrapped at rest by `keywrap.py`. The KMS wrapper
keeps the wrapping key inside the HSM; the envelope's private half is the same
key, so a database dump alone does not yield working keys.

## 9. Provisioning a board

`idf.py flash` **never provisions anything.** It writes images — bootloader,
partition table, app, models, filesystem — and stops there. A fused identity is
not an image: it is burned at runtime by the station, over UART, by the
`@MPXPROV` listener that only the pilot profile compiles in. So the two tools do
different jobs and you need both:

| | does | runs |
|---|---|---|
| `idf.py flash` | writes the firmware that *knows how* to be provisioned | host, over USB |
| `tools/mpx_provision.py` | generates ID + `S`, burns fuses, seals the key | host, over UART0 |
| `mpx_enroll.c` | introduces the robot to the cloud | device, on first WiFi connect — automatic |

```
# build the pilot profile (listener on, eFuse source)
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.pilot" -B build/pilot flash

# station generates ID + S, burns, prints the label
python3 tools/mpx_provision.py provision --port /dev/ttyUSB1 --out label.json

# optional: make it permanent
python3 tools/mpx_provision.py lock --port /dev/ttyUSB1 --confirm
```

The station talks `@MPXPROV {"cmd":…}` JSON lines to UART0 at 115200. The
decision table (`mpx_prov_sm.c`) is a pure function: nothing burned → burn both;
key only → burn record; record only → burn key; both and same ID → seal; both and
different ID → refuse. `lock` is opt-in, because a burned `S` cannot be replaced.

**Reset after provisioning.** The burn happens in a firmware that read its
identity at boot and found none, and the HMAC peripheral does not expose a
newly burned key until a reset. Power-cycle (or `Ctrl-T Ctrl-R` in `monitor`)
or the unit looks provisioned to the station and unprovisioned to itself, and
never enrols.

The pilot profile is also the only image that currently reads the eFuse identity.
Flashing the default profile onto a provisioned board would run it as whichever
development ID `APP_ROBOT_UUID` names — two boards sharing `MPX-DOG-01` look fine
one at a time — so the Kconfig path detects a burned record and logs an error at
boot (`components/mpx_identity/mpx_identity.cc`, `load_kconfig_identity()`).

## 10. Verifying

| what | how |
|---|---|
| ID format, check symbol, every substitution | `python3 tools/mpx_id.py selftest` |
| vectors match the code | `idf.py host-test` → `vectors_up_to_date`, `seal_vectors_up_to_date` |
| derivation and layout | `idf.py host-test` (45 C cases, 6 ctest targets) |
| firmware still builds with the source switched | CI job `firmware` |
| enrolment end to end, real PostgreSQL | `core_gateway/tests/test_enroll_e2e.py` (17 checks) |
| a live board | `mpx_provision.py status` → `sealed`, matching `key_fp` |

## 11. What this does not do yet

- **No key version on the wire.** The frame is still `[id][iv][ct][tag]` with no
  version in the AAD, so the cloud cannot tell which `kv` a frame used and a
  rotation needs both sides updated together. The API already exposes
  `mpx_identity_key_version()` so this is a wire change, not a re-plumbing.
- **No per-purpose subkeys.** One `K_v` covers chat, skills, WASM and voice. A
  nonce collision in any one of them endangers the others; HKDF over the same
  secret fixes that without touching eFuse.
- **Voice is not on the robot ID.** `main/voice/mpx_voice_ota.c:255` sends the
  MAC string as `Device-Id` (and a UUID as `Client-Id`), so the voice gateway
  identifies the robot differently from the core gateway.
- **No rotation or revocation.** `kv` exists and the eFuse record has a
  `REPLACEMENT` flag, but there is no path that re-keys an enrolled robot, and no
  way to lock out one that is lost. Today a lost robot means a manual row delete.
- **No secure boot, no flash encryption.** The record and `S` are protected; the
  firmware that reads them is not. Pilot units therefore do not take the
  irreversible path on the first burn.
- **The dev key is still in the repo.** `sdkconfig.defaults` and
  `docker-compose.yml` carry `MPX-DEV-…` values. They are safe only because the
  cloud rejects `DEV-` IDs and production builds select the eFuse source — which
  is a convention, not an enforced gate.

# Robot Identity & Key Generation — Implementation Design

**Status:** proposed design, not implemented yet. No code was changed to write it.
**Scope:** how each robot gets its **robot ID** (today `CONFIG_APP_ROBOT_UUID`)
and its **32-byte AES-256-GCM robot key** (today `CONFIG_APP_CHAT_AES_KEY_HEX`):
who generates them, where they live on the robot and in the cloud, how the two
sides get the same key without anyone in between seeing it, and how the
firmware and the cloud code have to change.

**Repos involved:**

| Short name | Location | Role |
|---|---|---|
| firmware | this repo (`mpx-dog`) | ESP32-S3 firmware |
| `cloud/` | `mpx-server` (runs on the GCE VM) | `core_gateway`, `voice_gateway`, `mpx-wasm-deploy.py` |
| `payments/` | `mpx-payments` | owns the SQL migrations (`migrations/001`–`008`) |
| `admin/` | `mpx-admin` | operator UI |

**Background notes:** this design builds on the analysis in `KEY_MANAGEMENT.md`,
`SKILL_PROTECTION.md` and `IMPLEMENTATION.md` (findings F1–F12, S1–S12, work
packages P0–P7). It replaces the generation/provisioning parts of
`KEY_MANAGEMENT.md` §7.3–§7.4 and of `IMPLEMENTATION.md` P3/P4. In a few places
it reaches different conclusions from those notes. §4 lists them.

**Product context (shapes several decisions):**

- **Robot IDs are permanent.** Orders (`orders.robot_uuid`) and skill
  assignments (`robot_skills.robot_uuid`) hang off the robot ID, so an ID is
  never deleted, never reused and never renumbered, including when a robot's
  main board is replaced or its key is rotated (D15).
- **No robots have shipped to customers yet.** There is no field fleet to
  migrate, and anything that is permanent once burned (§12.1) can still be
  chosen freely.
- **Pilot units go out before the factory exists.** Research teams and early
  developers get robots before launch. Those robots are the first ones outside
  the office, so the robot ↔ cloud contract (identity, wire protocol, sealed
  blob, provisioning protocol) is frozen before the first pilot unit ships. The
  factory line and the full production cloud come after (D18).
- **The cloud runs on a single GCE VM in Hong Kong (`asia-east2`) today**, and
  the plan is to give each product its own stack later (load balancer, GKE,
  …). The identity registry and key handling must work on the VM now and move
  to that layout without re-provisioning any robot (D16).

---

## 0. Summary

1. **Robot ID:** `MPX-` plus 12 Crockford-Base32 characters (11 random + 1 check
   character), e.g. `MPX-AJZ7J8Y0YTDS`. It is exactly 16 ASCII bytes, so it fits the
   existing 16-byte frame header **with no protocol change**. The cloud
   generates it, and a database `UNIQUE` constraint guarantees uniqueness. At the
   factory it is burned into the robot's user-data eFuse block (`BLOCK_USR_DATA`),
   so it survives reflashing and NVS erases.
2. **Root secret:** a 256-bit secret `S` is generated **on the robot** by the
   hardware true random number generator (with an entropy source enabled), then
   burned into an eFuse key block with purpose **`HMAC_UP`**. Hardware
   read-protects that purpose, so no software (including our own firmware) and
   no tool (`espefuse`, JTAG) can read `S` back. The only thing anyone can do with it is ask the HMAC
   peripheral for `HMAC-SHA256(S, message)`.
3. **Robot key:** `K = HMAC_hw(S, "mpx/v1/robot-key" ‖ 0x00 ‖ robot_id ‖ key_version)`.
   `K` is a drop-in replacement for today's `CONFIG_APP_CHAT_AES_KEY_HEX`: the
   chat frames, MPXE skill wrapping and robot token all keep working unchanged.
4. **Getting `K` to the cloud:** the robot encrypts `K` to a **Cloud KMS RSA-3072
   OAEP public key** compiled into the firmware and prints the resulting
   *sealed blob*. The factory station only relays that blob, so it never sees
   `S` or `K`. The cloud decrypts it with KMS, then re-wraps `K` under a KMS
   symmetric key-encryption key (KEK) for storage. The database never holds a
   plaintext key.
5. **Lifecycle:** `reserved → sealed → active → revoked`. The station reserves
   an ID and uploads the sealed blob. The robot only becomes `active` after its
   **first successful authenticated connection** to the gateway.
6. **IDs are permanent (D15).** Robot rows are never deleted (a database trigger
   enforces it, and the `orders` foreign key changes from `ON DELETE CASCADE`
   to `RESTRICT`). A replacement main board gets the **same** robot ID burned
   into it plus a fresh secret and a higher key version, so orders and skill
   assignments carry over untouched.
7. **Developers** keep a Kconfig path, but `tools/mpx_dev_identity.py` gives each
   developer a unique `DEV-…` ID and a random key. The production gateway
   rejects `DEV-` IDs and the publicly known `…0001` key.
8. **Cloud layout (D16):** one global identity registry, with the product kept
   in a column rather than in the ID, and one KMS key-encryption key per
   product. It starts inside `core_gateway` on the VM and can be lifted into
   its own service on GKE later without touching any robot.
9. **Pilot phase (D18):** before the factory and full production cloud exist,
   pilot units for research teams and developers are provisioned by hand on a
   bench station, using the same tool, firmware and API as the factory. They get
   permanent `MPX-` IDs in the production registry and speak the **final** wire
   protocol (v2, §6.9). They skip only Secure Boot, Flash Encryption and
   `lock`, so they run at `assurance = efuse` and stay flashable for developers.
10. **Phasing:** the identity and wire contract is frozen before the first pilot
   unit (§12.1, part 1). Secure Boot, Flash Encryption, the factory and the
   scaled-out cloud are settled before the first Kickstarter unit (§12.1,
   part 2). GKE and mTLS can follow later.
11. **OTA (D20):** today the firmware can't update itself (one app slot, no
   rollback). Before the first pilot unit it needs A/B app slots, automatic
   rollback after a failed health check, signature checks on every image
   (including pilot units without Secure Boot), and a product guard between
   MPX and MP4. Two 7 MB slots don't fit in 16 MB flash, so the flash size or
   the image size must be decided first.

---

## 1. Current state (verified against the code)

### 1.1 Firmware (this repo)

| What | Where | Notes |
|---|---|---|
| Robot ID | `main/Kconfig.projbuild:174-183`, `sdkconfig:616` | `"MPX-DOG-01"`, compile-time string |
| Robot key | `main/Kconfig.projbuild:185-193`, `sdkconfig:617` | `…0001`, compile-time hex string |
| eFuse key option | `main/Kconfig.projbuild:214-232` | `APP_MPX_EFUSE_BLK`. The help text says "not yet implemented" and also claims mbedTLS can use an eFuse key directly. **That is not possible on ESP32-S3** (§3.1). |
| Chat key load | `main/network/chat_ws.cc:588-601` | `load_aes_key()` parses Kconfig hex |
| Frame ID fill | `main/network/chat_ws.cc:49`, `:626-629` | `strncpy(…, ROBOT_UUID, 16)`, silently truncates |
| Frame header | `main/network/chat_ws.h:19-23` | `uint8_t robot_uuid[16]` |
| MPXE key | `main/wasm/wasm_decrypt.cc:46-69`, `:116-118` | refuses an all-zero key only under `NDEBUG`; `…0001` passes |
| Robot token | `main/agent/agent_config.cc:216-236` | `uuid + "." + HMAC(key, "mpx-ai-v1:"+uuid)[:16 bytes hex]` |
| Other ID users | `main/network/http_server.cc:2504`, `:2614`; `main/pi/pi_link.cc:65-66`, `:189` (`pi_protocol.h:184` has `char uuid[24]`) | |
| NVS init | `main/main.cc:60-68` | **erases the whole default `nvs` partition** on `NO_FREE_PAGES` / `NEW_VERSION_FOUND`, so identity must not live there |
| Security config | `sdkconfig:491-492`, `:2226`, `:953` | Secure Boot off, Flash Encryption off, NVS encryption off, no custom eFuse table |
| Git | `.gitignore:35` lists `sdkconfig`, **but `sdkconfig` is still tracked** (`git ls-files`) | the ignore rule has no effect until `git rm --cached sdkconfig` |
| Partitions | `partitions.csv:29-34` | 16 MB fully allocated. No room for a new partition without moving `storage`. |
| OTA | `partitions.csv`, `sdkconfig:422` | **No firmware OTA:** one `factory` app slot, no `ota_0`/`ota_1`/`otadata`, rollback disabled. `main/voice/mpx_voice_ota.c` is Xiaozhi config discovery, not an updater (D20). |

### 1.2 Cloud

| What | Where | Notes |
|---|---|---|
| Registration | `cloud/core_gateway/robots.py:38-74` | `POST /v1/robots` accepts **caller-supplied** `robot_uuid` + `aes_key_hex` with no auth |
| Key load | `cloud/core_gateway/crypto.py:76-105`, `main.py:116` | plaintext `aes_key_hex`, loaded once at startup |
| Dev key | `cloud/core_gateway/crypto.py:43-60`; `cloud/voice_gateway/vg/auth.py:43-59` | `MPX_DEV_AES_KEY_HEX` / `MPX_DEV_ROBOT_UUID` |
| Defaults | `cloud/docker-compose.yml:10-13`, `:88-89`; `cloud/.env.example:8-10` | the same `…0001` / `MPX-DOG-01` as the firmware |
| Deploy | `cloud/core_gateway/main.py:862-867` | `SELECT aes_key_hex` |
| Ingress auth | `cloud/core_gateway/main.py:1050-1072` | the first frame must decrypt; the ID comes from `rstrip("\x00")` on the header |
| Token verify | `cloud/voice_gateway/vg/auth.py:119-129` | 300 s TTL key cache |
| CLI | `cloud/mpx-wasm-deploy.py:69` | string-interpolated SQL |
| Schema | `payments/migrations/001_checkout.sql:14` | `orders.robot_uuid VARCHAR(64) REFERENCES robots(robot_uuid) ON DELETE CASCADE`: **deleting a robot row silently deletes its orders.** `robot_skills` is used by `robots.py:166-172`, `:247-249`, but neither its DDL nor the `robots` DDL is in any repo. |
| Deployment | one GCE VM in `asia-east2`, `docker-compose.yml` | both gateways share one Postgres database (named `mpx_marketplace_prod`) and one VM service account |

### 1.3 What this means

There is effectively one key per *build*, not per robot. It is public (it's in
git, in `docker-compose.yml`, and in every `.bin`), and nothing generates keys:
a human types a hex string into Kconfig and into `POST /v1/robots`.

---

## 2. Requirements

| # | Requirement |
|---|---|
| R1 | Every robot has a unique ID and a unique, uniformly random 256-bit key. |
| R2 | The ID fits the existing 16-byte frame header and every current string consumer (URLs, AAD strings, token prefix, `VARCHAR(64)`, Pi link `char[24]`), so phase 1 needs **no wire-protocol change**. |
| R3 | The key is never stored in plaintext: not in git, not in the firmware image, not in flash, not in the database, not on the factory PC. |
| R4 | One firmware image for every unit, with no per-unit builds. |
| R5 | Identity survives reflashing, OTA, `nvs_flash_erase()` and LittleFS reformat. |
| R6 | Provisioning is idempotent and resumable after a power loss or network failure at any step. |
| R7 | The cloud is the authority on which robots exist. Nothing can register itself. |
| R8 | Developers can still build and run without burning eFuses. |
| R9 | Robot IDs are permanent: never deleted, reused or renumbered. Orders and skill assignments survive key rotation, revocation and main-board replacement. |
| R10 | Rotation is possible without reflashing (later phase). |
| R11 | The registry and key storage move from the single VM to per-product stacks (load balancer, GKE) with no re-provisioning and no change to what is burned in robots. |
| R12 | Every permanent-once-burned choice is settled before the first unit leaves the office (pilot or Kickstarter, whichever needs it first). |
| R13 | Pilot units shipped before launch keep their IDs, orders and skills forever, and need no re-provisioning or recall when the factory and production cloud arrive. Every later change reaches them by OTA or cloud change only. |
| R14 | Every MPX and MP4 unit can be updated over the air for its whole life: signed images only, automatic rollback of a bad image, no cross-product installs, and no update while the robot is moving (D20). |

---

## 3. Research findings that drive the design

### 3.1 ESP32-S3 has no AES key that software cannot read

- The eFuse key purposes are `USER`, `XTS_AES_128/256_KEY*` (flash/PSRAM
  encryption only), `HMAC_DOWN_*`, `HMAC_UP`, and `SECURE_BOOT_DIGEST0-2`. There is no
  Key Manager or hardware unique key on S3. The AES peripheral **cannot** take
  its key from eFuse for general use. So the `APP_MPX_EFUSE_BLK` help text
  ("mbedTLS will use it directly via the hardware crypto engine's key
  register") describes hardware that does not exist on this chip.
- `USER` keys are readable by software, and by `espefuse` while download mode
  is open. A `USER` key with `RD_DIS` set reads back as zeros and has no
  hardware consumer, so it is **unusable**. The notes' "burn `USER`, set
  `RD_DIS`, read once at boot" (`KEY_MANAGEMENT.md` §7.4 step 1) contradicts itself.
- **`HMAC_UP` is the only purpose that is both unreadable and usable from the
  app.** `esp_efuse_write_key(…, ESP_EFUSE_KEY_PURPOSE_HMAC_UP, …)`
  automatically sets read and write protection, and `esp_hmac_calculate(HMAC_KEYn, msg,
  len, out)` returns HMAC-SHA256 computed by the peripheral. `HMAC_KEY0..5`
  map to `BLOCK_KEY0..5` (physical blocks 4–9).
  ([HMAC — ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/hmac.html),
  [`esp_efuse_write_key` source](https://github.com/espressif/esp-idf/blob/master/components/efuse/src/efuse_controller/keys/with_key_purposes/esp_efuse_api_key.c))

**Consequence:** we cannot make the AES key itself non-extractable from running
code. We *can* make the long-term secret non-extractable, and derive the AES key
from it at boot. Then the key exists only in RAM, never in flash, and is bound
to that particular chip.

### 3.2 Secure Boot closes the window for read-protecting keys

> "After Secure Boot is enabled, further read-protection of eFuse keys is not
> possible … If you want to keep the ability to read-protect an eFuse later in
> the application … enable `CONFIG_SECURE_BOOT_V2_ALLOW_EFUSE_RD_DIS` …
> make sure that you burn the eFuse `ESP_EFUSE_WR_DIS_RD_DIS` … once all the
> read-protected eFuse keys have been programmed."
> ([Secure Boot v2 — ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/secure-boot-v2.html))

The app burns the `HMAC_UP` key after the bootloader has already enabled Secure
Boot, so production builds **must** set
`CONFIG_SECURE_BOOT_V2_ALLOW_EFUSE_RD_DIS=y`, and provisioning **must** finish
by burning `WR_DIS_RD_DIS`. If this is missed, the burn fails on secured units.

### 3.3 eFuse blocks 1–10 can be written only once

ESP32-S3 automatically applies Reed-Solomon coding to `EFUSE_BLK1`–`BLK10`:
"if there is already pre-written data in the eFuse block … it is not possible
to write anything extra (even if the required bits are empty)". So the
`BLOCK_USR_DATA` record must be written **in a single batch**, and nothing in it
can be updated later. Any mutable state (key version, activation flag) has to
live in flash. Custom fields are declared in `main/esp_efuse_custom_table.csv`
and generated with `idf.py efuse-custom-table`, which rejects fields that
overlap built-in ones such as `CUSTOM_MAC`.
([eFuse Manager — ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/efuse.html))

Also: `BLOCK_KEY5` (block 9) cannot hold XTS-AES keys because of a hardware
quirk. That doesn't affect `HMAC_UP`, but it matters for the eFuse budget (§6.2).

### 3.4 The hardware RNG is only truly random while an entropy source is on

On ESP32-S3, `esp_fill_random()` produces true random numbers only while
Wi-Fi/BT is running, or between `bootloader_random_enable()` and
`bootloader_random_disable()`. Otherwise the output "should be considered as
pseudo-random only". `bootloader_random_*` must not overlap with ADC, Wi-Fi or
BT use.
([Random Number Generation — ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/random.html))

**Consequence:** the provisioning code must generate `S` inside an explicit
`bootloader_random_enable()` window, before Wi-Fi starts, or with Wi-Fi
confirmed up. Today `crypto::random_bytes()` (`main/network/crypto.cc:95-99`) is
fine for IVs after Wi-Fi is up, but it is **not** safe for generating a key at
an arbitrary moment.

### 3.5 Cloud KMS supports both halves of the export

- `ASYMMETRIC_DECRYPT` keys with `RSA_DECRYPT_OAEP_3072_SHA256` (the
  recommended algorithm), using MGF1 with SHA-256. KMS never releases the private key.
  `AsymmetricDecrypt` takes no OAEP label, so all binding data must go
  inside the plaintext.
  ([Key purposes and algorithms](https://cloud.google.com/kms/docs/algorithms))
- Symmetric `ENCRYPT_DECRYPT` keys accept `additionalAuthenticatedData`. That
  lets us bind each wrapped key to its `(robot_id, key_version)`.
- `GenerateRandomBytes` exists (HSM protection level only, 8–1024 bytes). We only
  need it for cloud-generated dev keys, and the OS CSPRNG (`secrets`) is
  adequate there.
- Envelope encryption guidance: one data-encryption key (DEK) per owner,
  wrapped by a KEK, stored next to the data
  ([Envelope encryption](https://cloud.google.com/kms/docs/envelope-encryption)).
  Here the DEK is the robot key `K` and the owner is the robot.
- **Region:** Cloud HSM keys are available in `asia-east2` (Hong Kong), where the
  VM runs. Key material "is confined to the selected region for the entire
  lifecycle of the key" and KMS resources "can't be moved to a different location
  after creation."
  ([Cloud KMS locations](https://cloud.google.com/kms/docs/locations))
  Moving region later therefore means *re-wrapping* stored keys under a new
  KEK, a server-side batch job, and adding a new sealing key through OTA
  (D16). No robot needs to be touched.

### 3.6 UUID standards

RFC 9562 UUIDs are 128 bits, and their canonical text form is 36 characters. For
high-impact uses the RFC says to back uniqueness with a database constraint
rather than rely on probability alone (§6.7). UUIDv7 is recommended for
time-ordered database keys, but it embeds a millisecond timestamp, which here
would leak manufacturing time.
([RFC 9562](https://www.rfc-editor.org/rfc/rfc9562))

---

## 4. Where this design differs from the earlier notes

| Topic | Earlier notes | This design | Why |
|---|---|---|---|
| eFuse purpose for the key | `USER` + `RD_DIS` | `HMAC_UP` + derivation | §3.1: `USER`+`RD_DIS` is unusable. `USER` without `RD_DIS` is readable. |
| Who generates the key | cloud ("Model A"); device-generated rejected as "usually weaker" | **device** generates, then **seals to KMS** | Sealing removes the weakness the notes cited (the station transporting the key). See D5. |
| DEK source | `KMS.GenerateRandom` | device TRNG for production, `secrets` for dev | With device generation the cloud never creates production keys. |
| Rotation by burning the next eFuse slot | dual-slot burn | derive `K_v` from the same `S` with a new `key_version` | Burning a new slot gives no benefit: anything that can read `S` from silicon can read the next slot too (D13). |
| Identity storage | NVS fallback when eFuse is exhausted | `BLOCK_USR_DATA` (ID) + `HMAC_UP` block (secret); NVS only for `key_version` | `main.cc:60-68` can erase NVS; R5. |
| `APP_MPX_EFUSE_BLK` | keep, point at block | **remove**, find the block by purpose | Fewer ways to misconfigure, and no per-unit Kconfig. |

Everything else in the notes (auth on `/v1/robots`, nonce-reuse fix,
key-cache refresh, Secure Boot / Flash Encryption, review gate) still stands and
is not repeated here.

---

## 5. Design decisions

### D1 — Robot ID format: `MPX-` + 11 random Base32 symbols + 1 check symbol

| Option | Fits 16-byte header as-is | Human-friendly | Notes |
|---|---|---|---|
| Canonical RFC 9562 text (36 chars) | ❌ | ❌ | would need a header change |
| Binary 16-byte UUIDv4 in header | ✅ (size) | ❌ | breaks every string consumer: `rstrip("\x00")` decode (`main.py:1066`), URLs, AAD strings, token prefix, `pi_protocol.h:184`. Effectively a protocol change. |
| UUIDv7 | as above | ❌ | also leaks build time |
| MAC-derived | ✅ | ~ | predictable, so the fleet can be enumerated, and it duplicates the voice `Device-Id` (`mpx_voice_ota.c:102-110`) |
| Sequential serial | ✅ | ✅ | leaks fleet size, enumerable |
| **`MPX-` + 12 Crockford Base32** | ✅ exactly 16 | ✅ | **chosen** |

Details:

- **Alphabet:** Crockford Base32 `0123456789ABCDEFGHJKMNPQRSTVWXYZ`. It has no `I L O U`,
  is case-insensitive on input, and is canonical upper-case. It is URL-, filename- and
  JSON-safe, and it contains no `.`, so it can't confuse the
  `rpartition(".")` in `vg/auth.py:122`.
- **Entropy:** 11 symbols = 55 random bits. The chance that a random draw
  collides with any existing ID is `n²/2⁵⁶`: about 1.4×10⁻⁹ at 10k robots and
  1.4×10⁻⁵ at 1M. The DB `UNIQUE` constraint plus regenerate-on-conflict makes
  the actual collision rate zero. A blind guess hits a real robot with
  probability ≈ n/2⁵⁵, about 3×10⁻¹¹ per guess at 1M robots.
- **Check symbol:** Luhn mod 32 over the 11 symbols. It catches every
  single-symbol typo and most adjacent swaps when support staff type an ID off a
  label. The gateway can also reject garbage IDs before a DB lookup.
- **Prefix:** `MPX-` makes IDs recognisable in logs and on labels. Dev IDs use
  `DEV-`, so the prod gateway can reject them by prefix alone. `MPX-` names
  the **company namespace, not a product**. Because IDs are permanent and
  burned into silicon, product, SKU and hardware revision live in DB columns
  (`robots.product`, `robots.hw_rev`). Products get renamed and split; a
  burned ID can't follow (D16).
- **Legacy IDs never collide with the new format:** `MPX-DOG-01` is 10 characters
  and contains `O`, which is outside the alphabet. The validation regex is
  `^(MPX|DEV)-[0-9A-HJKMNP-TV-Z]{12}$`.
- **The ID is not a secret.** It is a public routing label (cleartext in every
  frame), and nothing may treat possession of an ID as authentication.

> Naming: the DB column and the frame field stay `robot_uuid` for
> compatibility. This document calls the value the **robot ID**, because it is not an
> RFC 9562 UUID and we shouldn't pretend it is.

### D2 — The cloud generates the ID

The cloud is the only party that can enforce uniqueness, and the station needs
the ID before the burn so it can print the label. Generation is
`secrets.choice` over the alphabet, then `INSERT … state='reserved'`, retrying on
`UniqueViolationError`. Each reservation carries a 16-byte random `nonce`
(freshness for the sealed blob) and expires after 24 h.

An ID that has been issued is never issued again, even if its reservation
expires (`abandoned`): it may already be printed on a label or burned into a
chip. With 2⁵⁵ possible IDs, never recycling costs nothing.

### D3 — The ID lives in `BLOCK_USR_DATA` (eFuse)

| Option | Survives NVS erase | Survives reflash | Tamper | Chosen |
|---|---|---|---|---|
| Kconfig | ✅ | ❌ (per-unit builds, violates R4) | ❌ | dev only |
| Default `nvs` partition | ❌ (`main.cc:60-68`) | ✅ | ❌ | ❌ |
| New factory partition | ✅ | ✅ | ~ | ❌: `partitions.csv` is full, and shrinking `storage` reformats LittleFS |
| **`BLOCK_USR_DATA` eFuse** | ✅ | ✅ | ✅ (one-time write + `WR_DIS`) | ✅ |

Record layout in §6.2. Because of Reed-Solomon coding (§3.3) the record is
written once, in a single batch, together with its write-protect bit.

### D4 — The root secret is an `HMAC_UP` eFuse key

| Option | Key in flash dump | Key readable by any firmware | Clone (copy flash to another chip) works | Chosen |
|---|---|---|---|---|
| Kconfig (today) | ✅ plaintext | ✅ | ✅ | dev only |
| NVS + NVS encryption | ❌ with Flash Encryption on | ✅ | ✅ (with FE off) | ❌ |
| `USER` eFuse | ❌ | ✅ (and `espefuse` before download mode is closed) | ❌ | ❌ |
| **`HMAC_UP` eFuse + derive** | ❌ | ❌ for `S`; `K` only via the HMAC oracle | ❌ | ✅ |

This gives two properties even on boards **without** Secure Boot (dev and test
boards, D12): the key is no longer in the firmware image or in flash, and a copied
flash image produces the *wrong* key on any other chip. With Secure Boot on, the
only remaining extraction path is code execution in signed firmware on that
specific robot.

### D5 — The robot generates the secret and seals `K` to the cloud

| Model | Station sees | Needs custom device code | Matches Espressif's model |
|---|---|---|---|
| A: cloud generates, station burns with `espefuse burn_key` | `S` (plaintext, transiently) | no | no |
| B: device generates, station relays plaintext `K` | `K` | yes | partly |
| **B′: device generates, device seals `K` to the KMS public key** | **only ciphertext** | yes | yes: Flash Encryption also generates its key on-chip at first boot |

B′ is chosen because:

- A compromised or careless factory PC (contract manufacturer, shared laptop,
  logs, shell history) cannot leak any robot's key.
- The same code path handles rotation later (D13). With Model A the cloud would
  have to keep `S`, which defeats D4.
- It needs no per-unit image (R4). The station tool is ~300 lines of Python.

What B′ does **not** solve: anyone can encrypt to a public key, so a
compromised station could register *phantom* robots with keys it chose itself.
Mitigations are in §11: per-station quotas, reconciliation against production
counts, `active` only after first contact, and per-station audit. Proving that a
real ESP32-S3 is on the other end needs hardware attestation (RSA_DS). Its key
is burned at the factory (D17), and the protocol follows by OTA (§14).

### D6 — Key derivation

```
K_v = HMAC-SHA256_hw( S,  "mpx/v1/robot-key" ‖ 0x00 ‖ robot_id[16] ‖ uint32_be(v) )
```

- The message has a fixed length (37 bytes), so concatenation can't be
  ambiguous.
- Binding `robot_id` into `K` means a key is only valid for the ID it was made
  for.
- The label has its own namespace, so later purpose-specific derivations
  (`mpx/v1/dev-id`, `mpx/v1/attest`, …) can never produce the same output.
- `K_v` is exactly what the current code expects in `s_aes_key`
  (`chat_ws.cc`), `load_robot_key()` (`wasm_decrypt.cc`) and `robot_token()`
  (`agent_config.cc`). **No cloud-side protocol change is needed.**
- Per-purpose subkeys (chat-up / chat-down / wasm-wrap / robot-auth via HKDF
  from `K_v`, `KEY_MANAGEMENT.md` §7.2) are a separate, later protocol change.
  They derive from `K_v`, so this design doesn't block them.

**Key fingerprint** (safe to log, print and show in the admin UI):
`key_fp = hex(HMAC-SHA256(K_v, "mpx/v1/key-fp")[0:8])`. The robot logs it at
boot and the cloud stores it, which lets field support tell "wrong key" apart
from "wrong URL" without ever seeing the key.

### D7 — Sealed blob: RSA-OAEP-3072-SHA256 to a Cloud KMS key

- mbedTLS on the robot already supports `mbedtls_rsa_rsaes_oaep_encrypt` with
  SHA-256. One encryption costs a few milliseconds, once per unit.
- The public key ships in the firmware as `main/identity/prov_seal_pub.pem`.
  It is public and safe to commit. Under Secure Boot an attacker can't swap it.
- The blob carries a cleartext `seal_key_id` byte, so the KMS key can be rotated
  later and old firmware still works.
- Format in §6.4.

### D8 — At rest: KMS envelope with AAD

When the cloud receives a sealed blob it calls `AsymmetricDecrypt`, validates
the plaintext against the reservation, then calls KMS `Encrypt(K, aad =
"mpx/v1/robot-key-wrap" ‖ robot_id ‖ uint32_be(v))` with the symmetric KEK and stores the
ciphertext. Plaintext `K` exists only in gateway RAM (in the key cache) and is
never written anywhere. The AAD prevents a DB attacker from swapping wrapped keys
between rows.

Local development uses the same interface with a `LocalWrapper` (AES-256-GCM
under `MPX_LOCAL_KEK_HEX`) and a local RSA key pair. Both refuse to load when
`MPX_ENV=prod`.

### D9 — Entropy discipline on the robot

`mpx_provision` generates `S` as follows: `bootloader_random_enable()` → 
`esp_fill_random(S, 32)` → `bootloader_random_disable()`, all **before**
`init_wifi_*` and before any ADC use (the servo board and audio are not touched
in provisioning mode). After that it runs a cheap sanity check: reject all-zero,
all-equal, or a value equal to the previous boot's output. This doesn't
validate entropy. It catches a broken RNG path.

### D10 — eFuse budget and ordering

| Block | Purpose | Burned by | When |
|---|---|---|---|
| `BLOCK_KEY0` | `SECURE_BOOT_DIGEST0` | bootloader | first boot (Secure Boot) |
| `BLOCK_KEY1` | `XTS_AES_128_KEY` (or KEY1+KEY2 for XTS-AES-256) | bootloader | first boot (Flash Encryption) |
| `BLOCK_KEY2` *(or 3 if FE-256)* | **`HMAC_UP` — robot root secret `S`** | app (`mpx_provision`) | provisioning |
| `BLOCK_KEY3` *(or 4 if FE-256)* | `HMAC_DOWN_DIGITAL_SIGNATURE`, the RSA_DS attestation key (D17) | app (`mpx_provision`) | provisioning, **before `lock`, or never** |
| remaining (1–2) | spare | — | — |
| `BLOCK_USR_DATA` | robot ID record (§6.2) | app | provisioning, same session |

The app never hard-codes a block number. It finds the `HMAC_UP` block with
`esp_efuse_find_purpose(ESP_EFUSE_KEY_PURPOSE_HMAC_UP, &blk)`, and burns into the
first unused block with `esp_efuse_find_unused_key_block()`.

**Required ordering on a production unit** (§3.2):

1. Bootloader (first boot): burns the Flash Encryption key and enables Secure
   Boot, with `CONFIG_SECURE_BOOT_V2_ALLOW_EFUSE_RD_DIS=y`.
2. App (provisioning): burns `HMAC_UP` (read protection is set automatically),
   then the `USR_DATA` record and its `WR_DIS` bit.
3. App: burns `ESP_EFUSE_WR_DIS_RD_DIS`. From this point no key can be
   read-protected, which closes the denial-of-service / fault-injection window
   that Espressif warns about.
4. App (or station): finishes Flash Encryption release mode
   (`esp_flash_encryption_set_release_mode()`), if it isn't already set.

### D11 — Developer path

- Kconfig remains the identity source for dev builds
  (`CONFIG_APP_IDENTITY_SOURCE_KCONFIG`).
- `tools/mpx_dev_identity.py --gateway URL` generates a `DEV-…` ID and
  `secrets.token_bytes(32)`, registers it with a **dev** gateway (service token;
  refused when `MPX_ENV=prod`), and rewrites `CONFIG_APP_ROBOT_UUID` /
  `CONFIG_APP_CHAT_AES_KEY_HEX` in the developer's local `sdkconfig`.
- `git rm --cached sdkconfig` so the existing `.gitignore:35` rule takes effect.
  `sdkconfig.defaults` stays tracked and contains no identity.
- The shared `MPX-DOG-01` / `…0001` stays usable **only** with a local
  `docker compose` gateway. The prod gateway refuses to start if
  `MPX_DEV_AES_KEY_HEX` is set (`MPX_ENV=prod`), and rejects the public key at
  ingress.

### D12 — No field fleet: every customer unit is `secure`, and environments are separate

No robots have shipped, so there is no migration path for customer units: the
first Kickstarter unit leaves the factory with the full production profile.
Pilot units (D18) are the one planned exception.
The robot reports its security state in the sealed blob (`sec_flags`, §6.4), and
the cloud records `assurance`:

| `assurance` | Meaning | Allowed in the prod registry |
|---|---|---|
| `secure` | `HMAC_UP` + Secure Boot + Flash Encryption release + JTAG disabled | ✅ the only class for customer units |
| `efuse` | `HMAC_UP` identity, but Secure Boot or Flash Encryption off. Key is not in flash and clones fail, but custom firmware could query the HMAC oracle. | ✅ **only** for `cohort = 'pilot'` robots sealed by the bench station (D18); otherwise staging only |
| `dev` | Kconfig identity | ❌ dev only |
| `legacy` | today's hand-made rows (`MPX-DOG-01` style) | ❌ stays in the dev database |

The prod `seal` endpoint rejects anything except `secure`, apart from the pilot
exception above. That exception is tied to the station: only a station token
created with `allow_cohort = 'pilot'` can seal an `efuse` robot, and that robot
is marked `cohort = 'pilot'` permanently. `sec_flags` is self-reported, so it is
only trustworthy when Secure Boot is on. For pilot units the trust comes from
the bench being in-house, not from the flags.

**Start the production registry clean.** Today every robot row, test order and
skill assignment sits in one database called `mpx_marketplace_prod`. Since prod IDs
are permanent and will carry real orders, create a separate production database
(and a separate KMS key ring, D16) before the first **pilot** unit is
provisioned (D18). The current database becomes `dev`. `legacy`/`dev` rows and their
test orders then never mix with customer data, and nothing needs backfilling
into prod.

### D13 — Rotation (later phase)

- Rotate by deriving `K_{v+1}` from the same `S`. The cloud sends `key_rotate
  {kv: v+1}` over the authenticated socket. The robot derives `K_{v+1}`, seals it
  (same §6.4 format), and sends it back. The cloud stores it as `pending`,
  then flips it to `current` on the first frame that authenticates under `K_{v+1}`,
  keeping `K_v` as `previous` for a grace window.
- This defends against leaks of `K_v` from the cloud side (DB plus KMS misuse,
  logs, a gateway memory dump). It does **not** defend against code execution on
  that robot: such code can query the HMAC oracle for any `v`. Burning a fresh
  eFuse slot wouldn't help either, so we don't spend slots on it.
- The robot's key version is `max(kv_base, NVS mpx_id/kv)`. `kv_base` is burned
  in the eFuse ID record (§6.2): it is 1 for a new robot and higher for a
  replacement board (D15). If NVS is wiped, the cloud's ingress returns an
  **unauthenticated hint**: `{"error":"kv_mismatch","kv":N}`. The robot derives
  `K_N` and retries. A forged hint can only cause a failed connection, which an
  attacker on the path can cause anyway.
- **Installed skills:** `.mpxe` files on the robot's LittleFS are wrapped under
  `K_v`, so after a rotation they no longer decrypt. Rotation completes by
  re-deploying every assignment in `robot_skills` (the cloud is the source of
  truth), and the robot replaces its local copies. The assignments themselves
  never change.
- This requires a key-version byte in the frame header or AAD, so it ships with
  the protocol v2 work (§6.9 C3, C7), which is frozen in the pilot phase. The
  cloud can start rotating keys after launch with no firmware change.

### D14 — Phase 1 changes no wire format

The ID is 16 ASCII bytes and `K_1` has the same 32-byte role as today's key,
so frames, MPXE blobs, the robot token and the gateway's decrypt code stay
byte-for-byte unchanged. Only *where the values come from* changes. That keeps
phases A and B independent of the protocol-v2 changes in the notes, and lets
old and new robots share a gateway during development.

Protocol v2 itself is **not** deferred past launch: it is frozen in the pilot
phase (D18, §6.9), so no unit outside the office ever speaks v1.

### D15 — Robot IDs are permanent

The robot ID is the foreign key that orders and skill assignments hang off, so
it outlives every key, every board and every state change. Rules:

1. **Never delete, never renumber.** A trigger on `robots` raises on `DELETE`
   and on any `UPDATE` of `robot_uuid` (§6.8). `orders.robot_uuid` changes from
   `ON DELETE CASCADE` to `ON DELETE RESTRICT`, and so does `robot_skills`.
   Without this change, one mistaken `DELETE FROM robots` erases a customer's
   purchase history.
2. **States only move forward:** revoking, abandoning or replacing a robot
   changes `state` and the key rows. The `robots` row, its orders and its
   `robot_skills` rows stay.
3. **Keys can change, the ID can't.** `robot_keys` holds any number of
   versions per ID. Rotation (D13) and board replacement (below) add a version;
   neither touches `robots.robot_uuid`.
4. **The binding to hardware is tracked separately**, in `robot_hardware`:
   one row per main board that has ever carried the ID, with the MAC, the key
   versions it held, and bound/unbound timestamps (§6.8). The ID belongs to
   the robot (chassis + label + owner), not to one ESP32-S3.
5. **Ownership transfer** (resale, gift) needs no system change. There are no
   buyer accounts, so purchases belong to the robot and travel with it (D19).

**Main-board replacement (RMA).** The ESP32-S3 carries the burned ID and secret,
so a dead board would otherwise strand the robot's orders. The repair flow
re-uses the ID:

1. An admin opens a *replacement reservation* for the existing robot ID,
   bound to the new board's MAC: `POST /v1/provisioning/robots/{id}/replacements`.
   It is admin-only and audited, and it allocates
   `kv_new = max(existing key_version) + 1`.
2. The repair station runs the normal `provision` command with
   `{robot_id, nonce, kv: kv_new}`. The new board burns the **same** ID with
   `kv_base = kv_new` (§6.2), generates its own fresh `S`, and seals `K_kv_new`.
3. The cloud stores the new key as `current`, retires every older version, and
   closes the old `robot_hardware` row. The old board can never authenticate
   again, even if it is repaired.
4. The robot comes online, the firmware's normal skill sync re-installs every
   `robot_skills` assignment (MPXE blobs are wrapped under the new key), and
   orders are untouched.

The dead board keeps the ID burned forever, so it must be scrapped or held as a
spare for that same robot. It can never be re-provisioned under a different ID
(§6.6, `id_mismatch`).

### D16 — Cloud layout: one global registry now, per-product stacks later

**Today (single VM):** the registry is the `provisioning.py` router inside
`core_gateway` plus the `robots` / `robot_keys` / `robot_hardware` /
`robot_key_audit` tables. Only `provisioning.py` writes them. Everything else
(both gateways, deploy, payments) reads keys only through the shared
`robot_keys` library (§8.2). That boundary is what makes the later split cheap.

**Later (per-product stacks on load balancers / GKE):**

| Concern | Design now | Why it survives the split |
|---|---|---|
| ID uniqueness across products | **one** registry generates every ID, product stored in `robots.product` | a product stack never mints IDs, so collisions across stacks are impossible |
| Key isolation between products | **one KEK per (environment, product)**: `robot-kek-mpx-dog`, … | each product's GKE service account (Workload Identity) can decrypt only its own robots' keys |
| Sealing key | one `robot-seal-<product>` per product, listed in the firmware's `seal_key_id` table | firmware for a new product embeds its own; adding a key needs an OTA, never a re-provision |
| Registry location | module inside `core_gateway` | lift it into an `identity` service with its own database and an internal API: `GET /internal/v1/robots/{id}/key` returns `{key_wrapped, kek_name, key_version}`, and the **product stack unwraps with its own IAM**, so the registry never hands out plaintext |
| `orders` → `robots` foreign key | `RESTRICT` (D15) | once payments has its own database, the FK becomes a local, append-only copy of `(robot_uuid, product)`. That copy is safe **because IDs are never deleted**, so there are no deletes to propagate. |
| Key cache with many replicas | per-process cache, 60 s TTL + Postgres `LISTEN robot_keys_changed` | each pod listens independently. `LISTEN` needs a direct connection (it doesn't work through a transaction-mode pooler such as PgBouncer), so the 60 s TTL is the guaranteed bound and `NOTIFY` only speeds it up. Later this can move to Pub/Sub. |
| First-contact activation | `UPDATE robots SET state='active' … WHERE robot_uuid=$1 AND state='sealed'` | idempotent, so it is safe when two replicas race |
| Factory traffic | serve `/v1/provisioning/*` on its own hostname (e.g. `provision.<domain>`) from day one | the load balancer can then add mTLS and an IP allowlist (Cloud Armor) for factory networks without touching robot traffic |
| KMS region | key ring in `asia-east2` (HSM available, §3.5) | if stacks move region, re-wrap stored keys under a new-region KEK (server-side batch job) and add the new sealing key by OTA. Robots are unaffected. |
| Environments | separate `dev`, `staging` and `prod` key rings and databases (D12) | prod IDs and keys never touch dev infrastructure |

One unrelated note for the load-balancer move: `robot_rpc.register_robot`
(`main.py:1090`) keeps the robot → socket map in process memory, so RPCs to a
robot will need sticky routing or a shared bus. That is outside this document's
scope.

### D17 — Burn the attestation key at the factory (decide before the first unit)

The `lock` step burns `WR_DIS_RD_DIS` (D10 step 3). After that, the robot can
never again burn a key that must be read-protected, and both HMAC purposes
are in that category. So **a hardware attestation key can only be added before
`lock` on the factory line, or never** for that unit. For permanent robots that
will be in customers' hands for years, that is a one-way decision.

Recommendation: do the RSA_DS setup during provisioning, even though the
attestation *protocol* (§14) ships later by OTA:

1. The robot generates a 32-byte `K_ds` (same entropy rules as D9) and an
   RSA-3072 key pair (mbedTLS, using the S3's hardware multi-precision
   integer (MPI) accelerator).
2. `esp_ds_encrypt_params(&ds_data, iv, &p_data, K_ds)` produces the encrypted
   private-key blob (`esp_ds_data_t`, about 1.6 KB). Then
   `esp_efuse_write_key(blk, ESP_EFUSE_KEY_PURPOSE_HMAC_DOWN_DIGITAL_SIGNATURE, K_ds, 32)`,
   and the plaintext `K_ds` and private key are wiped from RAM.
3. `ds_data` is stored in NVS `mpx_id/ds` **and** uploaded with the seal. It is
   useless without this chip's eFuse key, so storing a copy in the cloud is
   safe, and it lets the cloud push it back if NVS is ever wiped.
4. The OAEP plaintext gains `sha256(ds_pubkey)` (§6.4), so the public key the
   station uploads is bound to the sealed key.

Cost: one more key block (the budget allows it, D10), the RSA key generation
time per unit (**measure on hardware**; if it is too slow for the line, run it
during burn-in), and ~1.6 KB of NVS. The benefit is that every Kickstarter unit
can later prove it is genuine MangDang silicon, which closes the phantom-robot
gap (§10) without a recall.

### D18 — Pilot phase: freeze the robot ↔ cloud contract before the factory

Research teams and early developers get robots before the factory line and the
full production cloud are ready. Those robots are permanent, sit outside the
office, and only update by OTA, so they force the contract to be final.
Everything else can still change.

**What a pilot unit is:**

| Aspect | Pilot unit | Kickstarter unit |
|---|---|---|
| Robot ID | permanent `MPX-…`, from the **production** registry | same |
| eFuse identity | `HMAC_UP` secret, `USR_DATA` record, `kv_base`, DS key (D17): **exactly the factory layout** | same |
| Firmware | the same image, with the identity source set to eFuse | same |
| Wire protocol | **v2** (§6.9) from the first boot | same |
| Provisioned by | bench station in the office, running `mpx_provision.py` with a pilot station token | factory station |
| Secure Boot / Flash Encryption | **off** | on, release mode |
| `lock` (`WR_DIS_RD_DIS`) | **not run**, so the unit can still be upgraded to `secure` over USB later | run |
| `assurance` / `cohort` | `efuse` / `pilot` | `secure` / `kickstarter` |
| Developer can flash own firmware | yes (it reads the same eFuse identity) | no |

**Why pilot units get production IDs, not `DEV-` IDs:** their owners will
build up skill assignments, and maybe orders, during the pilot. Moving those to
a new ID later would break the permanence rule (D15). `DEV-` IDs stay for
in-house firmware engineers on dev kits (D11).

**Why the contract is frozen now:** a v1 unit in a researcher's hands would
force the production gateway to support v1 indefinitely, or force a recall.
Shipping v2 from the first pilot unit means the production gateway is v2-only
from day one, and v1 support lives only in the dev stack.

**What the pilot phase needs from the cloud** is only the part that can't be
redone later: the production key ring and the production registry. The rest
stays on the current VM:

| Needed for the pilot | Not needed until Kickstarter |
|---|---|
| `mpx-identity-prod` key ring with HSM keys (§8.2) | factory station(s) and mTLS |
| a production Postgres database, on the existing VM is fine, with daily `pg_dump` to Cloud Storage | Cloud SQL / a separate prod VM |
| a second `docker compose` stack on the VM with `MPX_ENV=prod`, the prod DB and the prod key ring | load balancer, GKE, multiple replicas |
| final hostnames in DNS: `robot.<domain>` (gateway), `provision.<domain>` (stations) | Cloud Armor, per-product stacks |
| a bench station: a laptop, USB cable and a pilot station token | label printer, production runbook |
| an OTA channel that pilot units poll | — |

Moving the production database from the VM to Cloud SQL later is a
`pg_dump`/restore. The keys stay wrapped by the same KMS key-encryption key, so
nothing is re-wrapped and no robot notices. Pointing `robot.<domain>` at a load
balancer later is a DNS change, so **the firmware must use the final hostnames
from the first pilot build**, and verify TLS against the ESP-IDF CA bundle,
not a pinned certificate.

**Rules for pilot units:**

- They're accepted by production only because of the pilot cohort exception
  (D12). Paid third-party skills that rely on robot-side protection
  (`SKILL_PROTECTION.md`) should require `secure` and not be deployed to them,
  unless the skill's developer opts in.
- They keep `cohort = 'pilot'` forever. A unit returned to the office can be
  upgraded: enable Secure Boot and Flash Encryption over USB with the
  production profile, then run `lock`, then reseal at the same `kv` with the new
  `sec_flags`. The cloud raises `assurance` to `secure` but leaves the cohort.
  **Test this upgrade path on a sacrificial pilot board before relying on it.**
- They are numbered in the same ID space, so they sort and reconcile like any
  other robot. Reports can split by cohort.

**Exit criteria (pilot → factory):** the conformance suite (§11, test 14)
passes on the shipped firmware and on the production gateway; no change to
anything in §6.9 for an agreed soak period (suggested 4 weeks); at least one
OTA update has reached every pilot unit; and a board replacement (D15) has
been run end to end on a pilot unit.

### D19 — No accounts: purchases belong to the robot, and support recovers them

Customers don't create accounts, so `orders.robot_uuid` is the buyer. That
makes the permanent robot ID the only durable anchor for purchases. Two cases
need a person to step in, and both are handled by support in `mpx-admin`
(**Skill Recovery** screen), after matching the customer to the sales record
(Stripe receipt, Kickstarter backer record, photos of the robot labels):

| Case | What the admin does | Effect |
|---|---|---|
| **Main board died** | board replacement (D15) | same ID, nothing to transfer |
| **Whole robot lost, stolen or destroyed** | **succession** old → new robot | the new robot gets everything the old one was entitled to (purchases, earlier transfers, comped skills); the old robot's skills are removed; a robot can be succeeded only once |
| **Owner has several robots** | **fleet sync** source → each target | targets get the selected purchases; the source keeps them; comped skills are not carried |

Customers who don't want to contact support can simply buy again.

Rules, implemented in `mpx-payments` (`app/entitlements.py`, migration
`009_entitlement_transfers.sql`):

- **Orders are never edited or moved.** A transfer writes an
  `entitlement_transfers` record (kind, robots, evidence, support agent) and an
  `entitlement_grants` row per skill that points back at the **original**
  order, even across chained successions. Purchase history, developer revenue
  and the ledger don't change.
- **Refunds follow the money.** Refunding the original order (admin refund or
  Stripe `charge.refunded`) revokes every grant made from it and unassigns
  the skill, unless that robot has since bought the skill itself.
- **A retired robot can't be used again.** It can't be succeeded a second
  time, and nothing can be transferred to it.
- **Fail closed.** The admin endpoints are disabled unless `MPX_ADMIN_TOKEN` is
  set, and every transfer requires written evidence and the agent's name.
- Delivery uses the same gateway assign + enable as a purchase. A failed
  delivery leaves the grant recorded and can be retried from the history.

Succession works the same for any robot ID, including today's hand-made ones.
Once robot states exist (§6.5), a succession should also `revoke` the old
robot's key, so a "lost" robot that turns up can't keep using its skills.

### D20 — OTA firmware updates (required for MPX and MP4)

Every unit that leaves the office must be able to take a firmware update over
the air, safely, for its whole life. Pilot units depend on it (D18, R13),
and so do rotation (D13), attestation (§14), new sealing keys (D16) and any
security fix. **Today the firmware cannot do this at all:**

- `partitions.csv` has a single `factory` app slot (7 MB) with no `ota_0`,
  `ota_1` or `otadata`, so there is nowhere to write a new image while the
  old one runs.
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is off, so a bad image would brick
  the unit rather than fall back.
- `main/voice/mpx_voice_ota.c` is not a firmware updater. It is Xiaozhi's
  device-configuration lookup (it fetches the voice websocket URL).

The partition layout and the bootloader are fixed when a unit first ships
(§12.1 #8, #8a), so OTA has to be designed **before the first pilot unit**.

**Design:**

| Aspect | Decision | Why |
|---|---|---|
| Layout | Two equal app slots (`ota_0`, `ota_1`) plus `otadata`; no `factory` slot | Write the new image to the idle slot while the robot runs; a power cut mid-download leaves the old image untouched |
| Rollback | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. A new image boots as "pending verify" and calls `esp_ota_mark_app_valid_cancel_rollback()` only after a health check passes; otherwise the next reboot returns to the old image | A bad release can't brick the fleet |
| Health check | Local self-test (identity loads, storage mounts, servos and IMU initialise) and, if the robot was online before the update, a successful gateway connection **and a successful OTA check** within a time limit | The check proves the new image can still receive the *next* update; an image that breaks OTA rolls itself back |
| Authenticity | Kickstarter units: Secure Boot v2 verifies the signature. Pilot units (no Secure Boot, D18): `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` + `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`, so the running app verifies the new image's signature before switching. Same signing key for both. | Without this, anyone who controls the network path or the download server can install firmware on pilot units, and with it read their identity key through the HMAC oracle (§10) |
| Transport | `esp_https_ota` over HTTPS to the final hostname, verified against the ESP-IDF CA bundle (C8). Images can come from Cloud Storage. | The firmware is open source, so images aren't secret; integrity comes from the signature, not from hiding the URL |
| Product guard | Before switching, the robot reads the new image's `esp_app_desc_t` (`esp_https_ota_get_img_desc`) and refuses an image whose `project_name` isn't its own product, or whose layout version doesn't match its partition table | An MPX image on an MP4 (or vice versa) could drive the wrong servos |
| Version | `PROJECT_VER` set from the release tag by CI (`CONFIG_APP_PROJECT_VER_FROM_CONFIG`), semantic versioning | The cloud and support can see exactly what each robot runs |
| Downgrade protection | Kickstarter units: `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` with the eFuse `SECURE_VERSION` counter, raised **only** for security fixes (the counter has few steps, check the ESP32-S3 TRM). Pilot units: the OTA client refuses lower versions (soft check). | Stops an attacker re-installing an old, vulnerable signed image |
| When to install | Download in the background; apply only when the robot is idle (no motion or skill running) and charging or above a battery threshold. The robot app shows "update available"; the cloud can mark a release mandatory. | A robot must never reboot mid-step or with a flat battery |
| Data partitions | `system` (shipped SKILL.md files and `.recovery` seeds) and `model` (WakeNet) are **not** covered by app OTA. Each carries a version marker. A new app must work with the previous data contents; after it is marked valid it fetches a new data image if needed, checks its SHA-256, and re-downloads if interrupted. A missing or corrupt `system` partition must not stop the robot walking. | Data partitions can't be swapped atomically with only one copy each |
| Identity | OTA never touches eFuses or the identity record (R5). | Updates can't change who a robot is |

**Cloud side (contract item C10):**

- `GET https://robot.<domain>/v1/ota/check`, authenticated with the robot
  token (v2 auth once it exists). The robot sends its product, hardware
  revision, layout version, firmware version and contract version. The reply
  is `204` (nothing to do) or a manifest: `{version, url, size, sha256,
  mandatory, notes}`.
- Releases are rolled out by cohort (`dev` → office → `pilot` →
  `kickstarter`), then by percentage, and can be paused. The cloud records each
  robot's reported version, so `mpx-admin` can show the fleet's versions and a
  stuck rollout.
- The check endpoint and the manifest are part of the frozen contract
  (§6.9). They can only gain fields, never lose or change them, because every
  other change reaches robots through them.

**Flash budget (the blocking question).** The ESP32-S3-WROOM-1-N16R8 has
16 MB. The bootloader, partition table, `nvs`, `phy_init` and `otadata` take
about 0.1 MB; `model` and `system` take 1 MB each. The rest is split between
the two app slots and `storage` (installed skills, Lua, GIFs, agent memory),
which is 6.9 MB today:

| App slot size | `storage` left (16 MB) | `storage` left (32 MB flash) |
|---|---|---|
| 2 × 7 MB (today's slot) | **does not fit** | ≈ 15.9 MB |
| 2 × 6 MB | ≈ 2 MB | ≈ 17.9 MB |
| 2 × 5 MB | ≈ 4 MB | ≈ 19.9 MB |

The comments in `partitions.csv` say the image outgrew 6 MB, so on 16 MB flash
A/B OTA means **shrinking the image or giving up most of `storage`**. Options,
best first:

1. **32 MB flash on MPX/MP4 production hardware** (a BOM decision for the
   hardware team). Pilot units must use the same module, or they become a
   separate firmware variant forever (§12.1 #8).
2. **Shrink the image below the slot size.** Move large constant data that
   both slots would duplicate (for example the ~600 KB `trajopt_jump.h`
   tables, GIFs) into a shared, versioned data partition, so it's stored once.
   That data then follows the data-partition rule above.
3. **Smaller `storage`.** Workable only if installed skills can be
   re-downloaded from the cloud on demand.

Measure first: `idf.py size` on the release build gives the real image size,
and the secure bootloader may also move the partition table (§13 #7). Both
numbers decide the layout.

---

## 6. Specification

### 6.1 Robot ID

```python
ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"   # Crockford Base32
ID_RE    = re.compile(r"^(MPX|DEV)-[0-9A-HJKMNP-TV-Z]{12}$")

def _luhn32_check(body: str) -> str:
    factor, total = 2, 0
    for ch in reversed(body):
        a = factor * ALPHABET.index(ch)
        factor = 1 if factor == 2 else 2
        total += a // 32 + a % 32
    return ALPHABET[(32 - total % 32) % 32]

def new_robot_id(prefix: str = "MPX") -> str:
    body = "".join(secrets.choice(ALPHABET) for _ in range(11))
    return f"{prefix}-{body}{_luhn32_check(body)}"

def is_valid_robot_id(s: str) -> bool:
    s = s.strip().upper().replace("O", "0").replace("I", "1").replace("L", "1")
    return bool(ID_RE.match(s)) and _luhn32_check(s[4:15]) == s[15]
```

Normalisation (`O→0`, `I/L→1`, upper-case) applies **only to human input**
(admin UI, support tools). The gateway compares bytes exactly.

**Test vector:** body `AJZ7J8Y0YTD` → check `S` → `MPX-AJZ7J8Y0YTDS`. Changing
the second symbol (`MPX-A1Z7J8Y0YTDS`) fails validation.

### 6.2 eFuse record in `BLOCK_USR_DATA`

`main/esp_efuse_custom_table.csv` (enable `CONFIG_EFUSE_CUSTOM_TABLE=y`):

```
# field_name,            efuse_block, bit_start, bit_count, comment
USER_DATA.MPX_ROBOT_ID,  EFUSE_BLK3,  0,         128,       Robot ID, 16 ASCII bytes
USER_DATA.MPX_ID_FORMAT, EFUSE_BLK3,  128,       8,         Record format, 0x01
USER_DATA.MPX_ID_FLAGS,  EFUSE_BLK3,  136,       8,         bit0 = dev unit, bit1 = replacement board
USER_DATA.MPX_KV_BASE,   EFUSE_BLK3,  144,       16,        Lowest key version this board may use (1 = new robot)
```

`MPX_KV_BASE` is what lets a replacement board (D15) start at a higher key
version without depending on NVS.

Bits 160–255 stay zero, which leaves the built-in `CUSTOM_MAC` range free.
`idf.py efuse-custom-table` fails the build if a field overlaps it.
`WR_DIS` for `BLOCK_USR_DATA` is burned in the same batch.

Read path at boot: if `MPX_ID_FORMAT == 0x01`, the ID passes
`is_valid_robot_id`, **and** an `HMAC_UP` block exists, the robot is
provisioned. Any other combination is reported as a distinct state (§10).

### 6.3 Key derivation and test vectors

```
msg   = b"mpx/v1/robot-key" + b"\x00" + robot_id_ascii_16 + uint32_be(kv)   # 37 bytes
K_kv  = HMAC-SHA256(S, msg)                       # on device: esp_hmac_calculate()
key_fp= HMAC-SHA256(K_kv, b"mpx/v1/key-fp")[:8].hex()
token = robot_id + "." + HMAC-SHA256(K_kv, b"mpx-ai-v1:" + robot_id).hexdigest()[:32]   # unchanged
```

Vector (`S = 00 01 02 … 1f`, `robot_id = MPX-AJZ7J8Y0YTDS`, `kv = 1`):

| Value | Hex |
|---|---|
| `msg` | `6d70782f76312f726f626f742d6b6579004d50582d414a5a374a3859305954445300000001` |
| `K_1` | `27c6f7d89725761ebff5ad8f0a29ce353ed5b2ea282e6ce0abb2d04af5daf5ae` |
| `key_fp` | `6d5f50ad94184e23` |
| robot token | `MPX-AJZ7J8Y0YTDS.460a4a84a2e2e36cefade46a38a89c93` |

The firmware unit test burns nothing. It runs the software HMAC
(`mbedtls_md_hmac`) on the vector and checks it matches the cloud's Python.
On hardware, `esp_hmac_calculate` with a burned test key must give the same
`K_1` for the same `S`, which is a one-time check on a sacrificial board.

### 6.4 Sealed blob

```
offset size  field
0      4     magic "MPXS"
4      1     format = 0x01
5      1     seal_key_id      (index into the firmware's table of KMS public keys)
6      2     reserved = 0
8      384   RSA-OAEP-3072-SHA256( plaintext )
```

OAEP plaintext (≤ 318 bytes allowed; 149 used):

```
offset size  field
0      4     magic "MPXK"
4      1     format = 0x01
5      4     key_version (uint32 BE)
9      16    robot_id (ASCII)
25     16    nonce (from the reservation)
41     32    K_kv
73     6     base MAC (esp_efuse_mac_get_default)
79     2     sec_flags: bit0 secure_boot, bit1 flash_enc_release, bit2 jtag_disabled,
             bit3 usb_jtag_disabled, bit4 dl_mode_secure_or_disabled, bit5 hmac_up_rd_dis
81     4     esp_app_desc.secure_version
85     32    esp_app_desc.version (NUL-padded)
117    32    sha256(DS public key, DER), all zeros if no DS key (D17)
```

The DS public key and the encrypted `ds_data` travel next to the blob in the
`seal` request. They are not secret, and the hash above binds them to it.

The cloud rejects the blob unless:

- the magic and format match;
- `robot_id` and `nonce` equal the reservation, and the reservation has not
  expired;
- `key_version` equals the reservation's (1 for a new robot, `kv_new` for a
  replacement, or the pending rotation version);
- in prod, `sec_flags` meets the `secure` bar (D12);
- the MAC is not bound to a *different* robot ID in `robot_hardware`.

### 6.5 Cloud lifecycle

```
             reserve                 seal (valid blob)        first valid frame
  (none) ───────────▶ reserved ─────────────────────▶ sealed ──────────────────▶ active
                         │ 24 h                          │ 30 d without contact     │
                         ▼                               ▼                          ▼
                     abandoned                       abandoned               revoked (admin)
```

- `sealed` robots may authenticate at ingress. Their first valid frame flips them to
  `active` and sets `activated_at`. That proves a device holding `K` really came
  online.
- `abandoned` and `revoked` keys are never loaded into any gateway cache.
- **No state deletes anything.** `abandoned` and `revoked` robots keep their row,
  their orders and their `robot_skills` rows (D15).
- **Replacement** (D15) is the only way back from `revoked`, and also applies to
  `active` robots: an admin replacement reservation moves the robot to
  `sealed` with a new `key_version`, then to `active` on first contact. All
  earlier key versions become `retired`.
- Every transition writes a `robot_key_audit` row.

### 6.6 Station ↔ robot serial protocol

The robot runs this only while **unprovisioned**, or while provisioned but not
yet confirmed `active` by the cloud (NVS flag `mpx_id/activated`, set on the
first successful ingress auth). The transport is UART0 at 115200 (the existing
console, `sdkconfig:1515-1526`). Lines are prefixed so they stand out in the
log stream:

```
station → robot   @MPXPROV {"cmd":"status"}
robot  → station  @MPXPROV {"ok":true,"state":"unprovisioned","mac":"…","sec_flags":5,"fw":"…"}

station → robot   @MPXPROV {"cmd":"provision","robot_id":"MPX-…","nonce":"<32 hex>","kv":1}
robot  → station  @MPXPROV {"ok":true,"state":"sealed","key_fp":"…","sealed":"<base64>"}

station → robot   @MPXPROV {"cmd":"reseal","nonce":"<32 hex>"}     # resume after a failed upload
station → robot   @MPXPROV {"cmd":"lock"}                          # burn WR_DIS_RD_DIS (+ FE release)
```

`provision` is idempotent:

| Robot state on `provision` | Action |
|---|---|
| no `HMAC_UP`, no ID record | generate `S` (D9), burn `HMAC_UP`, set up DS (D17), burn ID record (`kv_base` = the requested `kv`) + `WR_DIS`, derive, seal |
| `HMAC_UP` present, no ID record (power lost mid-way) | burn ID record with the supplied ID, derive, seal |
| both present, same ID | derive, seal (same as `reseal`) |
| both present, **different** ID | refuse `{"ok":false,"error":"id_mismatch","robot_id":"<burned>"}`. The station must re-reserve against the burned ID (§9). |
| ID record present, no `HMAC_UP` | burn `HMAC_UP`, derive, seal |

`S` and `K` never cross the UART. Only `key_fp` and the sealed blob do.

### 6.7 Cloud API

New router `cloud/core_gateway/provisioning.py`. All routes require a station or
admin credential (§8.3).

| Method | Path | Body → Response |
|---|---|---|
| `POST` | `/v1/provisioning/reservations` | `{product, hw_rev, mac, serial?}` → `{robot_id, nonce, kv:1, expires_at}`; returns the existing reservation if `mac` is already reserved and unexpired |
| `POST` | `/v1/provisioning/robots/{robot_id}/replacements` | **admin only.** `{mac, reason}` → `{robot_id, nonce, kv: max+1, expires_at}`. Reuses the existing ID for a new main board (D15). |
| `POST` | `/v1/provisioning/robots/{robot_id}/seal` | `{sealed, ds_pubkey?, ds_data?}` (base64) → `{state:"sealed", key_fp, assurance}`. Idempotent: if the same blob is sent again, returns the same response. |
| `GET` | `/v1/provisioning/robots/{robot_id}` | → `{state, key_fp, assurance, key_version, activated_at}` (never key material) |
| `POST` | `/v1/provisioning/robots/{robot_id}/revoke` | admin only → `{state:"revoked"}` + `NOTIFY robot_keys_changed`. Orders and skill assignments are kept. |
| `POST` | `/v1/provisioning/dev-robots` | dev only (`MPX_ENV=dev`) → `{robot_id:"DEV-…", aes_key_hex}`. The only route that ever returns key material, and it is compiled out of prod. |

`POST /v1/robots` (`robots.py:38-74`): reject `aes_key_hex` from callers unless
`MPX_ENV=dev`, and require the service token. Rows only enter through the
routes above.

### 6.8 Schema migration

Add `payments/migrations/010_robot_identity.sql` next to 001–009, since that
directory already references `robots`. Before writing it, capture the current
`robots` DDL (`pg_dump -s -t robots`) and commit it as `000_robots_baseline.sql`.
Today the DDL is in no repo.

```sql
ALTER TABLE robots
  ADD COLUMN IF NOT EXISTS product      text NOT NULL DEFAULT 'mpx-dog',
  ADD COLUMN IF NOT EXISTS hw_rev       text,
  ADD COLUMN IF NOT EXISTS cohort       text NOT NULL DEFAULT 'dev'
      CHECK (cohort IN ('dev','pilot','kickstarter','retail')),   -- D18; never changes once set
  ADD COLUMN IF NOT EXISTS state        text NOT NULL DEFAULT 'active'
      CHECK (state IN ('reserved','sealed','active','abandoned','revoked')),
  ADD COLUMN IF NOT EXISTS assurance    text NOT NULL DEFAULT 'legacy'
      CHECK (assurance IN ('secure','efuse','dev','legacy')),
  ADD COLUMN IF NOT EXISTS station_id   text,
  ADD COLUMN IF NOT EXISTS prov_nonce   bytea,
  ADD COLUMN IF NOT EXISTS sec_flags    integer,
  ADD COLUMN IF NOT EXISTS fw_version   text,
  ADD COLUMN IF NOT EXISTS ds_pubkey    bytea,          -- D17, public
  ADD COLUMN IF NOT EXISTS ds_data      bytea,          -- D17, encrypted DS params (safe to store)
  ADD COLUMN IF NOT EXISTS reserved_at  timestamptz,
  ADD COLUMN IF NOT EXISTS sealed_at    timestamptz,
  ADD COLUMN IF NOT EXISTS activated_at timestamptz,
  ADD COLUMN IF NOT EXISTS updated_at   timestamptz NOT NULL DEFAULT now();

ALTER TABLE robots ALTER COLUMN aes_key_hex DROP NOT NULL;   -- dev DB only; never populated in prod

-- ── Permanence (D15) ────────────────────────────────────────────
CREATE OR REPLACE FUNCTION robots_are_permanent() RETURNS trigger AS $$
BEGIN
  IF TG_OP = 'DELETE' THEN
    RAISE EXCEPTION 'robots rows are permanent (robot %): revoke instead', OLD.robot_uuid;
  END IF;
  IF NEW.robot_uuid IS DISTINCT FROM OLD.robot_uuid THEN
    RAISE EXCEPTION 'robot IDs are permanent (% -> %)', OLD.robot_uuid, NEW.robot_uuid;
  END IF;
  IF NEW.cohort IS DISTINCT FROM OLD.cohort THEN
    RAISE EXCEPTION 'robot cohort is permanent (robot %)', OLD.robot_uuid;
  END IF;
  RETURN NEW;
END $$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS robots_permanent ON robots;
CREATE TRIGGER robots_permanent BEFORE DELETE OR UPDATE OF robot_uuid, cohort ON robots
  FOR EACH ROW EXECUTE FUNCTION robots_are_permanent();

-- orders: CASCADE → RESTRICT. Check the real constraint name with \d orders first.
ALTER TABLE orders DROP CONSTRAINT IF EXISTS orders_robot_uuid_fkey;
ALTER TABLE orders ADD CONSTRAINT orders_robot_uuid_fkey
  FOREIGN KEY (robot_uuid) REFERENCES robots(robot_uuid) ON DELETE RESTRICT;
-- robot_skills: same change; its DDL is not in any repo, so check \d robot_skills.

-- ── Which main board carries the ID (D15) ──────────────────────
CREATE TABLE IF NOT EXISTS robot_hardware (
  id           bigserial PRIMARY KEY,
  robot_uuid   varchar(64) NOT NULL REFERENCES robots(robot_uuid) ON DELETE RESTRICT,
  hw_mac       macaddr     NOT NULL,
  kv_base      integer     NOT NULL,
  reason       text        NOT NULL,          -- 'factory' | 'replacement:<ticket>'
  bound_at     timestamptz NOT NULL DEFAULT now(),
  unbound_at   timestamptz
);
CREATE UNIQUE INDEX IF NOT EXISTS robot_hardware_one_live_board
  ON robot_hardware (robot_uuid) WHERE unbound_at IS NULL;
CREATE UNIQUE INDEX IF NOT EXISTS robot_hardware_mac_once
  ON robot_hardware (hw_mac);                 -- a board's MAC belongs to one robot ID, forever

CREATE TABLE IF NOT EXISTS robot_keys (
  robot_uuid   varchar(64) NOT NULL REFERENCES robots(robot_uuid) ON DELETE RESTRICT,
  key_version  integer     NOT NULL,
  key_wrapped  bytea       NOT NULL,      -- KMS Encrypt(K, aad)
  kek_name     text        NOT NULL,      -- projects/…/cryptoKeys/…/cryptoKeyVersions/N
  key_fp       char(16)    NOT NULL,
  state        text        NOT NULL CHECK (state IN ('pending','current','previous','retired')),
  created_at   timestamptz NOT NULL DEFAULT now(),
  PRIMARY KEY (robot_uuid, key_version)
);
CREATE UNIQUE INDEX IF NOT EXISTS robot_keys_one_current
  ON robot_keys (robot_uuid) WHERE state = 'current';

CREATE TABLE IF NOT EXISTS robot_key_audit (
  id          bigserial PRIMARY KEY,
  robot_uuid  varchar(64) NOT NULL,
  key_version integer,
  action      text NOT NULL,   -- reserve|seal|activate|abandon|revoke|rotate|replace|auth_fail|unwrap
  actor       text NOT NULL,   -- station id | admin | 'ingress' | 'migration'
  detail      jsonb NOT NULL DEFAULT '{}',
  at          timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS robot_key_audit_robot ON robot_key_audit (robot_uuid, at DESC);

CREATE TABLE IF NOT EXISTS provisioning_stations (
  station_id    text PRIMARY KEY,
  token_sha256  bytea NOT NULL,
  daily_quota   integer NOT NULL DEFAULT 500,
  enabled       boolean NOT NULL DEFAULT true,
  created_at    timestamptz NOT NULL DEFAULT now()
);
```

**Prod vs dev (D12).** The production database starts empty and gets the
migration from scratch. No `aes_key_hex` value, legacy row or test order ever
reaches it. In the **dev** database the migration runs over existing rows, and a
one-off script (`actor='migration'`) wraps each `aes_key_hex` into
`robot_keys(kv=1, state='current')` with `assurance='legacy'`, so the dev
robots keep working. Readers dual-read (`robot_keys` first, then `aes_key_hex`)
only where `MPX_ENV=dev`.

Before the trigger goes in, check that nothing in the code deletes or renumbers
robots. Today nothing in `cloud/` does: the only robot-related delete is
`robots.py:302`, which removes one `robot_skills` row (an uninstall). Manual
SQL runbooks and any admin tooling outside these repos need the same check.

### 6.9 Robot ↔ cloud contract v2 (frozen before the first pilot unit)

"The contract" is everything a shipped robot and the production cloud must
agree on. After the freeze, each item can only change **additively and
behind a version number**, and the production gateway must keep accepting
every version that any shipped unit speaks.

| # | Item | v2 content | Source |
|---|---|---|---|
| C1 | Identity | ID format (§6.1), eFuse record (§6.2), derivation of `K_v` (§6.3) | this doc |
| C2 | Provisioning | sealed-blob format (§6.4), `@MPXPROV` serial protocol (§6.6), provisioning API (§6.7) | this doc |
| C3 | Frame header | adds a **protocol version byte** and a **key version** to today's 28-byte header (`packets.py:11`: 16-byte ID + 12-byte IV). Both are covered by the GCM AAD, so a frame can't be replayed under another version. | notes P4, `KEY_MANAGEMENT.md` §8 phase 3 |
| C4 | Purpose subkeys | chat-up, chat-down, MPXE wrap and robot-token keys are derived from `K_v` by HKDF with distinct labels (`mpx/v2/up`, …). `K_v` itself is never used directly. | notes phase 3 |
| C5 | Nonces and replay | fresh random IV for **every** downstream frame (fixes the live reuse at `main.py:1158` and `:1211`), and a monotonic counter in the downstream AAD that the robot checks | notes P7 |
| C6 | MPXE v2 | AAD adds skill version and `not_after`. Device binding comes from the per-robot `K_v` (no separate `device_id` needed, since `K_v` already differs per chip). | notes P4, adapted |
| C7 | Control messages | `key_rotate` (D13), the `kv_mismatch` hint (D13), skill re-sync after rotation or replacement (D15), and the DS challenge/response (§14), defined now even if the cloud only starts sending them later | D13, D15, D17 |
| C8 | Endpoints | final hostnames `robot.<domain>` and `provision.<domain>`, paths, TLS verified against the ESP-IDF CA bundle | D16, D18 |
| C9 | Voice gateway identity | the voice `Device-Id` becomes the robot ID (today it is MAC-derived, `mpx_voice_ota.c:102-110`), because the MAC changes on board replacement | §13 #8 |
| C10 | OTA | A/B layout, rollback and health check, signed images, product guard, the `/v1/ota/check` request and manifest; every later change depends on it | D20 |

The byte layouts for C3–C7 belong in a separate `docs/WIRE_PROTOCOL_V2.md`,
written and reviewed in phase P. This document only fixes what they must
contain. Each item gets golden test vectors (like §6.3) that the firmware and
Python implementations must both reproduce (§11, test 14).

**C7 rule:** a message defined in v2 that the cloud doesn't use yet must still be
implemented and tested in the pilot firmware. "Defined but never exercised" is
how a contract quietly changes later.

---

## 7. End-to-end factory flow

```
Station PC (mpx_provision.py)          Robot (signed release FW)              Cloud (core_gateway + KMS)
─────────────────────────────          ─────────────────────────              ──────────────────────────
esptool write_flash (plaintext,
  first and only time)          ──▶   1st boot: bootloader generates FE key,
                                      encrypts flash, enables Secure Boot
                                      (ALLOW_EFUSE_RD_DIS=y)
                                      app: unprovisioned → listens @MPXPROV
status                          ──▶
                                ◀──   state, mac, sec_flags
POST /reservations {product,mac,hw_rev} ─────────────────────────────────────▶  new_robot_id(), nonce
                                ◀──────────────────────────────────────────────  {robot_id, nonce, kv=1}
print label (robot_id, QR)
provision {robot_id,nonce,kv=1} ──▶   bootloader_random_enable → S, K_ds
                                      esp_efuse_write_key(HMAC_UP, S)  (RD_DIS auto)
                                      RSA-3072 keygen; esp_ds_encrypt_params
                                      esp_efuse_write_key(HMAC_DOWN_DS, K_ds)
                                      burn USR_DATA {robot_id,fmt,flags,kv_base} + WR_DIS
                                      K1 = esp_hmac_calculate(msg)
                                      sealed = OAEP(pub, K1‖id‖nonce‖mac‖flags…‖H(ds_pub))
                                      secure_zero(S, K_ds, RSA priv, K1 copies)
                                ◀──   {key_fp, sealed, ds_pubkey, ds_data}
POST /robots/{id}/seal {…}      ─────────────────────────────────────────────▶  KMS AsymmetricDecrypt
                                                                                 validate vs reservation
                                                                                 KMS Encrypt(K1, aad) → robot_keys
                                                                                 state = sealed
                                ◀──────────────────────────────────────────────  {key_fp, assurance}
assert cloud key_fp == robot key_fp
lock                            ──▶   burn WR_DIS_RD_DIS; FE release mode
                                ◀──   ok
QA: Wi-Fi test → robot connects ──────────────────────────────────────────────▶  first frame decrypts
                                                                                 state = active
```

Station output per unit: a log line with `robot_id, mac, key_fp, station_id,
timestamp` and no secrets. That is enough to reconcile against the cloud and
the production count.

---

## 8. Implementation

### 8.1 Firmware

**New component: `main/identity/`**

```c
// main/identity/mpx_identity.h
typedef enum {
    MPX_ID_UNPROVISIONED = 0,
    MPX_ID_KCONFIG,          // dev builds
    MPX_ID_EFUSE,            // HMAC_UP + USR_DATA
    MPX_ID_INCONSISTENT,     // e.g. ID record without HMAC_UP: needs station
} mpx_id_state_t;

esp_err_t       mpx_identity_init(void);                 // after init_nvs(), before chat/agent/wasm
mpx_id_state_t  mpx_identity_state(void);
const char     *mpx_identity_robot_id(void);             // "" unless provisioned; NUL-terminated, ≤16 chars
bool            mpx_identity_robot_key(uint8_t out[32]); // copy; caller must secure_zero
uint32_t        mpx_identity_key_version(void);
const char     *mpx_identity_key_fp(void);               // 16 hex chars
uint16_t        mpx_identity_sec_flags(void);
```

- `mpx_identity_init()` reads the eFuse record and finds the `HMAC_UP` block,
  sets `key_version = max(MPX_KV_BASE, NVS mpx_id/kv)` (D13, D15), computes
  `K` once with `esp_hmac_calculate`, keeps it in a static buffer, and logs
  `robot_id`, `kv`, `key_fp` and `state`.
- `main/identity/mpx_provision.cc` implements §6.6. It starts only when the state
  is `UNPROVISIONED`/`INCONSISTENT`, or when `mpx_id/activated` is unset, and
  **before** `init_wifi_*` (D9). It is a no-op in `KCONFIG` builds.
- `main/identity/prov_seal_pub.pem` is embedded via `EMBED_TXTFILES`.
- `main/esp_efuse_custom_table.csv` per §6.2.

**Kconfig (`main/Kconfig.projbuild:174-232`)**

```
choice APP_IDENTITY_SOURCE
    prompt "Robot identity source"
    default APP_IDENTITY_SOURCE_KCONFIG
config APP_IDENTITY_SOURCE_KCONFIG
    bool "Kconfig strings (development only)"
config APP_IDENTITY_SOURCE_EFUSE
    bool "eFuse HMAC_UP key + USR_DATA robot ID (production)"
endchoice

config APP_ROBOT_UUID          # depends on APP_IDENTITY_SOURCE_KCONFIG
config APP_CHAT_AES_KEY_HEX    # depends on APP_IDENTITY_SOURCE_KCONFIG
# APP_MPX_EFUSE_BLK: removed (block is found by purpose)
```

Build guards in `mpx_identity.cc`:

```c
#if CONFIG_SECURE_BOOT && !CONFIG_APP_IDENTITY_SOURCE_EFUSE
#error "Secure Boot builds must use the eFuse identity source"
#endif
#if CONFIG_APP_IDENTITY_SOURCE_EFUSE && CONFIG_SECURE_BOOT && !CONFIG_SECURE_BOOT_V2_ALLOW_EFUSE_RD_DIS
#error "HMAC_UP is burned after Secure Boot: enable SECURE_BOOT_V2_ALLOW_EFUSE_RD_DIS"
#endif
```

At runtime, in `KCONFIG` mode, the robot logs an error if the key is the public
`…0001`. That is a warning only, so local dev keeps working.

**Call sites to switch to `mpx_identity_*`**

| File:line | Today | After |
|---|---|---|
| `main/network/chat_ws.cc:49` | `constexpr ROBOT_UUID = CONFIG_APP_ROBOT_UUID` | `mpx_identity_robot_id()` |
| `main/network/chat_ws.cc:588-601` | `hex_to_bytes(CONFIG_APP_CHAT_AES_KEY_HEX)` | `mpx_identity_robot_key(s_aes_key)` |
| `main/network/chat_ws.cc:626-629` | `strncpy(…, 16)` | `memcpy` of exactly 16 bytes (ID is validated as 16 chars or a shorter legacy ID) |
| `main/network/chat_ws.cc:1453` | `if (!load_aes_key())` | also refuse when state is not `KCONFIG`/`EFUSE` |
| `main/wasm/wasm_decrypt.cc:46-69` | Kconfig parse + `NDEBUG` zero check | `mpx_identity_robot_key()` |
| `main/wasm/wasm_decrypt.cc:116-118` | `CONFIG_APP_ROBOT_UUID` in AAD | `mpx_identity_robot_id()` |
| `main/agent/agent_config.cc:216-236` | Kconfig key + ID | identity API; return `""` when unprovisioned |
| `main/network/http_server.cc:2504`, `:2614` | `CONFIG_APP_ROBOT_UUID` | `mpx_identity_robot_id()` |
| `main/pi/pi_link.cc:65-66`, `:189` | `#define` fallback + Kconfig | `mpx_identity_robot_id()` |
| `main/main.cc:72` | — | call `mpx_identity_init()` right after `init_nvs()` |

Unprovisioned behaviour: the local PWA, motion and Lua keep working, while cloud
chat, the robot token and MPXE decryption are disabled with a distinct log line
and a PWA status. A robot with no identity should still walk.

**As built (M1):** the component is `components/mpx_identity/`, and the split
that makes it testable is `mpx_id.c` + `mpx_derive.c` (no ESP headers, the HMAC
passed in as a callback) under `mpx_identity.cc` (Kconfig, NVS, eFuse). Two
deviations from this section: `mpx_identity_key_version()` was added early
because the frame AAD needs it in §6.9 C3, and the all-zero-key refusal moved
out of `wasm_decrypt.cc` into the identity so it covers every caller. The
eFuse source is a stub until Stage 2 — see `### M1 as built` in
`docs/FIRMWARE_IMPLEMENTATION_PLAN.md`.

**sdkconfig (production profile, `sdkconfig.defaults.prod`)**

```
CONFIG_APP_IDENTITY_SOURCE_EFUSE=y
CONFIG_EFUSE_CUSTOM_TABLE=y
CONFIG_SECURE_BOOT=y
CONFIG_SECURE_BOOT_V2_ALLOW_EFUSE_RD_DIS=y
CONFIG_SECURE_FLASH_ENC_ENABLED=y
CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y
```

Check before the first secure build: Secure Boot and Flash Encryption make the
bootloader bigger, and Espressif warns that this "might require updating
partition table offset". The table is at `0x8000` and `nvs` at `0x9000`
(`partitions.csv:29`). If the offset has to move, every existing robot's NVS
moves with it, so this needs a decision before the first secure unit ships.

### 8.2 Cloud

| File | Change |
|---|---|
| new `cloud/core_gateway/robot_ids.py` | §6.1 functions (shared by provisioning, admin, tests) |
| new `cloud/core_gateway/keywrap.py` | `KeyWrapper` interface: `KmsWrapper` (asym decrypt + sym encrypt/decrypt) and `LocalWrapper` (dev). The factory refuses `LocalWrapper` when `MPX_ENV=prod`. |
| new `cloud/core_gateway/provisioning.py` | §6.5–§6.7 routes and the state machine (including replacement, D15), plus audit rows and `NOTIFY robot_keys_changed`. The **only** writer of `robots` identity columns, `robot_keys` and `robot_hardware` (D16). |
| new `cloud/common/robot_keys.py` (shared by both gateways) | `async get_key(robot_id) -> (key, kv) \| None`: TTL cache (60 s) + negative cache (10 s) + `LISTEN` invalidation. Reads `robot_keys` joined with `robots.state IN ('sealed','active')`; dual-reads `aes_key_hex` in dev only. Picks the KEK from `robot_keys.kek_name`, so per-product KEKs need no code change. |
| `payments/migrations/010_robot_identity.sql` | §6.8, including the `orders` FK → `RESTRICT` and the permanence trigger |
| `cloud/core_gateway/crypto.py:43-138` | replace `_KEY_STORE` / `load_keys_from_db` with `robot_keys.get_key`. The dev key loads only when `MPX_ENV=dev`. |
| `cloud/core_gateway/main.py:116` | drop the startup-only load; start the `LISTEN` task |
| `cloud/core_gateway/main.py:1064-1074` | on the first valid frame: if the robot is `sealed`, flip it to `active` and set `last_seen_at`; on decrypt failure for a known ID, write an `auth_fail` audit row (rate-limited) |
| `cloud/core_gateway/main.py:862-867` | deploy path uses `robot_keys.get_key` |
| `cloud/core_gateway/robots.py:38-74` | service token required; caller-supplied `aes_key_hex` only in dev |
| `cloud/voice_gateway/vg/auth.py:43-92` | `KeyStore` delegates to `robot_keys.get_key` |
| `cloud/mpx-wasm-deploy.py:69` | delete the direct SQL; call the deploy API |
| `cloud/docker-compose.yml:10-13`, `:88-89`; `.env.example:8-10` | keep the dev defaults, add `MPX_ENV=dev`. The prod compose sets `MPX_ENV=prod` and no `MPX_DEV_*`. |

**KMS resources:** one key ring per environment in `asia-east2`
(`mpx-identity-dev`, `mpx-identity-staging`, `mpx-identity-prod`), and one key
pair per product inside it (D16). For `mpx-dog`:

| Key | Purpose | Algorithm | Who may use it |
|---|---|---|---|
| `robot-seal-mpx-dog` | `ASYMMETRIC_DECRYPT` | `RSA_DECRYPT_OAEP_3072_SHA256`, HSM | `core_gateway` SA: `useToDecrypt` |
| `robot-kek-mpx-dog` | `ENCRYPT_DECRYPT` | `GOOGLE_SYMMETRIC_ENCRYPTION`, HSM | `core_gateway` SA: encrypt + decrypt; `voice_gateway` SA: decrypt only |

The prod key ring belongs in a separate GCP project (or at least separate IAM)
from dev, so that a developer credential can never decrypt a customer robot's
key. KMS key rings and keys can't be deleted, only their versions can be
destroyed, so pick the names once.

Both gateways run on one VM today, so by default they share its service account.
Giving `voice_gateway` decrypt-only access needs a separate service-account
credential for that container.

### 8.3 Station authentication

Phase B: per-station bearer tokens, stored only as `sha256` in
`provisioning_stations`, sent as `X-MPX-Station-Token` (same pattern as
`robot_rpc.py:235-267`), with a daily quota enforced in SQL. Phase C: mTLS client
certificates once the gateway sits behind a TLS terminator that can verify
them. Admin routes reuse the JWT check from `main.py:689-704` with an `admin`
claim.

### 8.4 Tools (this repo, `tools/`)

| Tool | Does | Never does |
|---|---|---|
| `tools/mpx_provision.py --port COMx --gateway URL --station-token-file F` | §7 flow; `--replace ROBOT_ID` for board replacement (needs an admin-created replacement reservation, D15); `--resume` uses `status`/`reseal` | never sees `S` or `K`, never writes key material to disk or logs |
| `tools/mpx_dev_identity.py --gateway URL` | D11 | runs against a prod gateway (`/dev-robots` returns 404 there) |
| `tools/mpx_id.py check MPX-…` | validates and normalises an ID typed by a human | — |

---

## 9. Failure modes and recovery

| Failure | Detection | Recovery |
|---|---|---|
| Power loss after the `HMAC_UP` burn, before the ID record | `status` → `INCONSISTENT` (no ID record) | `provision` with a fresh reservation burns the ID and seals |
| Power loss after both burns, before the upload | `status` → provisioned, cloud still `reserved` | `reseal` with the same nonce → upload |
| Robot already has a different burned ID | `id_mismatch` | station calls `POST /reservations {mac, robot_id: <burned>}`; the cloud re-issues a reservation for that ID if it is `reserved`/`abandoned` and the MAC matches; otherwise the unit goes to manual review |
| Upload network failure | station retries | `seal` is idempotent; repeating it returns the same response |
| `key_fp` mismatch between robot and cloud | station assert | unit is quarantined (`state` stays `sealed`, flagged); investigate firmware / KMS key mismatch |
| eFuse burn fails (`ESP_ERR_CODING`, `ESP_ERR_BURN_WR_DIS`) | `provision` returns the error | **scrap / RMA**: do not retry blindly on a half-written Reed-Solomon block |
| RNG sanity check fails | `provision` error | do not burn; RMA |
| Robot NVS wiped (`main.cc:60-68`) | identity unaffected (eFuse); `kv` falls back to the eFuse `kv_base` | kv hint (D13) recovers rotated versions; `ds_data` is pushed back from the cloud copy (D17) |
| Robot never comes online after sealing | `sealed` for 30 d | `abandoned`; the key is never loaded, the ID is never reused |
| Unit is lost or stolen | owner/admin report | `revoke` → `NOTIFY` → rejected within one cache TTL. Orders and skill assignments stay. |
| Main board dies (customer robot) | support ticket | **replacement** (D15): admin reservation → same ID, new `S`, `kv_base = max+1` → old versions retired → skills re-sync from `robot_skills`. Scrap the old board. |
| Revoked robot is recovered (e.g. stolen, then returned) | owner/admin report | the board keeps its `S`, so no new burn is needed. Once rotation exists (D13), the admin reinstates the robot at `kv+1`, which derives a new `K` from the same `S`, and the old version stays retired. Before that, the only options are to reinstate the same key (an audited admin decision) or replace the main board. |
| Someone runs `DELETE FROM robots` or renumbers an ID | trigger raises (§6.8) | nothing to recover; use `revoke` |
| KMS unavailable | `robot_keys` unwrap error | serve cached keys until TTL; new robots fail closed with a `503`-equivalent close code; `/healthz` degraded |

---

## 10. Security analysis

| Attacker / event | Today | After this design |
|---|---|---|
| Reads git, the build artifact or `docker-compose.yml` | has the key | nothing: there is no key in any artifact |
| Dumps the database or a backup | every key | wrapped blobs, useless without KMS `decrypt` IAM |
| Compromised factory station | n/a (manual) | sees only sealed blobs; can register **phantom** robots up to its quota (mitigated by quota, reconciliation, `active`-on-first-contact, per-station audit and revoke-by-station) |
| `read_flash` on a robot (Flash Encryption off, legacy) | key in plaintext | no key in flash; only the ID |
| Copies a whole flash image to another ESP32-S3 | works | different `S` → wrong `K` → ingress rejects it |
| `espefuse dump` / JTAG on a provisioned robot | n/a | `HMAC_UP` block reads as zeros |
| Custom firmware on a robot **without** Secure Boot | key in image | can call the HMAC oracle and recover `K` for **that** robot only (`assurance = efuse`) |
| Code execution in signed firmware **with** Secure Boot | key in image | can recover `K` for that robot only; residual risk (`SKILL_PROTECTION.md` §8) |
| Guesses robot IDs | trivial (`MPX-DOG-01`) | 2⁵⁵ space, and an ID alone authenticates nothing |
| Rogue `@MPXPROV` on UART of an `active` robot | n/a | listener not running |
| Rogue `@MPXPROV` on an unprovisioned robot | n/a | it can provision the robot, but the cloud only accepts reservations made by a valid station or admin token |

| `DELETE FROM robots` by mistake or by a compromised admin tool | orders cascade-deleted (`001_checkout.sql:14`) | blocked by the trigger and `RESTRICT` (D15) |

What phase B does not do: prove to the cloud that a sealed blob came from
real Espressif silicon. The DS key burned at the factory (D17) makes that
possible later by OTA (§14).

---

## 11. Acceptance tests

**Unit (CI, no hardware)**

1. `robot_ids`: 10⁶ generated IDs all match `ID_RE` and pass the check; every
   single-symbol substitution fails; `MPX-DOG-01` fails; `normalise("mpx-ajz7j8y0ytds")` passes.
2. Derivation: firmware (`mbedtls_md_hmac` path, host test) and Python both
   reproduce the §6.3 vectors.
3. Seal round-trip: firmware-format blob encrypted with a test RSA key →
   `KmsWrapper` with a mocked decrypt → validation passes; flipping any byte of
   the ID, nonce, magic or format → rejected.
4. `provisioning.py` state machine: every row of §9 as a test; `seal` twice →
   one `robot_keys` row; an expired reservation → 409.
5. `MPX_ENV=prod` with `MPX_DEV_AES_KEY_HEX` set → the process refuses to start.
   `/v1/provisioning/dev-robots` → 404.
5a. Permanence (against a real Postgres): `DELETE FROM robots` raises;
   `UPDATE robots SET robot_uuid=…` raises; revoking a robot with orders and
   `robot_skills` rows keeps all of them.
5b. Replacement: robot with `kv=1`, 2 orders and 3 skills → replacement
   reservation → seal with `kv=2` → `kv=1` is `retired`, `robot_hardware` has
   one live row with the new MAC, orders and skills are unchanged, and a frame
   under the old key is rejected. A second replacement reservation for an MAC
   already bound to another robot → 409.

**Hardware (three sacrificial boards, no Secure Boot first)**

6. `provision` on a blank board → `espefuse summary` shows the `HMAC_UP` block
   with read protection, the `USR_DATA` record and `WR_DIS`; cloud `key_fp` == robot `key_fp`.
7. Power is cut at each step of `provision` (scripted relay) → `status` reports
   the expected state and `provision`/`reseal` completes.
8. Copy the full flash of provisioned board A onto board B → B reports its own
   eFuse ID (or `UNPROVISIONED` if B is blank), never A's, and nothing on B can
   authenticate as A.
9. `nvs_flash_erase` → the robot reconnects with the same ID and key.
10. One board with the full production profile (Secure Boot + FE release +
    `ALLOW_EFUSE_RD_DIS`) → provisioning succeeds (including the DS key, D17),
    `lock` burns `WR_DIS_RD_DIS`, and a second `esp_efuse_write_key` with a
    read-protected purpose fails. Record the RSA-3072 key generation time.
10b. DS smoke test: `esp_ds_sign` over a nonce verifies against the `ds_pubkey`
    stored in the cloud, and `sha256(ds_pubkey)` matches the sealed plaintext.
10c. Replacement on hardware: provision board A, then board B with
    `--replace` → B reports A's ID with `kv_base=2`; A can no longer connect.

**End to end**

11. Fresh robot: `reserved → sealed`, first chat → `active`, the robot token is
    accepted by `/v1/llm`, an MPXE skill deploys and runs.
12. `revoke` → the robot is disconnected or rejected within 60 s, with no gateway
    restart.
13. `grep -rn "aes_key_hex" cloud/` finds only dev-only migration code, and the
    prod database has no non-NULL `aes_key_hex`.

**Contract conformance (gates the pilot, D18)**

14. Golden vectors for every §6.9 item (frames in both directions, subkeys,
    MPXE v2, robot token, sealed blob, control messages). The firmware (host
    test) and the Python gateway both reproduce them byte for byte. Any later
    change that alters a vector fails CI.
15. The production gateway refuses v1 frames. The dev gateway accepts v1 and v2.
16. Pilot cohort rule: a pilot station token seals an `efuse` robot as
    `cohort='pilot'`; a non-pilot token sealing an `efuse` robot → rejected;
    `cohort` can't be updated.
17. Pilot upgrade (sacrificial board): bench-provisioned pilot unit → enable
    Secure Boot + Flash Encryption over USB → `lock` → reseal → `assurance`
    becomes `secure`, and the ID, `key_fp`, orders and skills are unchanged.

**OTA (gates the pilot, D20)**

18. Rollback: an image that fails the health check (for example one that
    can't reach `/v1/ota/check`) reboots back into the previous image, and the
    robot reports the failed version.
19. Refusals: an unsigned image, an image signed with another key, the other
    product's image, a lower version, and a truncated download are all
    refused, and the running image is untouched. Repeat on a pilot (signed
    apps without Secure Boot) and a Kickstarter-profile board.
20. Power cut at every stage (mid-download, after the slot switch, before
    marking valid): the unit always boots a working image.
21. Data partitions: a new `system` image interrupted mid-write is detected
    and re-downloaded, and the robot still walks meanwhile.
22. Identity survives: the ID, `key_fp`, orders and skills are unchanged after
    an update and after a rollback.

---

## 12. Rollout

| Phase | Content | Burns eFuses? | Wire change? |
|---|---|---|---|
| **A — cloud & refactor** (≈1 week) | migration 010 on dev (permanence trigger, `orders` FK → `RESTRICT`); **create the prod database and prod key ring** (D12); `keywrap`, `robot_keys` cache, `provisioning.py` (dev-robots only); `MPX_ENV` guards; firmware `mpx_identity` with **KCONFIG source only** and all call sites switched; `git rm --cached sdkconfig`; `mpx_dev_identity.py` | no | no |
| **B — eFuse provisioning** (≈1.5 weeks) | `mpx_provision` including DS setup (D17), custom eFuse table with `kv_base`, `prov_seal_pub.pem`, KMS keys, station tokens, `mpx_provision.py` (`--replace`); tests 5a–9 and 10b–10c on sacrificial boards **without** Secure Boot | yes (test boards) | no |
| **P — pilot: contract freeze** (≈3–4 weeks, D18) | **wire protocol v2** (§6.9: `WIRE_PROTOCOL_V2.md`, firmware + both gateways, golden vectors); IV-reuse fix first (`main.py:1158`, `:1211`); voice `Device-Id` → robot ID; prod `docker compose` stack on the VM with the prod DB and key ring; final hostnames; pilot station token + bench station; **OTA (D20): A/B layout, rollback and health check, signed images, product guard, `/v1/ota/check` and rollout by cohort**; image-size and bootloader-offset measurement deciding the flash layout (§12.1 #8, §13 #16); tests 14–22; then provision and ship pilot units | yes (pilot units) | **yes**, v2 only |
| **Pilot soak** (≥4 weeks) | pilot units in the field; OTA at least once; one board replacement end to end; no §6.9 changes | no | no |
| **C — production profile & factory** (≈1 week + factory trial) | `sdkconfig.defaults.prod`; settle §12.1 part 2; tests 10–12; factory station(s) and runbook; scale the prod cloud as needed (Cloud SQL, separate VM) | yes (production) | no |
| **Kickstarter units ship** | only after §12.1 parts 1 and 2 are signed off | — | — |
| **D — after launch** (by OTA / cloud only) | turn on rotation (D13) and DS attestation (§14) from the cloud side; mTLS for stations; GKE and per-product stacks (D16) | no | no: the messages already exist in v2 |

A, B and P must finish before the first pilot unit ships. C must finish before
the factory builds the first Kickstarter unit. Phase D needs no new eFuse burn
and no wire change, because v2 already defines everything it uses.

### 12.1 One-way doors

No robot is outside the office yet, so every item below is still free to
change. Once a unit ships, each item is fixed for that unit's lifetime (or
needs a USB re-flash or a recall). The pilot moves most of them earlier.

**Part 1: settle before the first pilot unit**

| # | Decision | Fixed by | Where |
|---|---|---|---|
| 1 | ID format, alphabet, check symbol, `MPX-` prefix | label + eFuse | D1, §6.1 |
| 2 | `USR_DATA` layout (ID, format byte, flags, `kv_base`) | eFuse | §6.2 |
| 3 | Derivation label `"mpx/v1/robot-key"` and message layout | eFuse secret + cloud | D6, §6.3 |
| 4 | Which key blocks are burned, with which purposes (`HMAC_UP`, DS; leave room for Secure Boot digest + XTS) | eFuse | D10, D17 |
| 5 | Burning the DS attestation key | it has to happen before any `lock`, so do it at bench provisioning | D17 |
| 6 | Wire contract v2 (§6.9) | every pilot unit speaks it until OTA | D18 |
| 7 | Final hostnames and TLS trust (CA bundle, no pinning) | firmware, changeable by OTA but only if OTA itself still reaches the server | D16, D18 |
| 8 | Partition table (including the two OTA app slots and their size), flash size and bootloader offset. The secure bootloader may not fit below `0x8000`. Pilot units must use the **same** layout and flash size as customer units, or they become a separate firmware variant forever. | flash layout (OTA can't move the bootloader or the partition table) | D20, §13 #7, #16 |
| 8a | Bootloader OTA settings: app rollback on; anti-rollback built in (switched on by the `SECURE_VERSION` eFuse only on secure units) | bootloader (not updated by OTA) | D20 |
| 8b | OTA signing key, and signed-app checks on pilot units without Secure Boot | the first pilot image; changing the key later needs a transition image signed with the old key | D20 |
| 9 | Sealing-key ID baked into the first firmware (`seal_key_id`) | firmware, can be changed by OTA | D16 |
| 10 | Prod database and prod key ring exist and are separate from dev | infrastructure | D12, D16 |

**Part 2: settle before the first Kickstarter unit**

| # | Decision | Fixed by | Where |
|---|---|---|---|
| 11 | Secure Boot signing key(s), plus `ALLOW_EFUSE_RD_DIS` in the bootloader | eFuse digest + bootloader | §3.2 |
| 12 | Flash Encryption mode (release), `DIS_DOWNLOAD_MODE` / secure download mode, JTAG disable | eFuse | §3.2, D10 |
| 13 | The `lock` step and its ordering on the line | eFuse | D10 |

Part 1 items should be tested on pilot hardware from the real production
batch. Part 2 items should be tested on the production profile (test 10), and
must work on a unit whose identity was burned **before** Secure Boot was enabled
(the pilot upgrade path, test 17).

---

## 13. Open decisions

| # | Decision | Recommendation |
|---|---|---|
| 1 | ID prefix and whether to reserve a symbol for product line / hardware revision | keep `MPX-` + 12; put the hardware revision in the DB, not in the ID |
| 2 | ~~Burn the ID into eFuse or keep it in flash~~ | **resolved:** eFuse. IDs are permanent (D15), and board replacement re-burns the same ID. |
| 3 | ~~KMS location and protection level~~ | **resolved:** `asia-east2`, HSM (§3.5), one key ring per environment (§8.2) |
| 4 | Station credential: tokens vs mTLS | tokens in phase B, mTLS on the `provision.<domain>` hostname in phase C (D16) |
| 5 | Which skills require `assurance ∈ {secure, efuse}` | prod accepts only `secure` (D12), so this only matters in dev |
| 6 | ~~Field units: USB self-enrollment or board swap~~ | **resolved:** there are no field units (D12) |
| 7 | Partition-table offset if the secure bootloader exceeds `0x8000` | **urgent:** measure in phase P, before the first pilot unit; it is free to change now and fixed after the first unit ships (§12.1 #8) |
| 8 | ~~Merge the voice `Device-Id` into the robot ID~~ | **resolved:** part of contract v2 (§6.9 C9), done in phase P |
| 9 | Burn the DS attestation key at the factory | **yes** (D17); confirm the RSA key generation time fits the line |
| 10 | Separate prod database and key ring, with the current DB becoming dev | **yes**, in phase A (D12) |
| 11 | Who may approve board replacements, and is a returned board required first | support lead + one admin; require the old board's return (or a photo of its label) before approving |
| 12 | Do pilot units need to run their owners' own firmware? | assume **yes**, so pilot units stay without Secure Boot (D18). If no pilot owner needs it, pilot units could ship with the full production profile instead, which would pull §12.1 part 2 forward. |
| 13 | Which marketplace skills may be deployed to `efuse` (pilot) units | free skills and MangDang's own: yes. Paid third-party skills: only with the developer's opt-in. |
| 14 | How many pilot units, and the contract-freeze soak period | pilot size decides how much bench time is needed; soak at least 4 weeks with no §6.9 change |
| 15 | Is OTA reliable enough to be the only update path for pilot units? | must be proven in phase P (D20, tests 18–22). Pilot owners should also have a documented USB recovery procedure. |
| 16 | Flash size and app slot size for A/B OTA on MPX and MP4 | **urgent, blocks the pilot:** measure the release image with `idf.py size`; prefer 32 MB flash on production hardware; otherwise shrink the image (shared data partition) before giving up `storage` (D20) |
| 17 | Who approves a release for each cohort, and when a release is mandatory | firmware lead approves; pilot cohort first, Kickstarter after a soak with no rollbacks; mandatory only for security fixes |

---

## 14. Hardware attestation protocol (RSA_DS), enforced after launch

The key material is set up at provisioning (D17), on the bench for pilot units
and at the factory for Kickstarter units, because it can't be added after
`lock`. The challenge/response messages are part of contract v2 (§6.9 C7), so
every unit can answer from its first boot: the cloud sends a nonce, the robot
signs it through `esp_ds_sign`, and the cloud verifies it against
`robots.ds_pubkey`. Only **enforcement** waits until phase D, when the cloud
starts rejecting robots that fail the challenge. This closes the
phantom-robot gap (§10) and is the hardware piece of `SKILL_PROTECTION.md` E3.

---

## 15. References

- ESP-IDF, HMAC (ESP32-S3): https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/hmac.html
- ESP-IDF, eFuse Manager (ESP32-S3), coding schemes, custom tables, `esp_efuse_write_key`: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/efuse.html
- `esp_efuse_write_key` implementation (auto read protection for HMAC purposes, BLOCK9 quirk): https://github.com/espressif/esp-idf/blob/master/components/efuse/src/efuse_controller/keys/with_key_purposes/esp_efuse_api_key.c
- ESP-IDF, Secure Boot v2 (ESP32-S3), "Burning read-protected keys": https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/secure-boot-v2.html
- ESP-IDF, Flash Encryption (ESP32-S3): https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/flash-encryption.html
- ESP-IDF, Random Number Generation (ESP32-S3): https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/random.html
- espefuse `burn-key` (ESP32-S3): https://docs.espressif.com/projects/esptool/en/latest/esp32s3/espefuse/burn-key-cmd.html
- Cloud KMS, key purposes and algorithms: https://cloud.google.com/kms/docs/algorithms
- Cloud KMS, `generateRandomBytes`: https://cloud.google.com/kms/docs/reference/rest/v1/projects.locations/generateRandomBytes
- Cloud KMS, envelope encryption: https://cloud.google.com/kms/docs/envelope-encryption
- RFC 9562, Universally Unique IDentifiers: https://www.rfc-editor.org/rfc/rfc9562
- Crockford Base32: https://www.crockford.com/base32.html
- OWASP Key Management Cheat Sheet: https://cheatsheetseries.owasp.org/cheatsheets/Key_Management_Cheat_Sheet.html

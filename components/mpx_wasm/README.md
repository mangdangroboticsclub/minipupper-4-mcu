# mpx_wasm

The WebAssembly skill runtime: WAMR, the MPXE encrypted-skill envelope, the
74-function host ABI, and the registry/runner/events/autorun machinery around
them.

Ported from `main/{wasm,sdk,skills}/` of the MPX-Dog firmware.

## The ABI is frozen

`MPX_ABI_VERSION` is **4** and must stay byte-compatible. Every skill anyone
already owns was compiled against it, and `.mpxe` skills are encrypted with a
key bound to `CONFIG_MP4_ROBOT_UUID` — their owners cannot rebuild them. Adding
a host function is fine; changing or reordering one is not.

## What changed in the port

| | MPX-Dog | Here |
|---|---|---|
| Skill storage | LittleFS root, `/moonwalk.wasm` | `<DATA>/mpx_skills/`, via `claw_paths_join()` |
| Filesystem access | `fs::` over `littlefs_manager` | `fs::` over POSIX, chrooted to the skills dir |
| AES key symbol | `CONFIG_APP_CHAT_AES_KEY_HEX` | `CONFIG_MP4_SKILL_AES_KEY_HEX` |
| Robot UUID | `CONFIG_APP_ROBOT_UUID` | `CONFIG_MP4_ROBOT_UUID` |
| `crypto::` | shared with the cloud chat pipeline | local, `src/mpx_crypto.cc` |
| Coupling to the robot | same component, direct calls | separate component, one-directional |

**The skill root is a chroot.** `src/mpx_skill_fs.cc` reinterprets every path
as relative to `<DATA>/mpx_skills`, and refuses anything containing `..`
outright rather than trying to normalise it. A normaliser is a thing you have
to get exactly right against input that arrives from a marketplace; a refusal
is a thing you cannot get wrong. `registry.cc` and `wasm_sandbox.cc` call the
same two functions they always did and did not need changing.

A side benefit of living under the DATA root: skills appear in ESP-Claw's
`/api/files` browser and to `cap_files` for free, so the agent can list and
delete them without any of this being explained to it.

**`crypto` survived the chat pipeline.** It had two consumers in MPX-Dog: the
AES-GCM frames of the cloud chat socket, and the MPXE envelope. The chat
pipeline is gone — the agent runs on the device now — so `wasm_decrypt.cc` is
the only caller left, which is why it lives here rather than in a shared
component.

## Boot order, and why it is what it is

```
mpx_robot_init()          servos, gait, IMU
mpx_wasm_init()           WAMR + host functions + the hook table
    ... wifi, http_server_start(), app_claw_start() ...
mpx_wasm_start_skills()   rescan, IMU event watcher, autorun
```

`mpx_wasm_start_skills()` is deliberately the last thing `app_main` does. An
autorun skill that crashes the robot runs again on the next boot, and the next.
A user with no serial cable then has a brick, and the thing that bricked it
arrived from a marketplace. By the time this runs the web server is listening,
so they can always uninstall it. That is the difference between a bad skill and
a brick, and it is the only reason the two `mpx_wasm_*` calls are not adjacent.

Recovery without a cable: `POST /v1/skills/safe-mode/clear`, or
`mpx_wasm_clear_safe_mode()`. Autorun disables itself after three failed boots
anyway — the counter is bumped *before* the skill starts and cleared by a task
that sleeps 20 s and then declares it proven.

## Threading

`load_and_run_bytes()` runs the module on a **pthread**, not an `xTaskCreate`d
FreeRTOS task. WAMR's ESP-IDF platform layer calls `pthread_self()` internally,
so every operation that touches WAMR — load, instantiate, lookup, execute —
has to be on a thread created that way. This is not stylistic and it is not
optional.

The watchdog is a 10 ms polling loop because ESP-IDF has no
`pthread_timedjoin`. On expiry it calls `wasm_runtime_terminate()` and sets
`s_hard_killed`, which is what stops `on_stop()` being called on an instance
the runtime has already unwound.

## Memory

| | |
|---|---|
| WAMR runtime heap | 128 KB, PSRAM |
| Operand stack | 32 KB, PSRAM |
| Linear memory | 128 KB, PSRAM |
| Native pthread stack | 16 KB, **internal** |

The operand stack is the interpreter's value stack, not linear memory. 8 KB
was too small — richer skills overflowed it — and WAMR takes it from PSRAM, so
32 KB is cheap.

`NATIVE_SYMBOLS` in `wasm_host_functions.cc` is **intentionally not `const`**.
WAMR sorts that table in place with `qsort()`; a const table lands in flash and
the write raises a cache-error panic on the ESP32-S3.

## The seam to mpx_robot

`mpx_robot` does not depend on this component. `mpx_wasm_init()` hands it two
predicates — "is a skill running", "does it own the joints" — through
`mpx_robot_set_skill_hooks()`. That keeps the two out of a requirement cycle
and lets `mpx_robot` link on a build with no WASM support at all.

The dependency the other way is deep and deliberate: the host functions use 45
distinct `robot::` symbols. `mpx_robot` therefore exposes `src/` publicly, as a
documented exception. Nothing except this component should use it.

## Skill manifest

Embedded in the `.wasm` as a custom section named `mpx`:

```json
{ "slug": "moonwalk", "provides_gait": "moonwalk", "abi": 4,
  "autorun": false, "behaviour": true, "on": ["boot", "imu.lifted", "chat:dance"] }
```

The metadata travels inside the artifact, so whatever moves the module moves
the metadata with it and the two cannot drift apart. A module with no `mpx`
section is still valid and runnable — it just cannot register a gait or a
trigger. The parser treats the file as untrusted input and bounds-checks every
length; a truncated or hostile module must produce "no metadata", never a read
past the end.

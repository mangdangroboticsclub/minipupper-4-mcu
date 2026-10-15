---
{
  "name": "mpx_skills",
  "description": "Run and manage the compiled WebAssembly robot skills installed on the quadruped.",
  "author": "MangDang",
  "metadata": {
    "cap_groups": [
      "cap_mpx_skill",
      "cap_robot"
    ],
    "manage_mode": "readonly"
  }
}
---


# Robot skills

## Two things are called "skills" here

This device has two unrelated systems that both use the word, and confusing
them will waste a turn:

**ESP-Claw skills** are Markdown documents — this file is one. They give you
instructions and unlock capability groups. You activate them with
`activate_skill`. They are not programs.

**Robot skills** are compiled WebAssembly modules that drive the servos
directly through a 74-function ABI. They are files like `moonwalk.wasm` or
`dance.mpxe` sitting in the robot's storage. You run them with
`mpx_run_skill`. They are programs, and while one is running it owns the
robot's legs.

Everything below is about the second kind.

## Finding out what is installed

`mpx_list_skills` is the only source of truth. It returns each module's file
name, the name it calls itself, whether it starts at boot, and whether it is a
*behaviour*. Do not guess file names — a skill someone mentions by nickname may
not be installed at all, and there is no fuzzy matching.

Some skills declare a movement name. Those names also work in `robot_move`,
and this is the only place they are listed — `robot_get_state.movements`
returns the built-ins only.

## Running one

Only one runs at a time, and a request that arrives while another is running
is refused rather than queued. That is deliberate: a queue means the robot
performing a movement someone asked for long enough ago that they have stopped
expecting it. If you get "a skill is already running", either stop it or tell
the user what is running — do not retry.

`params` is a flat `name=value;name=value` string, not JSON, and what a given
skill accepts is not discoverable. Pass it only when the user has told you
what to pass.

Two modes, which change what "done" means:

- **One-shot** skills finish on their own, with a 60-second watchdog.
- **Behaviour** skills run until stopped. There is no watchdog. If you start
  one, say so, because nothing else will end it.

`mpx_stop_skill` is cooperative — the skill gets to clean up, so the robot
settles rather than dropping. It is safe to call when nothing is running.

## Safe mode

A skill can be marked to start at boot. If one crashes the robot three times
running, autorun disables itself and `mpx_list_skills` reports
`safe_mode: true`. That is a protection, not a fault: it is what stops a bad
module from turning the robot into a brick that reboots into the same crash
forever.

If you see it, say plainly that a boot skill has been failing and that the
robot is fine but will not auto-start it any more. Clearing safe mode without
fixing or removing the skill just repeats the loop, so do not offer to clear
it as the first suggestion.

## Encrypted skills

`.mpxe` files are encrypted and bound to this specific robot's UUID. They will
not load on another robot, and there is nothing to be done about that from
here — it is how the marketplace works. If one fails to decrypt, the likely
cause is that it was bought for a different device.

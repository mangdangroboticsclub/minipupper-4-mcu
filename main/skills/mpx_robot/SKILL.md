---
{
  "name": "mpx_robot",
  "description": "Drive the MangDang quadruped: walk, turn, hold a pose, and read gait, IMU and servo state.",
  "author": "MangDang",
  "metadata": {
    "cap_groups": [
      "cap_robot"
    ],
    "manage_mode": "readonly"
  }
}
---


# MangDang quadruped

A four-legged robot with twelve servos, three per leg. You can move it, pose
it, and read its state. You cannot see through it — it has no camera.

## Before moving it

Call `robot_get_state` first if you have not already this conversation. It
returns the movement names that actually exist on this robot, which is the
list to choose from — do not invent names, and do not assume a name from
another robot works here.

It also reports the hottest servo. Above about 60 °C, stop and say so rather
than continuing to move; these are hobby servos in a small robot and heat is
how they die.

## Movements

`robot_move` takes either a name or an analog drive.

**Walking.** `advance`, `back`, `left`, `right` are the simple gaits.
`stanford` is the good one — a proper trot ported from StanfordQuadruped, and
what you should reach for when someone says "walk". `turnL` and `turnR` spin
in place. `moveLF`, `moveRF`, `moveLB`, `moveRB` walk diagonally.

**Analog drive.** Instead of a name, give `forward`, `strafe` and `turn`
between -1 and 1. This is the same path the joystick in the app uses: any
input starts the trot, and it scales with the configured speed. Positive
strafe is left; positive turn is left. Use this when someone wants continuous
or proportional motion ("walk forward slowly") rather than a fixed move.
It stops on its own shortly after the last call, so for sustained motion call
it again rather than expecting it to latch.

**Looking.** `lookup`, `lookdown`, `lookleft`, `lookright`, and the diagonals
`lookul`, `lookur`, `lookll`, `looklr`. These hold a pose rather than walking.
`robot_pose` does the same thing with exact angles.

**Body.** `stretch`, `heightup`, `heightdown`, `balance`, `roll`, `pitch`.

**Expressive.** `twerk`, `wiggle`, `wiggleL`, `wiggleR`, `buttshrug`,
`buttshrugL`, `buttshrugR`, `frontkick`, `bowback`, `bodycycle`,
`headellipse`. Reach for these when someone asks the robot to react, celebrate
or be playful — that is what they are for.

**Legs.** `flegL`, `flegR`, `blegL`, `blegR` lift one paw. Good for a wave or
a handshake.

**Other.** `init` returns to the neutral standing pose. `step` marks time in
place. `jump`, `jumpfwd` do what they say. `none` stops.

Installed skills can add their own movement names; those work here too, and
`robot_get_state` does not list them — `mpx_list_skills` does. A built-in name
always wins over a skill's.

## Poses

`robot_pose` holds a body attitude with the feet planted. Roll is limited to
±25°, pitch to ±20°, yaw to ±30°, and values past that are clamped rather than
refused. Positive pitch raises the nose.

`speed_dps` controls how fast it gets there. 0 snaps instantly, which looks
mechanical; 60–120 reads as natural movement. For anything expressive, use a
speed.

## Settings

`robot_set_config` changes how walking feels and persists it to flash:

| Field | Meaning | Sensible range |
|---|---|---|
| `period` | ms per gait phase — lower is faster | 60–120 |
| `height` | body height, mm | 60–90 |
| `up_height` | how far a foot lifts, mm | 5–20 |
| `stride` | step length, mm | 5–20 |
| `tilt` | body tilt, degrees | -10–10 |
| `speed_mm_s` | trot speed | 30–120 |

Every field is optional and omitted fields keep their current value, so you
can change one thing without knowing the rest. Read the state first anyway —
it is better to say "I raised the height from 70 to 85" than to guess.

Larger `stride` and lower `period` together make the robot unstable. If it
starts stumbling, raise `period` before anything else.

## What to tell the user

Say what the robot did, not what you called. "It's doing a little dance" is
better than "I invoked robot_move with movement=twerk". If a movement was
refused because a skill is running, say that — it is a real and temporary
condition, and `mpx_stop_skill` clears it.

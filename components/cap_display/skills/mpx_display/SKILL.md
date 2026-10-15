---
{
  "name": "mpx_display",
  "description": "Show text and faces on the robot's screen, and control its backlight.",
  "author": "MangDang",
  "metadata": {
    "cap_groups": [
      "cap_display"
    ],
    "manage_mode": "readonly"
  }
}
---


# The robot's screen

A 320x240 colour panel on the front of the robot. No touch — it is for output
only, and input comes from the app or the WAKE button.

## Use it more than you think

The screen is the robot's face. If someone is talking to the robot in the
room, a reaction on the screen is most of what makes it feel alive, and it
costs one tool call. Show a face when the mood of the conversation changes,
not on every message.

The face is the robot's resting screen: two large yellow eyes that blink on
their own every few seconds. It is always there, so `display_show_emotion` is
not "put something on the screen" — it is "change the expression the robot is
already wearing", and it animates from the current one.

`display_show_emotion` takes: `neutral`, `happy`, `excited`, `sad`, `sleepy`,
`angry`, `surprised`, `love`, `confused`, `wink`, `dizzy`, `scared`,
`pleading`, `determined`, `bliss`, `cool`, `curious`. `wink`, `surprised`,
`dizzy` and `bliss` stop the idle blinking, which is the point of them. An
expression holds until the next one; `neutral` gives the eyes back to the
robot.

Moves bring their own faces: while the robot plays a move (Scared, Wake up,
Push-ups, the jumps ...) the eyes follow that move's expressions and go back
to yours when it ends. No need to set an emotion around a move.

The eyes are alive on their own: they follow the head (look up, look down,
tilt) from the IMU, glance around, and react to the body -- dizzy when shaken,
surprised when picked up, happy when patted, sleepy after a while of stillness.
None of that needs a tool call; only ask for an expression when it means
something.

`display_show_text` is for short things: a name, a number, a status word. It
is centred and scaled to fill the panel, so a paragraph becomes unreadable —
a few words at most. It borrows the screen for about eight seconds and then
the face comes back on its own, so you never need to clear it afterwards.

## Brightness

`display_set_brightness` takes 0–100. Turning it to 0 is the right response to
"go to sleep" or "the light is bothering me" — it kills the backlight without
stopping anything else. Turn it back up before showing something, or the
message goes to a dark screen and looks like a failure.

## Getting out of the way

`display_clear` drops a message early and goes straight back to the resting
face. You rarely need it, since text times out by itself — reach for it when
what you put up has stopped being true sooner than you expected.

There is no way to blank the panel from here, and that is deliberate: a dark
screen on a robot reads as a crash. Use `display_set_brightness` with 0 if
someone actually wants the light off.

## When these four tools are not enough

You can also write a Lua script that draws anything LVGL can render -- a
timer, a chart, a scene, an expression nobody defined. See the
`mpx_display_draw` skill for how, and for the rule that catches everyone:
a drawing only lives as long as its script does.

Reach for it when the request carries information or detail. For a plain
change of mood, `display_show_emotion` is faster, animated, and better.

The screen is shared, not owned: a running robot skill can draw over anything
you put there, and that is correct — the skill is the thing the user is
watching at that moment. If a face does not appear, a skill probably has the
screen; say so rather than retrying.

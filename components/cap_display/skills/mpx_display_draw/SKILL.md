---
{
  "name": "mpx_display_draw",
  "description": "Draw anything on the robot's 320x240 screen by writing a Lua script, when the ten built-in expressions are not enough.",
  "author": "MangDang",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ],
    "manage_mode": "readonly"
  }
}
---


# Drawing on this robot's screen

You can write code that draws on the panel. `lua_module_lvgl` is available, so
anything LVGL can render, you can put on this robot's face — a shape nobody
defined, a chart, a countdown, a face with a mouth.

## First: you usually should not

`display_show_emotion` already animates the eyes to `neutral`, `happy`,
`excited`, `sad`, `sleepy`, `angry`, `surprised`, `love`, `confused`, `wink`,
`scared`, `pleading`, `determined`, `bliss`, `cool` or `curious`.
It is one tool call, it takes over instantly, it transitions smoothly from
whatever the face is currently doing, and it costs nothing.

If someone says "look sad", use that. Writing a script to draw a sad face is
slower, replaces the animation with something static, and is worse at the job.

Write a drawing when the request is genuinely not an expression:

- something with information in it — a timer, a score, a temperature, a name
- a shape or scene that is not a face
- something the person is directing in detail ("put a red circle in the
  corner", "draw the battery as a bar")

## The panel

- **320 wide, 240 tall.** Landscape. There is no touch controller, so nothing
  you draw can be pressed — no buttons, no sliders, no keyboard. They will
  render and then sit there doing nothing, which looks broken.
- Colour is RGB565. About 65 thousand colours, so smooth gradients band
  visibly. Flat colour looks better here than it does on a phone.
- It is seen from across a room on a small robot. Big shapes and few words.

The resting face, so you can match it or deliberately break from it:

| | |
|---|---|
| background | `#000000` |
| eye colour | `#FFE631` |
| eye size | 59 x 114, corner radius 8 |
| eye centres | (82, 120) and (236, 120) |
| status line | `#6B6B6B`, bottom centre |

## How to run a drawing

Three steps. All of them matter.

**1. Write the script** with `write_file` to `/fatfs/scripts/`, for example
`/fatfs/scripts/draw_timer.lua`. Both `write_file` and the Lua run tools
require **absolute** paths and reject relative ones. `/fatfs` is the data root
on this robot — it has no SD card slot, so that never changes.

**2. Run it with `lua_run_script_async`**, not `lua_run_script`, and always
with these arguments:

```json
{"path": "/fatfs/scripts/draw_timer.lua", "exclusive": "display", "replace": true}
```

`exclusive: "display"` means one drawing owns the panel at a time.
`replace: true` means yours takes over from whatever was drawing before,
instead of being refused.

**3. Stop it** with `lua_stop_async_job` when the thing it shows has stopped
being true. The face comes back by itself.

## The rule that catches everyone

**A drawing lives only as long as its script does.** `lvgl.init()` takes the
panel exclusively; when the script ends, the panel is released and the robot's
face returns immediately.

So a script that draws and exits shows nothing — the drawing is gone within a
frame. To leave something on screen, the script must stay alive, and the way to
do that is `lvgl.run()`, which loops until the job is stopped and then returns.

That is why the job is async. A synchronous `lua_run_script` would sit there
holding your turn until it timed out.

## A drawing that stays up

```lua
local lvgl = require("lvgl")

lvgl.init()

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

lvgl.label(scr, {
    text = "12:30",
    align = "center",
    text_color = "#FFE631",
})

scr:load()

lvgl.run()      -- holds the panel until the job is stopped
lvgl.deinit()   -- and then hands it back to the face
```

Stop it with `lua_stop_async_job` and the face returns.

## A drawing that shows itself and leaves

When the thing is momentary — a countdown, a reaction, a flash of something —
end it from inside the script rather than making the user ask you to stop it:

```lua
local lvgl = require("lvgl")

lvgl.init()

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

local label = lvgl.label(scr, { text = "3", align = "center", text_color = "#FFE631" })
scr:load()

for i = 3, 1, -1 do
    label:set_text(tostring(i))
    lvgl.process_events(1000)   -- draws, and waits a second
end

lvgl.deinit()
```

`lvgl.process_events(ms)` really does wait for up to that long — it sleeps in
20 ms steps, staying responsive to a stop request — so it doubles as the sleep
in a timed loop. `lvgl.run()` is for when the script has nothing to do but
wait.

## Drawing the face yourself

Two rounded rectangles. Use this when you want the face with something added,
or an expression that is not in the list:

```lua
local lvgl = require("lvgl")

lvgl.init()
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

local function eye(dx, w, h, r, dy)
    return lvgl.object(scr, {
        w = w, h = h,
        align = "center", x = dx, y = dy,
        bg_color = "#FFE631",
        radius = r,
        border_width = 0,
    })
end

eye(-77, 59, 114, 8, 0)   -- left, neutral
eye(77, 59, 114, 8, 0)    -- right, neutral

scr:load()
lvgl.run()
lvgl.deinit()
```

Shorter and rounder reads as happy; short, high and square reads as angry; a
thin slit reads as sleepy. Changing the height and radius does most of the
work — see `display_show_emotion` for shapes that are known to read correctly.

## Things that will bite you

- **Do not call `display.init()` and `lvgl.init()` in the same script.** Only
  one can own the runtime. Use `lvgl` unless you need raw framebuffer writes.
- **Always `lvgl.deinit()` before the script ends.** It is cleaned up
  automatically if the script dies, but do not rely on that.
- **Do not create interactive widgets.** No touch controller. A button is a
  rectangle that ignores you.
- **Object handles die** after `obj:delete()`, after their parent is deleted,
  or after `lvgl.deinit()`. Using one after that is a crash, not an error.
- **Test your text length.** A label that is too wide runs off the panel
  rather than wrapping, unless you set a width on it.
- **Do not leave a drawing up forever.** If it is not showing something that is
  still true, stop the job. The face is what the robot should be wearing when
  it has nothing to say.

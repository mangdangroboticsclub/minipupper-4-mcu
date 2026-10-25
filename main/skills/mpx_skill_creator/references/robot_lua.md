# The robot's Lua

Lua 5.5 on the robot itself. The same VM runs a script when you call
`lua_run_script` and when the owner presses **Run** on it in the app's Files
page, so write scripts that make sense either way.

## How a script runs

- One script at a time on the whole robot. A run waits up to 10 s for the one
  before it, then fails with "Lua is busy".
- Time limit: what you pass as `timeout_ms` (default 30 s, at most 60 s); the
  app's Run allows 60 s. Past it the script is stopped with "script timed out".
  While it runs, the conversation waits: keep scripts short.
- `print(...)` is the output. About 2 KB comes back; keep it to a few lines.
- Globals: a script gets its own. Anything it does not define is read from
  the shared libraries below; what it defines is gone when it ends.
- `args`: a table, always present. It holds `lua_run_script`'s `args`, and is
  **empty when the owner runs the script from the app** -- so every argument
  needs a default.
- `require("name")` looks for `name.lua` in the script's own folder first.
- Not available: ESP-Claw's Lua modules (`arg_schema`, `delay`, `lvgl`,
  `storage`, ...). Do not `require` them.
- There is no shell, so `os.execute(...)` cannot work: on this firmware it
  raises instead of silently doing nothing (it used to return `nil` without
  complaining, which made `os.execute("sleep 0.05")` look like a wait that
  never happened). To wait, use `robot.delay_ms(ms)`.

## Arguments

```lua
local seconds = tonumber(args.seconds) or 20
local colour  = type(args.colour) == "string" and args.colour or "red"
if seconds < 1 or seconds > 600 then
  error("seconds must be 1..600")
end
```

## tools -- everything the agent can do

```lua
local res, err = tools.call("display_draw", {
  background = "black",
  seconds = seconds,
  shapes = { { type = "circle", x = 160, y = 120, r = 50, color = colour } },
})
if not res then error(err) end
```

- Same names and arguments as your tools (`robot_move`, `robot_pose`,
  `robot_get_state`, `display_draw`, `display_show_emotion`,
  `display_show_text`, `display_clear`, `mpx_run_skill`, ...). `tools.list()`
  returns the names.
- Returns the tool's reply as a table (`res.ok`, ...), or `nil, message` when
  the call itself failed. A reply such as `{ok = false, error = "no such
  emotion"}` is a table: check `res.ok`.
- Lists are Lua sequences: `points = { {10, 20}, {30, 40} }`.
- `lua_run_script` is not reachable from a script.

## robot

- `robot.delay_ms(ms)`: wait; counts against the time limit.
- `robot.gait(name)`, `robot.get_mode()`: start a movement by name / the
  current one. `tools.call("robot_move", {movement = name})` also reaches
  skills that add movements.
- `robot.get_config()` -> `{period, height, up_height, stride, tilt}`;
  `robot.set_config(period, height, up_height, stride, tilt, speed)`.
- `robot.imu_read()` -> `{ax, ay, az, gx, gy, gz, mx, my, mz, mag_valid}`.
- Servos (ids 1..12): `robot.set_servo_angle(id, deg)` then `robot.flush()`;
  `robot.read_angle_cdeg(id)`, `robot.read_temperature(id)`,
  `robot.read_current(id)`. Leg IK: `robot.ik_fr/ik_fl/ik_rr/ik_rl(x, th0, z)`
  then `robot.flush()`. Prefer `robot_move` / `robot_pose` through `tools`
  unless the user wants joint-level motion.

## display -- pixel drawing

The screen is `display.width()` x `display.height()` (320 x 240), (0,0) top
left. Paint, then `display.show(hold_ms)`; the face comes back after the hold
(`0` = until something else takes the screen). The picture outlives the
script, so paint, show and return.

`display.show()` and `tools.call("display_draw", {seconds = ...})` do **not
wait**: they put the picture up and hand the hold to a timer, so it stays
there while the script carries on (or ends). Inside a loop that means "draw
the next frame immediately" -- see [Animating the screen](#animating-the-screen).

- `display.clear([color])`
- `display.rect(x, y, w, h, color [, {radius = r, outline = px}])`
- `display.circle(cx, cy, r, color [, {outline = px}])`
- `display.line(x1, y1, x2, y2, color [, width])`
- `display.arc(cx, cy, r, start_deg, end_deg, color [, width])`, 0 deg = 3
  o'clock, clockwise
- `display.text(x, y, str, color [, {size = 14|28|48, align = "left"|"center"|"right", w = px}])`
- `display.pixel(x, y, color)`, `display.blit(x, y, w, h, rgb565_bytes)`
- `display.face()`: the eyes, now

Colours: `0xRRGGBB`, `display.rgb(r, g, b)`, `"#rrggbb"` or a name (`"red"`).
`display_draw` through `tools` is usually simpler (ovals, polygons, text
centred on a point); use `display.*` for computed or animated pictures.

`display.text(x, y, ...)` puts the **top-left** of the text box at `x, y`;
`align` then centres (or right-aligns) the string inside `w` pixels of it.

## Animating the screen

An animation is a loop that draws one frame and then **waits**:
`robot.delay_ms(frame_ms)` is the wait that gives the rest of the robot its
turn -- Wi-Fi, the chat socket, the audio tasks. A loop with no real wait in
it (`while true do end`, or a wait that silently does not exist such as
`os.execute("sleep ...")`) simply keeps the CPU busy: the picture stutters,
the app's poll is answered late, and it is what tripped the task watchdog
before the VM learned to yield to the scheduler.

```lua
-- Bouncing logo. args.duration = seconds on screen, args.frame_ms = pace.
local w, h = display.width(), display.height()
local logo_w, logo_h = 60, 40
local duration = math.max(1, math.min(tonumber(args.duration) or 15, 50))
local frame_ms = math.max(30, math.min(tonumber(args.frame_ms) or 60, 1000))

-- Count frames instead of reading the clock: each frame costs exactly
-- frame_ms, and os.time() can jump when the robot syncs its time.
local frames = math.max(1, math.floor(duration * 1000 / frame_ms))

local x, y = math.random(0, w - logo_w), math.random(0, h - logo_h)
local vx, vy = 3, 2
local colours = { "red", "green", "blue", "yellow", "cyan", "magenta" }

for i = 1, frames do
  x, y = x + vx, y + vy
  if x <= 0 or x + logo_w >= w then vx = -vx end
  if y <= 0 or y + logo_h >= h then vy = -vy end
  x = math.max(0, math.min(x, w - logo_w))
  y = math.max(0, math.min(y, h - logo_h))

  display.clear(0x000000)
  display.rect(x, y, logo_w, logo_h, colours[i % #colours + 1], { radius = 6 })
  display.text(x, y + 12, "DVD", 0xFFFFFF, { size = 14, align = "center", w = logo_w })
  display.show(0)              -- put this frame up
  robot.delay_ms(frame_ms)     -- and let the robot breathe until the next one
end

display.face()
print("bounced for " .. duration .. " s")
```

- 40-100 ms per frame (10-25 fps) is a good place to start; raise `frame_ms`
  if the picture lags behind the drawing.
- `frames * frame_ms` is time the script spends, and the whole script has a
  budget (60 s from the app's **Run**, 30 s default for `lua_run_script`), so a
  30 s animation leaves 30 s for everything else it has to do.
- `display.show(0)` means "keep this frame until the next one"; call
  `display.face()` when the animation is over.
- `tools.call("display_draw", ...)` per frame works, but it re-sends the whole
  picture as JSON every frame; `display.*` is for loops.

## fs -- files

Paths are relative to the storage root: the agent's data root is `/agent`
here, the app's scripts are in `/lua`.

- `fs.read(path)` -> string or nil; `fs.exists(path)`; `fs.list(dir)` ->
  `{ {name, size, is_dir}, ... }`; `fs.info()` -> `{total, used}`
- `fs.write(path, text)`, `fs.delete(path)`: **ask the owner in the app** and
  wait up to 60 s for an answer; false if refused or nobody answers. (If the
  owner turned on "Allow writes" in Settings, they go through at once.) Do
  not write files from a skill script unless that is the point of the skill.

## Errors

End a script with a short success line. For a failure, `error("what went
wrong")`: the message comes back to you, and to the owner in the app.

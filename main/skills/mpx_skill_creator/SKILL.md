---
{
  "name": "mpx_skill_creator",
  "description": "Create, update or remove a reusable skill on this robot (optionally with a Lua script the owner can also run and edit in the app) when the user asks to add a feature, teach a new trick, or save a routine such as \"make a skill that draws a crab\".",
  "author": "MangDang",
  "metadata": {
    "cap_groups": [
      "cap_skill",
      "cap_files",
      "cap_mpx_lua"
    ],
    "manage_mode": "readonly"
  }
}
---

# Creating skills on this robot

A skill is a folder under the agent's data root:

```text
/fs/agent/skills/<skill_id>/
├── SKILL.md            required: when to use it, and what to do
└── scripts/
    └── <name>.lua      optional: a script run with lua_run_script
```

Write files with `write_file`, then register the skill (below).

## Script or no script?

- **Prefer a Lua script** when the skill is a routine the owner may want to
  run or tweak themselves: a drawing, a dance, a sequence of moves, a timer.
  The owner sees it in the app's **Files** page under
  `agent/skills/<skill_id>/scripts/`, can open and edit it there and press
  **Run**. Keep each skill to one script, and reuse that path when iterating
  (no `foo2.lua`).
- **No script** when the skill is judgement, not a routine (how to answer, which
  tool to pick). Then `SKILL.md` alone describes the tool calls.

## Writing the script

Read `{CUR_SKILL_DIR}/references/robot_lua.md` first: it is the whole API, and
it is not ESP-Claw's Lua -- no `arg_schema`, no `delay` or `lvgl` modules.
The essentials:

- `args` is always a table, and is **empty when the owner presses Run**: give
  every argument a default.
- `tools.call("display_draw", {...})` (and every other robot tool, same names
  and arguments) does what that tool call would do. Tool names are not Lua
  functions: `display_draw(...)` on its own is an error.
- `print` a short result line; `error("...")` on failure.
- Runs are limited to 60 s and block the robot's Lua meanwhile: do the thing
  and return.

```lua
-- Draw a crab. args.seconds: how long it stays (default 20).
local seconds = tonumber(args.seconds) or 20

local res, err = tools.call("display_draw", {
  background = "black",
  seconds = seconds,
  shapes = {
    { type = "oval", x = 160, y = 140, rx = 75, ry = 50, color = "#FF5A2A" },
    { type = "circle", x = 135, y = 70, r = 11, color = "white" },
    { type = "circle", x = 185, y = 70, r = 11, color = "white" },
  },
})
if not res then error(err) end
print("crab on screen for " .. seconds .. " s")
```

After writing it, run it once with `lua_run_script` (`path` is the absolute
path you wrote, `args` optional) and fix it until it succeeds.

## The SKILL.md

````md
---
{
  "name": "draw_crab",
  "description": "Draw a red crab on the robot's screen when the user asks for a crab.",
  "metadata": {
    "cap_groups": ["cap_mpx_lua"],
    "manage_mode": "runtime"
  }
}
---

# Draw a crab

Run `/fs/agent/skills/draw_crab/scripts/draw_crab.lua` with `lua_run_script`.

Args: `seconds` (number, 1-600, default 20): how long the crab stays.

```json
{"path":"/fs/agent/skills/draw_crab/scripts/draw_crab.lua","args":{"seconds":20}}
```

Report the script's output. If it fails, tell the user the error.
````

- `name` must equal `skill_id`: lowercase letters, digits, `_` or `-`, named
  after what it does for the user. Do not reuse an id unless asked to update
  that skill.
- `description`: one sentence, in the words a user would say.
- `cap_groups`: `cap_mpx_lua` for a script skill; otherwise the groups whose
  tools it calls (`cap_robot`, `cap_display`, `cap_mpx_skill`, `cap_music`).
- `manage_mode`: `runtime` for skills you create.
- Refer to the script by its absolute path, `/fs/agent/skills/<skill_id>/scripts/<name>.lua`,
  everywhere: in SKILL.md, `write_file` and `lua_run_script`.

## Register it

Call `register_skill` with `{"skill_id":"<skill_id>","file":"<skill_id>/SKILL.md"}`.
To update a skill, rewrite its files and register again; to remove one, call
`unregister_skill` (and `delete_file` its script). Report the tool's result as
it is; if writing or registering failed, say so and do not claim the skill
exists. Then tell the user the skill id, and that the script is in the app's
Files page if they want to change it.

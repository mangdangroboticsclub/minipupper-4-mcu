---
{
  "name": "mpx_skill_creator",
  "description": "Create, update or remove a reusable skill on this robot when the user asks to add a feature, teach a new trick, or remember a routine such as \"make a skill that draws a crab\".",
  "author": "MangDang",
  "metadata": {
    "cap_groups": [
      "cap_skill",
      "cap_files"
    ],
    "manage_mode": "readonly"
  }
}
---

# Creating skills on this robot

A skill here is **one `SKILL.md` document**: instructions you will follow later
by calling the robot's tools. There is nothing to execute besides those tools.

## No Lua, no scripts

You have **no tool that runs Lua** (or any other script) on this robot. Do not
write `.lua` files, do not `require` modules such as `arg_schema`, and do not
treat tool names as functions inside code: `display_draw(...)` written in a
script does nothing, `display_draw` called as a tool draws. If a request sounds
like it needs a script, write the steps as tool calls in the skill instead.

## What a skill can use

The robot's own tools, called directly:

- Moving: `robot_move`, `robot_pose`, `robot_get_state`, `robot_set_config`
- Screen: `display_draw`, `display_show_emotion`, `display_show_text`,
  `display_set_colors`, `display_set_brightness`, `display_clear`
- Robot skills (installed `.wasm` files): `mpx_list_skills`, `mpx_run_skill`,
  `mpx_stop_skill`
- Music, when this robot has it: `music_play`, `music_stop`

Put the exact arguments in the skill (for a drawing: the whole `shapes` list),
so running it later is one tool call, not a redesign. Activate the skill that
documents a tool (`mpx_robot`, `mpx_display_draw`, ...) before writing about it.

## Where it goes

`skills/<skill_id>/SKILL.md` under the agent's data root (on this robot
`/fs/agent/skills/<skill_id>/SKILL.md`), written with `write_file`. Nothing else
belongs in that folder.

`skill_id`: lowercase letters, digits, `_` or `-`; named after what it does for
the user (`draw_crab`, `morning_stretch`), not how. Do not reuse an existing id
unless the user asked to update that skill.

## The document

```md
---
{
  "name": "draw_crab",
  "description": "Draw a red crab on the robot's screen when the user asks for a crab.",
  "metadata": {
    "cap_groups": ["cap_display"],
    "manage_mode": "runtime"
  }
}
---

# Draw a crab

Call `display_draw` with exactly these arguments:

{ "background": "black", "seconds": 30, "shapes": [ ... ] }

Then tell the user the crab is on the screen.
```

- `name` must equal `skill_id`.
- `description`: one sentence, in the words a user would say, plus any
  prerequisite.
- `cap_groups`: the groups whose tools the skill calls: `cap_robot`,
  `cap_display`, `cap_mpx_skill`, `cap_music`.
- `manage_mode`: `runtime` for skills you create.

## Register it

Right after writing the file, call `register_skill` with
`{"skill_id":"<skill_id>","file":"<skill_id>/SKILL.md"}`. To update a skill,
rewrite the file and call `register_skill` again; to remove one, call
`unregister_skill`. Report the tool's result as it is; if writing or
registering failed, say so and do not claim the skill exists.

Then try it once if the user is there to see it, and tell them the skill id.

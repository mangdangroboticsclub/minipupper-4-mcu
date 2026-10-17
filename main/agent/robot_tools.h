/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * robot_tools -- the robot's tools, as a model sees them: one table of
 * name + description + JSON Schema + body, used by both brains:
 *
 *   - voice_tools.cc offers it to the MangDang voice gateway's model over
 *     MCP (tools/list, tools/call), as mpv2-claw's mpx_mcp_ws did;
 *   - agent_caps.cc registers it with ESP-Claw's capability registry for
 *     the on-device agent, as mpv2-claw's cap_robot, cap_display,
 *     cap_mpx_skill and cap_music components did.
 *
 * The bodies follow those four mpv2-claw components, so a prompt tuned
 * against mpv2 behaves the same, and the SKILL.md files in main/skills/
 * (copied from them) describe exactly these tools. The `group` of each tool
 * is the mpv2-claw capability group id its SKILL.md names in cap_groups.
 *
 * Compiled in every build. The music tools exist only with
 * CONFIG_APP_VOICE_ENABLE (cap_music is part of the voice stack); the
 * display tools only with CONFIG_APP_DISPLAY_ENABLE.
 */
#pragma once

#include <cstddef>
#include <string>

#include "cJSON.h"

namespace robot_tools {

/* A tool reply: text for the model, plus whether the caller should flag it
 * as an error (which makes the model retry or apologise -- so "no such
 * movement" is information, not an error, exactly as mpv2's cap_robot
 * decides). */
struct Reply {
    std::string text;
    bool error = false;
};

struct Tool {
    const char *name;           /*!< wire name, e.g. "robot_move" */
    const char *group;          /*!< ESP-Claw capability group: cap_robot, cap_display, cap_mpx_skill, cap_music */
    const char *description;    /*!< for the voice server (MCP); any length */
    const char *brief;          /*!< for ESP-Claw, whose descriptions are cut at 256 bytes; nullptr = description fits */
    const char *schema;         /*!< JSON Schema for the arguments */
    Reply (*run)(const cJSON *args);
};

/** The table and its length. */
const Tool *all(size_t *count);

/** By wire name; tolerates xiaozhi's "self." prefix. nullptr if unknown. */
const Tool *find(const char *name);

/** The description ESP-Claw gets: brief when set, else description. */
const char *claw_description(const Tool &t);

/** MCP-style descriptor: {"name","description","inputSchema"}. Caller frees. */
cJSON *descriptor(const Tool &t);

/** Run a tool on parsed arguments (nullptr = none). */
Reply run(const Tool &t, const cJSON *args);

/** Run by name on a JSON text of arguments ("" or nullptr = none). An
 *  unknown name is an error reply. */
Reply call(const char *name, const char *arguments_json);

}  // namespace robot_tools

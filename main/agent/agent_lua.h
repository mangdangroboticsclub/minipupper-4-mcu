/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * agent_lua -- the on-device agent's Lua tool: lua_run_script runs a script
 * on the robot's own Lua VM (main/lua), the same one the app's Files page
 * runs, so a skill script the agent writes is one the owner can open, edit
 * and run too.
 *
 * Not ESP-Claw's cap_lua (left out: it links a second Lua). Kept out of
 * robot_tools' table on purpose: that table is also offered to the voice
 * gateway over MCP, and is what a script's tools.call() reaches -- a script
 * must not be able to start a script.
 */
#pragma once

#include <cstddef>

#include "agent/robot_tools.h"

namespace agent_lua {

/** The agent-only tools (group cap_mpx_lua) and their count. */
const robot_tools::Tool *tools(size_t *count);

}  // namespace agent_lua

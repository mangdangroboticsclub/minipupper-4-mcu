/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The robot's tools, as the voice server sees them.
 *
 * The MangDang voice gateway speaks the xiaozhi protocol, and in that
 * protocol the server's model reaches the device through MCP: JSON-RPC
 * messages wrapped as {"type":"mcp","payload":{...}} on the voice socket.
 * The device answers tools/list with what it can do and tools/call when the
 * model wants it done -- "look up", "make the screen blue", "play some jazz".
 *
 * mpv2-claw does this in mpx_mcp_ws, which lists the tools of ESP-Claw's
 * capability registry (claw_cap) through Espressif's mcp-c-sdk engine. There
 * is no capability registry in this firmware (the agent is phase 5, see
 * .cursor/PORT_NOTES.md), so this is the same wire protocol over a static
 * table: the robot, display, skill and music tools mpv2-claw exposes, with
 * the same names and argument schemas, calling this firmware's own APIs.
 * The one tool that cannot be here is ask_robot_agent -- it IS the agent.
 *
 * All handling runs on one task of its own with an internal-RAM stack:
 * robot_set_config writes NVS, and (unlike mpv2-claw) this firmware does not
 * run code from PSRAM, so a PSRAM stack would be unreachable during the
 * flash write.
 */
#pragma once

#include <string>

#include "esp_err.h"

namespace voice_tools {

/** Create the inbox and the task and register as mpx_voice_link's MCP sink.
 *  Call once at boot, before hands-free listening starts. Idempotent. */
esp_err_t start();

/** The tools/list "tools" array as the server would receive it (every page
 *  in one), for GET /v1/voice/tools. */
std::string list_json();

/** Run one tool directly, as POST /v1/voice/tools does for bench checks:
 *  the tool's text reply. `is_error` (may be null) reports the MCP flag. */
std::string call(const char *name, const char *arguments_json, bool *is_error);

}  // namespace voice_tools

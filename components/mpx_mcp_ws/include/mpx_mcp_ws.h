/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Phase 3 of docs/voice-plan.md: the MCP bridge.
 *
 * Phase 2 proved the socket. This turns it into the transport for a second
 * MCP server -- our own `esp_mcp_t` engine, independent of ESP-Claw's own
 * cap_mcp_server (which is hardcoded to HTTP and has never been started in
 * this firmware) -- so that JSON-RPC tool calls arriving as
 * {"type":"mcp","payload":{...}} over the xiaozhi socket reach the same
 * claw_cap_call() interface the reference plan pointed at, and results go
 * back the same way.
 *
 * What is exposed: every capability the ESP-Claw agent itself can call --
 * the same list, descriptions and schemas claw_cap gives the on-device LLM
 * (Lua, files, scheduler, skills, system, web search, HTTP, the robot, the
 * display). tools/list and tools/call are answered straight from claw_cap;
 * the esp_mcp engine only handles the session-level methods. See the note at
 * the top of the tools section in mpx_mcp_ws.c for why it is done that way.
 *
 * The cloud model therefore has exactly the reach the local agent has,
 * including write_file, lua_run_script and restart_device. That is the
 * point -- but it is worth knowing.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the engine, wires the transport to
 * mpx_voice_link, and starts the dispatch task. Idempotent -- call once
 * after mpx_voice_link is up (it registers a sink with it). */
esp_err_t mpx_mcp_ws_init(void);

void register_mcp_command(void);

/* http_server_webim_api.c mirrors every outbound message from the onboard
 * agent here; the bridge keeps the ones addressed to its own chat id (the
 * replies to ask_robot_agent) and ignores the rest. Safe to call from any
 * task, with any chat id. */
void mpx_mcp_ws_on_agent_reply(const char *chat_id, const char *text);

/* "own" AI mode: route voice through the onboard agent (device brain). The
 * server still transcribes and speaks; this robot's agent, with the owner's
 * own LLM key, writes the answers. Call after mpx_mcp_ws_init(). */
esp_err_t mpx_mcp_ws_enable_device_brain(void);

#ifdef __cplusplus
}
#endif

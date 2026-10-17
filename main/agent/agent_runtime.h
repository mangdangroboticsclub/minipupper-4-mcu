#pragma once

/*
 * agent_runtime -- the on-device agent (ESP-Claw) on this firmware.
 *
 * What mpv2-claw's main.c does around app_claw_start(), in one place:
 * the filesystem roots (agent_fs), the LLM from the owner's AI mode
 * (agent_config), the robot's tools (agent_caps), the agent itself, and the
 * "web" chat channel (cap_im_local) the PWA's chat is bridged to.
 *
 * The bridge: chat_ws.cc hands a conversation's message in with submit();
 * the agent's replies and progress notes come back through the one outbound
 * callback (set_outbound), on the agent's task. The session id is the PWA's
 * conversation id, so ESP-Claw's session history is per conversation.
 *
 * Compiled with the agent off too (CONFIG_APP_AGENT_ENABLE=n): then
 * available() is false, start() is a no-op and submit() refuses.
 */

#include "esp_err.h"

namespace agent {

/** Is the agent in this build? */
bool available();

/** Did start() bring it up? */
bool running();

/**
 * Boot wiring. After the storage LittleFS is mounted, Wi-Fi is up and the
 * HTTP server is running (the tools start skills through its helpers);
 * before voice_glue::start() so the agent is there when the voice gateway
 * hands a turn over later. Never fatal to the robot.
 */
esp_err_t start();

/** The AI mode or the owner's LLM changed (POST /v1/ai): project
 *  agent_config into the running agent. Live, no restart. */
esp_err_t apply_config();

/**
 * A message from the PWA for the local brain. session_id is the
 * conversation's uuid (<= 63 chars). Returns ESP_ERR_INVALID_STATE when the
 * agent is not running or the AI mode is off (the caller tells the user),
 * otherwise the message is queued for the agent and ESP_OK returned at once.
 */
esp_err_t submit(const char *session_id, const char *text);

/**
 * Where the agent's output goes. is_step is true for a progress note (ESP-
 * Claw prefixes those with the lobster, U+1F99E, which is stripped) and
 * false for a reply. Runs on the agent's or the event router's task: do not
 * block for long. One callback; a second call replaces the first.
 */
using OutboundFn = void (*)(const char *session_id, const char *text, bool is_step);
void set_outbound(OutboundFn fn);

}  // namespace agent

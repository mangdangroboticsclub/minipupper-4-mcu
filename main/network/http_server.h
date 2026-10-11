#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace network {

/* ── Skills, for callers other than the web app ────────────────────────────
 *
 * POST /v1/skills/run owns the one-skill-at-a-time rule and the task that
 * runs a skill; the voice tools (voice/voice_tools.cc, mpv2-claw's
 * mpx_run_skill / mpx_stop_skill) need the same thing without an HTTP
 * request, so the handler's helpers are exposed here rather than copied. */

/** Start a skill by file name or registry slug (what /v1/skills/run
 *  accepts), with an optional "name=value;..." parameter string. Returns
 *  nullptr on success, else a short reason ("a skill is already running",
 *  "no such skill", "failed to start skill task"). `resolved` (may be null)
 *  gets the name /v1/skills/status will report. */
const char *skill_start(const std::string &given, const std::string &params,
                        std::string *resolved);

/** Start the skill that provides a movement name (registry provides_gait),
 *  as POST /v1/robot/gait does for a non-built-in name. Same returns as
 *  skill_start(); "no such movement" when nothing provides it. */
const char *skill_start_for_movement(const char *movement, std::string *resolved);

bool        skill_running();
const char *skill_running_name();   /*!< "" when none */
void        skill_stop();           /*!< cooperative: wasm::request_stop() */

/**
 * @brief Root directory inside LittleFS where www assets are stored.
 */
constexpr const char *WWW_ROOT = "/fs/www";

/**
 * @brief Start the HTTP / WebSocket server on port 80.
 *
 * Serves PWA static assets from LittleFS under WWW_ROOT.
 * Supports pre-compressed .gz files with Content-Encoding: gzip.
 * Provides WebSocket telemetry at /v1/telemetry/stream.
 *
 * Must be called after init_wifi_ap() and fs::init_littlefs().
 *
 * @return true on success.
 */
bool start_http_server();

/**
 * @brief Stop the HTTP server.
 */
void stop_http_server();

}  // namespace network

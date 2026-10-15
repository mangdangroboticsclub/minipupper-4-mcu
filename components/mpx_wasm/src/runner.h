#pragma once

#include <cstdint>
#include <string>

/* ── Starting and stopping skills, from anywhere ──────────────────────────
 *
 * Running a skill used to be something only the HTTP layer could do: the
 * "is one running", "which one", "how did the last one end" state were file
 * statics inside http_server.cc, reachable from nothing else.
 *
 * Three things now need to start a skill -- a movement name the web UI sent, a
 * boot-time autorun, an event trigger -- and all three need the same guarantee
 * the HTTP path already had: exactly one skill at a time, with a truthful
 * answer about what is running. So it lives here instead.
 */
namespace skills {

enum class Mode {
	OneShot,    /**< 60 s watchdog. What a movement or a demo is.          */
	Behaviour,  /**< No total watchdog. Must be stopped, or stop itself.   */
};

/**
 * @brief Start a skill, if nothing else is running.
 *
 * @param path    LittleFS path, e.g. "/moonwalk.wasm".
 * @param name    Display name, usually the slug.
 * @param params  "speed=0.4;repeats=3", or empty.
 * @param mode    OneShot or Behaviour.
 * @param why     What started it -- "api", "gait", "boot", "event:imu.lifted".
 *                Goes in the log and in status, because "why is my robot
 *                moving" is a question the answer should already be waiting for.
 * @return false if a skill is already running, or the task could not start.
 */
bool start(const char *path, const char *name, const char *params,
           Mode mode, const char *why);

/** Ask the running skill to stop. Cooperative: on_stop() still runs. */
void stop();

bool               running();
const std::string &current();      /**< Name of the running skill, or "". */
const std::string &started_by();   /**< The `why` it was started with.    */
Mode               current_mode();

/** Name, result code and duration of the last run that finished. */
const std::string &last_name();
int                last_result();
std::uint32_t      last_ms();
std::uint32_t      running_ms();

/** Everything above as JSON, for GET /v1/skills/status. */
std::string status_json();

}  // namespace skills

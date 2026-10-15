/*
 * SPDX-FileCopyrightText: 2026 MangDang
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * mpx_wasm -- the WASM skill runtime, as a C facade.
 *
 * Skills are WebAssembly modules running under WAMR, talking to the robot
 * through a stable ABI (MPX_ABI_VERSION 4, 74 host functions). Encrypted
 * skills (.mpxe) are unwrapped with a key bound to CONFIG_MP4_ROBOT_UUID, so
 * a module bought for one robot will not load on another.
 *
 * The runtime itself is C++ and stays that way -- it is a direct port of the
 * MPX-Dog implementation and the ABI must remain byte-compatible or every
 * skill anyone already owns stops working. This header is what the rest of
 * the firmware uses.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up WAMR and register the 74 host functions.
 *
 * Also hands mpx_robot the two predicates its gait loop needs -- "is a skill
 * running" and "does it own the joints" -- so the two components stay
 * one-directional. Call after mpx_robot_init().
 *
 * @return false if the runtime could not start. The firmware should carry on:
 *         a device that cannot run skills is still a usable robot.
 */
bool mpx_wasm_init(void);

/**
 * @brief Scan the skills directory, start the IMU event watcher, and run the
 *        autorun skill if one is installed and safe mode is clear.
 *
 * Call this LAST, after the HTTP server is listening.
 *
 * The ordering is not a detail. An autorun skill that crashes the robot runs
 * again on the next boot, and the next; a user with no serial cable then has
 * a brick, and the thing that bricked it arrived from a marketplace. Starting
 * the web UI first means they can always uninstall it. That is the difference
 * between a bad skill and a brick.
 */
void mpx_wasm_start_skills(void);

/* ── Running a skill ───────────────────────────────────────────────────── */

/**
 * @brief Start a skill by file name, e.g. "moonwalk.wasm".
 *
 * One skill runs at a time. A request that arrives while another is running is
 * refused rather than queued -- a queue means the robot performing a movement
 * someone asked for long enough ago that they have stopped expecting it.
 *
 * @param params  optional "name=value;name=value" string, readable from the
 *                skill through mpx_param_f/mpx_param_i. Deliberately not JSON.
 * @param why     provenance for the status endpoint: "api", "gait", "boot",
 *                or "event:<name>". Surfaced as `started_by`, because "why is
 *                my robot moving" is a question the answer should already be
 *                waiting for.
 */
bool mpx_wasm_run_skill(const char *file, const char *params, const char *why);

/** @brief Ask the running skill to stop. Cooperative; on_stop() still runs. */
void mpx_wasm_stop_skill(void);

/** @brief Whether a skill is executing right now. */
bool mpx_wasm_skill_running(void);

/** @brief Name of the running skill, or NULL. */
const char *mpx_wasm_running_skill_name(void);

/* ── The registry ──────────────────────────────────────────────────────── */

/** @brief Re-read every module's embedded manifest from the skills directory. */
void mpx_wasm_rescan(void);

/** @brief How many skills are installed. */
int mpx_wasm_skill_count(void);

/**
 * @brief Details of the i-th installed skill. Any out pointer may be NULL.
 *
 * Strings point into registry-owned storage and are invalidated by a rescan.
 */
bool mpx_wasm_skill_at(int index,
                       const char **out_file,
                       const char **out_slug,
                       const char **out_provides_gait,
                       int *out_abi,
                       bool *out_autorun,
                       bool *out_behaviour);

/* ── Movement ──────────────────────────────────────────────────────────── */

typedef enum {
    MPX_MOVEMENT_STARTED = 0,
    MPX_MOVEMENT_UNKNOWN,        /*!< no such movement                       */
    MPX_MOVEMENT_BUSY,           /*!< a skill is already running             */
    MPX_MOVEMENT_NOT_PERMITTED,  /*!< a skill cannot start another skill     */
} mpx_movement_result_t;

/**
 * @brief Run a movement by name, built-in or skill-provided.
 *
 * Resolution order is built-in first, then the registry. Built-ins win on
 * purpose: a downloaded skill must not be able to shadow "advance".
 */
mpx_movement_result_t mpx_wasm_movement_run(const char *name, bool from_skill);

/** @brief Human-readable text for a movement result. Never NULL. */
const char *mpx_wasm_movement_result_text(mpx_movement_result_t r);

/* ── The skills directory ──────────────────────────────────────────────── */
/*
 * Every name here is relative to <DATA>/mpx_skills and cannot escape it: the
 * implementation refuses anything containing "..". The HTTP upload handler
 * additionally applies the stricter filename rule the marketplace flow relies
 * on (see skill_filename_ok in the /v1/skills handlers) before it gets here.
 */

/** @brief Write a whole file. A partial write is removed rather than left. */
bool mpx_wasm_skill_file_write(const char *name, const void *data, size_t len);

/**
 * @brief Read a whole file into `buf`.
 * @return bytes read, or 0 if missing, unreadable, or larger than `buf_size`.
 */
size_t mpx_wasm_skill_file_read(const char *name, void *buf, size_t buf_size);

/** @brief Delete a file. False if it was not there. */
bool mpx_wasm_skill_file_delete(const char *name);

/** @brief Whether a file exists. */
bool mpx_wasm_skill_file_exists(const char *name);

/* ── Events and safe mode ──────────────────────────────────────────────── */

/**
 * @brief Fire a chat-derived event.
 *
 * Matches the first alphanumeric word of `message`, lowercased, so both
 * "Dance!" and "dance" fire `chat:dance`. Wired to the chat bridge so a skill
 * declaring "on": ["chat:dance"] reacts while the reply is still composing.
 */
bool mpx_wasm_fire_chat(const char *message);

/** @brief Whether autorun is disabled after repeated boot failures. */
bool mpx_wasm_safe_mode(void);

/** @brief Clear the safe-mode counter so autorun runs again next boot. */
void mpx_wasm_clear_safe_mode(void);

#ifdef __cplusplus
}
#endif

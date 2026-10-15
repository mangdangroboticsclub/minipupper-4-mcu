#pragma once

#include <string>

/* ── One name space for movements ─────────────────────────────────────────
 *
 * Adding a named movement used to mean three firmware edits -- the GaitCmd
 * enum, the name table in host_robot_gait(), and the switch in gait_task() --
 * and a reflash. For the one thing this SDK exists for, the supported path was
 * to modify firmware.
 *
 * A skill can now declare `"provides_gait": "moonwalk"` in its manifest.
 * `mpx-cli build` embeds that in the .wasm, the registry reads it, and
 * everything that takes a movement name comes through here:
 *
 *     built-in name  ->  robot::send_gait_cmd()
 *     registered     ->  run that skill
 *     neither        ->  a refusal that says so
 *
 * So the web UI, the marketplace and mpx-cli see one list, and a
 * skill-provided move is triggerable from a phone exactly like "advance".
 */
namespace skills {

enum class MovementResult {
	Started,       /**< A built-in gait was sent, or a skill was launched. */
	Unknown,       /**< No built-in gait and no skill provides that name.  */
	Busy,          /**< It is a skill, and another skill is running.       */
	NotPermitted,  /**< A skill asked for a skill-provided movement.       */
};

/**
 * @brief Perform a movement by name.
 *
 * @param name        "advance", "moonwalk", anything.
 * @param from_skill  True when the caller is a running skill. Skill-provided
 *                    movements are refused in that case: one skill cannot
 *                    launch another, because only one runs at a time and the
 *                    recursion would deadlock rather than fail.
 */
MovementResult run(const char *name, bool from_skill);

/** Human-readable, for logs and HTTP responses. */
const char *result_text(MovementResult r);

/** Every movement this robot can perform, built-in and skill-provided.
 *  `{"movements":[{"name":..,"source":"builtin"|"skill","skill":..}]}` */
std::string list_json();

}  // namespace skills

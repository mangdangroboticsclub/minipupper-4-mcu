#pragma once

/* ── Starting a skill at boot, without being able to brick the robot ──────
 *
 * A skill marked `"autorun": true` starts when the robot powers on. That is
 * the difference between a robot that performs tricks and one that has a
 * personality, and it is also the single most dangerous thing in this SDK.
 *
 * The failure mode is specific: an autorun skill that crashes the robot, or
 * drives it somewhere it cannot recover from, runs again on the next boot. And
 * the next. A user with no serial cable has a brick, and the thing that
 * bricked it arrived from a marketplace.
 *
 * SAFE MODE. A counter in NVS is incremented before the autorun skill starts
 * and cleared once the robot has been up for AUTORUN_PROVEN_MS without needing
 * a reboot. If the count reaches AUTORUN_MAX_ATTEMPTS, autorun is skipped and
 * stays skipped until something explicitly clears it -- the robot comes up
 * bare, the web UI works, and the skill can be uninstalled.
 *
 * So the worst case is a robot that boots normally and says why, rather than
 * one that needs opening up.
 */
namespace skills {

/** Consecutive boots that may attempt autorun before it is disabled. */
inline constexpr int AUTORUN_MAX_ATTEMPTS = 3;

/** How long the robot must stay up before a boot counts as successful. */
inline constexpr int AUTORUN_PROVEN_MS = 20000;

/**
 * @brief Start the autorun skill, unless safe mode says otherwise.
 *
 * Call once at boot, after the filesystem, the robot and the registry are up.
 * Does nothing when no skill is marked autorun.
 */
void autorun_boot();

/** True when autorun was skipped because of repeated failures. */
bool safe_mode();

/** Re-enable autorun after safe mode tripped. For the web UI and the CLI. */
void clear_safe_mode();

}  // namespace skills

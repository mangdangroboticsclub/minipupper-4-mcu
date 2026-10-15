#pragma once

#include <cstdint>

/* ── Skills that start themselves ─────────────────────────────────────────
 *
 * A skill only ever ran because a human pressed run. So "do this when the
 * robot is picked up" was not a skill you could write -- it was firmware.
 *
 * A manifest can now say:
 *
 *     "on": ["boot", "imu.lifted"]
 *
 * and this dispatches. The event set is deliberately small: each one has to be
 * something the firmware can detect reliably and describe honestly, and a long
 * list of half-working triggers would be worse than a short list of real ones.
 *
 * SAFETY. Events fire on their own, so every one of them is a way for a robot
 * to start moving with nobody having asked just then. Three rules, all
 * enforced here rather than left to the skill:
 *
 *   - one skill at a time, so an event during a run is dropped, not queued
 *   - a per-event cooldown, so a noisy sensor cannot thrash the sandbox
 *   - boot events are subject to the same safe-mode guard as autorun
 */
namespace skills {

/** Names a manifest may use in "on". */
inline constexpr const char *EVENT_BOOT      = "boot";
inline constexpr const char *EVENT_LIFTED    = "imu.lifted";
inline constexpr const char *EVENT_FALLEN    = "imu.fallen";
inline constexpr const char *EVENT_SHAKEN    = "imu.shaken";
/* "chat:<word>" is matched by prefix, so a manifest can say "chat:dance". */
inline constexpr const char *EVENT_CHAT_PREFIX = "chat:";

/** Start the IMU watcher task. Call once at boot, after the IMU is up. */
void events_start();

/**
 * @brief Fire an event: run whatever skill asked for it.
 *
 * Returns true if a skill was started. Dropped silently (with a log line) when
 * something is already running or the event is still in cooldown.
 */
bool fire(const char *event);

/** Fire "chat:<word>" for a chat message. Non-owning; copies what it needs. */
bool fire_chat(const char *message);

/** Seconds an event must wait after firing before it may fire again. */
inline constexpr int EVENT_COOLDOWN_MS = 4000;

}  // namespace skills

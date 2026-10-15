#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

/* ── Named numbers out of a running skill ─────────────────────────────────
 *
 * A skill's only output is text through print(). That is fine for "I got
 * here" and useless for "what is my knee error doing" -- and when you cannot
 * see what a control loop is doing, building the firmware and watching a
 * serial monitor starts to look like the reasonable option. That is the pull
 * this ring exists to remove.
 *
 * mpx_trace("knee_err", 1.25f) puts a named float in here with the skill's
 * own timestamp. `mpx-cli trace` polls it and plots it.
 *
 * Deliberately the same shape as log_ring: a small fixed ring in INTERNAL
 * RAM, sequence-numbered, polled over plain HTTP. The reasons are the same
 * ones written down there -- httpd runs with max_open_sockets = 5 and
 * lru_purge_enable, so a permanently-held socket is a fifth of the budget and
 * a candidate for eviction. A trace channel is not worth risking the chat
 * socket for.
 *
 * Names are truncated to 15 characters and interned by value, not pointer:
 * the caller's string lives in WASM linear memory, which is gone by the time
 * anyone reads this.
 *
 * 256 samples at 24 bytes is 6 KB. At a 50 Hz tick tracing two signals that
 * is about 2.5 seconds of history, which is the right order for "what just
 * happened" and deliberately not a recording facility.
 */
namespace util {

constexpr std::size_t TRACE_NAME_MAX = 16;   /* including the terminator */

/** Drop every sample. Called when a skill starts, so runs do not bleed. */
void trace_ring_reset();

/**
 * @brief Record one named sample. Safe to call from the WASM thread.
 *
 * @param name   Signal name; truncated to TRACE_NAME_MAX-1 characters.
 * @param value  The sample.
 * @param t_ms   Milliseconds since the skill started (wasm::skill_millis()).
 */
void trace_ring_put(const char *name, float value, std::uint32_t t_ms);

/** Total samples ever recorded; also the sequence number of the next one. */
std::uint32_t trace_ring_next_seq();

/**
 * @brief Samples newer than @p since, as a JSON array.
 *
 * Returns `[{"t":123,"n":"knee_err","v":1.25}, ...]`.
 *
 * A caller that falls far enough behind for its `since` to have been
 * overwritten resumes from the oldest sample still held rather than failing.
 * A gap in a trace is better than no trace -- and a plot with a gap in it is
 * itself information about how far behind the reader got.
 */
std::string trace_ring_json(std::uint32_t since, std::size_t max_samples,
                            std::uint32_t &out_next);

}  // namespace util

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

/* ── In-memory capture of the robot's own log ─────────────────────────────
 *
 * Everything the firmware knows about a failing skill already goes to
 * ESP_LOGx: the gait name that did not match, the servo id that was out of
 * range, the WAMR trap message. All of it went to a UART nobody developing
 * over Wi-Fi could see, so from the developer's side a broken skill and a
 * working one looked the same.
 *
 * This captures the same stream into a small RAM ring so it can be served
 * over HTTP. Serial output is unaffected — the hook chains to the original
 * vprintf rather than replacing it.
 *
 * Deliberately a ring in INTERNAL RAM, not PSRAM: the log is most valuable
 * when something is wrong, and "something is wrong" includes PSRAM failing
 * to come up. 16 KB is roughly 200 lines, enough to cover a skill run.
 *
 * Deliberately polled over plain HTTP rather than pushed over a WebSocket:
 * httpd is configured with max_open_sockets = 5 and lru_purge_enable, so a
 * permanently-held log socket would be a fifth of the budget and a candidate
 * for eviction — it could get itself, or the chat socket, dropped.
 */
namespace util {

/** Install the vprintf hook. Safe to call once, early in app_main(). */
void log_ring_init();

/** Total lines ever captured. Also the sequence number of the next line. */
uint32_t log_ring_next_seq();

/**
 * @brief Collect lines newer than @p since, as a JSON array of strings.
 *
 * @param since     Sequence number the caller already has. 0 = from the
 *                  oldest line still held.
 * @param max_lines Cap on how many lines to return in one response.
 * @param out_next  Receives the sequence number to pass as `since` next time.
 * @return JSON array text, e.g. `["I (123) wasm: ...","W (140) ..."]`.
 *
 * If the caller falls far enough behind that its `since` has been overwritten,
 * it silently resumes from the oldest line still held rather than failing —
 * a gap in a log is better than no log.
 */
std::string log_ring_json(uint32_t since, std::size_t max_lines,
                          uint32_t &out_next);

}  // namespace util

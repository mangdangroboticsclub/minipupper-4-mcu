#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace wasm {

/**
 * @brief Result of a sandbox execution attempt.
 */
enum class SandboxResult {
	Success,
	LoadFailed,
	InstantiateFailed,
	FunctionNotFound,
	ExecutionFailed,
	Timeout,
	NotInitialized,
};

/**
 * @brief Initialize the WAMR runtime.
 *
 * Allocates a fixed heap pool in PSRAM for the runtime.
 * Must be called once before any sandbox operations.
 *
 * @return true on success.
 */
bool init_sandbox();

/**
 * @brief Register a set of native host functions with the runtime.
 *
 * Can be called any time after init_sandbox() and before
 * the first load_and_run(). Multiple registrations are additive.
 *
 * @param module_name  The import module name the wasm uses (e.g. "env").
 * @param symbols      Array of NativeSymbol entries.
 * @param count        Number of symbols in the array.
 * @return true on success.
 */
bool register_natives(const char *module_name,
					  void *symbols, std::uint32_t count);

/**
 * @brief Load a .wasm or .mpxe file from LittleFS, instantiate it,
 *        and call an exported function.
 *
 * If the file starts with "MPXE" magic (encrypted skill), it is
 * automatically decrypted using the robot's AES-256 root key before
 * loading.  Plain .wasm files pass through unchanged.
 *
 * By convention, encrypted files should use the .mpxe extension and
 * plain developer-mode files should use .wasm, but the loader
 * detects the format by content (magic bytes), not by extension.
 *
 * If func_name is nullptr or empty, calls the default "on_start"
 * entry.  The instance is destroyed after the call returns.
 *
 * @param path       Path within LittleFS (e.g. "/skill.wasm" or "/skill.mpxe").
 * @param func_name  Name of the exported function to call.
 * @param timeout_ms Maximum execution time in milliseconds (0 = no watchdog).
 * @return SandboxResult indicating the outcome.
 */
SandboxResult load_and_run(const char *path,
						   const char *func_name = nullptr,
						   std::uint32_t timeout_ms = 3000);

/**
 * @brief Load a .wasm module from an in-memory buffer, instantiate it,
 *        call an exported function, then tear down.
 *
 * Identical to load_and_run() but takes a pointer + size instead of a
 * LittleFS path. Useful for embedded binaries.
 *
 * @param wasm_bytes  Pointer to the .wasm bytecode in memory.
 * @param wasm_size   Number of bytes.
 * @param func_name   Exported function to call (default: "on_start").
 * @param timeout_ms  Watchdog timeout (default: 3000).
 * @return SandboxResult
 */
SandboxResult load_and_run_bytes(const uint8_t *wasm_bytes,
								 std::size_t wasm_size,
								 const char *func_name = nullptr,
								 std::uint32_t timeout_ms = 3000);

/**
 * @brief Check whether the currently running WASM invocation has been
 *        cancelled by the watchdog timer.
 *
 * Long-running host functions (e.g. robot_delay_ms) should call this
 * periodically and return early if true, allowing the WASM thread to
 * terminate promptly after a watchdog timeout.
 */
bool was_cancelled();

/**
 * @brief Check whether a WASM skill is currently executing.
 *
 * Used by the gait task to avoid flushing neutral positions while a
 * skill is running, preventing the gait task from overwriting the
 * skill's servo commands.
 */
bool is_running();


/**
 * @brief Milliseconds since the current skill's entry point was called.
 *
 * Zero before the first skill runs. This is the clock behind mpx_millis();
 * it is per-run rather than since-boot so a skill's timeline always starts
 * at 0 and never has to subtract a start time it had no way to capture.
 */
std::uint32_t skill_millis();

/* ── on_tick: running inside the control loop ──────────────────────────────
 *
 * Until now a skill was a script: it ran once, on its own thread, and while
 * it ran the gait task stood aside. Anything that has to react continuously
 * -- stay level on a slope, stiffen a leg under load, stabilise while the
 * built-in walk runs -- had nowhere to live except the firmware.
 *
 * A skill that calls tick_every() during on_start() gets a second optional
 * export called repeatedly after on_start() returns:
 *
 *     MPX_EXPORT void on_tick(int dt_ms);
 *
 * WHY IT IS NOT CALLED FROM THE GAIT TASK. The obvious design is for
 * gait_task() to call on_tick between generating a frame and flushing it,
 * which gives perfect phase alignment. It also puts arbitrary maker code
 * inside a priority-22 real-time loop, and WAMR's exec_env is bound to the
 * thread that created it, so it would need a second exec_env and a rendezvous
 * with the WASM thread on every tick.
 *
 * Instead the WASM thread paces itself and the gait task is left alone. A
 * slow or wedged on_tick then costs its own timing and nothing else -- it
 * cannot stall the loop that keeps the robot standing. The cost is that a
 * tick is not phase-locked to a gait frame; combined with the overlay, which
 * is applied at flush time whenever flush happens, that does not matter for
 * the trimming and stabilising this is for.
 */

/** Ask for on_tick() every @p period_ms after on_start() returns.
 *  Clamped to 10..1000 ms. 0 stops ticking. Resets to 0 for each run. */
void tick_every(int period_ms);

/** Stop the tick loop; the skill then ends normally through on_stop(). */
void tick_stop();

/** The current tick period in ms, or 0 if this skill is not ticking. */
int tick_period();

/**
 * @brief Ask the running skill to stop, from outside it.
 *
 * Cooperative, not a kill: host functions begin returning MPX_ERR_CANCELLED,
 * the tick loop exits at the end of the current tick, and on_stop() still runs
 * so the skill can park the robot. That is the difference between stopping a
 * behaviour and killing a runaway, and a behaviour has no watchdog to kill it.
 *
 * Safe to call when nothing is running.
 */
void request_stop();

/** True once request_stop() or the watchdog has asked the skill to finish. */
bool stopping();

/**
 * @brief Supply the parameters for the next skill run.
 *
 * Format is a flat, comma- or semicolon-separated list of `name=value` pairs,
 * e.g. "speed=0.4;repeats=3". Values are parsed as floats; mpx_param_i()
 * truncates. Deliberately not JSON: this crosses an HTTP handler that already
 * hand-parses its body, and a parameter list is not worth a parser dependency.
 *
 * Cleared automatically at the end of each run, so parameters never leak from
 * one skill into the next.
 *
 * @param kv  The parameter string, or nullptr to clear.
 */
void set_params(const char *kv);

/**
 * @brief Look up one parameter supplied by set_params().
 * @return true when the name was present; *out is only written then.
 */
bool param_get(const char *name, float *out);

/**
 * @brief Destroy the WAMR runtime and free all resources.
 */
void destroy_sandbox();

}  // namespace wasm

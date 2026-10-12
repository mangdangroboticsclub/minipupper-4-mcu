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
 * @brief Report time a host function spent waiting on the user, not running.
 *
 * The watchdog counts execution time against the skill's budget; a permission
 * prompt (fs_write / fs_delete) is the user deciding, not the skill running,
 * so that wait is added back here. The watchdog drains this into its budget on
 * its next poll. Mirrors lua_vm_extend_deadline().
 */
void extend_watchdog(std::uint32_t ms);

/**
 * @brief Check whether a WASM skill is currently executing.
 *
 * Used by the gait task to avoid flushing neutral positions while a
 * skill is running, preventing the gait task from overwriting the
 * skill's servo commands.
 */
bool is_running();

// ═══════════════════════════════════════════════════════════════
//  ABI v3-v4 support: the per-run clock, skill parameters, and the
//  tick loop. Everything here is per-run state, cleared at teardown.
// ═══════════════════════════════════════════════════════════════

/**
 * @brief Milliseconds since this skill run started.
 *
 * Per-run, not since boot, so a skill measuring a frame does not have to
 * subtract a boot time it has no way to observe. The origin is set just
 * before on_start() is called.
 */
std::uint32_t skill_millis();

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
 * @brief Ask for on_tick() to be called at this period in ms.
 *
 * Clamped to 10..1000. 0 or negative disables it. Called from inside
 * on_start(); the loop starts once on_start() returns cleanly, and only if
 * the module exports on_tick - a missing export is a build mistake, and is
 * logged as one rather than treated as a runtime error.
 */
void tick_every(int period_ms);

/** Stop the tick loop after the current tick returns. */
void tick_stop();

/** The current tick period in ms, or 0 if the loop is not running. */
int tick_period();

/**
 * @brief Cancel the run AND stop the tick loop.
 *
 * Distinct from the watchdog's was_cancelled(): this is a deliberate stop
 * request, and it aborts long host calls so the skill ends promptly rather
 * than at the end of whatever delay it was in.
 */
void request_stop();

/** Has a stop been requested? */
bool stopping();

/**
 * @brief Destroy the WAMR runtime and free all resources.
 */
void destroy_sandbox();

}  // namespace wasm

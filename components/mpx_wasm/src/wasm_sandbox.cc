#include "wasm_sandbox.h"
#include "trace_ring.h"
#include "robot.h"
#include "wasm_decrypt.h"

// Forward-declared rather than including sdk/wasm_host_functions.h: that
// header carries the NativeSymbol table itself, and this file needs a few
// functions from it.
namespace sdk {
void control_reset();
int  restore_skill_gains();   // put back gains the skill changed
void forget_skill_gains();    // drop the record without writing
}

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <pthread.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wasm_export.h"

#include "mpx_skill_fs.h"

static const char *TAG = "wasm_sandbox";

// ── Cooperative cancellation flag ────────────────────────────
// Set by the watchdog polling loop when the timeout expires.
// Long-running host functions check this via was_cancelled()
// and return early, allowing the WASM thread to terminate.
static std::atomic<bool> s_cancelled{false};

// ── WASM execution active flag ──────────────────────────────
// Set while a WASM module is executing.  The gait task checks
// this to avoid flushing neutral positions that would overwrite
// the skill's servo commands.
static std::atomic<bool> s_running{false};


// ── Per-run clock ────────────────────────────────────────────
// Set immediately before the entry point is called, so mpx_millis() reads 0
// at the first instruction of on_start() rather than "microseconds since the
// ESP32 booted", which a skill has no way to subtract.
static std::atomic<int64_t> s_start_us{0};

// Tick state. Both are per-run and reset before on_start(), so a skill that
// never calls mpx_tick_every() behaves exactly as it did before on_tick
// existed: on_start returns, the skill ends.
static std::atomic<int>  s_tick_period_ms{0};
static std::atomic<bool> s_tick_stop{false};

// s_cancelled means "wind up now" and is set by BOTH the watchdog and a
// cooperative stop. Only the watchdog calls wasm_runtime_terminate(), which
// unwinds the instance and makes calling into it undefined -- so the two cases
// must be told apart or a cleanly-stopped behaviour never gets its on_stop().
// That distinction is this flag, and nothing else.
static std::atomic<bool> s_hard_killed{false};

// How many consecutive ticks may overrun their period before the loop gives
// up. One overrun is a slow frame; three in a row is a skill that cannot keep
// the rate it asked for, and quietly running it late forever is worse than
// saying so and stopping.
static constexpr int TICK_OVERRUN_LIMIT = 3;

// ── Per-run parameters ───────────────────────────────────────
// Sixteen is well past what a skill's UI can usefully show, and a fixed array
// keeps this off the heap in a path that already runs under a watchdog.
static constexpr int MAX_PARAMS = 16;
static constexpr int MAX_PARAM_NAME = 24;
struct ParamKV { char name[MAX_PARAM_NAME]; float value; };
static ParamKV s_params[MAX_PARAMS];
static int s_param_count = 0;

// --- Resource budgets -------------------------------------------------------
// The runtime heap: used by WAMR for internal data structures.
// Allocated in PSRAM to conserve internal DRAM for WiFi/TCP.
static constexpr std::size_t RUNTIME_HEAP_SIZE = 128 * 1024;

// Linear memory limits per wasm instance (REQ-ROB-03: ≤128 KB).
// NOTE: this is the WASM *operand stack* (interpreter value stack), NOT linear
// memory.  8 KB was too small — richer skills (e.g. dance-uptown) overflowed it
// with "wasm operand stack overflow".  WAMR allocates this from PSRAM, so 32 KB
// is cheap and gives deep skills plenty of headroom.
static constexpr std::size_t DEFAULT_STACK_SIZE = 32 * 1024;  // 32 KB wasm operand stack
static constexpr std::size_t DEFAULT_HEAP_SIZE = 128 * 1024;  // 128 KB host-managed heap

// --- Global state -----------------------------------------------------------
static bool s_initialized = false;

// ── PSRAM-aware allocators (matching esp-wasmachine approach) ──────────────

static void *wamr_malloc(unsigned int size)
{
#ifdef CONFIG_SPIRAM
    // Prefer external PSRAM (keeps scarce internal DRAM free for WiFi/TCP).
    void *p = heap_caps_aligned_alloc(8, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
    // Fall back to internal DRAM if PSRAM is exhausted or unavailable, so a
    // large linear-memory request can't silently fail wasm instantiation.
    return heap_caps_aligned_alloc(8, size, MALLOC_CAP_8BIT);
#else
    return heap_caps_aligned_alloc(8, size, MALLOC_CAP_8BIT);
#endif
}

static void *wamr_realloc(void *ptr, unsigned int size)
{
    void *new_ptr = wamr_malloc(size);
    if (new_ptr && ptr) {
        size_t old_size = heap_caps_get_allocated_size(ptr);
        memcpy(new_ptr, ptr, old_size < size ? old_size : size);
        heap_caps_free(ptr);
    }
    return new_ptr;
}

static void wamr_free(void *ptr)
{
    heap_caps_free(ptr);
}

// --- Forward declaration from the SDK host functions module -----------------
extern "C" void wasm_host_functions_register();

// ── Struct for passing args to the pthread runner ─────────────────────────
struct LoadRunArgs {
	const uint8_t *wasm_bytes;
	std::size_t wasm_size;
	const char *func_name;
	uint32_t timeout_ms;
	volatile bool completed;
	wasm::SandboxResult result;

	// Shared module instance handle — set by the pthread after
	// instantiation, so the watchdog thread can call
	// wasm_runtime_terminate() to forcibly terminate an infinite
	// WASM loop.
	wasm_module_inst_t module_inst;
};

/**
 * @brief Drive on_tick() until the skill stops, traps, or the watchdog fires.
 *
 * Runs on the WASM thread after on_start() has returned, on its own exec_env
 * -- the same pattern on_stop already uses, and safe for the same reason:
 * the calls are sequential, never concurrent, on one module instance.
 */
static void wasm_run_tick_loop(wasm_module_inst_t inst)
{
	const int period_ms = s_tick_period_ms.load();
	if (period_ms <= 0 || s_cancelled.load()) return;

	wasm_function_inst_t tick_fn =
		wasm_runtime_lookup_function(inst, "on_tick", nullptr);
	if (!tick_fn) {
		// Asking to tick without exporting on_tick is a build mistake, not a
		// runtime one: say so rather than silently ending the skill.
		ESP_LOGW(TAG, "mpx_tick_every(%d) was called but the module exports no "
		              "on_tick -- add MPX_EXPORT void on_tick(int dt_ms)", period_ms);
		return;
	}

	wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, DEFAULT_STACK_SIZE);
	if (!env) {
		ESP_LOGE(TAG, "on_tick: could not create exec env");
		return;
	}

	ESP_LOGI(TAG, "on_tick: starting at %d ms", period_ms);

	const int64_t period_us = static_cast<int64_t>(period_ms) * 1000;
	int64_t next_us  = esp_timer_get_time() + period_us;
	int64_t prev_us  = esp_timer_get_time();
	int     overruns = 0;
	uint32_t ticks   = 0;

	s_running = true;
	while (!s_cancelled.load() && !s_tick_stop.load()) {
		// Sleep to the next boundary. Absolute rather than relative so the
		// period does not drift by however long the last tick took.
		int64_t now_us = esp_timer_get_time();
		if (next_us > now_us) {
			const int64_t wait_ms = (next_us - now_us) / 1000;
			if (wait_ms > 0) vTaskDelay(pdMS_TO_TICKS(wait_ms));
			else             vTaskDelay(1);
		} else {
			// Already late: yield once so a fast period cannot starve
			// anything else on this core, then carry on.
			vTaskDelay(1);
		}
		if (s_cancelled.load() || s_tick_stop.load()) break;

		now_us = esp_timer_get_time();
		uint32_t argv[1] = { static_cast<uint32_t>((now_us - prev_us) / 1000) };
		prev_us = now_us;

		const int64_t call_start = now_us;
		if (!wasm_runtime_call_wasm(env, tick_fn, 1, argv)) {
			const char *exc = wasm_runtime_get_exception(inst);
			ESP_LOGE(TAG, "on_tick trapped after %" PRIu32 " ticks: %s",
			         ticks, exc ? exc : "unknown");
			wasm_runtime_clear_exception(inst);
			break;
		}
		++ticks;

		const int64_t took_us = esp_timer_get_time() - call_start;
		if (took_us > period_us) {
			if (++overruns >= TICK_OVERRUN_LIMIT) {
				ESP_LOGW(TAG, "on_tick overran %d ms %d times in a row "
				              "(last %lld ms); stopping the tick loop",
				         period_ms, overruns, (long long)(took_us / 1000));
				break;
			}
		} else if (overruns) {
			overruns = 0;
		}

		next_us += period_us;
		// If we fell far behind, resynchronise instead of trying to catch up
		// by running a burst of back-to-back ticks.
		if (next_us < esp_timer_get_time()) next_us = esp_timer_get_time() + period_us;
	}
	s_running = false;

	ESP_LOGI(TAG, "on_tick: stopped after %" PRIu32 " ticks", ticks);
	wasm_runtime_destroy_exec_env(env);
}

/**
 * @brief Run the full wasm lifecycle in a pthread.
 *
 * WAMR's ESP-IDF platform layer uses pthread_self() internally, so ALL
 * operations that touch WAMR (load, instantiate, lookup, execute) MUST
 * run in a thread created via pthread_create, not a bare xTaskCreate'd
 * FreeRTOS task.
 */
static void *wasm_load_run_thread(void *arg)
{
	auto *args = static_cast<LoadRunArgs *>(arg);

	char error_buf[256];

	// 2. Load the module
	wasm_module_t module = wasm_runtime_load(
		const_cast<uint8_t *>(args->wasm_bytes),
		static_cast<uint32_t>(args->wasm_size),
		error_buf, sizeof(error_buf));

	if (!module) {
		ESP_LOGE(TAG, "WAMR load failed: %s", error_buf);
		args->result = wasm::SandboxResult::LoadFailed;
		args->completed = true;
		return nullptr;
	}

	// 3. Instantiate
	const char *func_name = args->func_name;
	if (!func_name || func_name[0] == '\0') {
		func_name = "on_start";
	}

	wasm_module_inst_t inst = wasm_runtime_instantiate(
		module,
		DEFAULT_STACK_SIZE,
		DEFAULT_HEAP_SIZE,
		error_buf, sizeof(error_buf));

	if (!inst) {
		ESP_LOGE(TAG, "WAMR instantiate failed: %s", error_buf);
		wasm_runtime_unload(module);
		args->result = wasm::SandboxResult::InstantiateFailed;
		args->completed = true;
		return nullptr;
	}

	// Share the instance handle so the watchdog can terminate us
	args->module_inst = inst;

	ESP_LOGI(TAG, "Module instantiated (stack=%zuKB, linear_mem=%zuKB)",
			 DEFAULT_STACK_SIZE / 1024, DEFAULT_HEAP_SIZE / 1024);

	// 4. Look up the exported function
	wasm_function_inst_t func = wasm_runtime_lookup_function(inst, func_name, nullptr);
	if (!func) {
		func = wasm_runtime_lookup_function(inst, "_start", nullptr);
	}
	if (!func) {
		ESP_LOGW(TAG, "No exported function '%s' or '_start' found", func_name);
		args->module_inst = nullptr;
		wasm_runtime_deinstantiate(inst);
		wasm_runtime_unload(module);
		args->result = wasm::SandboxResult::FunctionNotFound;
		args->completed = true;
		return nullptr;
	}

	// 5. Create exec env and call the function
	wasm_exec_env_t exec_env = wasm_runtime_create_exec_env(inst,
															DEFAULT_STACK_SIZE);
	if (!exec_env) {
		ESP_LOGE(TAG, "Failed to create exec env");
		args->module_inst = nullptr;
		wasm_runtime_deinstantiate(inst);
		wasm_runtime_unload(module);
		args->result = wasm::SandboxResult::ExecutionFailed;
		args->completed = true;
		return nullptr;
	}

	uint32_t argv[1] = {0};

	// Per-run state. Arbitration starts unclaimed so a skill that never calls
	// mpx_control_take() sees exactly the v2 write behaviour, and the clock
	// starts here so mpx_millis() is 0 on the first instruction of on_start().
	sdk::control_reset();
	sdk::forget_skill_gains();   /* no record survives from the previous run */
	s_tick_period_ms = 0;
	s_tick_stop      = false;
	s_hard_killed    = false;
	util::trace_ring_reset();
	s_start_us = esp_timer_get_time();

	s_running = true;
	bool exec_ok = wasm_runtime_call_wasm(exec_env, func, 0, argv);
	s_running = false;

	// ── on_tick ─────────────────────────────────────────────────────────────
	// Only after a clean on_start. A skill that trapped on the way in has no
	// business being handed the joints every 20 ms.
	if (exec_ok && !s_cancelled.load()) {
		wasm_run_tick_loop(inst);
	}

	// ── on_stop ─────────────────────────────────────────────────────────────
	// Optional export, called with why the skill ended:
	//   0 = returned normally   1 = trapped   2 = watchdog
	//
	// It runs only when the instance is still callable, which means NOT after a
	// hard watchdog terminate — wasm_runtime_terminate() has already unwound
	// that instance and calling into it is undefined. A skill that must park
	// deliberately has to finish before the timeout; one that hits the timeout
	// gets the firmware's safe stop below instead, which halts motion rather
	// than guessing at a pose.
	//
	// on_stop shares the run's remaining time budget. It is not a second 60 s.
	{
		// NOT s_cancelled: a behaviour asked to stop is cancelled but its
		// instance is intact, and parking the robot is exactly what it needs
		// to do on the way out.
		const bool hard_killed = s_hard_killed.load();
		if (!hard_killed) {
			if (!exec_ok) wasm_runtime_clear_exception(inst);
			wasm_function_inst_t stop_fn =
				wasm_runtime_lookup_function(inst, "on_stop", nullptr);
			if (stop_fn) {
				wasm_exec_env_t stop_env =
					wasm_runtime_create_exec_env(inst, DEFAULT_STACK_SIZE);
				if (stop_env) {
					// 0 returned normally · 1 trapped · 3 asked to stop
					uint32_t sargv[1] = { !exec_ok       ? 1u
					                    : s_cancelled.load() ? 3u
					                                         : 0u };
					ESP_LOGI(TAG, "calling on_stop(%" PRIu32 ")", sargv[0]);
					s_running = true;
					if (!wasm_runtime_call_wasm(stop_env, stop_fn, 1, sargv)) {
						ESP_LOGW(TAG, "on_stop trapped; ignoring");
						wasm_runtime_clear_exception(inst);
					}
					s_running = false;
					wasm_runtime_destroy_exec_env(stop_env);
				}
			}
		} else {
			// Watchdog path: stop moving. Deliberately GaitCmd::None and not
			// Init — halting is unambiguously safe, whereas standing up is
			// itself a motion and could be the last thing a tipped-over robot
			// should attempt.
			ESP_LOGW(TAG, "skill killed by watchdog; halting gait");
			robot::send_gait_cmd(robot::GaitCmd::None);
		}
	}
	wasm::set_params(nullptr);   // parameters never leak into the next run

	// Put back any gains the skill changed, to whatever they were before it
	// ran. Gains live on the driver boards, so they outlive the module: a skill
	// that leaves Kp at 95 makes every built-in gait afterwards walk slightly
	// wrong, with nothing on screen to explain it. Same class of leak as the
	// overlay below, and it needs the same treatment.
	//
	// BEFORE the bus is released, and that ordering is load-bearing: a config
	// write is a request/reply PAIR, and gait traffic in between loses the
	// reply. Restoring after the gait resumes would fail on most of the writes.
	//
	// To keep a tuning deliberately, mpx_gain_save() burns it to the board's
	// own flash, which this does not touch.
	if (robot::servo_owned_by_skill()) {
		sdk::restore_skill_gains();
	} else {
		// The skill released the bus itself, so take it back for the moment the
		// restore needs. If Servo Studio has claimed it meanwhile, servo_lock()
		// refuses and we leave the gains alone rather than fighting a human.
		if (robot::servo_lock()) {
			sdk::restore_skill_gains();
		} else {
			ESP_LOGW(TAG, "could not retake the bus to restore gains — they "
						  "stay as the skill left them");
			sdk::forget_skill_gains();
		}
	}

	// A skill that took the servo bus must not keep it. This runs whether the
	// skill returned cleanly, trapped, or was killed by the watchdog — otherwise
	// one crashed skill leaves the gait parked until the robot is rebooted.
	robot::release_skill_bus_lock();

	// An overlay outliving its skill would silently bias every later movement,
	// including the built-in gaits, with nothing on screen to explain it.
	robot::clear_overlay();

	wasm_runtime_destroy_exec_env(exec_env);

	if (!exec_ok) {
		const char *exc = wasm_runtime_get_exception(inst);
		ESP_LOGE(TAG, "WASM execution failed: %s",
				 exc ? exc : "unknown error");
		args->module_inst = nullptr;
		wasm_runtime_deinstantiate(inst);
		wasm_runtime_unload(module);
		args->result = wasm::SandboxResult::ExecutionFailed;
		args->completed = true;
		return nullptr;
	}

	ESP_LOGI(TAG, "WASM function '%s' completed successfully", func_name);

	// 6. Clean up
	args->module_inst = nullptr;
	wasm_runtime_deinstantiate(inst);
	wasm_runtime_unload(module);

	args->result = wasm::SandboxResult::Success;
	args->completed = true;
	return nullptr;
}

// ============================================================================
//  Public API
// ============================================================================

namespace wasm {

bool init_sandbox()
{
	if (s_initialized) {
		ESP_LOGW(TAG, "Sandbox already initialised");
		return true;
	}

	RuntimeInitArgs init_args;
	memset(&init_args, 0, sizeof(init_args));

	init_args.mem_alloc_type = Alloc_With_Allocator;
	init_args.mem_alloc_option.allocator.malloc_func = (void *)wamr_malloc;
	init_args.mem_alloc_option.allocator.realloc_func = (void *)wamr_realloc;
	init_args.mem_alloc_option.allocator.free_func = (void *)wamr_free;

	if (!wasm_runtime_full_init(&init_args)) {
		ESP_LOGE(TAG, "WAMR runtime initialisation failed");
		return false;
	}

	s_initialized = true;

	// Register host functions defined in the SDK module
	wasm_host_functions_register();

	ESP_LOGI(TAG, "WAMR sandbox initialised (mode=interp, allocator=psram)");

	return true;
}

bool register_natives(const char *module_name,
					  void *symbols, uint32_t count)
{
	if (!s_initialized) {
		ESP_LOGE(TAG, "Cannot register natives: sandbox not initialised");
		return false;
	}

	bool ok = wasm_runtime_register_natives(
		module_name,
		static_cast<NativeSymbol *>(symbols),
		count);

	if (!ok) {
		ESP_LOGE(TAG, "Failed to register %" PRIu32 " native(s) in module '%s'",
				 count, module_name);
	} else {
		ESP_LOGI(TAG, "Registered %" PRIu32 " native(s) in module '%s'",
				 count, module_name);
	}

	return ok;
}

SandboxResult load_and_run(const char *path,
						   const char *func_name,
						   uint32_t timeout_ms)
{
	if (!s_initialized) {
		ESP_LOGE(TAG, "Sandbox not initialised");
		return SandboxResult::NotInitialized;
	}

	// ── Extension whitelist & routing ──────────────────────────
	// .wasm → plain WASM, pass directly to WAMR.
	// .mpxe → MPXE encrypted blob, decrypt before loading.
	// Anything else is rejected before any file I/O.
	const char *ext = std::strrchr(path, '.');
	if (!ext || (std::strcmp(ext, ".wasm") != 0 &&
				 std::strcmp(ext, ".mpxe") != 0)) {
		ESP_LOGE(TAG, "Rejected '%s': unsupported extension '%s' "
				 "(only .wasm and .mpxe are allowed)", path,
				 ext ? ext : "(none)");
		return SandboxResult::LoadFailed;
	}

	bool is_mpxe = (std::strcmp(ext, ".mpxe") == 0);

	// Read file from LittleFS
	auto raw_bytes = fs::read_file(path);
	if (raw_bytes.empty()) {
		ESP_LOGE(TAG, "Failed to read '%s' from LittleFS", path);
		return SandboxResult::LoadFailed;
	}

	ESP_LOGI(TAG, "Read %zu bytes from '%s'", raw_bytes.size(), path);

	// ── Route by extension ─────────────────────────────────────
	uint8_t *wasm_data = nullptr;
	size_t wasm_size = 0;
	bool needs_free = false;
	SandboxResult result;

	if (is_mpxe) {
		// .mpxe → decrypt the encrypted blob
		DecryptResult dr = decrypt_mpxe(raw_bytes.data(), raw_bytes.size(),
										&wasm_data, &wasm_size);
		if (dr != DecryptResult::Success) {
			ESP_LOGE(TAG, "MPXE decryption failed (result=%d), rejecting skill",
					 static_cast<int>(dr));
			return SandboxResult::LoadFailed;
		}

		ESP_LOGI(TAG, "Decrypted MPXE blob → %zu bytes plain WASM", wasm_size);
		needs_free = true;

		// Diagnostic: print WASM magic + version
		if (wasm_size >= 8) {
			ESP_LOGI(TAG, "WASM header: %02x %02x %02x %02x %02x %02x %02x %02x",
					 wasm_data[0], wasm_data[1], wasm_data[2], wasm_data[3],
					 wasm_data[4], wasm_data[5], wasm_data[6], wasm_data[7]);
		} else {
			ESP_LOGW(TAG, "Decrypted WASM too small: %zu bytes (min 8)", wasm_size);
		}
	} else {
		// .wasm → plain, pass through unchanged (developer mode)
		wasm_data = const_cast<uint8_t *>(raw_bytes.data());
		wasm_size = raw_bytes.size();
	}

	result = load_and_run_bytes(wasm_data, wasm_size,
								func_name, timeout_ms);

	// Zero and free the decrypted buffer if we allocated one
	if (needs_free && wasm_data) {
		// Secure zero before free — see §6.3 Step 6 of WASM_ENCRYPTION.md
		volatile uint8_t *p = wasm_data;
		for (size_t i = 0; i < wasm_size; i++) {
			p[i] = 0;
		}
		std::free(wasm_data);
	}

	return result;
}

SandboxResult load_and_run_bytes(const uint8_t *wasm_bytes,
								 std::size_t wasm_size,
								 const char *func_name,
								 uint32_t timeout_ms)
{
	if (!s_initialized) {
		ESP_LOGE(TAG, "Sandbox not initialised");
		return SandboxResult::NotInitialized;
	}

	// Diagnostic: log WASM header for troubleshooting
	if (wasm_size >= 8) {
		ESP_LOGI(TAG, "WASM header: %02x %02x %02x %02x %02x %02x %02x %02x",
				 wasm_bytes[0], wasm_bytes[1], wasm_bytes[2], wasm_bytes[3],
				 wasm_bytes[4], wasm_bytes[5], wasm_bytes[6], wasm_bytes[7]);
	}

	// Everything that touches WAMR (load, instantiate, lookup, execute)
	// must run in a pthread, not a FreeRTOS task, because WAMR's ESP-IDF
	// platform layer calls pthread_self() internally.
	pthread_t thread;
	pthread_attr_t attr;
	LoadRunArgs args = { wasm_bytes, wasm_size, func_name, timeout_ms,
						 false, SandboxResult::NotInitialized, nullptr };

	pthread_attr_init(&attr);
	// Native C thread stack (internal RAM) — separate from the WASM operand
	// stack above.  16 KB is plenty for WAMR's interpreter recursion; keep it
	// fixed so growing the WASM operand stack doesn't inflate internal-RAM use.
	pthread_attr_setstacksize(&attr, 16 * 1024);

	int ret = pthread_create(&thread, &attr, wasm_load_run_thread, &args);
	pthread_attr_destroy(&attr);

	if (ret != 0) {
		ESP_LOGE(TAG, "Failed to create loader thread: %d", ret);
		return SandboxResult::ExecutionFailed;
	}

	// Wait with polling timeout (ESP-IDF lacks timedjoin/tryjoin)
	// timeout_ms == 0 is a BEHAVIOUR: no total time limit. That is safe only
	// because the tick loop bounds each individual on_tick() call and stops
	// after repeated overruns, and because request_stop() can always end it.
	// A one-shot skill keeps its watchdog.
	if (timeout_ms > 0) {
		const int poll_ms = 10;
		int elapsed = 0;
		while (elapsed < static_cast<int>(timeout_ms) && !args.completed) {
			vTaskDelay(pdMS_TO_TICKS(poll_ms));
			elapsed += poll_ms;
		}
		if (!args.completed) {
			ESP_LOGW(TAG, "WASM execution exceeded %" PRIu32 " ms watchdog — "
					 "signalling cooperative cancellation", timeout_ms);

			s_cancelled   = true;
			s_hard_killed = true;

			if (args.module_inst != nullptr) {
				ESP_LOGW(TAG, "Calling wasm_runtime_terminate()");
				wasm_runtime_terminate(args.module_inst);
			}

			while (!args.completed) {
				vTaskDelay(pdMS_TO_TICKS(poll_ms));
			}

			pthread_join(thread, nullptr);
			s_cancelled   = false;
			s_hard_killed = false;
			return SandboxResult::Timeout;
		}
	} else {
		while (!args.completed) {
			vTaskDelay(pdMS_TO_TICKS(10));
		}
	}

	pthread_join(thread, nullptr);
	s_cancelled   = false;
	s_hard_killed = false;
	return args.result;
}


std::uint32_t skill_millis()
{
	const int64_t start = s_start_us.load();
	if (start == 0) return 0;
	const int64_t now = esp_timer_get_time();
	return static_cast<std::uint32_t>((now - start) / 1000);
}

void tick_every(int period_ms)
{
	if (period_ms <= 0) { s_tick_period_ms = 0; return; }
	// The floor is one FreeRTOS tick; below that the loop would busy-wait
	// against the scheduler rather than run faster. The ceiling keeps a
	// mistyped period from looking like a hang.
	if (period_ms < 10)   period_ms = 10;
	if (period_ms > 1000) period_ms = 1000;
	s_tick_period_ms = period_ms;
}

void tick_stop()
{
	s_tick_stop = true;
}

void request_stop()
{
	// Both flags: s_tick_stop ends the loop at the end of this tick, and
	// s_cancelled makes any long host call the skill is inside (a delay, a bus
	// read) return promptly instead of running to completion first.
	s_tick_stop  = true;
	s_cancelled  = true;
}

bool stopping()
{
	return s_cancelled.load() || s_tick_stop.load();
}

int tick_period()
{
	return s_tick_period_ms.load();
}

void set_params(const char *kv)
{
	s_param_count = 0;
	if (!kv || !*kv) return;

	const char *p = kv;
	while (*p && s_param_count < MAX_PARAMS) {
		while (*p == ' ' || *p == ',' || *p == ';') ++p;
		if (!*p) break;

		const char *name_start = p;
		while (*p && *p != '=' && *p != ',' && *p != ';') ++p;
		if (*p != '=') {                       // malformed pair; skip it
			while (*p && *p != ',' && *p != ';') ++p;
			continue;
		}
		int n = static_cast<int>(p - name_start);
		if (n > MAX_PARAM_NAME - 1) n = MAX_PARAM_NAME - 1;
		++p;                                    // step over '='

		ParamKV &slot = s_params[s_param_count];
		std::memcpy(slot.name, name_start, static_cast<std::size_t>(n));
		slot.name[n] = '\0';
		slot.value = std::strtof(p, nullptr);
		++s_param_count;

		while (*p && *p != ',' && *p != ';') ++p;
	}
	ESP_LOGI(TAG, "skill parameters: %d", s_param_count);
}

bool param_get(const char *name, float *out)
{
	if (!name || !out) return false;
	for (int i = 0; i < s_param_count; ++i) {
		if (std::strcmp(s_params[i].name, name) == 0) {
			*out = s_params[i].value;
			return true;
		}
	}
	return false;
}

bool was_cancelled()
{
	return s_cancelled.load();
}

bool is_running()
{
	return s_running.load();
}

void destroy_sandbox()
{
	if (!s_initialized) {
		return;
	}

	wasm_runtime_destroy();
	s_initialized = false;

	ESP_LOGI(TAG, "WAMR sandbox destroyed");
}

}  // namespace wasm

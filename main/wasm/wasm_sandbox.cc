#include "wasm/wasm_sandbox.h"
#include "wasm/wasm_decrypt.h"

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

#include "fs/littlefs_manager.h"
#include "robot/robot.h"
#include "sdk/wasm_host_functions.h"
#include "util/trace_ring.h"

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

// ── Watchdog extension ──────────────────────────────────────
// Milliseconds a host function spent waiting on something that is not the
// skill running -- fs.write's / fs.delete's permission prompt is the only
// caller. The watchdog loop drains this into its own budget each poll, so
// that wait does not count against the skill's deadline, exactly as
// lua_vm_extend_deadline() does for the Lua VM.
static std::atomic<std::uint32_t> s_watchdog_extra_ms{0};

// ── Per-run clock ────────────────────────────────────────────
// Origin for wasm::skill_millis(). Set just before on_start() is called, so a
// skill measuring a frame does not have to subtract a boot time it has no way
// to observe.
static std::atomic<std::uint32_t> s_start_ms{0};

// ── Per-run parameters ───────────────────────────────────────
// Sixteen is well past what a skill's UI can usefully show, and a fixed array
// keeps this off the heap in a path that already runs under a watchdog.
static constexpr int MAX_PARAMS = 16;
static constexpr int MAX_PARAM_NAME = 24;
struct ParamKV { char name[MAX_PARAM_NAME]; float value; };
static ParamKV s_params[MAX_PARAMS];
static int s_param_count = 0;

// ── Tick loop state ──────────────────────────────────────────
// Cross-thread: the wasm thread sets the period from inside on_start(), the
// HTTP thread can ask for a stop, and the loop itself reads both.
static std::atomic<int>  s_tick_period_ms{0};
static std::atomic<bool> s_tick_stop{false};
static std::atomic<bool> s_stopping{false};

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

static constexpr int TICK_OVERRUN_LIMIT = 3;

/* Run on_tick() at a fixed period until the skill asks to stop.
 *
 * Its own exec_env, because WAMR's is thread-bound: the loop cannot borrow the
 * one on_start() ran in.
 *
 * Sleeps to an ABSOLUTE next boundary rather than sleeping for a duration.
 * 600 frames of delay(16) run long by however much 600 frames of host calls
 * cost; 600 frames of "sleep until start + n*16" do not.
 *
 * Deliberately not driven from gait_task(). WAMR's exec_env is thread-bound and
 * putting arbitrary downloaded code inside a priority-22 control loop is not
 * acceptable. The consequence is that a tick is not phase-locked to a gait
 * frame - which is what the overlay exists to cover. */
static void run_tick_loop(wasm_module_inst_t inst)
{
	const int period = s_tick_period_ms.load();
	if (period <= 0) return;

	wasm_function_inst_t fn = wasm_runtime_lookup_function(inst, "on_tick", nullptr);
	if (!fn) {
		/* A build mistake, not a runtime error: the skill asked to be ticked
		 * but does not export the function. Say so and let the run finish
		 * normally rather than reporting a failure the author cannot act on. */
		ESP_LOGW(TAG, "mpx_tick_every() was called but the module exports no "
					  "on_tick() - ending the run without a tick loop");
		return;
	}

	wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, DEFAULT_STACK_SIZE);
	if (!env) {
		ESP_LOGE(TAG, "tick loop: could not create exec env");
		return;
	}

	uint32_t next = s_start_ms.load() + static_cast<uint32_t>(period);
	int overruns = 0;

	while (!s_tick_stop.load() && !s_cancelled.load()) {
		const uint32_t now =
			static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
		const int32_t remaining = static_cast<int32_t>(next - now);
		if (remaining > 0) {
			vTaskDelay(pdMS_TO_TICKS(remaining));
			continue;
		}

		uint32_t argv[1] = { static_cast<uint32_t>(period) };
		if (!wasm_runtime_call_wasm(env, fn, 1, argv)) {
			const char *exc = wasm_runtime_get_exception(inst);
			ESP_LOGW(TAG, "on_tick() threw: %s", exc ? exc : "unknown error");
			break;
		}

		next += static_cast<uint32_t>(period);

		const uint32_t after =
			static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
		if (static_cast<int32_t>(after - next) > period) {
			if (++overruns >= TICK_OVERRUN_LIMIT) {
				ESP_LOGW(TAG, "on_tick() overran its %d ms period %d times in a "
							  "row - stopping the loop rather than falling further "
							  "behind", period, overruns);
				break;
			}
			/* Resynchronise instead of trying to catch up for ever: a tick that
			 * is late is worth less than the next one being on time. */
			next = after + static_cast<uint32_t>(period);
		} else {
			overruns = 0;
		}
	}

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

	// ── Per-run setup, before anything can call a host function ────────
	//
	// The order below is the order the teardown unwinds, and it is not
	// arbitrary:
	//
	//  1. arbitration back to NONE, so a claim left by the previous run cannot
	//     refuse this one's very first call
	//  2. forget the previous run's gain bookkeeping, so a stale "before" value
	//     cannot be restored over a value this run legitimately sets
	//  3. tick state cleared - on_start() is allowed to ask for a period
	//  4. the trace ring emptied, so one run's samples do not appear in the
	//     next run's chart
	//  5. the clock origin set, because on_start() may itself measure time
	sdk::control_reset();
	sdk::forget_skill_gains();
	s_tick_period_ms = 0;
	s_tick_stop      = false;
	s_stopping       = false;
	util::trace_ring_reset();
	s_start_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000ULL);

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

	s_running = true;
	bool exec_ok = wasm_runtime_call_wasm(exec_env, func, 0, argv);
	wasm_runtime_destroy_exec_env(exec_env);

	// on_start() has returned. If it asked for a tick period, run the loop -
	// unless it trapped or the run is being cancelled, in which case there is
	// nothing left to tick.
	if (exec_ok && !s_cancelled.load() && s_tick_period_ms.load() > 0) {
		run_tick_loop(inst);
	}

	s_running = false;

	// on_stop(reason): 0 = returned normally, 1 = trapped, 3 = cancelled.
	// The numbers are the contract the C SDK documents; do not renumber them.
	{
		const int32_t reason = !exec_ok ? 1 : (s_cancelled.load() ? 3 : 0);
		wasm_function_inst_t stop_fn =
			wasm_runtime_lookup_function(inst, "on_stop", nullptr);
		if (stop_fn) {
			wasm_exec_env_t senv =
				wasm_runtime_create_exec_env(inst, DEFAULT_STACK_SIZE);
			if (senv) {
				uint32_t sargv[1] = { static_cast<uint32_t>(reason) };
				wasm_runtime_call_wasm(senv, stop_fn, 1, sargv);
				wasm_runtime_destroy_exec_env(senv);
			}
		}
	}

	if (!exec_ok) {
		const char *exc = wasm_runtime_get_exception(inst);
		ESP_LOGE(TAG, "WASM execution failed: %s",
				 exc ? exc : "unknown error");
	}

	// ── Per-run teardown ───────────────────────────────────────────────
	wasm::set_params(nullptr);

	/* Gains live on the AT32 boards, not in the module, so they outlive the
	 * skill that wrote them - the lock being released, the run returning, even
	 * the skill crashing. That is deliberate: it is how a skill tunes the
	 * motors and then lets the firmware's own gait walk with that tuning. It is
	 * also a trap, and the same shape of trap the sandbox already closes for
	 * overlays and for the bus lock: a skill that leaves Kp at 95 makes every
	 * built-in gait afterwards walk slightly wrong, with nothing on screen to
	 * explain why.
	 *
	 * So put back what was there BEFORE the skill's first write to each slot -
	 * not "restore stock", which would throw away a tuning a human set from
	 * Servo Studio and has every reason to expect to still be theirs.
	 *
	 * This must happen while the bus is still held: a config write is a
	 * request/reply pair and the firmware refuses it without the lock. */
	if (robot::servo_owned_by_skill()) {
		sdk::restore_skill_gains();
	} else if (robot::servo_lock()) {
		sdk::restore_skill_gains();
		robot::servo_unlock();
	} else {
		/* Servo Studio has the bus. Do not fight a human for it just to put
		 * gains back; leave them and say so, rather than reporting success. */
		sdk::forget_skill_gains();
		ESP_LOGW(TAG, "could not retake the servo bus to restore gains this "
					  "skill changed - they are still in effect");
	}

	robot::release_skill_bus_lock();
	robot::clear_overlay();   /* an overlay must not outlive its skill */
	sdk::control_reset();
	s_tick_period_ms = 0;
	s_tick_stop      = false;
	s_stopping       = false;

	ESP_LOGI(TAG, "WASM function '%s' %s", func_name,
			 exec_ok ? "completed" : "failed");

	// 6. Clean up
	args->module_inst = nullptr;
	wasm_runtime_deinstantiate(inst);
	wasm_runtime_unload(module);

	args->result = exec_ok ? wasm::SandboxResult::Success
						   : wasm::SandboxResult::ExecutionFailed;
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
	// A fresh run starts with a fresh watchdog budget: whatever the previous
	// skill asked for must not carry over.
	s_watchdog_extra_ms = 0;

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
	if (timeout_ms > 0) {
		const int poll_ms = 10;
		int elapsed = 0;
		// The budget grows whenever a host function reports time it spent
		// waiting on the user (see extend_watchdog).
		std::uint32_t budget = timeout_ms;
		while (elapsed < static_cast<int>(budget) && !args.completed) {
			vTaskDelay(pdMS_TO_TICKS(poll_ms));
			elapsed += poll_ms;
			budget += s_watchdog_extra_ms.exchange(0);
		}
		if (!args.completed) {
			ESP_LOGW(TAG, "WASM execution exceeded %" PRIu32 " ms watchdog — "
					 "signalling cooperative cancellation", timeout_ms);

			s_cancelled = true;

			if (args.module_inst != nullptr) {
				ESP_LOGW(TAG, "Calling wasm_runtime_terminate()");
				wasm_runtime_terminate(args.module_inst);
			}

			while (!args.completed) {
				vTaskDelay(pdMS_TO_TICKS(poll_ms));
			}

			pthread_join(thread, nullptr);
			s_cancelled = false;
			return SandboxResult::Timeout;
		}
	} else {
		while (!args.completed) {
			vTaskDelay(pdMS_TO_TICKS(10));
		}
	}

	pthread_join(thread, nullptr);
	s_cancelled = false;
	return args.result;
}

bool was_cancelled()
{
	return s_cancelled.load();
}

void extend_watchdog(std::uint32_t ms)
{
	s_watchdog_extra_ms.fetch_add(ms);
}

bool is_running()
{
	return s_running.load();
}

// ── Per-run clock ────────────────────────────────────────────

std::uint32_t skill_millis()
{
	const std::uint32_t now =
		static_cast<std::uint32_t>(esp_timer_get_time() / 1000ULL);
	return now - s_start_ms.load();
}

// ── Skill parameters ─────────────────────────────────────────

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
		if (*p != '=') {                       /* malformed pair; skip it */
			while (*p && *p != ',' && *p != ';') ++p;
			continue;
		}
		int n = static_cast<int>(p - name_start);
		if (n > MAX_PARAM_NAME - 1) n = MAX_PARAM_NAME - 1;
		++p;                                    /* step over '=' */

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

// ── Tick loop control ────────────────────────────────────────

void tick_every(int period_ms)
{
	if (period_ms <= 0) {
		s_tick_period_ms = 0;
		return;
	}
	/* 10 ms floor: a downloaded skill asking to run at 1 kHz does not get to
	 * decide that on behalf of the whole robot. 1 s ceiling: past that it is a
	 * delay loop with extra steps and on_stop() is the cheaper shape. */
	if (period_ms < 10)   period_ms = 10;
	if (period_ms > 1000) period_ms = 1000;
	s_tick_period_ms = period_ms;
}

void tick_stop()
{
	s_tick_stop = true;
}

int tick_period()
{
	return s_tick_period_ms.load();
}

void request_stop()
{
	/* Both flags on purpose. s_tick_stop ends the loop at the next tick;
	 * s_cancelled aborts any long host call the skill is currently inside, so
	 * "stop" does not wait out a robot_delay_ms(5000) first. */
	s_tick_stop = true;
	s_stopping  = true;
	s_cancelled = true;
}

bool stopping()
{
	return s_stopping.load();
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

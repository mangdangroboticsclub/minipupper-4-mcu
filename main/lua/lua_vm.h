#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the Lua VM with PSRAM heap.
 *        Registers all built-in modules and robot bindings.
 *        Safe to call multiple times (idempotent — returns ESP_OK
 *        if already initialised).
 *
 * @return ESP_OK on success, ESP_FAIL if Lua state creation fails.
 */
esp_err_t lua_init(void);

/**
 * @brief Run a Lua script from a C string (synchronous).
 *        Blocks until the script completes or timeout expires.
 *
 * There is one Lua state, shared by every caller (the HTTP server and the
 * chat worker), so runs are serialised: a caller
 * waits up to 10 s for the one before it, then gets ESP_ERR_INVALID_STATE.
 * On an error the message is appended to the output after anything the
 * script printed.
 *
 * @param script      Lua source code (null-terminated)
 * @param output      Output buffer (stdout capture — written via print redirect)
 * @param output_size Size of output buffer
 * @param timeout_ms  Max execution time in ms (0 = no limit)
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if timeout reached,
 *         ESP_ERR_INVALID_STATE if the VM is down or busy,
 *         ESP_FAIL if Lua runtime error occurred
 */
esp_err_t lua_run_string(const char *script,
                         char *output, size_t output_size,
                         uint32_t timeout_ms);

/**
 * @brief Run a Lua script from a LittleFS file (synchronous).
 *
 * Unlike an inline snippet, a file runs in its own global environment
 * (reads fall through to the shared one: robot, display, tools, ...), sees
 * a global `args` table ({} here), and require() searches the script's own
 * folder first. Modules it loads are dropped when it ends.
 *
 * @param path        "/lua/test.lua" (relative to /fs), "/fs/...", "/system/...",
 *                    or a bare name under /fs/lua
 * @param output      Output buffer (stdout capture)
 * @param output_size Size of output buffer
 * @param timeout_ms  Max execution time in ms (0 = no limit)
 * Serialised and reported like lua_run_string().
 *
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if file missing,
 *         ESP_ERR_TIMEOUT if timeout reached,
 *         ESP_ERR_INVALID_STATE if the VM is down or busy,
 *         ESP_FAIL if Lua runtime error occurred
 */
esp_err_t lua_run_file(const char *path,
                       char *output, size_t output_size,
                       uint32_t timeout_ms);

/**
 * @brief lua_run_file() with `args` taken from @p args_json (a JSON object;
 *        nullptr, "" or anything else gives {}).
 */
esp_err_t lua_run_file_args(const char *path, const char *args_json,
                            char *output, size_t output_size,
                            uint32_t timeout_ms);

/**
 * @brief For bindings that block in C (robot.delay_ms): raise the timeout
 *        error if the running script is past its deadline. The deadline is
 *        otherwise only checked between Lua instructions, which a script
 *        sleeping in C does not execute.
 */
void lua_vm_check_deadline(struct lua_State *L);

/**
 * @brief Push the running script's deadline back by @p ms -- for time spent
 *        waiting on the user (fs.write's permission prompt), not running.
 */
void lua_vm_extend_deadline(uint32_t ms);

/**
 * @brief Get the current Lua VM state (for advanced use).
 *        Returns NULL if lua_init() has not been called.
 */
struct lua_State *lua_get_state(void);

/**
 * @brief De-initialise the Lua VM and free all resources.
 *        Safe to call even if not initialised.
 */
void lua_deinit(void);

#ifdef __cplusplus
}
#endif

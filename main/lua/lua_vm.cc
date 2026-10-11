/*
 * SPDX-FileCopyrightText: 2026 MPX-Dog Project Contributors
 * SPDX-License-Identifier: MIT
 *
 * Lua VM lifecycle + execution wrappers for ESP32-S3.
 *
 * Uses PSRAM (via heap_caps_aligned_alloc) for the Lua state.
 * Captures print() output into a user-supplied buffer.
 * Supports synchronous execution with optional timeout.
 */

#include "lua_vm.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

#include "lua_bindings.h"

static const char *TAG = "lua_vm";

/* ── PSRAM-aware allocator for Lua ─────────────────────────────────────── */

/*
 * Lua uses realloc/free internally via l_newlstr, luaM_*, etc.
 * We hook the default allocator by overriding l_alloc() at state creation.
 * The default Lua allocator (l_alloc) calls free/realloc which eventually
 * go through newlib. To force PSRAM, we use a custom allocator.
 *
 * However, Lua's internal allocation pattern is fine-grained (many small
 * allocations for strings, tables, etc.). Using PSRAM for everything is
 * acceptable since our PSRAM is abundant (~7.5 MB free).
 */

static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;
    (void)osize;

    if (nsize == 0) {
        /* Free */
#ifdef CONFIG_SPIRAM
        heap_caps_free(ptr);
#else
        free(ptr);
#endif
        return NULL;
    }

    if (ptr == NULL) {
        /* Allocate */
#ifdef CONFIG_SPIRAM
        uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        void *mem = heap_caps_aligned_alloc(8, nsize, caps);
        if (mem == NULL) {
            /* Fall back to DRAM if PSRAM exhausted */
            mem = malloc(nsize);
        }
        return mem;
#else
        return malloc(nsize);
#endif
    }

    /* Realloc */
#ifdef CONFIG_SPIRAM
    void *new_ptr = heap_caps_aligned_alloc(8, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (new_ptr == NULL) {
        new_ptr = malloc(nsize);
    }
    if (new_ptr) {
        size_t copy_size = osize < nsize ? osize : nsize;
        memcpy(new_ptr, ptr, copy_size);
        heap_caps_free(ptr);
    }
    return new_ptr;
#else
    return realloc(ptr, nsize);
#endif
}

/* ── Global Lua state ─────────────────────────────────────────────────── */

static lua_State *g_L = NULL;
static SemaphoreHandle_t s_run_lock;   /* one script at a time; see run_chunk() */

/* ── Print capture ────────────────────────────────────────────────────── */

/*
 * We replace Lua's print/writeline to capture output into a buffer.
 * The buffer pointer is stored in the Lua registry.
 */
#define CAPTURE_BUF_REG_KEY "lua_vm_capture_buf"

typedef struct {
    char   *buf;
    size_t  size;
    size_t  written;
} capture_buf_t;

/*
 * Custom writer function for Lua's output.
 * Appends to the capture buffer (thread-local via registry).
 */
static int lua_print_writer(lua_State *L)
{
    /* Get the capture buffer from the registry */
    lua_pushstring(L, CAPTURE_BUF_REG_KEY);
    lua_rawget(L, LUA_REGISTRYINDEX);
    capture_buf_t *cap = (capture_buf_t *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (cap == NULL || cap->buf == NULL) {
        return 0; /* No capture buffer — silently discard */
    }

    /* Get the string to print (first argument) */
    int n = lua_gettop(L);
    lua_getglobal(L, "tostring");
    for (int i = 1; i <= n; i++) {
        if (i > 1) {
            /* Append tab separator */
            if (cap->written < cap->size - 1) {
                cap->buf[cap->written++] = '\t';
            }
        }
        lua_pushvalue(L, -1);       /* tostring function */
        lua_pushvalue(L, i);        /* argument */
        lua_call(L, 1, 1);          /* tostring(arg) */
        const char *s = lua_tostring(L, -1);
        if (s == NULL) s = "(nil)";
        size_t slen = strlen(s);
        size_t remaining = cap->size - cap->written - 1;
        if (slen > remaining) slen = remaining;
        if (slen > 0) {
            memcpy(cap->buf + cap->written, s, slen);
            cap->written += slen;
        }
        lua_pop(L, 1);              /* pop result */
    }
    lua_pop(L, 1); /* pop tostring */

    /* Append newline */
    if (cap->written < cap->size - 1) {
        cap->buf[cap->written++] = '\n';
    }
    cap->buf[cap->written] = '\0';

    return 0;
}

/* ── Initialisation ───────────────────────────────────────────────────── */

esp_err_t lua_init(void)
{
    if (g_L != NULL) {
        ESP_LOGW(TAG, "Lua VM already initialised");
        return ESP_OK;
    }

    if (s_run_lock == NULL) {
        s_run_lock = xSemaphoreCreateMutex();
        if (s_run_lock == NULL) {
            ESP_LOGE(TAG, "Failed to create the Lua run lock");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Create Lua state with PSRAM-aware allocator */
    g_L = lua_newstate(l_alloc, NULL, luaL_makeseed(NULL));
    if (g_L == NULL) {
        ESP_LOGE(TAG, "Failed to create Lua state (OOM)");
        return ESP_FAIL;
    }

    /* Open standard libraries */
    luaL_openlibs(g_L);

    /* Register robot bindings */
    lua_register_robot_bindings(g_L);

    /* Register wasm bindings (.wasm skill execution) */
    lua_register_wasm_bindings(g_L);

    /* Register crypto bindings (base64_decode for WASM deploy) */
    lua_register_crypto_bindings(g_L);

    /* Register fs bindings (file I/O with user permission) */
    lua_register_fs_bindings(g_L);

    /* Register display bindings (drawing on the screen's canvas) */
    lua_register_display_bindings(g_L);

    /* Override print to use our capture mechanism */
    /* We push our custom print function as _G.print */
    lua_pushcfunction(g_L, lua_print_writer);
    lua_setglobal(g_L, "print");

    /* Create Lua scripts directory on LittleFS (if it doesn't exist) */
    mkdir("/fs/lua", 0777);

    ESP_LOGI(TAG, "Lua VM initialised (PSRAM allocator, LUA 5.5)");
    return ESP_OK;
}

void lua_deinit(void)
{
    if (g_L) {
        lua_close(g_L);
        g_L = NULL;
        ESP_LOGI(TAG, "Lua VM deinitialised");
    }
}

lua_State *lua_get_state(void)
{
    return g_L;
}

/* ── Execution: one script at a time, with a deadline ─────────────────────
 *
 * The state is shared by the HTTP server (/v1/lua/run) and the chat worker,
 * each on its own task; s_run_lock makes them take turns. Before it they
 * simply raced on it.
 *
 * The timeout every caller passes was documented but not enforced, so a
 * `while true do end` held its task (the HTTP server, for /v1/lua/run)
 * forever. A count hook now checks the clock every kHookEvery instructions
 * and raises an error past the deadline; bindings that sleep in C call
 * lua_vm_check_deadline() themselves. Once past it the error comes back on
 * every check, so a script cannot pcall its way out. */

static constexpr int      kHookEvery = 1000;   /* VM instructions between clock checks */
static constexpr uint32_t kBusyWaitMs = 10000;

static int64_t           s_deadline_us;        /* 0 = no limit */
static bool              s_timed_out;

void lua_vm_check_deadline(lua_State *L)
{
    if (s_deadline_us != 0 && esp_timer_get_time() > s_deadline_us) {
        s_timed_out = true;
        luaL_error(L, "script timed out");
    }
}

void lua_vm_extend_deadline(uint32_t ms)
{
    if (s_deadline_us != 0) {
        s_deadline_us += (int64_t)ms * 1000;
    }
}

static void deadline_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    lua_vm_check_deadline(L);
}

static void capture_append(capture_buf_t *cap, const char *text)
{
    if (!cap->buf || cap->size == 0 || !text) return;
    size_t remaining = cap->size - cap->written - 1;
    size_t len = strlen(text);
    if (len > remaining) len = remaining;
    memcpy(cap->buf + cap->written, text, len);
    cap->written += len;
    cap->buf[cap->written] = '\0';
}

typedef int (*chunk_loader_t)(lua_State *L, const char *arg);

static int load_string(lua_State *L, const char *script) { return luaL_loadstring(L, script); }
static int load_file(lua_State *L, const char *path)     { return luaL_loadfile(L, path); }

/* Load with `loader`, run under the deadline, report into `output`. A load
 * failure returns `load_err` (a missing file and a syntax error are
 * different answers to the caller). */
static esp_err_t run_chunk(chunk_loader_t loader, const char *arg, esp_err_t load_err,
                           char *output, size_t output_size, uint32_t timeout_ms)
{
    if (output && output_size > 0) output[0] = '\0';
    if (g_L == NULL || s_run_lock == NULL) {
        ESP_LOGE(TAG, "Lua VM not initialised — call lua_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    capture_buf_t cap;
    cap.buf     = (output && output_size > 0) ? output : NULL;
    cap.size    = output_size;
    cap.written = 0;

    if (xSemaphoreTake(s_run_lock, pdMS_TO_TICKS(kBusyWaitMs)) != pdTRUE) {
        capture_append(&cap, "Lua is busy running another script; try again shortly.");
        return ESP_ERR_INVALID_STATE;
    }

    lua_pushstring(g_L, CAPTURE_BUF_REG_KEY);
    lua_pushlightuserdata(g_L, &cap);
    lua_rawset(g_L, LUA_REGISTRYINDEX);

    esp_err_t result = ESP_OK;
    if (loader(g_L, arg) != LUA_OK) {
        const char *err = lua_tostring(g_L, -1);
        ESP_LOGE(TAG, "Lua load error: %s", err ? err : "(unknown)");
        capture_append(&cap, err ? err : "could not load the script");
        lua_pop(g_L, 1);
        result = load_err;
    } else {
        s_timed_out = false;
        s_deadline_us = timeout_ms ? esp_timer_get_time() + (int64_t)timeout_ms * 1000 : 0;
        lua_sethook(g_L, deadline_hook, LUA_MASKCOUNT, kHookEvery);

        const int ret = lua_pcall(g_L, 0, 0, 0);

        lua_sethook(g_L, NULL, 0, 0);
        s_deadline_us = 0;
        if (ret != LUA_OK) {
            const char *err = lua_tostring(g_L, -1);
            ESP_LOGE(TAG, "Lua error: %s", err ? err : "(unknown)");
            if (cap.written > 0 && cap.buf[cap.written - 1] != '\n') {
                capture_append(&cap, "\n");
            }
            capture_append(&cap, err ? err : "(unknown error)");
            lua_pop(g_L, 1);
            result = s_timed_out ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }
    }

    lua_pushstring(g_L, CAPTURE_BUF_REG_KEY);
    lua_pushnil(g_L);
    lua_rawset(g_L, LUA_REGISTRYINDEX);

    xSemaphoreGive(s_run_lock);
    return result;
}

esp_err_t lua_run_string(const char *script,
                         char *output, size_t output_size,
                         uint32_t timeout_ms)
{
    return run_chunk(load_string, script, ESP_FAIL, output, output_size, timeout_ms);
}

esp_err_t lua_run_file(const char *path,
                       char *output, size_t output_size,
                       uint32_t timeout_ms)
{
    /* Resolve path to VFS path:
     *   "walk.lua"      → "/fs/lua/walk.lua"
     *   "/lua/walk.lua" → "/fs/lua/walk.lua"
     *   "/fs/lua/..."   → unchanged (already a VFS path)
     */
    char vfs_path[256];
    if (path[0] != '/') {
        snprintf(vfs_path, sizeof(vfs_path), "/fs/lua/%s", path);
    } else if (strncmp(path, "/fs/", 4) != 0) {
        snprintf(vfs_path, sizeof(vfs_path), "/fs%s", path);
    } else {
        strncpy(vfs_path, path, sizeof(vfs_path) - 1);
        vfs_path[sizeof(vfs_path) - 1] = '\0';
    }

    return run_chunk(load_file, vfs_path, ESP_ERR_NOT_FOUND, output, output_size, timeout_ms);
}

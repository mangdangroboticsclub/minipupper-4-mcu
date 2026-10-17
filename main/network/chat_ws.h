#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_http_server.h"

namespace network {

/**
 * @brief Binary frame layout for upstream outbound segment.
 *
 * Matches the SDD §2.2 specification:
 *   [0:16]   robot_uuid       (plain-text, also used as AAD)
 *   [16:28]  IV nonce         (12-byte, unique per frame)
 *   [28:-16] ciphertext payload
 *   [-16:]   auth tag         (16-byte GCM tag)
 */
struct BinaryFrameHeader {
    uint8_t robot_uuid[16];       // Plain-text routing header
    uint8_t iv[12];               // AES-GCM initialization vector
    // ciphertext and tag follow immediately after
} __attribute__((packed));

constexpr size_t BINARY_FRAME_HEADER_SIZE = sizeof(BinaryFrameHeader);  // 28 bytes
constexpr size_t BINARY_FRAME_TAG_SIZE    = 16;                         // GCM auth tag

/**
 * @brief Initialise the chat pipeline.
 *
 * Parses the AES-256-GCM key from Kconfig hex string.
 * In echo mode (CONFIG_APP_CHAT_SERVER_IP == "echo"), no cloud
 * connection is attempted.
 * In cloud mode, connects to the upstream ingress server via
 * WebSocket on STA interface.
 *
 * Must be called after init_wifi_sta() and start_http_server().
 *
 * @return true on success.
 */
bool init_chat_pipeline();

/**
 * @brief WebSocket handler for the local chat UI endpoint.
 *
 * Registered at /v1/chat/ui.
 * Accepts JSON text frames from the phone and either echoes them
 * back (echo mode) or encrypts+forwards them to the cloud ingress.
 */
esp_err_t chat_ws_handler(httpd_req_t *req);

/**
 * @brief Release a PWA WebSocket client by socket fd.
 *
 * Called from the HTTP server's close_fn callback when ANY session
 * is closed.  This is the ONLY reliable way to detect that a PWA
 * WebSocket has disconnected, because with handle_ws_control_frames
 * = false the ESP-IDF HTTP server handles CLOSE frames internally
 * without calling the user handler, so release_pwa_client() via the
 * handler path is never reached for a normal browser close().
 */
void pwa_release_fd(httpd_handle_t hd, int sockfd);

/**
 * @brief REST handler for POST /v1/chat/send.
 *
 * Accepts JSON body {"type":"user_chat_input","text":"..."} and
 * returns a chat_reply JSON response.
 *
 * This is the primary chat interface — more reliable than WebSocket
 * for the local echo use case. WebSocket handler above is kept for
 * future cloud streaming.
 */
esp_err_t chat_send_handler(httpd_req_t *req);

/* ═══════════════════════════════════════════════════════════════
 *  OpenClaw Action Telemetry & Permission API
 * ═══════════════════════════════════════════════════════════════ */

/**
 * @brief Broadcast an action telemetry message to all connected PWA clients.
 *
 * Sends a JSON message of type "openclaw_action" with the given details.
 * PWA clients render these as collapsible action panels.
 *
 * @param session_id  Session context (can be empty).
 * @param type        Action type (e.g. "file_write", "file_delete", "wasm_run").
 * @param description Human-readable description of the action.
 * @param status      Status string: "pending", "approved", "denied", "completed", "failed".
 * @param action_id   Unique ID for permission tracking (empty for non-permission actions).
 */
void broadcast_action(const char *session_id,
                      const char *type,
                      const char *description,
                      const char *status,
                      const char *action_id = "");

/**
 * @brief Report an action from C++ code (e.g. from Lua bindings).
 *
 * Convenience wrapper that calls broadcast_action with an empty session_id.
 */
void report_action(const char *type,
                   const char *description,
                   const char *status);

/**
 * @brief Request user permission for a potentially dangerous operation.
 *
 * Broadcasts an action telemetry with status "pending" to all PWA
 * clients, then blocks on a semaphore waiting for the user to
 * approve or deny via the WebSocket.
 *
 * @param type         Action type (e.g. "file_write", "file_delete").
 * @param description  Human-readable description shown in the PWA.
 * @param timeout_ms   How long to wait for approval (0 = no timeout).
 * @return true if the user approved, false if denied or timeout.
 */
bool request_permission(const char *type,
                        const char *description,
                        uint32_t timeout_ms);

/**
 * @brief Settings' "Allow writes (YOLO)": while on, request_permission()
 *        approves at once (still broadcasting "approved" to the PWA)
 *        instead of prompting. Off by default.
 */
bool allow_writes();

/**
 * @brief Turn "Allow writes" on or off and save it in NVS.
 * @return false if it could not be saved (the setting is then unchanged).
 */
bool set_allow_writes(bool allow);

/**
 * @brief Queue a Lua script for async execution in the worker task.
 *
 * Non-blocking — pushes the script to the Lua command queue and
 * returns immediately.  The script is executed asynchronously
 * in the lua_worker_task (separate from the HTTP handler).
 *
 * Used by the deploy flow so that long-running scripts with
 * permission dialogs don't block the HTTP server.
 *
 * @param script  Lua source code (up to 1024 bytes).
 * @return true if queued, false if queue full.
 */
bool queue_lua_script(const char *script);

}  // namespace network

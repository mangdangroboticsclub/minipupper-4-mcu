/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * /v1/chat/ui -- the PWA's chat WebSocket, now spoken by the on-device agent.
 *
 * This is the point of the whole integration. The old firmware answered this
 * URL by relaying every message to a cloud "OpenClaw" backend over a second
 * WebSocket; the robot was a microphone and a speaker for something else's
 * brain. Here the same URL, the same message types and the same PWA speak to
 * ESP-Claw's agent running on this chip. Nothing about the browser side
 * changed, and no conversation leaves the device except the LLM call the agent
 * makes itself.
 *
 * That also deletes an attack surface rather than porting it. The cloud socket
 * could push fs.write / crypto.base64_decode / wasm.run_bytes at the robot,
 * which together are arbitrary code execution gated only by a permission
 * prompt. There is no upstream socket any more, so that channel does not exist
 * to be gated.
 *
 * ── The protocol (fixed by the PWA, not by us) ───────────────────────────
 *
 * Browser -> robot:
 *   {"type":"session_reset","session_id":"<uuid>"}      on connect, on switch
 *   {"type":"user_chat_input","text":"...","session_id":"<uuid>"}
 *   {"type":"permission_response","action_id":"...","approved":bool}
 *
 * Robot -> browser:
 *   {"type":"ack","status":"sent"}
 *   {"type":"step","seq":N,"total":M,"text":"..."}      agent progress
 *   {"type":"chat_reply","text":"...","commands":[]}    the answer
 *   {"type":"chat_reply_chunk","session_id","seq","total","text"}
 *   {"type":"error","text":"..."}
 *
 * ── How it attaches to the agent ──────────────────────────────────────────
 *
 * Inbound is cap_im_local_emit_user_message() on channel "web" with the PWA's
 * session UUID as the chat_id -- the same door /api/webim/send uses, so a
 * conversation is one conversation whichever screen you opened it from, and
 * ESP-Claw's own session, memory and skill machinery applies unchanged.
 *
 * Outbound is where a wrinkle lives: cap_im_local has exactly one outbound
 * callback slot and http_server_webim_api.c already owns it. Rather than fight
 * over the slot, that callback mirrors every "web" message here as well, and
 * this file delivers it to whichever socket claimed that chat_id. One owner,
 * two views.
 *
 * Stage notes (the agent narrating "Round 2: display_show_text(...)") arrive
 * through that same callback, because claw_core publishes them as ordinary
 * out-messages on the request's channel. What marks them is the prefix
 * ESP-Claw's stage publisher puts on every one of them, so that is what this
 * file matches on to turn them into "step" frames rather than answers.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "cap_im_local.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "http_server_priv.h"

static const char *TAG = "http_mpx_chat";

/* The same channel http_server_webim_api.c uses. Deliberate: see the header
 * comment. app_claw binds "web" outbound to the local_send_message cap. */
#define MPX_CHAT_CHANNEL        "web"
#define MPX_CHAT_SENDER         "pwa_user"

#define MPX_CHAT_MAX_CLIENTS    4
#define MPX_CHAT_SESSION_MAX    64
#define MPX_CHAT_TEXT_MAX       2048    /* longest user message we accept   */
#define MPX_CHAT_FRAME_MAX      8192    /* longest frame we will read       */

/* Above this many bytes a reply is chunked. The PWA reassembles chunks keyed
 * on session_id, so the split is invisible to the reader; the reason to split
 * at all is that a single large WS frame on a robot with ~130 KB of internal
 * heap is a bad way to spend it. */
#define MPX_CHAT_SINGLE_MAX     2048
#define MPX_CHAT_CHUNK_TEXT     1536

/* Every verbose stage note claw_core publishes starts with this. It is the
 * only thing that distinguishes progress from an answer by the time the
 * message reaches cap_im_local -- the event's "stage" kind is not carried
 * through the send_message cap. */
#define MPX_CHAT_STAGE_PREFIX   "\xF0\x9F\xA6\x9E"   /* U+1F99E lobster */

typedef struct {
    int      fd;
    char     session[MPX_CHAT_SESSION_MAX];
    uint32_t step_seq;
} mpx_chat_client_t;

static httpd_handle_t     s_httpd;
static SemaphoreHandle_t  s_lock;
static mpx_chat_client_t  s_clients[MPX_CHAT_MAX_CLIENTS];
static size_t             s_client_count;

/* ── Client table ──────────────────────────────────────────────────────── */

static bool chat_lock_ensure(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock != NULL;
}

/* Drop entries httpd no longer considers WebSockets. Without this a phone that
 * walked out of Wi-Fi range holds a slot until the table is full and the next
 * real client is refused. */
static void chat_gc_locked(void)
{
    size_t write = 0;

    if (!s_httpd) {
        return;
    }
    for (size_t i = 0; i < s_client_count; i++) {
        if (httpd_ws_get_fd_info(s_httpd, s_clients[i].fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
            s_clients[write++] = s_clients[i];
        } else {
            ESP_LOGD(TAG, "gc: pruned stale fd=%d", s_clients[i].fd);
        }
    }
    s_client_count = write;
}

static mpx_chat_client_t *chat_find_locked(int fd)
{
    for (size_t i = 0; i < s_client_count; i++) {
        if (s_clients[i].fd == fd) {
            return &s_clients[i];
        }
    }
    return NULL;
}

static void chat_client_add(int fd)
{
    if (fd < 0 || !chat_lock_ensure()) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!chat_find_locked(fd)) {
        if (s_client_count >= MPX_CHAT_MAX_CLIENTS) {
            chat_gc_locked();
        }
        if (s_client_count < MPX_CHAT_MAX_CLIENTS) {
            s_clients[s_client_count].fd = fd;
            s_clients[s_client_count].session[0] = '\0';
            s_clients[s_client_count].step_seq = 0;
            s_client_count++;
            ESP_LOGI(TAG, "chat client fd=%d (n=%u)", fd, (unsigned)s_client_count);
        } else {
            ESP_LOGW(TAG, "chat client fd=%d rejected: table full", fd);
        }
    }
    xSemaphoreGive(s_lock);
}

static void chat_client_remove(int fd)
{
    if (fd < 0 || !s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_client_count; i++) {
        if (s_clients[i].fd == fd) {
            s_clients[i] = s_clients[s_client_count - 1];
            s_client_count--;
            break;
        }
    }
    xSemaphoreGive(s_lock);
}

void http_server_mpx_chat_ws_fd_remove(int fd)
{
    chat_client_remove(fd);
}

static void chat_client_set_session(int fd, const char *session)
{
    if (!chat_lock_ensure()) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    mpx_chat_client_t *c = chat_find_locked(fd);
    if (c) {
        strlcpy(c->session, session ? session : "", sizeof(c->session));
        c->step_seq = 0;
    }
    xSemaphoreGive(s_lock);
}

/* Collect the sockets watching one session. A message for a session nobody is
 * watching is dropped rather than broadcast: two phones in two conversations
 * must not see each other's replies. */
static size_t chat_fds_for_session(const char *session, int *out_fds, size_t max_fds)
{
    size_t count = 0;

    if (!session || !s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    chat_gc_locked();
    for (size_t i = 0; i < s_client_count && count < max_fds; i++) {
        if (strcmp(s_clients[i].session, session) == 0) {
            out_fds[count++] = s_clients[i].fd;
        }
    }
    xSemaphoreGive(s_lock);
    return count;
}

static uint32_t chat_next_step_seq(const char *session)
{
    uint32_t seq = 1;

    if (!session || !s_lock) {
        return seq;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_client_count; i++) {
        if (strcmp(s_clients[i].session, session) == 0) {
            s_clients[i].step_seq++;
            seq = s_clients[i].step_seq;
        }
    }
    xSemaphoreGive(s_lock);
    return seq;
}

static void chat_reset_step_seq(const char *session)
{
    if (!session || !s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_client_count; i++) {
        if (strcmp(s_clients[i].session, session) == 0) {
            s_clients[i].step_seq = 0;
        }
    }
    xSemaphoreGive(s_lock);
}

/* ── Sending ───────────────────────────────────────────────────────────────
 *
 * Frames are handed to httpd's own task with httpd_queue_work rather than
 * pushed from whatever task the agent finished on -- httpd_ws_send_frame_async
 * wants to run where httpd lives. */

typedef struct {
    char  *json;
    size_t len;
    int    fds[MPX_CHAT_MAX_CLIENTS];
    size_t fd_count;
} mpx_chat_job_t;

static void chat_job_run(void *arg)
{
    mpx_chat_job_t *job = arg;
    httpd_ws_frame_t pkt;

    if (!job) {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = HTTPD_WS_TYPE_TEXT;
    pkt.payload = (uint8_t *)job->json;
    pkt.len = job->len;

    for (size_t i = 0; i < job->fd_count; i++) {
        if (httpd_ws_send_frame_async(s_httpd, job->fds[i], &pkt) != ESP_OK) {
            chat_client_remove(job->fds[i]);
        }
    }
    free(job->json);
    free(job);
}

static void chat_queue_json(const int *fds, size_t fd_count, char *json /* takes ownership */)
{
    mpx_chat_job_t *job;

    if (!json) {
        return;
    }
    if (!s_httpd || fd_count == 0) {
        free(json);
        return;
    }
    job = calloc(1, sizeof(*job));
    if (!job) {
        free(json);
        return;
    }
    job->json = json;
    job->len = strlen(json);
    job->fd_count = fd_count;
    memcpy(job->fds, fds, fd_count * sizeof(int));

    if (httpd_queue_work(s_httpd, chat_job_run, job) != ESP_OK) {
        free(job->json);
        free(job);
    }
}

/* Serialise and send. Takes ownership of root either way. */
static void chat_send_object(const int *fds, size_t fd_count, cJSON *root)
{
    char *json;

    if (!root) {
        return;
    }
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    chat_queue_json(fds, fd_count, json);
}

static void chat_send_simple(int fd, const char *type, const char *key, const char *value)
{
    cJSON *root = cJSON_CreateObject();

    if (!root) {
        return;
    }
    http_server_json_add_string(root, "type", type);
    if (key) {
        http_server_json_add_string(root, key, value);
    }
    cJSON_AddNumberToObject(root, "ts", (double)(esp_timer_get_time() / 1000000LL));
    chat_send_object(&fd, 1, root);
}

/* ── Outbound from the agent ───────────────────────────────────────────────
 *
 * Called by http_server_webim_api.c's cap_im_local callback for every message
 * on the "web" channel. */

/* Longest prefix of text that is at most limit bytes and does not split a
 * UTF-8 character.
 *
 * This matters more than it looks. Each chunk is JSON-encoded and sent as its
 * own WebSocket TEXT frame, and a frame carrying half a character is not valid
 * UTF-8 -- the browser's JSON.parse rejects it and the PWA drops the frame,
 * losing that piece of the reply forever. The reassembly happens after
 * decoding, so it cannot repair a split the encoder made. Emoji in a reply are
 * not an edge case here; the agent uses them constantly. */
static size_t chat_utf8_prefix_len(const char *text, size_t limit)
{
    size_t len = strlen(text);
    size_t cut = len < limit ? len : limit;

    while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80) {
        cut--;   /* back off continuation bytes to the lead byte */
    }
    return cut;
}

static void chat_send_reply(const int *fds, size_t fd_count,
                            const char *session, const char *text)
{
    size_t len = strlen(text);

    if (len <= MPX_CHAT_SINGLE_MAX) {
        cJSON *root = cJSON_CreateObject();

        if (!root) {
            return;
        }
        http_server_json_add_string(root, "type", "chat_reply");
        http_server_json_add_string(root, "session_id", session);
        http_server_json_add_string(root, "text", text);
        cJSON_AddItemToObject(root, "commands", cJSON_CreateArray());
        cJSON_AddNumberToObject(root, "ts", (double)(esp_timer_get_time() / 1000000LL));
        chat_send_object(fds, fd_count, root);
        return;
    }

    /* Two passes: count the chunks, then send them. The PWA allocates an array
     * of "total" slots on the first chunk it sees, so the count has to be
     * right before the first frame goes out -- and it cannot simply be
     * len/CHUNK because backing off UTF-8 boundaries makes chunks uneven. */
    size_t total = 0;
    for (size_t off = 0; off < len; ) {
        size_t take = chat_utf8_prefix_len(text + off, MPX_CHAT_CHUNK_TEXT);
        if (take == 0) {
            break;   /* cannot make progress; refuse to loop forever */
        }
        off += take;
        total++;
    }
    if (total == 0) {
        return;
    }

    size_t seq = 0;
    for (size_t off = 0; off < len && seq < total; seq++) {
        size_t take = chat_utf8_prefix_len(text + off, MPX_CHAT_CHUNK_TEXT);
        char *part;
        cJSON *root;

        if (take == 0) {
            break;
        }
        part = malloc(take + 1);
        if (!part) {
            return;
        }
        memcpy(part, text + off, take);
        part[take] = '\0';
        off += take;

        root = cJSON_CreateObject();
        if (!root) {
            free(part);
            return;
        }
        http_server_json_add_string(root, "type", "chat_reply_chunk");
        http_server_json_add_string(root, "session_id", session);
        cJSON_AddNumberToObject(root, "seq", (double)seq);
        cJSON_AddNumberToObject(root, "total", (double)total);
        http_server_json_add_string(root, "text", part);
        free(part);
        chat_send_object(fds, fd_count, root);
    }
    ESP_LOGI(TAG, "reply chunked into %u frames (%u bytes)",
             (unsigned)total, (unsigned)len);
}

static void chat_send_step(const int *fds, size_t fd_count,
                           const char *session, const char *text)
{
    cJSON *root = cJSON_CreateObject();
    /* The PWA renders "step N of M" and has no way to know M in advance -- the
     * agent decides how many rounds it needs as it goes. Reporting the running
     * count as both is the honest reading: "step 3, and 3 so far". */
    uint32_t seq = chat_next_step_seq(session);

    if (!root) {
        return;
    }
    http_server_json_add_string(root, "type", "step");
    http_server_json_add_string(root, "session_id", session);
    http_server_json_add_string(root, "text", text);
    cJSON_AddNumberToObject(root, "seq", (double)seq);
    cJSON_AddNumberToObject(root, "total", (double)seq);
    cJSON_AddNumberToObject(root, "ts", (double)(esp_timer_get_time() / 1000000LL));
    chat_send_object(fds, fd_count, root);
}

void http_server_mpx_chat_on_outbound(const cap_im_local_message_t *message)
{
    int fds[MPX_CHAT_MAX_CLIENTS];
    size_t fd_count;
    const char *text;
    bool is_step;

    if (!message || !message->chat_id || !message->chat_id[0]) {
        return;
    }
    text = message->text ? message->text : "";
    if (text[0] == '\0') {
        return;
    }

    fd_count = chat_fds_for_session(message->chat_id, fds, MPX_CHAT_MAX_CLIENTS);
    if (fd_count == 0) {
        return;   /* nobody is watching this conversation from the PWA */
    }

    is_step = strncmp(text, MPX_CHAT_STAGE_PREFIX, sizeof(MPX_CHAT_STAGE_PREFIX) - 1) == 0;
    if (is_step) {
        chat_send_step(fds, fd_count, message->chat_id, text);
    } else {
        /* The answer ends the turn, so the step counter starts over. */
        chat_reset_step_seq(message->chat_id);
        chat_send_reply(fds, fd_count, message->chat_id, text);
    }
}

/* ── Inbound from the browser ──────────────────────────────────────────── */

static void chat_handle_session_reset(int fd, const cJSON *root)
{
    char session[MPX_CHAT_SESSION_MAX] = {0};

    http_server_json_read_string((cJSON *)root, "session_id", session, sizeof(session));
    if (session[0] == '\0') {
        chat_send_simple(fd, "error", "text", "session_reset without a session_id");
        return;
    }
    chat_client_set_session(fd, session);
    ESP_LOGI(TAG, "fd=%d now watching session %s", fd, session);

    /* The PWA looks for this exact phrase to flash its "session reset" badge.
     * Nothing on the agent side needs resetting: a new conversation is a new
     * session_id, which is a new chat_id, which is a new ESP-Claw session. */
    chat_send_simple(fd, "chat_reply", "text", "Session reset acknowledged.");
}

static void chat_handle_user_input(int fd, const cJSON *root)
{
    char session[MPX_CHAT_SESSION_MAX] = {0};
    const cJSON *text_item = cJSON_GetObjectItem(root, "text");
    const char *text;
    esp_err_t err;

    http_server_json_read_string((cJSON *)root, "session_id", session, sizeof(session));
    if (session[0] == '\0') {
        chat_send_simple(fd, "error", "text", "message without a session_id");
        return;
    }
    if (!cJSON_IsString(text_item) || text_item->valuestring[0] == '\0') {
        chat_send_simple(fd, "error", "text", "empty message");
        return;
    }
    text = text_item->valuestring;
    if (strlen(text) > MPX_CHAT_TEXT_MAX) {
        chat_send_simple(fd, "error", "text", "message too long");
        return;
    }

    /* A socket can send before it has reset, and the session it names is the
     * one that matters -- adopt it rather than dropping the message. */
    chat_client_set_session(fd, session);

    /* AI turned off by the owner: say so plainly instead of letting the
     * agent answer that its LLM is "not configured". */
    if (app_config_running_ai_mode() == APP_AI_OFF) {
        chat_send_simple(fd, "error", "text",
                         "AI is turned off on this robot. Turn it on in Settings > AI & Voice.");
        return;
    }

    err = cap_im_local_emit_user_message(MPX_CHAT_CHANNEL, session, MPX_CHAT_SENDER,
                                         NULL, text, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "emit_user_message failed: %s", esp_err_to_name(err));
        chat_send_simple(fd, "error", "text",
                         "the agent is not running -- check the LLM settings");
        return;
    }

    ESP_LOGI(TAG, "session=%s user: %.60s", session, text);
    chat_send_simple(fd, "ack", "status", "sent");
}

static void chat_handle_frame(int fd, const char *payload)
{
    cJSON *root = cJSON_Parse(payload);
    const cJSON *type;

    if (!root) {
        ESP_LOGW(TAG, "fd=%d sent an unparseable frame", fd);
        return;
    }
    type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "session_reset") == 0) {
        chat_handle_session_reset(fd, root);
    } else if (strcmp(type->valuestring, "user_chat_input") == 0) {
        chat_handle_user_input(fd, root);
    } else if (strcmp(type->valuestring, "permission_response") == 0) {
        /* Accepted and ignored. The prompts these answered came from the cloud
         * backend asking to run code on the robot; with that gone, nothing
         * asks. The message type stays understood so an older PWA build does
         * not see an error, and so the flow is here to hang a real
         * confirmation on if the agent ever needs one. */
        ESP_LOGI(TAG, "fd=%d permission_response for a prompt nothing issued", fd);
    } else {
        ESP_LOGD(TAG, "fd=%d unknown frame type %s", fd, type->valuestring);
    }
    cJSON_Delete(root);
}

static esp_err_t chat_ws_handler(httpd_req_t *req)
{
    httpd_ws_frame_t pkt;
    uint8_t *buf = NULL;
    esp_err_t err;
    int fd = httpd_req_to_sockfd(req);

    /* The first call is the HTTP upgrade, with no frame to read yet. */
    if (req->method == HTTP_GET) {
        chat_client_add(fd);
        ESP_LOGI(TAG, "chat WS connected fd=%d", fd);
        return ESP_OK;
    }

    chat_client_add(fd);

    memset(&pkt, 0, sizeof(pkt));
    err = httpd_ws_recv_frame(req, &pkt, 0);
    if (err != ESP_OK) {
        chat_client_remove(fd);
        return err;
    }

    if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
        chat_client_remove(fd);
        return ESP_OK;
    }
    if (pkt.type == HTTPD_WS_TYPE_PING) {
        httpd_ws_frame_t pong;

        memset(&pong, 0, sizeof(pong));
        pong.type = HTTPD_WS_TYPE_PONG;
        return httpd_ws_send_frame(req, &pong);
    }
    if (pkt.len == 0 || pkt.len > MPX_CHAT_FRAME_MAX) {
        if (pkt.len > MPX_CHAT_FRAME_MAX) {
            ESP_LOGW(TAG, "fd=%d frame of %u bytes refused", fd, (unsigned)pkt.len);
        }
        return ESP_OK;
    }

    buf = calloc(1, pkt.len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    pkt.payload = buf;
    err = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (err == ESP_OK) {
        chat_handle_frame(fd, (const char *)buf);
    }
    free(buf);
    return err;
}

/* ── Registration ──────────────────────────────────────────────────────── */

esp_err_t http_server_register_mpx_chat_routes(httpd_handle_t server)
{
    static const httpd_uri_t handlers[] = {
        {
            .uri = "/v1/chat/ui",
            .method = HTTP_GET,
            .handler = chat_ws_handler,
            .user_ctx = NULL,
            .is_websocket = true,
        },
    };

    s_httpd = server;
    (void)chat_lock_ensure();

    return http_server_register_uri_table(server, handlers,
                                          sizeof(handlers) / sizeof(handlers[0]),
                                          "mpx chat");
}

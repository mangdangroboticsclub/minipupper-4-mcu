#include "network/http_server.h"
#include "network/captive.h"
#include "network/chat_ws.h"
#include "network/http_display.h"
#include "network/http_voice.h"
#include "network/marketplace_proxy.h"
#include "network/wifi_ap.h"
#include "network/wifi_sta.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cerrno>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"

#include "fs/littlefs_manager.h"
#include "lua/lua_vm.h"
#include "pi/pi_link.h"
#include "sdkconfig.h"
#include "robot/robot.h"
#include "robot/driver_board.h"
#include "util/sys_monitor.h"
#include "util/trace_ring.h"
#include "wasm/registry.h"
#include "wasm/wasm_sandbox.h"

#include "lwip/sockets.h"
#ifndef LWIP_SOCKET_OFFSET
#define LWIP_SOCKET_OFFSET 0
#endif

static const char *TAG = "http_server";

namespace network {
namespace {

httpd_handle_t s_server = nullptr;

/* ── MIME type helpers ──────────────────────────────────────── */

const char *get_mime_type(const char *path)
{
    const char *ext = std::strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    if (std::strcmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    if (std::strcmp(ext, ".js")   == 0) return "application/javascript; charset=utf-8";
    if (std::strcmp(ext, ".css")  == 0) return "text/css; charset=utf-8";
    if (std::strcmp(ext, ".svg")  == 0) return "image/svg+xml";
    if (std::strcmp(ext, ".json") == 0) return "application/json";
    if (std::strcmp(ext, ".png")  == 0) return "image/png";
    if (std::strcmp(ext, ".ico")  == 0) return "image/x-icon";
    if (std::strcmp(ext, ".wasm") == 0) return "application/wasm";
    if (std::strcmp(ext, ".mpxe") == 0) return "application/wasm";
    return "application/octet-stream";
}

/* ── URL-decode a percent-encoded string ───────────────────── */
static std::string url_decode(const std::string &src)
{
    std::string out;
    out.reserve(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (src[i] == '%' && i + 2 < src.size()) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
            char *end = nullptr;
            long val = std::strtol(hex, &end, 16);
            if (end == hex + 2) {
                out += static_cast<char>(val);
                i += 2;
            } else {
                out += src[i];
            }
        } else if (src[i] == '+') {
            out += ' ';
        } else {
            out += src[i];
        }
    }
    return out;
}

/* ── Build a full filesystem path from a URI ────────────────── */
std::string uri_to_path(const char *uri)
{
    // Default to index.html for root
    if (std::strcmp(uri, "/") == 0) {
        return std::string(WWW_ROOT) + "/index.html";
    }

    // Map favicon.ico to the app's SVG icon
    if (std::strcmp(uri, "/favicon.ico") == 0) {
        return std::string(WWW_ROOT) + "/md.svg";
    }

    std::string path = std::string(WWW_ROOT) + uri;

    // Strip trailing slash
    if (path.size() > 0 && path.back() == '/') {
        path += "index.html";
    }

    return path;
}

/* ── Check if client accepts gzip ───────────────────────────── */
bool accepts_gzip(httpd_req_t *req)
{
    char buf[64] = {};
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", buf, sizeof(buf)) != ESP_OK) {
        return false;
    }
    return std::strstr(buf, "gzip") != nullptr;
}

/* ── Send a file from the filesystem ────────────────────────── */
esp_err_t send_file(httpd_req_t *req, const char *fs_path,
                           const char *mime, bool is_gzipped)
{
    if (!req || !fs_path || !mime) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = std::fopen(fs_path, "rb");
    if (!f) {
        return ESP_FAIL;
    }

    // Get file size
    struct stat st;
    if (stat(fs_path, &st) != 0) {
        std::fclose(f);
        return ESP_FAIL;
    }

    // Set Content-Type
    if (httpd_resp_set_type(req, mime) != ESP_OK) {
        std::fclose(f);
        return ESP_FAIL;
    }

    // Set Content-Encoding if serving a gzipped file
    if (is_gzipped) {
        if (httpd_resp_set_hdr(req, "Content-Encoding", "gzip") != ESP_OK) {
            std::fclose(f);
            return ESP_FAIL;
        }
    }

    // Set Cache-Control for static assets
    if (httpd_resp_set_hdr(req, "Cache-Control",
                           "public, max-age=31536000, immutable") != ESP_OK) {
        std::fclose(f);
        return ESP_FAIL;
    }

    // Stream the file in chunks (small buffer to save stack)
    constexpr size_t CHUNK_SIZE = 512;
    char buf[CHUNK_SIZE];
    size_t remaining = static_cast<size_t>(st.st_size);

    while (remaining > 0) {
        const size_t to_read = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;
        const size_t read_bytes = std::fread(buf, 1, to_read, f);

        if (read_bytes == 0) break;

        if (httpd_resp_send_chunk(req, buf, read_bytes) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send chunk (connection closed?)");
            std::fclose(f);
            return ESP_FAIL;
        }
        remaining -= read_bytes;
    }

    std::fclose(f);

    // Terminate chunked response
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

/* ── Try to open and serve a file, return ESP_OK on success ─── */
static bool try_serve(httpd_req_t *req, const std::string &path,
                      const char *mime, bool is_gzipped)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);

    return send_file(req, path.c_str(), mime, is_gzipped) == ESP_OK;
}

/* ── Static asset handler (with gzip support) ───────────────── */
esp_err_t static_handler(httpd_req_t *req)
{
    // Reject API/WebSocket paths
    if (std::strncmp(req->uri, "/v1/", 4) == 0) {
        return ESP_OK;
    }

    const std::string fs_path = uri_to_path(req->uri);
    const std::string mime = get_mime_type(fs_path.c_str());
    const bool gzip = accepts_gzip(req);

    // Try gzipped first, then uncompressed. The claw design ships its large
    // files as .gz only (see main/CMakeLists.txt), so as a last resort the
    // .gz goes out even to a client that did not advertise gzip: every
    // browser decodes it anyway, and the alternative is the SPA fallback
    // handing index.html to a request for m.js.
    if ((gzip && try_serve(req, fs_path + ".gz", mime.c_str(), true)) ||
        try_serve(req, fs_path, mime.c_str(), false) ||
        (!gzip && try_serve(req, fs_path + ".gz", mime.c_str(), true))) {
        return ESP_OK;
    }

    // Fallback: serve index.html for SPA routing
    const std::string fallback = std::string(WWW_ROOT) + "/index.html";
    if ((gzip && try_serve(req, fallback + ".gz", "text/html; charset=utf-8", true)) ||
        try_serve(req, fallback, "text/html; charset=utf-8", false)) {
        return ESP_OK;
    }

    // Nothing found — send a minimal manual response and return OK
    // (returning ESP_FAIL triggers a cleanup crash in ESP-IDF's httpd)
    ESP_LOGW(TAG, "File not found, sending 404: %s", req->uri);
    const char *body = "404 Not Found";
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── WebSocket telemetry handler ────────────────────────────── */
esp_err_t websocket_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket connection established");
        return ESP_OK;
    }

    // Handle WebSocket frame
    httpd_ws_frame_t ws_pkt{};
    uint8_t buf[256] = {};

    ws_pkt.payload = buf;
    ws_pkt.len = sizeof(buf);

    const esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, sizeof(buf));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket recv error: %s", esp_err_to_name(ret));
        return ret;
    }

    // Echo or handle the message
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        ESP_LOGD(TAG, "WS text: %.*s", (int)ws_pkt.len, (const char *)ws_pkt.payload);
        // Echo back for now
        httpd_ws_send_frame(req, &ws_pkt);
    } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "WebSocket closed by client");
    }

    return ESP_OK;
}

}  // anonymous namespace

/* ── REST API handlers ─────────────────────────────────────── */

/* ── Helper: check if a path is under /www/ (read-only PWA assets) ── */
static bool is_www_path(const std::string &path)
{
    return path.compare(0, 5, "/www/") == 0 || path == "/www";
}

static std::string json_escape(const std::string &s);

/* GET /v1/skills/list — list .wasm files (excluding www) */
static esp_err_t api_skills_list(httpd_req_t *req)
{
    auto files = fs::list_files("/");
    std::string json = "[\n";

    bool first = true;
    for (const auto &f : files) {
        // Accept .wasm and .mpxe skill files
        bool is_skill = (f.size() >= 6 && f.substr(f.size() - 5) == ".wasm")
                     || (f.size() >= 6 && f.substr(f.size() - 5) == ".mpxe");
        if (!is_skill) continue;
        if (is_www_path(f)) continue;

        if (!first) json += ",\n";
        first = false;

        std::size_t sz = fs::file_size(f.c_str());
        std::string name = f;
        if (name.size() > 0 && name[0] == '/') name = name.substr(1);

        json += "  {\"name\":\"" + name + "\",\"size\":" + std::to_string(sz) + "}";
    }
    json += "\n]";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.size());
    return ESP_OK;
}

/* ── Async skill execution ───────────────────────────────────────
 * WASM skills can run for up to 60 s.  Running them inline in the HTTP
 * handler blocked the whole web server for that entire time — connections
 * piled up and the socket pool was exhausted ("accept (23)", stuck after a
 * dance).  Instead we launch the skill on its own FreeRTOS task and return
 * immediately, so httpd stays responsive while the skill runs.
 */
static volatile bool s_skill_running = false;
// Name of the skill running now, for GET /v1/skills/status. A fixed buffer
// rather than std::string because the run task and httpd both touch it.
static char s_skill_name[64] = "";

struct SkillRunArgs { std::string path; std::string name; std::string params; };

static void skill_run_task(void *arg)
{
    SkillRunArgs *a = static_cast<SkillRunArgs *>(arg);

    /* Hand the parameters to the sandbox before the module runs, because the
     * skill's very first mpx_param_*() call may be in on_start(). Cleared by
     * the sandbox at the end of the run, so they cannot leak into the next
     * skill.
     *
     * A malformed list is not an error: every mpx_param_*() then falls back to
     * the skill's own default, which is exactly what a skill run with no
     * parameters at all gets. */
    wasm::set_params(a->params.empty() ? nullptr : a->params.c_str());

    ESP_LOGI(TAG, "Running skill (async): %s", a->path.c_str());
    auto result = wasm::load_and_run(a->path.c_str(), "on_start", 60000);
    ESP_LOGI(TAG, "Skill '%s' completed with result=%d",
             a->name.c_str(), static_cast<int>(result));
    delete a;
    s_skill_name[0] = 0;
    s_skill_running = false;
    vTaskDelete(nullptr);
}

/* Launch a skill on its own task. Shared by POST /v1/skills/run and by
 * POST /v1/robot/gait when the name belongs to a skill's provides_gait.
 * Returns an HTTP status string on failure ("409 Conflict" or "500 ..."). */
static const char *start_skill_async(const std::string &path,
                                     const std::string &name,
                                     const std::string &params)
{
    // ── Reject if a skill is already running (single WASM instance) ──
    if (s_skill_running) return "409 Conflict";

    s_skill_running = true;
    std::strncpy(s_skill_name, name.c_str(), sizeof(s_skill_name) - 1);
    s_skill_name[sizeof(s_skill_name) - 1] = 0;
    auto *args = new SkillRunArgs{ path, name, params };
    // No core affinity: the scheduler keeps httpd (core 0) responsive while the
    // skill yields during its robot_delay_ms calls.  8 KB task stack is enough;
    // the WASM operand stack is allocated separately inside load_and_run.
    BaseType_t created = xTaskCreate(skill_run_task, "skill_run", 8192,
                                     args, 4, nullptr);
    if (created != pdPASS) {
        s_skill_running = false;
        s_skill_name[0] = 0;
        delete args;
        return "500 Internal Server Error";
    }
    ESP_LOGI(TAG, "Skill '%s' started (async)", name.c_str());
    return nullptr;
}

/* Turn what a client calls a skill into a file path and a display name.
 *
 * The old apps send the file name ("moonwalk.wasm"); the mpv2 app sends the
 * registry slug ("moonwalk") when the skill declares one. Both must land on
 * the same file, and the name reported by /v1/skills/status must be the one
 * the app compares against, which is the slug when there is one. */
static void resolve_skill_name(const std::string &given, std::string &path,
                               std::string &name)
{
    path = (given[0] == '/') ? given : "/" + given;
    name = given;
    if (const skills::Entry *e = skills::by_slug(given.c_str())) {
        path = e->file;
        name = e->slug;
        return;
    }
    for (const skills::Entry &e : skills::all()) {
        if (e.file == path) {
            if (!e.slug.empty()) name = e.slug;
            return;
        }
    }
    if (!fs::file_exists(path.c_str()) && fs::file_exists((path + ".wasm").c_str())) {
        path += ".wasm";
    }
}

/* The public face of the three helpers above (http_server.h), for the voice
 * tools. Same rules as the HTTP handlers; only the error reporting differs
 * (a reason string rather than a status line). */
const char *skill_start(const std::string &given, const std::string &params,
                        std::string *resolved)
{
    if (given.empty()) return "no such skill";
    std::string path, name;
    resolve_skill_name(given, path, name);
    if (!fs::file_exists(path.c_str())) return "no such skill";
    const char *fail = start_skill_async(path, name, params);
    if (fail) return fail[0] == '4' ? "a skill is already running" : "failed to start skill task";
    if (resolved) *resolved = name;
    return nullptr;
}

const char *skill_start_for_movement(const char *movement, std::string *resolved)
{
    const skills::Entry *e = movement ? skills::by_gait(movement) : nullptr;
    if (!e) return "no such movement";
    const char *fail = start_skill_async(e->file, e->slug, "");
    if (fail) return fail[0] == '4' ? "a skill is already running" : "failed to start skill task";
    if (resolved) *resolved = e->slug;
    return nullptr;
}

bool skill_running() { return s_skill_running; }

const char *skill_running_name() { return s_skill_running ? s_skill_name : ""; }

void skill_stop()
{
    if (s_skill_running) wasm::request_stop();
}

/* POST /v1/skills/run — execute a .wasm skill via WAMR (async) */
static esp_err_t api_skills_run(httpd_req_t *req)
{
    // 1024 rather than 256: the body carries both the skill name and the
    // optional per-run parameter list, and silently truncating the parameters
    // would look to the skill like they were never supplied at all.
    char buf[1024] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    const char *key = "\"skill\":\"";
    const char *val = std::strstr(buf, key);
    if (!val) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing skill name", -1);
        return ESP_OK;
    }
    val += std::strlen(key);
    std::string skill_name;
    while (*val && *val != '"') skill_name += *val++;
    if (skill_name.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing skill name", -1);
        return ESP_OK;
    }

    std::string path;
    {
        const std::string given = skill_name;
        resolve_skill_name(given, path, skill_name);
    }

    /* Optional "params":"name=value;name=value" for mpx_param_f()/i().
     *
     * Extracted as a raw string rather than parsed here: the sandbox owns the
     * format, this handler would only be a second place for it to be wrong.
     * Backslash escapes are honoured so a value can contain a quote. */
    std::string params;
    {
        const char *pkey = "\"params\":\"";
        const char *p = std::strstr(buf, pkey);
        if (p) {
            p += std::strlen(pkey);
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) ++p;
                params += *p++;
            }
        }
    }

    // ── Launch the skill on its own task and return immediately ──
    const char *fail = start_skill_async(path, skill_name, params);
    httpd_resp_set_type(req, "application/json");
    if (fail) {
        httpd_resp_set_status(req, fail);
        httpd_resp_send(req, fail[0] == '4'
                             ? "{\"output\":\"a skill is already running\"}"
                             : "{\"output\":\"failed to start skill task\"}", -1);
        return ESP_OK;
    }
    httpd_resp_send(req, "{\"output\":\"started\"}", -1);
    return ESP_OK;
}

/* GET /v1/skills/status — {"running":bool,"name":"...","safe_mode":false}
 *
 * safe_mode is mpv2-claw's "a skill crashed the robot at boot, autorun is
 * suspended" flag. Autorun is not ported yet, so it is always false here;
 * the key stays so the app's Skills tab reads the same JSON on both. */
static esp_err_t api_skills_status(httpd_req_t *req)
{
    std::string json = std::string("{\"running\":") + (s_skill_running ? "true" : "false");
    if (s_skill_running && s_skill_name[0]) {
        json += ",\"name\":\"" + json_escape(s_skill_name) + "\"";
    }
    json += ",\"safe_mode\":false}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.size());
    return ESP_OK;
}

/* POST /v1/skills/stop — ask the running skill to stop.
 *
 * Cooperative: the sandbox sets its stop flag and the skill's next host call
 * returns it. A skill that never calls the host keeps running until its
 * watchdog expires, which is the same contract mpv2-claw has. */
static esp_err_t api_skills_stop(httpd_req_t *req)
{
    const bool was_running = s_skill_running;
    if (was_running) wasm::request_stop();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, was_running ? "{\"ok\":true,\"stopping\":true}"
                                     : "{\"ok\":true,\"stopping\":false}", -1);
    return ESP_OK;
}

/* GET /v1/skills/registry — what the installed skills declare about
 * themselves (embedded "mpx" manifest sections). See wasm/registry.h. */
static esp_err_t api_skills_registry(httpd_req_t *req)
{
    const std::string json = skills::to_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.size());
    return ESP_OK;
}

/* Escape a string for JSON embedding. */
static std::string json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

/* GET /v1/fs/list — list files + dirs on LittleFS. Query: ?path=/lua (default /) */
static esp_err_t api_fs_list(httpd_req_t *req)
{
    std::string dir_path = "/";
    const char *query = strchr(req->uri, '?');
    if (query) {
        const char *p = strstr(query, "path=");
        if (p) {
            dir_path = p + 5;
            auto amp = dir_path.find('&');
            if (amp != std::string::npos) dir_path = dir_path.substr(0, amp);
            dir_path = url_decode(dir_path);
        }
    }

    // Build VFS path, open directory, enumerate files + subdirs
    std::string vfs = (dir_path == "/") ? "/fs" : "/fs" + dir_path;
    DIR *dir = opendir(vfs.c_str());

    std::string files_json, dirs_json;
    bool first_file = true, first_dir = true;

    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name(entry->d_name);
            if (name == ".") continue;

            if (entry->d_type == DT_DIR) {
                if (!first_dir) dirs_json += ",";
                first_dir = false;
                dirs_json += "\"" + json_escape(name) + "\"";
            } else if (entry->d_type == DT_REG) {
                std::string full = dir_path + "/" + name;
                std::size_t sz = fs::file_size(full.c_str());
                bool ro = is_www_path(full);
                if (!first_file) files_json += ",";
                first_file = false;
                files_json += "{\"n\":\"" + json_escape(name) + "\""
                            + ",\"s\":" + std::to_string(sz)
                            + ",\"r\":" + (ro ? "true" : "false") + "}";
            }
        }
        closedir(dir);
    }

    std::string json = "{\"f\":[" + files_json + "],\"d\":[" + dirs_json + "]}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.size());
    return ESP_OK;
}

/* GET /v1/fs/read?path=/lua/walk.lua — read file content as JSON */
static esp_err_t api_fs_read(httpd_req_t *req)
{
    std::string file_path;
    const char *query = strchr(req->uri, '?');
    if (query) {
        const char *p = strstr(query, "path=");
        if (p) {
            file_path = p + 5;
            auto amp = file_path.find('&');
            if (amp != std::string::npos) file_path = file_path.substr(0, amp);
            file_path = url_decode(file_path);
        }
    }

    if (file_path.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing path", -1);
        return ESP_OK;
    }

    auto data = fs::read_file(file_path.c_str());
    if (data.empty() && !fs::file_exists(file_path.c_str())) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "file not found", -1);
        return ESP_OK;
    }

    std::string content(data.begin(), data.end());
    std::string resp = "{\"ok\":true,\"p\":\"" + json_escape(file_path)
                     + "\",\"c\":\"" + json_escape(content) + "\"}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp.c_str(), resp.size());
    return ESP_OK;
}

/* GET /v1/fs/info — filesystem stats */
static esp_err_t api_fs_info(httpd_req_t *req)
{
    std::size_t total = 0, used = 0;
    fs::stats(total, used);

    char json[128];
    std::snprintf(json, sizeof(json),
                  "{\"total\":%zu,\"used\":%zu}", total, used);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, -1);
    return ESP_OK;
}

/* POST /v1/fs/delete — delete a file (reject www paths) */
static esp_err_t api_fs_delete(httpd_req_t *req)
{
    char buf[256] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    const char *key = "\"path\":\"";
    const char *val = std::strstr(buf, key);
    if (!val) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing path", -1);
        return ESP_OK;
    }
    val += std::strlen(key);
    std::string file_path = "/";
    while (*val && *val != '"') file_path += *val++;

    // Check sudo mode
    bool sudo = (std::strstr(buf, "\"sudo\":true") != nullptr);

    // Reject deletion of www assets (even with sudo)
    if (is_www_path(file_path)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_send(req, "cannot delete www assets", -1);
        return ESP_OK;
    }

    // Without sudo, only allow .wasm and .lua deletion
    if (!sudo) {
        std::string lower = file_path;
        for (auto &c : lower) c = std::tolower(c);
        bool allowed = (lower.size() >= 5 && lower.substr(lower.size() - 5) == ".wasm")
                    || (lower.size() >= 5 && lower.substr(lower.size() - 5) == ".mpxe")
                    || (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".lua");
        if (!allowed) {
            httpd_resp_set_status(req, "403 Forbidden");
            httpd_resp_send(req, "sudo required to delete this file", -1);
            return ESP_OK;
        }
    }

    if (fs::delete_file(file_path.c_str())) {
        skills::rescan();          // a gait it provided is gone with it
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":true}", -1);
    } else {
        httpd_resp_set_status(req, "500 Server Error");
        httpd_resp_send(req, "delete failed", -1);
    }
    return ESP_OK;
}

static std::string json_get_str(const char *body, const char *key);
static char *read_body(httpd_req_t *req);

/* Whole JSON body, so escaping counts against it. Keep in sync with
 * MAX_SAVE_BYTES in pwa-claw/src/lib/FileViewer.svelte. */
static constexpr std::size_t FS_WRITE_MAX_BODY = 32 * 1024;

/* POST /v1/fs/write — overwrite an existing file with text content.
 *   Body: {"path":"/lua/walk.lua","content":"..."}
 *   /www is never writable; the file must already exist. */
static esp_err_t api_fs_write(httpd_req_t *req)
{
    if (req->content_len > FS_WRITE_MAX_BODY) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_send(req, "file too large", -1);
        return ESP_OK;
    }

    char *buf = read_body(req);
    if (!buf) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }

    const bool has_content = std::strstr(buf, "\"content\":\"") != nullptr;
    std::string file_path = json_get_str(buf, "path");
    std::string content   = json_get_str(buf, "content");
    free(buf);

    if (file_path.empty() || !has_content) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "need 'path' and 'content' fields", -1);
        return ESP_OK;
    }
    if (file_path[0] != '/') file_path = "/" + file_path;

    if (file_path.find("..") != std::string::npos) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "invalid path", -1);
        return ESP_OK;
    }
    if (is_www_path(file_path)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_send(req, "cannot modify www assets", -1);
        return ESP_OK;
    }
    if (!fs::file_exists(file_path.c_str())) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "file not found", -1);
        return ESP_OK;
    }

    if (!fs::write_file(file_path.c_str(), content.data(), content.size())) {
        httpd_resp_set_status(req, "500 Server Error");
        httpd_resp_send(req, "write failed", -1);
        return ESP_OK;
    }

    std::string resp = "{\"ok\":true,\"p\":\"" + json_escape(file_path)
                     + "\",\"s\":" + std::to_string(content.size()) + "}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp.c_str(), resp.size());
    return ESP_OK;
}

/* POST /v1/skills/upload — upload a .wasm file (raw body, name in query) */
static esp_err_t api_skills_upload(httpd_req_t *req)
{
    // Extract filename from query string: /v1/skills/upload?name=foo.wasm
    std::string filename = "skill.wasm";
    const char *query = strchr(req->uri, '?');
    if (query) {
        const char *nkey = "name=";
        const char *nval = std::strstr(query + 1, nkey);
        if (nval) {
            nval += std::strlen(nkey);
            filename = "";
            while (*nval && *nval != '&') filename += *nval++;
        }
    }

    // Read the raw binary body
    size_t content_len = req->content_len;
    if (content_len == 0 || content_len > 256 * 1024) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "invalid size", -1);
        return ESP_OK;
    }

    auto *data = new uint8_t[content_len];
    size_t total_read = 0;

    while (total_read < content_len) {
        int ret = httpd_req_recv(req, (char *)data + total_read,
                                 content_len - total_read);
        if (ret <= 0) break;
        total_read += ret;
    }

    if (total_read != content_len) {
        delete[] data;
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "read error", -1);
        return ESP_OK;
    }

    // Write to LittleFS
    std::string fs_path = "/" + filename;
    if (fs::write_file(fs_path.c_str(), data, total_read)) {
        ESP_LOGI(TAG, "Uploaded %s (%zu bytes)", filename.c_str(), total_read);
        skills::rescan();          // pick up its manifest (provides_gait etc.)
        std::string resp = "{\"ok\":true,\"path\":\"" + filename + "\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp.c_str(), resp.size());
    } else {
        httpd_resp_set_status(req, "500 Server Error");
        httpd_resp_send(req, "write failed", -1);
    }

    delete[] data;
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════
 *  Robot control API
 * ═══════════════════════════════════════════════════════════════ */

/* ── Helper: parse a JSON string value (handles escape sequences) ── */
static std::string json_get_str(const char *body, const char *key)
{
    std::string needle = "\"" + std::string(key) + "\":\"";
    const char *p = std::strstr(body, needle.c_str());
    if (!p) return {};
    p += needle.size();
    std::string val;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++; // skip backslash
            if (*p == '"')       val += '"';
            else if (*p == '\\') val += '\\';
            else if (*p == 'n')  val += '\n';
            else if (*p == 't')  val += '\t';
            else if (*p == 'r')  val += '\r';
            else if (*p == 'b')  val += '\b';
            else if (*p == 'f')  val += '\f';
            else if (*p == '/')  val += '/';
            else if (*p == 'u') {
                // Simple \\u00xx for basic ASCII range
                if (p[1] && p[2] && p[3] && p[4]) {
                    char hex[5] = {p[1], p[2], p[3], p[4], 0};
                    unsigned int code;
                    sscanf(hex, "%x", &code);
                    val += (char)code;
                    p += 4;
                }
            } else {
                val += *p; // pass through unknown escape
            }
        } else {
            val += *p;
        }
        p++;
    }
    return val;
}

/* ── Helper: parse a JSON number value ─────────────────────── */
static int json_get_int(const char *body, const char *key, int def)
{
    std::string needle = "\"" + std::string(key) + "\":";
    const char *p = std::strstr(body, needle.c_str());
    if (!p) return def;
    p += needle.size();
    // Skip whitespace
    while (*p == ' ') ++p;
    bool neg = (*p == '-');
    if (neg) ++p;
    int val = 0;
    while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); ++p; }
    return neg ? -val : val;
}

/* ── Helper: parse a JSON float value ──────────────────────── */
static float json_get_float(const char *body, const char *key, float def)
{
    std::string needle = "\"" + std::string(key) + "\":";
    const char *p = std::strstr(body, needle.c_str());
    if (!p) return def;
    p += needle.size();
    while (*p == ' ') ++p;
    char *end = nullptr;
    float val = std::strtof(p, &end);
    return (end == p) ? def : val;
}

/* ── Gait name ↔ GaitCmd ──────────────────────────────────────
 *
 * Both directions are one call into robot.cc now. They used to be a 46-branch
 * if-chain here and another 46-case switch below, plus a third copy inside the
 * SDK's robot_gait() - three hand-maintained tables that had already drifted,
 * so a name the SDK accepted could be one the HTTP layer rejected.
 *
 * There is one table, in robot.cc. If you add a gait, add it there.
 * (api_robot_gait calls robot::gait_from_name() directly now, because it
 * needs to know whether the name was found before trying the skill registry.) */
static const char *gait_cmd_to_name(robot::GaitCmd cmd)
{
    return robot::gait_to_name(cmd);
}

/* 423 while the Raspberry Pi (ROS) drives the robot. robot.cc fences local
 * motion off anyway (robot::master_active()); a clear answer beats a move that
 * never happens. Same body as mpv2-claw, which the PWA already understands. */
static esp_err_t send_pi_in_control(httpd_req_t *req)
{
    httpd_resp_set_status(req, "423 Locked");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, R"({"ok":false,"error":"pi_in_control","detail":"The Raspberry Pi (ROS) is controlling the robot"})", -1);
    return ESP_OK;
}

/* POST /v1/robot/gait — set gait mode */
static esp_err_t api_robot_gait(httpd_req_t *req)
{
    char buf[256] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    std::string mode = json_get_str(buf, "mode");
    if (mode.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing 'mode' field", -1);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "POST /v1/robot/gait  mode=%s", mode.c_str());
    if (robot::master_active()) return send_pi_in_control(req);

    robot::GaitCmd cmd = robot::GaitCmd::None;
    if (!robot::gait_from_name(mode.c_str(), cmd)) {
        // Not a built-in: a skill may provide a movement by this name
        // (its embedded manifest's provides_gait). Start it like
        // POST /v1/skills/run would, so the app's move grid can mix both.
        if (const skills::Entry *e = skills::by_gait(mode.c_str())) {
            const char *fail = start_skill_async(e->file, e->slug, "");
            httpd_resp_set_type(req, "application/json");
            if (fail) {
                httpd_resp_set_status(req, fail);
                httpd_resp_send(req, "{\"ok\":false,\"error\":\"a skill is already running\"}", -1);
                return ESP_OK;
            }
            const std::string resp = "{\"ok\":true,\"mode\":\"" + json_escape(mode)
                                   + "\",\"skill\":\"" + json_escape(e->slug) + "\"}";
            httpd_resp_send(req, resp.c_str(), resp.size());
            return ESP_OK;
        }
        // Unknown name falls through as `none`, which the older apps rely
        // on getting a 200 for (mpv2-claw answers 404 here instead).
    }

    robot::send_gait_cmd(cmd);

    char resp[128];
    std::snprintf(resp, sizeof(resp), R"({"ok":true,"mode":"%s"})", gait_cmd_to_name(cmd));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, -1);
    return ESP_OK;
}

/* GET /v1/robot/moves — every movement the robot knows, built-in and
 * skill-provided: {"moves":[{"name":"advance","continuous":true},...]}
 *
 * `continuous` tells the app whether releasing a button should send `none`
 * (walks, pose holds) or leave the move to finish on its own (jump, roll).
 * Skill moves are one-shots: the skill ends itself. */
static esp_err_t api_robot_moves(httpd_req_t *req)
{
    std::string json = "{\"moves\":[";
    bool first = true;
    for (int i = 0; i < robot::gait_name_count(); ++i) {
        const char *name = robot::gait_name_at(i);
        robot::GaitCmd cmd = robot::GaitCmd::None;
        robot::gait_from_name(name, cmd);
        if (cmd == robot::GaitCmd::None) continue;      // not a move
        if (!first) json += ',';
        first = false;
        const char *note = robot::gait_note(name);
        json += std::string("{\"name\":\"") + name + "\",\"note\":\""
              + json_escape(note ? note : "") + "\",\"continuous\":"
              + (robot::gait_is_continuous(cmd) ? "true" : "false") + "}";
    }
    for (const skills::Entry &e : skills::all()) {
        if (e.provides_gait.empty()) continue;
        if (!first) json += ',';
        first = false;
        json += "{\"name\":\"" + json_escape(e.provides_gait)
              + "\",\"continuous\":false,\"skill\":\"" + json_escape(e.slug) + "\"}";
    }
    json += "]}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.size());
    return ESP_OK;
}

/* GET /v1/robot/look?yaw=-30..30&pitch=-20..20 — analog head look.
 *
 * GET with query parameters, not POST, because the app fires it from a
 * thumbstick at ~12 Hz and a GET is one packet with no body to read. */
static esp_err_t api_robot_look(httpd_req_t *req)
{
    float yaw = 0.0f, pitch = 0.0f;
    char query[96] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(query, "yaw", val, sizeof(val)) == ESP_OK) {
            yaw = std::strtof(val, nullptr);
        }
        if (httpd_query_key_value(query, "pitch", val, sizeof(val)) == ESP_OK) {
            pitch = std::strtof(val, nullptr);
        }
    }
    if (robot::master_active()) return send_pi_in_control(req);
    robot::look_input(yaw, pitch);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, R"({"ok":true})", -1);
    return ESP_OK;
}

/* /v1/robot/power — rest when unused (same JSON as mpv2-claw).
 *
 * GET  -> {"ok":true,"rest_after_s":300,"state":"holding","resting":false}
 * POST {"rest_after_s":N}   0 = never rest; saved on the robot
 * POST {"action":"rest"}    lie down and switch the servos off now
 * POST {"action":"hold"}    power down: drop the current cap to the
 *                            configured hold value while staying in position
 *                            control — the controller's Power button
 * POST {"action":"wake"}    stand back up */
static void send_power_json(httpd_req_t *req)
{
    char resp[220];
    std::snprintf(resp, sizeof(resp),
                  R"({"ok":true,"rest_after_s":%lu,"state":"%s","resting":%s,"holding":%s})",
                  static_cast<unsigned long>(robot::rest_after()),
                  robot::power_state_name(),
                  robot::servos_resting() ? "true" : "false",
                  robot::servos_holding() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, -1);
}

static esp_err_t api_robot_power_get(httpd_req_t *req)
{
    send_power_json(req);
    return ESP_OK;
}

static esp_err_t api_robot_power_post(httpd_req_t *req)
{
    char buf[128] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    const std::string action = json_get_str(buf, "action");
    const int rest_after = json_get_int(buf, "rest_after_s", -1);

    if (action == "rest") {
        robot::rest_now();
    } else if (action == "hold") {
        robot::hold_servos();
    } else if (action == "wake") {
        robot::wake_servos();
    } else if (rest_after >= 0) {
        robot::set_rest_after(static_cast<uint32_t>(rest_after));
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "expected rest_after_s or action rest|hold|wake", -1);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "POST /v1/robot/power  action=%s rest_after_s=%d",
             action.c_str(), rest_after);
    send_power_json(req);
    return ESP_OK;
}

/* /v1/agent/permissions — Settings' "Allow writes (YOLO)".
 *
 * GET  -> {"ok":true,"allow_writes":false}
 * POST {"allow_writes":true|false}   saved on the robot
 * While on, file write/delete requests from scripts and skills are approved
 * without the permission prompt (see network::request_permission). */
static void send_permissions_json(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, network::allow_writes() ? R"({"ok":true,"allow_writes":true})"
                                                 : R"({"ok":true,"allow_writes":false})", -1);
}

static esp_err_t api_agent_permissions_get(httpd_req_t *req)
{
    send_permissions_json(req);
    return ESP_OK;
}

static esp_err_t api_agent_permissions_post(httpd_req_t *req)
{
    char buf[64] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    const bool on  = std::strstr(buf, "\"allow_writes\":true") != nullptr;
    const bool off = std::strstr(buf, "\"allow_writes\":false") != nullptr;
    if (on == off) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "expected {\"allow_writes\":true|false}", -1);
        return ESP_OK;
    }
    if (!network::set_allow_writes(on)) {
        httpd_resp_set_status(req, "500 Server Error");
        httpd_resp_send(req, "could not save the setting", -1);
        return ESP_OK;
    }
    send_permissions_json(req);
    return ESP_OK;
}

/* POST /v1/robot/joy — web joystick input (mini_pupper_web_controller
 * style).  Body: {"f":-1..1,"s":-1..1,"t":-1..1}
 *   f = forward/back, s = strafe (+left), t = turn (+left).
 * Touching a pad auto-starts the Stanford walk; releasing (zeros)
 * steps in place.  Velocities scale with Config::sg_speed.        */
static esp_err_t api_robot_joy(httpd_req_t *req)
{
    char buf[160] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    const float f = json_get_float(buf, "f", 0.0f);
    const float s = json_get_float(buf, "s", 0.0f);
    const float t = json_get_float(buf, "t", 0.0f);

    if (robot::master_active()) return send_pi_in_control(req);
    robot::joy_input(f, s, t);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, R"({"ok":true})", -1);
    return ESP_OK;
}

/* GET /v1/robot/status — current gait mode */
static esp_err_t api_robot_status(httpd_req_t *req)
{
    robot::GaitCmd cmd = robot::current_gait_cmd();
    robot::Config cfg = robot::get_config();

    ESP_LOGI(TAG, "GET  /v1/robot/status  mode=%s", gait_cmd_to_name(cmd));

    char resp[512];
    int n = std::snprintf(resp, sizeof(resp),
        R"({"mode":"%s")"
        R"(,"config":{"period":%d,"height":%d,"up_height":%d,"stride":%d,"tilt":%d,"sg_speed":%d})"
        R"(,"offsets":[)",
        gait_cmd_to_name(cmd),
        cfg.period, cfg.height, cfg.up_height, cfg.stride, cfg.tilt, cfg.sg_speed);

    for (int i = 1; i <= 12; ++i) {
        n += std::snprintf(resp + n, sizeof(resp) - n, "%.1f%s",
                           robot::get_offset(i), (i < 12) ? "," : "");
    }
    /* mpv2's field: "standalone", "monitor" or "slave" (the Raspberry Pi link). */
    n += std::snprintf(resp + n, sizeof(resp) - n, R"(],"pi_link":"%s"})",
                       pi_link::state_str());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, -1);
    return ESP_OK;
}

/* GET /v1/pi/status — the Raspberry Pi link, for the app and for debugging
 * the wiring: host_pin shows the yellow wire (IO3), heartbeat the UART. Same
 * fields as mpv2 / mpv2-claw. */
static esp_err_t api_pi_status(httpd_req_t *req)
{
    char buf[1024];   // with the "rl" object (pi/rl_link.cc)
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (pi_link::status_json(buf, sizeof(buf)) < 0) {
        httpd_resp_send(req, R"({"state":"standalone","in_control":false,"error":"pi link unavailable"})", -1);
        return ESP_OK;
    }
    httpd_resp_send(req, buf, -1);
    return ESP_OK;
}

/* POST /v1/pi/power — {"action":"shutdown"|"reboot"}: switch the Raspberry
 * Pi off (or restart it) cleanly, so pulling the power can't corrupt its SD
 * card. The robot forwards the request over the network to the Pi's
 * pi_power service (tools/pi/pi_power.py) at the IP the Pi announces in
 * HOST_INFO. Not over the J-PI UART: the ROS bridge and RL policies read that
 * port, and a second reader on the Pi would steal their bytes.
 *
 * Answers {"ok":true,...} once the Pi has accepted, or ok:false with
 * error = pi_not_seen | pi_unreachable | bad_token | pi_refused. */
static esp_err_t send_pi_power_json(httpd_req_t *req, const char *status,
                                    bool ok, const char *action,
                                    const char *pi_ip, const char *error)
{
    char resp[200];
    if (ok) {
        std::snprintf(resp, sizeof(resp),
                      R"({"ok":true,"action":"%s","pi_ip":"%s"})", action, pi_ip);
    } else {
        std::snprintf(resp, sizeof(resp),
                      R"({"ok":false,"action":"%s","pi_ip":"%s","error":"%s"})",
                      action, pi_ip, error);
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, resp, -1);
    return ESP_OK;
}

static esp_err_t api_pi_power(httpd_req_t *req)
{
    char buf[96] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len > 0) buf[len] = 0;
    const std::string action = json_get_str(buf, "action");
    if (action != "shutdown" && action != "reboot") {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "expected action shutdown|reboot", -1);
        return ESP_OK;
    }

#if CONFIG_APP_PI_LINK_ENABLE
    char pi_ip[16] = "";
    if (!pi_link::ip(pi_ip, sizeof(pi_ip))) {
        ESP_LOGW(TAG, "POST /v1/pi/power  %s: no HOST_INFO, Pi IP unknown", action.c_str());
        return send_pi_power_json(req, "409 Conflict", false, action.c_str(), "", "pi_not_seen");
    }

    char url[80];
    std::snprintf(url, sizeof(url), "http://%s:%d/%s",
                  pi_ip, CONFIG_APP_PI_POWER_PORT, action.c_str());

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 3000;          // the HTTP server task waits: keep it short
    cfg.disable_auto_redirect = true;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return send_pi_power_json(req, "500 Internal Server Error", false,
                                  action.c_str(), pi_ip, "no_memory");
    }
    esp_http_client_set_header(client, "X-MPX-Token", CONFIG_APP_PI_POWER_TOKEN);
    esp_http_client_set_post_field(client, "", 0);
    const esp_err_t err = esp_http_client_perform(client);
    const int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "POST /v1/pi/power  %s -> %s: %s, HTTP %d",
             action.c_str(), url, esp_err_to_name(err), status);

    if (err != ESP_OK) {
        return send_pi_power_json(req, "502 Bad Gateway", false, action.c_str(), pi_ip, "pi_unreachable");
    }
    if (status == 403) {
        return send_pi_power_json(req, "502 Bad Gateway", false, action.c_str(), pi_ip, "bad_token");
    }
    if (status != 200) {
        return send_pi_power_json(req, "502 Bad Gateway", false, action.c_str(), pi_ip, "pi_refused");
    }
    return send_pi_power_json(req, "200 OK", true, action.c_str(), pi_ip, "");
#else
    return send_pi_power_json(req, "503 Service Unavailable", false, action.c_str(), "", "pi_link_disabled");
#endif
}

/* POST /v1/robot/config — update robot configuration */
static esp_err_t api_robot_config(httpd_req_t *req)
{
    char buf[256] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    robot::Config cfg = robot::get_config();

    int val;
    if ((val = json_get_int(buf, "period", -1))   >= 0)   cfg.period    = val;
    if ((val = json_get_int(buf, "height", -1))   >= 0)   cfg.height    = val;
    if ((val = json_get_int(buf, "up_height", -1)) >= 0)   cfg.up_height = val;
    if ((val = json_get_int(buf, "stride", -1))   >= 0)   cfg.stride    = val;
    if ((val = json_get_int(buf, "tilt", -1))     >= 0)   cfg.tilt      = val;
    if ((val = json_get_int(buf, "sg_speed", -1)) >= 0)   cfg.sg_speed  = val;

    ESP_LOGI(TAG, "POST /v1/robot/config  period=%d height=%d up_height=%d stride=%d tilt=%d sg_speed=%d",
             cfg.period, cfg.height, cfg.up_height, cfg.stride, cfg.tilt, cfg.sg_speed);

    robot::set_config(cfg);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, R"({"ok":true})", -1);
    return ESP_OK;
}

/* GET /v1/robot/config — get robot configuration */
static esp_err_t api_robot_get_config(httpd_req_t *req)
{
    robot::Config cfg = robot::get_config();

    ESP_LOGI(TAG, "GET  /v1/robot/config  period=%d height=%d up_height=%d stride=%d tilt=%d",
             cfg.period, cfg.height, cfg.up_height, cfg.stride, cfg.tilt);

    char resp[256];
    std::snprintf(resp, sizeof(resp),
        R"({"period":%d,"height":%d,"up_height":%d,"stride":%d,"tilt":%d,"sg_speed":%d})",
        cfg.period, cfg.height, cfg.up_height, cfg.stride, cfg.tilt, cfg.sg_speed);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, -1);
    return ESP_OK;
}

/* POST /v1/robot/calibrate — set a servo offset */
static esp_err_t api_robot_calibrate(httpd_req_t *req)
{
    char buf[256] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    int servo = json_get_int(buf, "servo", -1);
    float offset = json_get_float(buf, "offset", 0.0f);

    if (servo < 1 || servo > 12) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "servo must be 1-12", -1);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "POST /v1/robot/calibrate  servo=%d offset=%.1f", servo, offset);

    robot::set_offset(servo, offset);

    char resp[128];
    std::snprintf(resp, sizeof(resp),
                  R"({"ok":true,"servo":%d,"offset":%.1f})", servo, offset);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, -1);
    return ESP_OK;
}

/* POST /v1/robot/calibrate/reset — reset all offsets to 0 */
static esp_err_t api_robot_cal_reset(httpd_req_t *req)
{
    ESP_LOGI(TAG, "POST /v1/robot/calibrate/reset");
    robot::reset_offsets();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, R"({"ok":true})", -1);
    return ESP_OK;
}

/* POST /v1/robot/diagnostic/ping — ping a servo by ID */
static esp_err_t api_robot_diag_ping(httpd_req_t *req)
{
    char buf[64] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    int servo = json_get_int(buf, "servo", 1);
    if (servo < 1 || servo > 12) servo = 1;

    ESP_LOGI(TAG, "POST /v1/robot/diagnostic/ping  servo=%d", servo);

    // There is no bus ping on this hardware, so this does one parameter read
    // and reports whether the channel answered. The old reply carried a
    // "model" number because the SCSCL servos returned one; the driver boards
    // do not, and inventing a 1 there would be worse than saying nothing.
    int result = robot::ping_servo(servo);
    char resp[128];
    if (result > 0) {
        std::snprintf(resp, sizeof(resp),
                      R"({"ok":true,"servo":%d,"responding":true})", servo);
    } else {
        std::snprintf(resp, sizeof(resp),
                      R"({"ok":false,"servo":%d,"responding":false,"error":%d})",
                      servo, result);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, -1);
    return ESP_OK;
}

/* POST /v1/robot/diagnostic/sweep — sweep a single servo ±45° */
static esp_err_t api_robot_diag_sweep(httpd_req_t *req)
{
    char buf[64] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    int servo = json_get_int(buf, "servo", 1);
    if (servo < 1 || servo > 12) servo = 1;

    ESP_LOGI(TAG, "POST /v1/robot/diagnostic/sweep  servo=%d", servo);

    /* Park the gait for the duration.
     *
     * Without this the sweep does not happen. The gait task's idle branch
     * re-solves the neutral stand and calls flush() every tick, so anything
     * this handler writes to a goal buffer is overwritten within ~10 ms and
     * the servo twitches instead of sweeping. Studio mode is the mechanism
     * that already exists for exactly this, and flush() honours it.
     *
     * The old version called set_all_servo_speed(200) here, which on the
     * previous hardware at least meant something and on this one is stored and
     * never sent - so it was doubly not working. The current cap below is the
     * value that actually governs how hard the sweep pushes. */
    robot::set_studio_mode(true);
    const uint16_t prev_current = robot::get_servo_current(servo);
    robot::set_servo_current(servo, 600);   // modest: this is a bench test

    for (int i = 0; i < 3; ++i) {
        robot::set_servo_angle(servo, -45.0f);
        robot::flush();
        vTaskDelay(pdMS_TO_TICKS(400));

        robot::set_servo_angle(servo, 45.0f);
        robot::flush();
        vTaskDelay(pdMS_TO_TICKS(400));
    }

    // Return to centre, then hand the bus back.
    robot::set_servo_angle(servo, 0.0f);
    robot::flush();
    robot::set_servo_current(servo, prev_current);
    robot::set_studio_mode(false);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, R"({"ok":true})", -1);
    return ESP_OK;
}

/* GET /v1/trace - named numbers emitted by the running skill.
 *
 * A skill calls mpx_trace("knee_err", v); this drains those samples so a
 * client can plot them. Sequence-numbered and polled over plain HTTP rather
 * than pushed down a held socket, because httpd runs with max_open_sockets=5
 * plus lru_purge_enable and a pinned trace socket would starve the PWA.
 *
 * Read-only, so it is not behind any write gate.
 */
static esp_err_t api_trace(httpd_req_t *req)
{
    uint32_t since = 0;
    std::size_t max_samples = 200;

    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[24];
        if (httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
            since = static_cast<uint32_t>(std::strtoul(val, nullptr, 10));
        }
        if (httpd_query_key_value(query, "max", val, sizeof(val)) == ESP_OK) {
            const long m = std::strtol(val, nullptr, 10);
            if (m > 0 && m < 500) max_samples = static_cast<std::size_t>(m);
        }
    }

    uint32_t next = since;
    std::string samples = util::trace_ring_json(since, max_samples, next);

    std::string resp = "{\"next\":" + std::to_string(next)
                     + ",\"samples\":" + samples + "}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp.c_str(), resp.size());
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════
 *  Servo Studio API   (/v1/studio/...)
 *
 *  A thin JSON wrapper over robot/driver_board.h, ported from the reference
 *  firmware's /api/... routes. This is the only way to tune the AT32 boards'
 *  own position and current loops, to read their NTC temperatures, and to
 *  prove which leg is on which connector - all three of which are required
 *  before the gait can be trusted on a new chassis.
 *
 *  Two rules, both about who owns the SPI bus:
 *
 *   - Every PARAMETER operation requires studio mode. A config exchange is a
 *     request followed by a NOP that clocks the reply back out, and the two
 *     must not be separated: splitting them leaves a config frame pending in
 *     the AT32's tx buffer, which the next feedback decode reads as a
 *     temperature (a real 31.03 degC reply decodes as 1540.8 degC).
 *   - Read-only telemetry (status, temps) deliberately does NOT require it.
 *     The AT32 puts position, current and NTC temperature in every feedback
 *     frame, so while the gait is running those are already fresh and free.
 * ═══════════════════════════════════════════════════════════════ */

static int studio_qint(httpd_req_t *r, const char *key, int def)
{
    char q[128], v[24];
    if (httpd_req_get_url_query_str(r, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, key, v, sizeof v) == ESP_OK) {
        return atoi(v);
    }
    return def;
}

static float studio_qfloat(httpd_req_t *r, const char *key, float def)
{
    char q[128], v[24];
    if (httpd_req_get_url_query_str(r, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, key, v, sizeof v) == ESP_OK) {
        return strtof(v, nullptr);
    }
    return def;
}

static esp_err_t studio_json(httpd_req_t *r, const char *s)
{
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, s, HTTPD_RESP_USE_STRLEN);
}

#define STUDIO_NEED_MODE(r)                                                     \
    do {                                                                        \
        if (!robot::studio_mode())                                              \
            return studio_json((r),                                             \
                R"({"ok":false,"err":"studio mode off - GET /v1/studio/mode?on=1"})"); \
    } while (0)

#define STUDIO_NEED_ID(r, id)                                                   \
    do {                                                                        \
        if ((id) < 1 || (id) > 12)                                              \
            return studio_json((r), R"({"ok":false,"err":"bad id"})");          \
    } while (0)

/* GET /v1/studio/mode[?on=0|1] - read or set studio mode. */
static esp_err_t studio_mode_handler(httpd_req_t *req)
{
    const int on = studio_qint(req, "on", -1);
    if (on == 0 || on == 1) {
        robot::set_studio_mode(on == 1);
    }
    char b[64];
    std::snprintf(b, sizeof b, R"({"ok":true,"studio":%s})",
                  robot::studio_mode() ? "true" : "false");
    return studio_json(req, b);
}

/* GET /v1/studio/status - all 12 servos: position, current, temperature.
 * Works with the gait running; that is the point of it. */
static esp_err_t studio_status_handler(httpd_req_t *req)
{
    // Parked gait means a stale cache - replay each board's last frame so the
    // numbers on screen are live. Safe: an idle servo stays idle.
    if (robot::studio_mode()) {
        for (int bd = 0; bd < 4; ++bd) driver_board_poll_board(bd);
    }

    char b[768];
    int n = std::snprintf(b, sizeof b, R"({"ok":true,"studio":%s,"servos":[)",
                          robot::studio_mode() ? "true" : "false");
    for (int id = 1; id <= 12; ++id) {
        const uint16_t scs = driver_board_present_position(id);
        // The boards' own range, not a constant - same reason as everywhere else.
        const float    deg = static_cast<float>(scs)
                           * driver_board_get_scale_dd() / 10240.0f;
        const float    t   = driver_board_present_temperature(id);
        n += std::snprintf(b + n, sizeof b - n,
                           R"(%s{"id":%d,"scs":%u,"deg":%.1f,"ma":%d,"c":)",
                           id == 1 ? "" : ",", id, scs, deg,
                           driver_board_present_current(id));
        if (t > DB_TEMP_INVALID) n += std::snprintf(b + n, sizeof b - n, "%.1f}", t);
        else                     n += std::snprintf(b + n, sizeof b - n, "null}");
    }
    std::snprintf(b + n, sizeof b - n, "]}");
    return studio_json(req, b);
}

/* GET /v1/studio/scan - which channels answer a parameter read. */
static esp_err_t studio_scan_handler(httpd_req_t *req)
{
    STUDIO_NEED_MODE(req);
    char b[128];
    int n = std::snprintf(b, sizeof b, R"({"ok":true,"found":[)");
    bool first = true;
    for (int id = 1; id <= 12; ++id) {
        float v;
        if (driver_board_get_param(id, DB_PARAM_KP_POSITION, &v)) {
            n += std::snprintf(b + n, sizeof b - n, "%s%d", first ? "" : ",", id);
            first = false;
        }
    }
    std::snprintf(b + n, sizeof b - n, "]}");
    return studio_json(req, b);
}

/* GET /v1/studio/dump?id=N - every config parameter of one servo. */
static esp_err_t studio_dump_handler(httpd_req_t *req)
{
    const int id = studio_qint(req, "id", 0);
    STUDIO_NEED_MODE(req);
    STUDIO_NEED_ID(req, id);

    char b[640];
    int n = std::snprintf(b, sizeof b, R"({"ok":true,"id":%d,"params":[)", id);
    for (int p = 0; p < DB_PARAM_COUNT; ++p) {
        float v;
        const char *name = driver_board_param_name(p);
        if (driver_board_get_param(id, p, &v)) {
            n += std::snprintf(b + n, sizeof b - n, R"(%s{"p":%d,"n":"%s","v":%g})",
                               p ? "," : "", p, name ? name : "", v);
        } else {
            n += std::snprintf(b + n, sizeof b - n, R"(%s{"p":%d,"n":"%s","v":null})",
                               p ? "," : "", p, name ? name : "");
        }
    }
    std::snprintf(b + n, sizeof b - n, "]}");
    return studio_json(req, b);
}

/* GET /v1/studio/set?id=N&p=P&v=V - write one parameter to RAM, read it back. */
static esp_err_t studio_set_handler(httpd_req_t *req)
{
    const int   id = studio_qint(req, "id", 0);
    const int   p  = studio_qint(req, "p", -1);
    const float v  = studio_qfloat(req, "v", 0);
    STUDIO_NEED_MODE(req);
    STUDIO_NEED_ID(req, id);
    if (p < 0 || p >= DB_PARAM_COUNT) {
        return studio_json(req, R"({"ok":false,"err":"bad param"})");
    }

    float rb = 0;
    if (driver_board_set_param(id, p, v) && driver_board_get_param(id, p, &rb)) {
        char b[96];
        std::snprintf(b, sizeof b, R"({"ok":true,"p":%d,"v":%g})", p, rb);
        return studio_json(req, b);
    }
    return studio_json(req, R"({"ok":false,"err":"no reply"})");
}

/* GET /v1/studio/setall?p=P&v=V[&save=1] - one parameter to all 12 servos. */
static esp_err_t studio_setall_handler(httpd_req_t *req)
{
    const int   p    = studio_qint(req, "p", -1);
    const int   save = studio_qint(req, "save", 0);
    const float v    = studio_qfloat(req, "v", 0);
    STUDIO_NEED_MODE(req);
    if (p < 0 || p >= DB_PARAM_COUNT) {
        return studio_json(req, R"({"ok":false,"err":"bad param"})");
    }

    char fail[64];
    int fn = 0, n = 0;
    fail[0] = '\0';
    for (int id = 1; id <= 12; ++id) {
        float rb = 0;
        if (driver_board_set_param(id, p, v) && driver_board_get_param(id, p, &rb)) {
            n++;
        } else {
            fn += std::snprintf(fail + fn, sizeof fail - fn, "%s%d", fn ? "," : "", id);
        }
        vTaskDelay(pdMS_TO_TICKS(2));   // give the AT32 time between writes
    }
    const bool saved = (save && n) ? driver_board_save_config(-1) : false;

    char b[192];
    std::snprintf(b, sizeof b,
                  R"({"ok":%s,"p":%d,"v":%g,"n":%d,"fail":[%s],"saved":%s})",
                  n ? "true" : "false", p, v, n, fail, saved ? "true" : "false");
    return studio_json(req, b);
}

/* GET /v1/studio/live?id=N - the AT32's live control-loop values.
 * Needs AT32 firmware that implements GET_LIVE; falls back to the feedback
 * cache if it does not, and says which of the two it answered with. */
static esp_err_t studio_live_handler(httpd_req_t *req)
{
    const int id = studio_qint(req, "id", 0);
    STUDIO_NEED_MODE(req);
    STUDIO_NEED_ID(req, id);

    float lv[DB_LIVE_COUNT];
    bool ok = true;
    // Everything below DB_LIVE_TEMPERATURE_C is required; temperature is
    // optional so a board on older AT32 firmware degrades to "no temp"
    // instead of dropping the whole trace to the reduced path.
    for (int i = 0; i < DB_LIVE_TEMPERATURE_C && ok; ++i) {
        ok = driver_board_get_live(id, i, &lv[i]);
    }

    char b[640];
    if (ok) {
        float t;
        bool have_t = driver_board_get_live(id, DB_LIVE_TEMPERATURE_C, &t);
        if (!have_t || t <= DB_TEMP_INVALID) {
            t = driver_board_present_temperature(id);
            have_t = (t > DB_TEMP_INVALID);
        }
        int n = std::snprintf(b, sizeof b,
            R"({"ok":true,"full":true,"pos_adc":%g,"cur_adc":%g,)"
            R"("set_deg":%g,"now_deg":%g,"err_deg":%g,)"
            R"("cap_ma":%g,"set_ma":%g,"now_ma":%g,"err_ma":%g,)"
            R"("duty":%g,"mode":%d,"loop":%lu)",
            lv[DB_LIVE_POS_ADC], lv[DB_LIVE_CUR_ADC],
            lv[DB_LIVE_SETPOINT_POS_DEG], lv[DB_LIVE_PRESENT_POS_DEG],
            lv[DB_LIVE_ERROR_POS_DEG],
            lv[DB_LIVE_MAX_CURRENT_MA], lv[DB_LIVE_SETPOINT_CUR_MA],
            lv[DB_LIVE_PRESENT_CUR_MA], lv[DB_LIVE_ERROR_CUR_MA],
            lv[DB_LIVE_PWM_DUTY], static_cast<int>(lv[DB_LIVE_MODE]),
            static_cast<unsigned long>(lv[DB_LIVE_LOOP_COUNTER]));
        if (have_t) n += std::snprintf(b + n, sizeof b - n, R"(,"temp_c":%.1f)", t);
        std::snprintf(b + n, sizeof b - n, "}");
    } else if (driver_board_poll(id)) {
        const float t = driver_board_present_temperature(id);
        int n = std::snprintf(b, sizeof b,
            R"({"ok":true,"full":false,"now_deg":%g,"now_ma":%d)",
            static_cast<float>(driver_board_present_position(id))
                * driver_board_get_scale_dd() / 10240.0f,
            driver_board_present_current(id));
        if (t > DB_TEMP_INVALID) n += std::snprintf(b + n, sizeof b - n, R"(,"temp_c":%.1f)", t);
        std::snprintf(b + n, sizeof b - n, "}");
    } else {
        std::snprintf(b, sizeof b, R"({"ok":false,"err":"spi"})");
    }
    return studio_json(req, b);
}

/* GET /v1/studio/temps - NTC temperature of all 12. No studio mode needed. */
static esp_err_t studio_temps_handler(httpd_req_t *req)
{
    if (robot::studio_mode()) {
        for (int bd = 0; bd < 4; ++bd) driver_board_poll_board(bd);
    }
    char b[512];
    int n = std::snprintf(b, sizeof b, R"({"ok":true,"studio":%s,"temps":[)",
                          robot::studio_mode() ? "true" : "false");
    for (int id = 1; id <= 12; ++id) {
        const float t = driver_board_present_temperature(id);
        if (t > DB_TEMP_INVALID) {
            n += std::snprintf(b + n, sizeof b - n, R"(%s{"id":%d,"c":%.1f})",
                               id == 1 ? "" : ",", id, t);
        } else {
            n += std::snprintf(b + n, sizeof b - n, R"(%s{"id":%d,"c":null})",
                               id == 1 ? "" : ",", id);
        }
    }
    std::snprintf(b + n, sizeof b - n, "]}");
    return studio_json(req, b);
}

/* GET /v1/studio/save?id=N - commit board config to AT32 flash (0 = all). */
static esp_err_t studio_save_handler(httpd_req_t *req)
{
    const int id = studio_qint(req, "id", 0);
    STUDIO_NEED_MODE(req);
    const int board = (id >= 1 && id <= 12) ? (id - 1) / 3 : -1;
    return studio_json(req, driver_board_save_config(board)
                              ? R"({"ok":true})"
                              : R"({"ok":false,"err":"save failed"})");
}

/* GET /v1/studio/restore?id=N - factory defaults, RAM only (0 = all).
 * RAM only, so this is undoable by power-cycling; follow with save?id=N to
 * make it stick, which is why the two are separate routes. */
static esp_err_t studio_restore_handler(httpd_req_t *req)
{
    const int id = studio_qint(req, "id", 0);
    STUDIO_NEED_MODE(req);
    const int board = (id >= 1 && id <= 12) ? (id - 1) / 3 : -1;
    return studio_json(req, driver_board_factory_restore(board)
                              ? R"({"ok":true})"
                              : R"({"ok":false,"err":"restore failed"})");
}

/* GET /v1/studio/direct?id=N&m=0|1|2&deg=..&cur=..
 * m: 0 idle (motor off), 1 position hold, 2 torque. deg is the RAW AT32 angle
 * with 135 = centre - the boards' own units, NOT the gait frame. */
static esp_err_t studio_direct_handler(httpd_req_t *req)
{
    const int   id  = studio_qint(req, "id", 0);
    const int   m   = studio_qint(req, "m", 0);
    const float deg = studio_qfloat(req, "deg", 135.0f);
    const float cur = studio_qfloat(req, "cur", 130.0f);
    STUDIO_NEED_MODE(req);
    STUDIO_NEED_ID(req, id);

    const uint16_t mode = (m == 1) ? DB_MODE_POSITION
                        : (m == 2) ? DB_MODE_TORQUE
                                   : DB_MODE_IDLE;
    return studio_json(req, driver_board_direct(id, mode, deg,
                                                static_cast<int16_t>(cur))
                              ? R"({"ok":true})"
                              : R"({"ok":false,"err":"spi"})");
}

/* ═══════════════════════════════════════════════════════════════
 *  WiFi config API
 * ═══════════════════════════════════════════════════════════════ */

/* GET /v1/wifi/status — current AP + STA status */
static esp_err_t api_wifi_status(httpd_req_t *req)
{
    StaState sta = wifi_sta_get_state();
    std::string sta_ip = wifi_sta_get_ip();
    std::string sta_ssid = wifi_sta_get_ssid();

    const char *sta_state_str = "disconnected";
    switch (sta) {
        case StaState::Disconnected: sta_state_str = "disconnected"; break;
        case StaState::Connecting:   sta_state_str = "connecting";   break;
        case StaState::Connected:    sta_state_str = "connected";    break;
        case StaState::Failed:       sta_state_str = "failed";       break;
    }

    /* "pi": {"ip": "..."} -- empty when no Pi has reported in the last 6 s.
     * The PWA shows a Pi section in Settings when this object is present. */
    char pi_ip[16] = "";
    (void)pi_link::ip(pi_ip, sizeof(pi_ip));

    char resp[512];
    std::snprintf(resp, sizeof(resp),
        R"({"ap":{"ssid":"%s","ip":"%s"})"
        R"(,"sta":{"state":"%s","ssid":"%s","ip":"%s"})"
        R"(,"pi":{"ip":"%s"}})",
        wifi_ap_get_ssid(), AP_IP_ADDR,
        sta_state_str, sta_ssid.c_str(), sta_ip.c_str(), pi_ip);

    ESP_LOGI(TAG, "GET  /v1/wifi/status  ap_ssid=%s sta=%s ssid=%s ip=%s",
             wifi_ap_get_ssid(), sta_state_str, sta_ssid.c_str(), sta_ip.c_str());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, -1);
    return ESP_OK;
}

/* GET /v1/system/stats — CPU, RAM and task load; see util/sys_monitor.h.
 * Serves the monitor's latest sample, so it costs no more than a copy. */
static esp_err_t api_system_stats(httpd_req_t *req)
{
    const std::string body = util::sys_monitor_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, body.c_str(), body.size());
    return ESP_OK;
}

/* GET /v1/wifi/scan — networks the robot can hear, strongest first.
 *
 * {"ok":true,"networks":[{"ssid","rssi","bars":1..3,"secure":bool},...]}
 * or {"ok":false,"error":"busy"} while the STA is mid-join (the app retries).
 * Blocks this request for about a second; see wifi_sta_scan(). */
static esp_err_t api_wifi_scan(httpd_req_t *req)
{
    std::vector<ScanResult> nets;
    const esp_err_t err = wifi_sta_scan(nets);
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        const std::string resp = std::string("{\"ok\":false,\"error\":\"")
            + (err == ESP_ERR_WIFI_STATE ? "busy" : esp_err_to_name(err)) + "\"}";
        httpd_resp_send(req, resp.c_str(), resp.size());
        return ESP_OK;
    }
    std::string json = "{\"ok\":true,\"networks\":[";
    for (std::size_t i = 0; i < nets.size(); ++i) {
        if (i) json += ',';
        json += "{\"ssid\":\"" + json_escape(nets[i].ssid)
              + "\",\"rssi\":" + std::to_string(nets[i].rssi)
              + ",\"bars\":" + std::to_string(nets[i].bars)
              + ",\"secure\":" + (nets[i].secure ? "true" : "false") + "}";
    }
    json += "]}";
    httpd_resp_send(req, json.c_str(), json.size());
    return ESP_OK;
}

/* POST /v1/wifi/ap-config — save AP SSID/password and restart.
 *
 * Body: {"ssid":"MyAP","password":"secret123"}
 * Responds 200 before restarting so the client gets a clean response.
 * The ESP will reboot ~500 ms after the response is sent, giving the
 * PWA time to display the reconnect instructions.
 */
static esp_err_t api_wifi_ap_config(httpd_req_t *req)
{
    char buf[256] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    std::string ssid     = json_get_str(buf, "ssid");
    std::string password = json_get_str(buf, "password");

    if (ssid.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing 'ssid' field", -1);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "POST /v1/wifi/ap-config  ssid=%s  pass_len=%zu",
             ssid.c_str(), password.size());

    if (!wifi_ap_save_config(ssid.c_str(), password.c_str())) {
        httpd_resp_set_status(req, "500 Server Error");
        httpd_resp_send(req, "nvs write failed", -1);
        return ESP_OK;
    }

    // Respond to the client before restarting
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, R"({"ok":true,"restarting":true})", -1);

    ESP_LOGW(TAG, "AP config saved — restarting ESP in 500 ms ...");

    // Flush logs, then restart
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK; // never reached
}

/* POST /v1/wifi/connect — connect to a Wi-Fi network (STA mode) */
static esp_err_t api_wifi_connect(httpd_req_t *req)
{
    char buf[256] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }
    buf[len] = 0;

    std::string ssid = json_get_str(buf, "ssid");
    std::string password = json_get_str(buf, "password");

    if (ssid.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing 'ssid' field", -1);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "POST /v1/wifi/connect  ssid=%s", ssid.c_str());

    if (!wifi_sta_connect(ssid.c_str(), password.c_str())) {
        httpd_resp_set_status(req, "500 Server Error");
        httpd_resp_send(req, "connect failed", -1);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, R"({"ok":true})", -1);
    return ESP_OK;
}

/* POST /v1/wifi/disconnect — disconnect from STA */
static esp_err_t api_wifi_disconnect(httpd_req_t *req)
{
    ESP_LOGI(TAG, "POST /v1/wifi/disconnect");
    wifi_sta_disconnect();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, R"({"ok":true})", -1);
    return ESP_OK;
}

/* POST /v1/wifi/forget — forget saved credentials and disconnect */
static esp_err_t api_wifi_forget(httpd_req_t *req)
{
    ESP_LOGI(TAG, "POST /v1/wifi/forget");
    wifi_sta_forget();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, R"({"ok":true})", -1);
    return ESP_OK;
}

/* ── Body reader helper (heap-allocated, no large stack buffers) ──────── */

/**
 * Read the full request body into a heap-allocated buffer.
 * Caller must free() the returned pointer. Returns NULL on failure.
 */
static char *read_body(httpd_req_t *req)
{
    int content_len = req->content_len;
    if (content_len <= 0) return nullptr;

    char *buf = (char *)malloc(content_len + 1);
    if (!buf) return nullptr;

    int total = 0;
    while (total < content_len) {
        int ret = httpd_req_recv(req, buf + total, content_len - total);
        if (ret <= 0) {
            free(buf);
            return nullptr;
        }
        total += ret;
    }
    buf[total] = '\0';
    return buf;
}

/* ── Lua API endpoints ─────────────────────────────────────── */

/* POST /v1/lua/enqueue — enqueue a Lua script for async execution.
 * Saves the script to a temp file and queues the path to the worker.
 * Non-blocking — returns immediately.  Script runs in lua_worker_task. */
static esp_err_t api_lua_enqueue(httpd_req_t *req)
{
    char *buf = read_body(req);
    if (!buf) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }

    std::string script = json_get_str(buf, "script");
    free(buf);

    if (script.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "need 'script' field", -1);
        return ESP_OK;
    }

    // Write script to temp file on LittleFS
    static int enqueue_seq = 0;
    char path[64];
    std::snprintf(path, sizeof(path), "/lua/_deploy_%d.lua", enqueue_seq++);
    if (!fs::write_file(path, script.data(), script.size())) {
        httpd_resp_set_status(req, "500 Server Error");
        httpd_resp_send(req, R"({"ok":false,"error":"Failed to save script"})", -1);
        return ESP_OK;
    }

    // Enqueue the file path — worker reads and executes via lua_run_file
    if (!network::queue_lua_script(path)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, R"({"ok":false,"error":"Lua queue full"})", -1);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, R"({"ok":true})", -1);
    return ESP_OK;
}

/* POST /v1/lua/run — run a Lua script
 *   Body: {"script":"..."}  or  {"path":"/lua/foo.lua"}
 *   Response: {"ok":true,"output":"..."}  or  {"ok":false,"error":"..."}
 */
static esp_err_t api_lua_run(httpd_req_t *req)
{
    char *buf = read_body(req);
    if (!buf) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }

    std::string script = json_get_str(buf, "script");
    std::string path   = json_get_str(buf, "path");
    free(buf);

    if (script.empty() && path.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "need 'script' or 'path' field", -1);
        return ESP_OK;
    }

    // Use a heap-allocated output buffer (2048 bytes is enough for most results)
    char *output = (char *)malloc(2048);
    if (!output) {
        httpd_resp_set_status(req, "500 Internal Error");
        httpd_resp_send(req, "oom", -1);
        return ESP_OK;
    }
    output[0] = '\0';

    // Saved files are routines (walk, dance) and get the chat worker's
    // minute; inline snippets keep 5 s. Both are now enforced.
    esp_err_t err;
    if (!path.empty()) {
        err = lua_run_file(path.c_str(), output, 2048, 60000);
    } else {
        err = lua_run_string(script.c_str(), output, 2048, 5000);
    }

    std::string escaped = json_escape(output);
    free(output);

    std::string resp = (err == ESP_OK)
        ? R"({"ok":true,"output":")" + escaped + "\"}"
        : R"({"ok":false,"error":")" + escaped + "\"}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp.c_str(), resp.size());
    return ESP_OK;
}

/* GET /v1/lua/list — list .lua files on LittleFS
 *   Response: {"ok":true,"files":["walk.lua","test.lua"]}
 */
static esp_err_t api_lua_list(httpd_req_t *req)
{
    auto files = fs::list_files("/lua");
    std::string json = R"({"ok":true,"files":[)";
    for (size_t i = 0; i < files.size(); i++) {
        if (i > 0) json += ",";
        json += "\"" + json_escape(files[i]) + "\"";
    }
    json += "]}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.size());
    return ESP_OK;
}

/* POST /v1/lua/save — save a Lua script to LittleFS
 *   Body: {"name":"walk.lua","code":"print('hi')"}
 */
static esp_err_t api_lua_save(httpd_req_t *req)
{
    char *buf = read_body(req);
    if (!buf) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }

    std::string name = json_get_str(buf, "name");
    std::string code = json_get_str(buf, "code");
    free(buf);

    if (name.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing 'name' field", -1);
        return ESP_OK;
    }

    std::string path = "/lua/" + name;
    bool ok = fs::write_file(path.c_str(), code.data(), code.size());

    char resp[256];
    std::snprintf(resp, sizeof(resp),
                  R"({"ok":%s,"path":"%s"})", ok ? "true" : "false",
                  json_escape(path).c_str());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, -1);
    return ESP_OK;
}

/* GET /v1/lua/read?name=walk.lua — read a Lua script */
static esp_err_t api_lua_read(httpd_req_t *req)
{
    std::string name;
    std::string query(req->uri);
    auto pos = query.find("name=");
    if (pos != std::string::npos) {
        name = query.substr(pos + 5);
        auto amp = name.find('&');
        if (amp != std::string::npos) name = name.substr(0, amp);
        name = url_decode(name);
    }

    if (name.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing 'name' query param", -1);
        return ESP_OK;
    }

    std::string path = "/lua/" + name;
    auto data = fs::read_file(path.c_str());

    if (data.empty()) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "file not found", -1);
        return ESP_OK;
    }

    std::string code(data.begin(), data.end());
    std::string resp = R"({"ok":true,"name":")" + json_escape(name)
                     + "\",\"code\":\"" + json_escape(code) + "\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp.c_str(), resp.size());
    return ESP_OK;
}

/* POST /v1/lua/delete — delete a Lua script
 *   Body: {"name":"walk.lua"}
 */
static esp_err_t api_lua_delete(httpd_req_t *req)
{
    char *buf = read_body(req);
    if (!buf) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "empty body", -1);
        return ESP_OK;
    }

    std::string name = json_get_str(buf, "name");
    free(buf);

    if (name.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "missing 'name' field", -1);
        return ESP_OK;
    }

    std::string path = "/lua/" + name;
    bool ok = fs::delete_file(path.c_str());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, ok ? R"({"ok":true})" : R"({"ok":false})", -1);
    return ESP_OK;
}

/* ── Registering a handler, with the failure made loud ───────────────────
 *
 * httpd_register_uri_handler() returns ESP_ERR_HTTPD_HANDLERS_FULL when the
 * table is full, and the table fills in registration order. Discarding that
 * code does not produce an error - it produces routes that quietly do not
 * exist, and they are always the ones registered LAST. The symptom is an
 * endpoint that 404s (or 405s) while an identical one beside it works, which
 * reads like a client bug and gets debugged as one.
 *
 * COUNT THE TABLE WHEN YOU ADD ROUTES. The arithmetic in start_http_server()
 * is: register_api() calls + register_static() calls + 2 websockets + 4
 * marketplace wildcard methods. Adding the Servo Studio routes took that
 * total from 41 to 52, past the 48 the table used to hold - which is how this
 * was found. max_uri_handlers is now 64; if you exceed it, this logs by name.
 */
static int s_uri_reg_failed = 0;

static bool register_uri(httpd_handle_t server, const httpd_uri_t *h,
                         const char *method_str)
{
    const esp_err_t rc = httpd_register_uri_handler(server, h);
    if (rc == ESP_OK) return true;

    s_uri_reg_failed++;
    ESP_LOGE(TAG, "  UNREGISTERED: %s %s (%s) - this endpoint will 404/405",
             method_str, h->uri, esp_err_to_name(rc));
    return false;
}

/* ── Helper to register an API handler ─────────────────────── */
static void register_api(httpd_handle_t server, const char *method_str,
                         const char *uri, httpd_method_t method,
                         esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t h = {
        .uri       = uri,
        .method    = method,
        .handler   = handler,
        .user_ctx  = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    if (register_uri(server, &h, method_str)) {
        ESP_LOGI(TAG, "  API: %s %s", method_str, uri);
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  Marketplace Gateway Proxy API
 * ═══════════════════════════════════════════════════════════════ */

/* GET /v1/gateway/config — return gateway connection info */
static esp_err_t api_gateway_config(httpd_req_t *req)
{
    std::string host = CONFIG_APP_CHAT_SERVER_IP;
    int port = CONFIG_APP_CHAT_SERVER_PORT;
    std::string uuid = CONFIG_APP_ROBOT_UUID;

    char resp[512];
    int n = std::snprintf(resp, sizeof(resp),
        R"({"host":"%s","port":%d,"robot_uuid":"%s"})",
        host.c_str(), port, uuid.c_str());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, n);
    return ESP_OK;
}

/**
 * @brief Generic proxy handler for all /v1/marketplace/ endpoints.
 *
 * Translates the local URI into the corresponding Gateway path and
 * forwards the request via gateway_request().
 *
 * URI translation rules:
 *   /v1/marketplace/skills[/...]        -> /v1/skills[/...]
 *   /v1/marketplace/robot/skills[/...]  -> /v1/robots/{uuid}/skills[/...]
 */
/* ── Marketplace GET response cache ──────────────────────────────
 * Serving repeated marketplace polls from a short-lived cache avoids one
 * outbound proxy socket PER request.  Under a stress test the browser hammers
 * these read-only endpoints, and each proxied request cost an inbound + an
 * outbound socket — doubling demand until the LWIP pool ran dry.  Caching GETs
 * for a few seconds collapses that back to (at most) one proxy call per TTL.
 */
struct MpxCacheEntry {
    std::string  path;
    std::string  body;
    TickType_t   expires = 0;
    bool         valid = false;
};
static MpxCacheEntry     s_mpx_cache[4];
static SemaphoreHandle_t s_mpx_cache_mutex = nullptr;
static constexpr uint32_t MPX_CACHE_TTL_MS = 15000;   // 15 s

static SemaphoreHandle_t mpx_cache_mutex()
{
    if (!s_mpx_cache_mutex) s_mpx_cache_mutex = xSemaphoreCreateMutex();
    return s_mpx_cache_mutex;
}

// Return cached body for `path` if present and unexpired, else empty string.
static std::string mpx_cache_get(const std::string &path)
{
    SemaphoreHandle_t m = mpx_cache_mutex();
    if (!m) return {};
    std::string out;
    xSemaphoreTake(m, portMAX_DELAY);
    TickType_t now = xTaskGetTickCount();
    for (auto &e : s_mpx_cache) {
        if (e.valid && e.path == path && (int32_t)(e.expires - now) > 0) {
            out = e.body;
            break;
        }
    }
    xSemaphoreGive(m);
    return out;
}

static void mpx_cache_put(const std::string &path, const std::string &body)
{
    SemaphoreHandle_t m = mpx_cache_mutex();
    if (!m) return;
    xSemaphoreTake(m, portMAX_DELAY);
    TickType_t now = xTaskGetTickCount();
    // Reuse a matching/empty/expired slot, else the oldest.
    MpxCacheEntry *slot = nullptr;
    for (auto &e : s_mpx_cache) {
        if (!e.valid || e.path == path || (int32_t)(e.expires - now) <= 0) { slot = &e; break; }
    }
    if (!slot) slot = &s_mpx_cache[0];
    slot->path    = path;
    slot->body    = body;
    slot->expires = now + pdMS_TO_TICKS(MPX_CACHE_TTL_MS);
    slot->valid   = true;
    xSemaphoreGive(m);
}

// Drop all cached entries (called after any write — deploy/remove/etc.).
static void mpx_cache_invalidate_all()
{
    SemaphoreHandle_t m = mpx_cache_mutex();
    if (!m) return;
    xSemaphoreTake(m, portMAX_DELAY);
    for (auto &e : s_mpx_cache) e.valid = false;
    xSemaphoreGive(m);
}

static esp_err_t api_marketplace_proxy(httpd_req_t *req)
{
    // ── Determine the target Gateway path ─────────────────────
    const char *uri = req->uri;
    std::string target_path;

    const char *prefix_skills = "/v1/marketplace/skills";
    const char *prefix_orders = "/v1/marketplace/orders";
    const char *prefix_robot  = "/v1/marketplace/robot/";

    if (std::strncmp(uri, prefix_robot, std::strlen(prefix_robot)) == 0) {
        // /v1/marketplace/robot/{action}[/...] → /v1/robots/{uuid}/{action}[/...]
        // e.g.  /v1/marketplace/robot/skills      → /v1/robots/{uuid}/skills
        //       /v1/marketplace/robot/skills/{id}  → /v1/robots/{uuid}/skills/{id}
        //       /v1/marketplace/robot/deploy       → /v1/robots/{uuid}/deploy
        //       /v1/marketplace/robot/checkout     → /v1/robots/{uuid}/checkout
        //       /v1/marketplace/robot/orders       → /v1/robots/{uuid}/orders
        std::string suffix = uri + std::strlen(prefix_robot);
        target_path = "/v1/robots/" + std::string(CONFIG_APP_ROBOT_UUID) + "/" + suffix;
    } else if (std::strncmp(uri, prefix_skills, std::strlen(prefix_skills)) == 0) {
        // /v1/marketplace/skills[/...] → /v1/skills[/...]
        // Strip "/v1/marketplace/" (16 chars), prepend "/v1/"
        target_path = std::string("/v1/") + (uri + 16);
    } else if (std::strncmp(uri, prefix_orders, std::strlen(prefix_orders)) == 0) {
        // /v1/marketplace/orders[/...] → /v1/orders[/...]  (checkout order polls)
        target_path = std::string("/v1/") + (uri + 16);
    } else {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "unknown marketplace path", -1);
        return ESP_OK;
    }

    // ── Read request body if present ──────────────────────────
    std::string body;
    if (req->content_len > 0 && req->content_len < 4096) {
        char *buf = static_cast<char *>(std::malloc(req->content_len + 1));
        if (buf) {
            int total = 0;
            while (total < req->content_len) {
                int r = httpd_req_recv(req, buf + total, req->content_len - total);
                if (r <= 0) break;
                total += r;
            }
            buf[total] = 0;
            body.assign(buf, total);
            std::free(buf);
        }
    }

    // ── Map HTTP method ───────────────────────────────────────
    const char *method_str = "GET";
    if (req->method == HTTP_POST)   method_str = "POST";
    else if (req->method == HTTP_PATCH)  method_str = "PATCH";
    else if (req->method == HTTP_DELETE) method_str = "DELETE";

    const bool is_get = (req->method == HTTP_GET);

    // Order-status polls must NOT be cached — the PWA polls every couple of
    // seconds and a stale "pending" body would delay the owned→paid flip.
    const bool cacheable = is_get &&
        std::strncmp(target_path.c_str(), "/v1/orders", 10) != 0;

    // ── Serve GETs from the short-lived cache (no proxy socket) ──
    if (cacheable) {
        std::string cached = mpx_cache_get(target_path);
        if (!cached.empty()) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, cached.c_str(), cached.size());
            return ESP_OK;
        }
    } else if (!is_get) {
        // Any write changes the catalog — drop cached reads.
        mpx_cache_invalidate_all();
    }

    // ── Forward to Gateway ────────────────────────────────────
    bool ok = false;
    std::string resp_body = gateway_request(method_str, target_path.c_str(), body, ok);

    ESP_LOGI(TAG, "Marketplace proxy [socketfix-v14]: %s %s → %s (ok=%d, body=%zu bytes)",
             method_str, uri, target_path.c_str(), ok, resp_body.size());

    // Cache successful GET responses so repeated polls don't re-proxy.
    if (cacheable && ok && !resp_body.empty()) {
        mpx_cache_put(target_path, resp_body);
    }

    if (!ok) {
        // Gateway returned an error or is unreachable
        if (resp_body.empty()) {
            ESP_LOGW(TAG, "Gateway unreachable for %s", target_path.c_str());
            httpd_resp_set_status(req, "502 Bad Gateway");
            httpd_resp_send(req, "gateway unreachable", -1);
        } else {
            ESP_LOGW(TAG, "Gateway error response: %.*s",
                     (int)std::min(resp_body.size(), size_t(256)),
                     resp_body.c_str());
            httpd_resp_set_status(req, "502 Bad Gateway");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, resp_body.c_str(), resp_body.size());
        }
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp_body.c_str(), resp_body.size());
    return ESP_OK;
}

/* ── Dead-connection reaper ──────────────────────────────────────
 * The socket census proved the pool was exhausted by DEAD browser
 * connections on port 80 (peer=0.0.0.0) that httpd never freed.  This task
 * periodically peeks every port-80 socket; if the peer has closed (recv
 * returns 0 / the socket is no longer connected), it asks httpd to close that
 * session.  Using httpd_sess_trigger_close (not a raw close) keeps it safe —
 * httpd owns the actual close and ignores fds that aren't its sessions.
 */
static void socket_reaper_task(void *)
{
    // Only httpd's own client sessions, and only through httpd. The version before this one
    // close()d raw fds - port-80 sockets httpd still had in its session table, and any socket whose
    // getsockname() failed, whoever owned it. httpd then select()ed on a closed fd ("error in
    // select (9)", "Closing invalid socket -1"), and once lwIP reused the number for a new
    // connection, httpd closed or answered the wrong socket: the app "could not reach the robot"
    // after a run (21 Oct). httpd_sess_trigger_close() queues the close on httpd's own task.
    static int fds[CONFIG_LWIP_MAX_SOCKETS];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(4000));
        if (!s_server) continue;

        size_t n = sizeof(fds) / sizeof(fds[0]);
        if (httpd_get_client_list(s_server, &n, fds) != ESP_OK) continue;
        int reaped = 0;
        for (size_t i = 0; i < n; ++i) {
            const int fd = fds[i];
            // Peek one byte, non-blocking: 0 = the peer closed (EOF); a hard error other than
            // "would block" = the link is dead. A live idle client returns EWOULDBLOCK.
            char probe;
            const int r = recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
            const bool dead = (r == 0) || (r < 0 && errno != EWOULDBLOCK && errno != EAGAIN);
            if (dead && httpd_sess_trigger_close(s_server, fd) == ESP_OK) reaped++;
        }
        if (reaped) {
            ESP_LOGW(TAG, "socket_reaper: closed %d dead web connection(s)", reaped);
        }
    }
}

/* ── Public API ─────────────────────────────────────────────── */

bool start_http_server()
{
    if (s_server) {
        ESP_LOGW(TAG, "HTTP server already running");
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // 52 register_api() + up to 12 register_static() + 2 websockets + 4
    // marketplace wildcard methods + 5 display routes (http_display.cc) + 6
    // voice routes (http_voice.cc) = 81.
    // See the note on register_uri() - this number is load-bearing, and being
    // one short deletes a route without saying so. Each slot is a pointer, so
    // the headroom is cheap.
    config.max_uri_handlers = 96;
    // LWIP has CONFIG_LWIP_MAX_SOCKETS (16) total.  HTTPD also reserves 3
    // internal sockets on top of max_open_sockets.  At 5, HTTPD uses at most
    // 5+3=8, GUARANTEEING ~8 sockets stay free for OUTBOUND use (upstream cloud
    // WebSocket, marketplace proxy, DNS) plus the PWA WS clients.  A browser
    // opens up to ~6 persistent connections per tab, so a lower cap here forces
    // it to reuse/cycle connections instead of pinning the whole pool.  With
    // lru_purge_enable the oldest idle connection is dropped when the cap is hit
    // rather than starving everything else.
    config.max_open_sockets = 5;
    config.stack_size = 8192;
    config.task_priority = 6;            // Priority 6 (per REQ-ROB-02)
    config.core_id = 0;                  // Pin to Core 0 (PRO_CPU)
    config.server_port = 80;
    config.uri_match_fn = httpd_uri_match_wildcard;  // Enable wildcard (*) URIs
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 60;       // Allow idle WS up to 60 s between frames
    config.send_wait_timeout = 30;       // Allow slow sends (e.g. large replies)
    config.close_fn = pwa_release_fd;    // Clean up PWA client slots on session close

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return false;
    }

    // ── BUILD MARKER — confirms which firmware is actually running ──
    ESP_LOGW(TAG, "==== MPX BUILD socketfix-v14 : max_open_sockets=%d ====",
             config.max_open_sockets);

    // Start the dead-connection reaper so leaked port-80 sockets can't
    // accumulate and exhaust the pool.
    xTaskCreateWithCaps(socket_reaper_task, "sock_reaper", 3072, nullptr, 3, nullptr,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   // internal RAM is for DMA

    // ── Helper to register a static file handler ──
    auto register_static = [&](const char *uri) {
        httpd_uri_t h = {
            .uri       = uri,
            .method    = HTTP_GET,
            .handler   = static_handler,
            .user_ctx  = nullptr,
            .is_websocket = false,
            .handle_ws_control_frames = false,
            .supported_subprotocol = nullptr,
        };
        register_uri(s_server, &h, "GET");
    };

    // ── Register handlers for each known static file ──
    register_static("/");
    register_static("/index.html");
    register_static("/m.js");
    register_static("/index.css");
    register_static("/manifest.json");
    register_static("/sw.js");
    register_static("/favicon.ico");
    register_static("/md.svg");
    register_static("/eye-open.svg");
    register_static("/eye-close.svg");
    register_static("/m-pupper3d.js");         // lazily loaded three.js viewer
    register_static("/pupper_model.json");     // three.js calibration model

    // ── Register WebSocket endpoint (telemetry) ──
    httpd_uri_t ws_uri = {
        .uri       = "/v1/telemetry/stream",
        .method    = HTTP_GET,
        .handler   = websocket_handler,
        .user_ctx  = nullptr,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    register_uri(s_server, &ws_uri, "GET");

    // ── Register Chat WebSocket endpoint ──
    httpd_uri_t chat_ws_uri = {
        .uri       = "/v1/chat/ui",
        .method    = HTTP_GET,
        .handler   = chat_ws_handler,
        .user_ctx  = nullptr,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    register_uri(s_server, &chat_ws_uri, "GET");

    // ── Register REST API endpoints ──
    register_api(s_server, "GET",  "/v1/skills/list",   HTTP_GET,  api_skills_list);
    register_api(s_server, "POST", "/v1/skills/run",    HTTP_POST, api_skills_run);
    register_api(s_server, "GET",  "/v1/fs/list",       HTTP_GET,  api_fs_list);
    register_api(s_server, "GET",  "/v1/fs/info",       HTTP_GET,  api_fs_info);
    register_api(s_server, "GET",  "/v1/fs/read",       HTTP_GET,  api_fs_read);
    register_api(s_server, "POST", "/v1/fs/delete",     HTTP_POST, api_fs_delete);
    register_api(s_server, "POST", "/v1/fs/write",      HTTP_POST, api_fs_write);
    register_api(s_server, "POST", "/v1/skills/upload", HTTP_POST, api_skills_upload);
    // ── Ported from mpv2-claw (see .cursor/PORT_NOTES.md) ──
    register_api(s_server, "GET",  "/v1/skills/status",   HTTP_GET,  api_skills_status);
    register_api(s_server, "POST", "/v1/skills/stop",     HTTP_POST, api_skills_stop);
    register_api(s_server, "GET",  "/v1/skills/registry", HTTP_GET,  api_skills_registry);

    // ── Register robot control API endpoints ──
    register_api(s_server, "POST", "/v1/robot/gait",           HTTP_POST, api_robot_gait);
    register_api(s_server, "POST", "/v1/robot/joy",            HTTP_POST, api_robot_joy);
    register_api(s_server, "GET",  "/v1/robot/status",         HTTP_GET,  api_robot_status);
    // ── Ported from mpv2-claw (see .cursor/PORT_NOTES.md) ──
    register_api(s_server, "GET",  "/v1/robot/moves",          HTTP_GET,  api_robot_moves);
    register_api(s_server, "GET",  "/v1/robot/look",           HTTP_GET,  api_robot_look);
    register_api(s_server, "GET",  "/v1/robot/power",          HTTP_GET,  api_robot_power_get);
    register_api(s_server, "POST", "/v1/robot/power",          HTTP_POST, api_robot_power_post);
    register_api(s_server, "GET",  "/v1/agent/permissions",    HTTP_GET,  api_agent_permissions_get);
    register_api(s_server, "POST", "/v1/agent/permissions",    HTTP_POST, api_agent_permissions_post);
    register_api(s_server, "POST", "/v1/robot/config",         HTTP_POST, api_robot_config);
    register_api(s_server, "GET",  "/v1/robot/config",         HTTP_GET,  api_robot_get_config);
    register_api(s_server, "POST", "/v1/robot/calibrate",      HTTP_POST, api_robot_calibrate);
    register_api(s_server, "POST", "/v1/robot/calibrate/reset",HTTP_POST, api_robot_cal_reset);
    register_api(s_server, "POST", "/v1/robot/diagnostic/ping", HTTP_POST, api_robot_diag_ping);
    register_api(s_server, "POST", "/v1/robot/diagnostic/sweep",HTTP_POST, api_robot_diag_sweep);
    register_api(s_server, "GET",  "/v1/trace",               HTTP_GET,  api_trace);

    // ── Servo Studio ────────────────────────────────────────────
    // Parameter routes need studio mode (?on=1 first); status and temps do not,
    // because the AT32 ships that telemetry on every feedback frame anyway.
    register_api(s_server, "GET", "/v1/studio/mode",    HTTP_GET, studio_mode_handler);
    register_api(s_server, "GET", "/v1/studio/status",  HTTP_GET, studio_status_handler);
    register_api(s_server, "GET", "/v1/studio/scan",    HTTP_GET, studio_scan_handler);
    register_api(s_server, "GET", "/v1/studio/dump",    HTTP_GET, studio_dump_handler);
    register_api(s_server, "GET", "/v1/studio/set",     HTTP_GET, studio_set_handler);
    register_api(s_server, "GET", "/v1/studio/setall",  HTTP_GET, studio_setall_handler);
    register_api(s_server, "GET", "/v1/studio/live",    HTTP_GET, studio_live_handler);
    register_api(s_server, "GET", "/v1/studio/temps",   HTTP_GET, studio_temps_handler);
    register_api(s_server, "GET", "/v1/studio/save",    HTTP_GET, studio_save_handler);
    register_api(s_server, "GET", "/v1/studio/restore", HTTP_GET, studio_restore_handler);
    register_api(s_server, "GET", "/v1/studio/direct",  HTTP_GET, studio_direct_handler);

    // ── Register WiFi config API endpoints ──
    register_api(s_server, "GET",  "/v1/wifi/status",          HTTP_GET,  api_wifi_status);
    register_api(s_server, "GET",  "/v1/pi/status",            HTTP_GET,  api_pi_status);
    register_api(s_server, "POST", "/v1/pi/power",             HTTP_POST, api_pi_power);
    register_api(s_server, "GET",  "/v1/system/stats",         HTTP_GET,  api_system_stats);
    register_api(s_server, "GET",  "/v1/wifi/scan",            HTTP_GET,  api_wifi_scan);
    register_api(s_server, "POST", "/v1/wifi/connect",         HTTP_POST, api_wifi_connect);
    register_api(s_server, "POST", "/v1/wifi/disconnect",      HTTP_POST, api_wifi_disconnect);
    register_api(s_server, "POST", "/v1/wifi/forget",          HTTP_POST, api_wifi_forget);
    register_api(s_server, "POST", "/v1/wifi/ap-config",       HTTP_POST, api_wifi_ap_config);

    // ── The screen (ported from mpv2-claw): 5 routes, in http_display.cc ──
    network::register_display_routes(s_server, register_uri);

    // ── AI & Voice (ported from mpv2-claw): up to 6 routes, in http_voice.cc ──
    network::register_voice_routes(s_server, register_uri);

    // ── Register Lua async enqueue endpoint (non-blocking) ──
    register_api(s_server, "POST", "/v1/lua/enqueue",          HTTP_POST, api_lua_enqueue);

    // ── Register Lua API endpoints ──
    register_api(s_server, "POST", "/v1/lua/run",              HTTP_POST, api_lua_run);
    register_api(s_server, "GET",  "/v1/lua/list",             HTTP_GET,  api_lua_list);
    register_api(s_server, "POST", "/v1/lua/save",             HTTP_POST, api_lua_save);
    register_api(s_server, "GET",  "/v1/lua/read",             HTTP_GET,  api_lua_read);
    register_api(s_server, "POST", "/v1/lua/delete",           HTTP_POST, api_lua_delete);

    // ── Register Chat REST endpoint ──
    register_api(s_server, "POST", "/v1/chat/send",            HTTP_POST, chat_send_handler);

    // ── Register Marketplace Gateway endpoints ──
    register_api(s_server, "GET",  "/v1/gateway/config",             HTTP_GET,   api_gateway_config);

    // Wildcard: catch all /v1/marketplace/* paths
    {
        httpd_uri_t h = {
            .uri       = "/v1/marketplace/*",
            .method    = HTTP_GET,
            .handler   = api_marketplace_proxy,
            .user_ctx  = nullptr,
            .is_websocket = false,
            .handle_ws_control_frames = false,
            .supported_subprotocol = nullptr,
        };
        register_uri(s_server, &h, "GET");
        h.method = HTTP_POST;
        register_uri(s_server, &h, "POST");
        h.method = HTTP_PATCH;
        register_uri(s_server, &h, "PATCH");
        h.method = HTTP_DELETE;
        register_uri(s_server, &h, "DELETE");
        ESP_LOGI(TAG, "  API: GET|POST|PATCH|DELETE /v1/marketplace/* (proxied to gateway)");
    }

    // The phones' "is there internet?" checks (captive.h). LAST: with wildcard matching the first
    // registered match wins, so this only sees what nothing above answers (and 404s the rest).
    {
        httpd_uri_t h = {
            .uri       = "/*",
            .method    = HTTP_GET,
            .handler   = network::captive_http_handler,
            .user_ctx  = nullptr,
            .is_websocket = false,
            .handle_ws_control_frames = false,
            .supported_subprotocol = nullptr,
        };
        register_uri(s_server, &h, "GET");
    }

    ESP_LOGI(TAG, "HTTP server running on port 80 (Core 0, Priority 6)");
    if (s_uri_reg_failed) {
        // Say it once, at the end, in one place. A route that failed to
        // register is a missing feature, not a warning - see register_uri().
        ESP_LOGE(TAG, "%d URI handler(s) FAILED to register (table is %d). "
                      "Those endpoints do not exist. Raise max_uri_handlers.",
                 s_uri_reg_failed, config.max_uri_handlers);
    }
    ESP_LOGI(TAG, "  → PWA:  http://%s/", AP_IP_ADDR);
    ESP_LOGI(TAG, "  → WS:   ws://%s/v1/telemetry/stream", AP_IP_ADDR);
    ESP_LOGI(TAG, "  → WS:   ws://%s/v1/chat/ui", AP_IP_ADDR);

    return true;
}

void stop_http_server()
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = nullptr;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

}  // namespace network

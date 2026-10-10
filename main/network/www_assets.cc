#include "network/www_assets.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fs/littlefs_manager.h"
#include "sdkconfig.h"

static const char *TAG = "www_assets";

/* ── LittleFS mount point (must match fs/littlefs_manager.cc) ── */
static constexpr const char *MOUNT_POINT = "/fs";

/* ── Embedded binary symbols (linked via EMBED_FILES in CMakeLists) ── */

// Each www file is linked as _binary_<filename>_start / _end
// NOTE: symbol names are based on the filename only (path prefix stripped).

#define DECLARE_EMBED(name)                                                  \
    extern const uint8_t _binary_##name##_start[];                           \
    extern const uint8_t _binary_##name##_end[]

// ── App shell (the large files are embedded as .gz only, see main/CMakeLists.txt)
DECLARE_EMBED(index_html);
DECLARE_EMBED(index_html_gz);
DECLARE_EMBED(index_css_gz);
DECLARE_EMBED(m_js_gz);
DECLARE_EMBED(manifest_json);
DECLARE_EMBED(manifest_json_gz);
DECLARE_EMBED(sw_js);
DECLARE_EMBED(sw_js_gz);

// ── Icons and the calibration viewer ─────────────────────────
DECLARE_EMBED(md_svg);
DECLARE_EMBED(md_svg_gz);
DECLARE_EMBED(eye_open_svg);
DECLARE_EMBED(eye_open_svg_gz);
DECLARE_EMBED(eye_close_svg);
DECLARE_EMBED(eye_close_svg_gz);
DECLARE_EMBED(m_pupper3d_js_gz);       // three.js viewer chunk, gz only
DECLARE_EMBED(pupper_model_json_gz);   // gz only, see main/CMakeLists.txt

/* ── File descriptor table ──────────────────────────────────── */

struct AssetFile {
    const char *name;              // path within /fs/www/
    const uint8_t *start;
    const uint8_t *end;

    std::size_t size() const { return static_cast<std::size_t>(end - start); }
};

// Helper macro to avoid repetition in the table
#define ASSET_ENTRY(path, sym)  { path, _binary_##sym##_start, _binary_##sym##_end }

static const AssetFile ASSETS[] = {
    // ── Common ───────────────────────────────────────────────
    ASSET_ENTRY("/index.html",       index_html),
    ASSET_ENTRY("/index.html.gz",    index_html_gz),
    ASSET_ENTRY("/index.css.gz",     index_css_gz),
    ASSET_ENTRY("/m.js.gz",          m_js_gz),
    ASSET_ENTRY("/manifest.json",    manifest_json),
    ASSET_ENTRY("/manifest.json.gz", manifest_json_gz),
    ASSET_ENTRY("/sw.js",            sw_js),
    ASSET_ENTRY("/sw.js.gz",         sw_js_gz),

    // ── Icons and the calibration viewer ─────────────────────
    ASSET_ENTRY("/md.svg",           md_svg),
    ASSET_ENTRY("/md.svg.gz",        md_svg_gz),
    ASSET_ENTRY("/eye-open.svg",     eye_open_svg),
    ASSET_ENTRY("/eye-open.svg.gz",  eye_open_svg_gz),
    ASSET_ENTRY("/eye-close.svg",    eye_close_svg),
    ASSET_ENTRY("/eye-close.svg.gz", eye_close_svg_gz),
    ASSET_ENTRY("/m-pupper3d.js.gz",     m_pupper3d_js_gz),
    ASSET_ENTRY("/pupper_model.json.gz", pupper_model_json_gz),
};

static constexpr int NUM_ASSETS = sizeof(ASSETS) / sizeof(ASSETS[0]);

/* ── Public API ─────────────────────────────────────────────── */

bool network::deploy_www_assets()
{
    // Re-deploy assets every boot to pick up firmware-embedded updates.
    // The time cost (~50ms for 50KB) is negligible.

    // Create the /www/ subdirectory inside LittleFS
    {
        char dir_path[64];
        std::snprintf(dir_path, sizeof(dir_path), "%s/www", MOUNT_POINT);
        mkdir(dir_path, 0755);
        ESP_LOGI(TAG, "Ensured www directory exists: %s", dir_path);
    }

    size_t total_bytes = 0;
    int deployed = 0;

    for (int i = 0; i < NUM_ASSETS; i++) {
        const auto &a = ASSETS[i];
        const size_t sz = a.size();

        if (sz == 0) {
            ESP_LOGW(TAG, "Skipping empty asset: %s", a.name);
            continue;
        }

        // Build the full LittleFS path
        char littlefs_path[64];
        std::snprintf(littlefs_path, sizeof(littlefs_path), "/www%s", a.name);

        if (fs::write_file(littlefs_path, a.start, sz)) {
            total_bytes += sz;
            deployed++;
            ESP_LOGD(TAG, "  Deployed %s (%zu bytes)", a.name, sz);
        } else {
            ESP_LOGE(TAG, "  Failed to deploy %s", a.name);
            return false;
        }
    }

    ESP_LOGI(TAG, "Deployed %d PWA assets (%zu bytes) to LittleFS",
             deployed, total_bytes);

    return true;
}

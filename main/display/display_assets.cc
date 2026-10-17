/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The three animations that ship with the firmware (blink, smile, sleepy).
 *
 * mpv2-claw bakes them into its storage image at build time. mpx-dog has no
 * build-time LittleFS image -- its PWA assets are embedded in the app and
 * copied out at boot -- so the GIFs go the same way, into <fs>/display/.
 * Unlike the PWA assets they are written only when missing or of a
 * different size: 310 KB per boot would be needless flash wear, and a user
 * cannot overwrite a built-in id from the app (http_display.cc refuses), so
 * a matching size means the file is ours.
 */

#include "display/display_assets.h"

#include <cstdio>
#include <sys/stat.h>

#include "esp_log.h"

static const char *TAG = "display_assets";

#define DECLARE_EMBED(name)                                  \
    extern const uint8_t _binary_##name##_start[];           \
    extern const uint8_t _binary_##name##_end[]

DECLARE_EMBED(blink_gif);
DECLARE_EMBED(smile_gif);
DECLARE_EMBED(sleepy_gif);

namespace {

struct Asset {
    const char    *file;   /* "<id>.gif" */
    const uint8_t *start;
    const uint8_t *end;
    size_t size() const { return static_cast<size_t>(end - start); }
};

#define ASSET(file, sym) { file, _binary_##sym##_start, _binary_##sym##_end }

const Asset ASSETS[] = {
    ASSET("blink.gif",  blink_gif),
    ASSET("smile.gif",  smile_gif),
    ASSET("sleepy.gif", sleepy_gif),
};

}  // namespace

bool display::deploy_builtin_gifs(const char *dir)
{
    char path[96];
    bool ok = true;
    int written = 0;

    if (!dir || !dir[0]) {
        return false;
    }
    mkdir(dir, 0755);

    for (const Asset &a : ASSETS) {
        struct stat st;
        std::snprintf(path, sizeof(path), "%s/%s", dir, a.file);
        if (stat(path, &st) == 0 && static_cast<size_t>(st.st_size) == a.size()) {
            continue;
        }
        FILE *f = std::fopen(path, "wb");
        const bool good = f && std::fwrite(a.start, 1, a.size(), f) == a.size();
        if (f) {
            std::fclose(f);
        }
        if (!good) {
            ESP_LOGE(TAG, "could not write %s", path);
            std::remove(path);
            ok = false;
            continue;
        }
        written++;
    }
    if (written) {
        ESP_LOGI(TAG, "%d built-in animation(s) written to %s", written, dir);
    }
    return ok;
}

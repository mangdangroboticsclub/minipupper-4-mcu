/*
 * SPDX-FileCopyrightText: 2026 MangDang
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mpx_skill_fs.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"

extern "C" {
#include "claw_paths.h"
}

static const char *TAG = "mpx_skill_fs";

namespace fs {
namespace {

constexpr const char *kSkillsDirName = "mpx_skills";

/* Resolve a caller path onto the skills root.
 *
 * Refuses anything containing ".." rather than trying to normalise it. A
 * normaliser is a thing you have to get exactly right against input that
 * arrives from a marketplace; a refusal is a thing you cannot get wrong.  */
bool resolve(const char *path, std::string &out)
{
    const std::string &root = skills_root();
    if (root.empty()) {
        return false;
    }
    if (path == nullptr) {
        return false;
    }
    if (std::strstr(path, "..") != nullptr) {
        ESP_LOGW(TAG, "refusing path with '..': %s", path);
        return false;
    }

    const char *rel = path;
    while (*rel == '/') {
        rel++;
    }

    out = root;
    if (*rel != '\0') {
        out += '/';
        out += rel;
    }
    return true;
}

}  // namespace

const std::string &skills_root()
{
    static std::string s_root;
    static bool s_tried = false;

    if (!s_tried) {
        s_tried = true;
        char buf[192];
        if (claw_paths_join(CLAW_PATH_DATA, kSkillsDirName, buf, sizeof(buf)) == ESP_OK) {
            struct stat st = {};
            if (stat(buf, &st) != 0) {
                if (mkdir(buf, 0775) != 0 && errno != EEXIST) {
                    ESP_LOGE(TAG, "cannot create %s: %s", buf, std::strerror(errno));
                    return s_root;
                }
            } else if (!S_ISDIR(st.st_mode)) {
                ESP_LOGE(TAG, "%s exists and is not a directory", buf);
                return s_root;
            }
            s_root = buf;
            ESP_LOGI(TAG, "skills root: %s", s_root.c_str());
        } else {
            ESP_LOGE(TAG, "DATA root not available; skills unavailable");
        }
    }
    return s_root;
}

std::vector<std::uint8_t> read_file(const char *path)
{
    std::vector<std::uint8_t> out;
    std::string full;
    if (!resolve(path, full)) {
        return out;
    }

    FILE *f = std::fopen(full.c_str(), "rb");
    if (f == nullptr) {
        return out;
    }

    struct stat st = {};
    if (stat(full.c_str(), &st) != 0 || st.st_size <= 0) {
        std::fclose(f);
        return out;
    }

    out.resize((size_t)st.st_size);
    const size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);

    if (got != out.size()) {
        ESP_LOGW(TAG, "short read on %s (%u of %u)", full.c_str(),
                 (unsigned)got, (unsigned)out.size());
        out.clear();
    }
    return out;
}

std::vector<std::string> list_files(const char *dir_path)
{
    std::vector<std::string> out;
    std::string full;
    if (!resolve(dir_path != nullptr ? dir_path : "/", full)) {
        return out;
    }

    DIR *dir = opendir(full.c_str());
    if (dir == nullptr) {
        return out;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        /* FATFS on ESP-IDF fills d_type, but not every VFS does. Fall back to
         * stat when it says DT_UNKNOWN rather than assuming.               */
        if (ent->d_type == DT_DIR) {
            continue;
        }
        if (ent->d_type == DT_UNKNOWN) {
            struct stat st = {};
            const std::string probe = full + "/" + ent->d_name;
            if (stat(probe.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
                continue;
            }
        }
        out.emplace_back(std::string("/") + ent->d_name);
    }
    closedir(dir);
    return out;
}

bool write_file(const char *path, const std::uint8_t *data, std::size_t len)
{
    std::string full;
    if (!resolve(path, full) || data == nullptr) {
        return false;
    }

    FILE *f = std::fopen(full.c_str(), "wb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "cannot open %s for write: %s", full.c_str(), std::strerror(errno));
        return false;
    }

    const size_t put = std::fwrite(data, 1, len, f);
    const bool ok = (put == len) && (std::fclose(f) == 0);

    if (!ok) {
        /* Do not leave a truncated module behind: the registry would parse it
         * on the next rescan and a half-written .wasm is not a thing anyone
         * wants to debug. */
        ESP_LOGE(TAG, "write failed on %s (%u of %u), removing",
                 full.c_str(), (unsigned)put, (unsigned)len);
        unlink(full.c_str());
        return false;
    }
    return true;
}

bool delete_file(const char *path)
{
    std::string full;
    if (!resolve(path, full)) {
        return false;
    }
    return unlink(full.c_str()) == 0;
}

bool exists(const char *path)
{
    std::string full;
    if (!resolve(path, full)) {
        return false;
    }
    struct stat st = {};
    return stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

}  // namespace fs

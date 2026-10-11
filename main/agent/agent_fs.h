#pragma once

/*
 * agent_fs -- the two filesystem roots ESP-Claw works from.
 *
 *   SYSTEM  the read-only `system` LittleFS partition, mounted at /system:
 *           the SKILL.md files shipped with the firmware (skill_builder puts
 *           every build component's skills/ there) and the `.recovery` seeds.
 *   DATA    CONFIG_APP_AGENT_DATA_DIR (<fs>/agent) on the storage LittleFS
 *           mounted by fs::init_littlefs(): sessions, memory, installed
 *           skills, router rules, schedules, inbox. Kept apart from <fs>/skills
 *           (this firmware's WebAssembly robot skills) and <fs>/lua.
 *
 * mpv2-claw's app_fs.c does the same for FAT; here it is LittleFS and the
 * storage partition is already mounted when this runs.
 */

#include "esp_err.h"

namespace agent_fs {

/** Where the read-only system image is mounted. */
constexpr const char *kSystemRoot = "/system";

/**
 * Mount /system (read-only), create the DATA tree, copy every file under
 * /system/.recovery that is missing from DATA, and tell ESP-Claw both roots
 * (claw_paths_set). Call once, before agent startup; safe to call again
 * (returns the first result).
 */
esp_err_t init();

/** True once init() succeeded. */
bool ready();

}  // namespace agent_fs

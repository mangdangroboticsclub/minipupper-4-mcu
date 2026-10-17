#pragma once

/*
 * agent_caps -- the robot's tools as ESP-Claw capabilities.
 *
 * robot_tools' table, registered with ESP-Claw's capability registry in the
 * four groups mpv2-claw's components used (cap_robot, cap_display,
 * cap_mpx_skill, cap_music), so the SKILL.md files in main/skills/ -- whose
 * cap_groups name those ids -- unlock exactly these tools. Each group is an
 * external capability group of app_claw, registered before app_claw_start().
 */

#include "esp_err.h"

namespace agent_caps {

/** Hand the groups to app_claw. Before app_claw_start(); once. */
esp_err_t register_groups();

}  // namespace agent_caps

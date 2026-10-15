/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * `policy` on the serial console.
 */
#include <dirent.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#ifndef CONFIG_MP4_POLICY_INTERNAL_RESERVE_KB
#define CONFIG_MP4_POLICY_INTERNAL_RESERVE_KB 96
#endif

#include "mpx_policy.h"
#include "policy_priv.h"

static const char *TAG = "policy";

static void print_status(void)
{
    mpx_policy_status_t st;
    mpx_policy_get_status(&st);
    printf("state        %s\n", mpx_policy_state_name(st.state));
    printf("policy       %s (for %s%s, %s)\n", st.name[0] ? st.name : "(none)", st.robot,
           strcmp(st.robot, CONFIG_MP4_POLICY_ROBOT) ? " - NOT this robot" : "",
           st.omni ? "omni: vx vy wz" : "forward only: vx");
    printf("calibration  %s offsets\n", st.robot_offsets ? "the robot's own" : "the policy contract's");
    printf("command      vx %.3f m/s  vy %.3f m/s  wz %.3f rad/s\n", st.cmd[0], st.cmd[1], st.cmd[2]);
    printf("last run     %s%lu steps, %lu late, network <= %lu us%s%s\n", st.dry ? "DRY, " : "",
           (unsigned long)st.steps,
           (unsigned long)st.late_steps, (unsigned long)st.forward_us_max,
           st.last_stop[0] ? ", ended: " : "", st.last_stop);
    if (st.steps > 0) {
        printf("timing       tick <= %lu us (a tick is 5000 us; over 7500 it counts late), servo exchange <= %lu us%s\n",
               (unsigned long)st.tick_us_max, (unsigned long)st.exchange_us_max, st.dry ? " (dry: none)" : "");
    }
    if (st.getup_ready) {
        printf("get-up       %s; never stays down %s; last run: %lu get-ups, %lu stood\n", st.getup_name,
               st.auto_getup ? "ON" : "off", (unsigned long)st.getups, (unsigned long)st.getups_ok);
    } else {
        printf("get-up       none built in (components/mpx_policy/policies/getup.mpxp)\n");
    }
    if (st.state != MPX_POLICY_IDLE) {
        printf("live         tilt %.1f deg, hottest servo %.1f degC, gyro bias %.4f %.4f %.4f rad/s\n",
               st.tilt_deg, st.temp_max_c, st.gyro_bias[0], st.gyro_bias[1], st.gyro_bias[2]);
    }
}

/* Where the weights are, and what internal RAM is left. */
static void print_ram(void)
{
    size_t in_int = 0, total = 0;
    policy_weights_ram(&in_int, &total);
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    printf("weights      %u of %u KB in internal RAM (the rest in PSRAM, slower); internal RAM free %u KB, "
           "largest block %u KB, keep %d KB free\n",
           (unsigned)(in_int / 1024), (unsigned)(total / 1024), (unsigned)(heap_caps_get_free_size(caps) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(caps) / 1024), CONFIG_MP4_POLICY_INTERNAL_RESERVE_KB);
}

static void print_info(void)
{
    const pol_net_t *n = policy_net();
    if (!n) {
        printf("no policy loaded\n");
        return;
    }
    const mpxp_header_t *h = n->h;
    printf("name         %s (%s)\n", h->name, policy_is_builtin() ? "built in" : "from " POLICY_DIR);
    print_ram();
    printf("robot        %s (this firmware: %s)\n", h->robot, CONFIG_MP4_POLICY_ROBOT);
    printf("network      ");
    for (uint32_t l = 0; l <= h->n_layers; ++l) printf("%s%lu", l ? " -> " : "", (unsigned long)h->dims[l]);
    printf(", ELU, %s\n", (h->flags & MPXP_FLAG_OMNI) ? "omni" : "forward only");
    printf("timing       %.0f Hz policy, %.0f Hz servo tick\n", 1.0 / h->ctrl_dt, 1.0 / h->state_dt);
    printf("command max  vx %.2f  vy %.2f m/s  wz %.2f rad/s\n", h->cmd_max[0], h->cmd_max[1], h->cmd_max[2]);
    printf("limits       step %.0f deg, tilt %.0f deg, stop at %.0f degC (start below %.0f), cap %.0f mA\n",
           h->step_limit_deg, h->tilt_limit_deg, h->temp_stop_c, h->temp_start_c, h->cap_ma);
    printf("imu mount    [%g %g %g; %g %g %g; %g %g %g]\n", h->imu_R[0], h->imu_R[1], h->imu_R[2], h->imu_R[3],
           h->imu_R[4], h->imu_R[5], h->imu_R[6], h->imu_R[7], h->imu_R[8]);
    int stand[12];
    policy_stand_dd(stand);
    printf("stand dd    ");
    for (int i = 0; i < 12; ++i) printf(" %d", stand[i]);
    printf("\nservo cfg   ");
    for (uint32_t k = 0; k < h->n_cfg; ++k) printf(" p%lu=%g", (unsigned long)h->cfg[k].param_id, h->cfg[k].value);
    printf("\n");
}

static void selftest(void)
{
    if (mpx_policy_active()) {
        printf("not while a run is on (the run is using the network)\n");
        return;
    }
    const pol_net_t *n = policy_net();
    if (!n) {
        printf("no policy loaded\n");
        return;
    }
    float *work = policy_work();
    const float e = pol_selftest(n, work);
    printf("self-test: max |robot - training| %.2g over %lu fixtures (%s)\n", e,
           (unsigned long)n->h->n_fixtures, e < 1e-4f ? "ok" : "FAIL");
    float avg, worst;
    if (policy_time_forward(200, &avg, &worst)) {
        /* as a run sees it: core 1, the policy task's priority */
        printf("network: %.0f us per step, worst %.0f us (%.1f%% of the %.0f ms step)\n", avg, worst,
               avg / (n->h->ctrl_dt * 1e4), n->h->ctrl_dt * 1e3);
        print_ram();
    } else {
        printf("network: not timed (no memory for the timing task)\n");
    }
}

/* Which way the robot is lying, from the gravity direction in the body
 * frame (gravity points down: g.z = -1 upright). */
static const char *pose_words(const double g[3])
{
    const double ax = fabs(g[0]), ay = fabs(g[1]), az = fabs(g[2]);
    if (az >= ax && az >= ay) return g[2] < 0 ? "upright" : "upside down";
    if (ax >= ay) return g[0] > 0 ? "nose down" : "nose up";
    return g[1] > 0 ? "left side down" : "right side down";
}

/* The IMU through the policy's mounting: what the network will be told. */
static void show_imu(float seconds)
{
    if (!policy_net()) {
        printf("no policy loaded (its contract holds the IMU mounting)\n");
        return;
    }
    printf("body frame: x forward, y left, z up. Upright and still: gravity (0, 0, -1), tilt 0.\n"
           "nose down: gravity x > 0; left side down: gravity y > 0; turn left (yaw): gyro z > 0\n");
    const int64_t end = esp_timer_get_time() + (int64_t)(seconds * 1e6f);
    do {
        double g[3], w[3], tilt;
        if (policy_imu_now(g, w, &tilt)) {
            printf("gravity %+.3f %+.3f %+.3f   gyro %+.3f %+.3f %+.3f rad/s   tilt %5.1f deg  (%s)\n", g[0],
                   g[1], g[2], w[0], w[1], w[2], tilt, pose_words(g));
        } else {
            printf("no IMU sample\n");
        }
        if (seconds > 0.0f) vTaskDelay(pdMS_TO_TICKS(250));
    } while (esp_timer_get_time() < end);
}

/* The last policy step: what went in, what came out. */
static void show_obs(void)
{
    float obs[45], raw[12];
    int dd[12];
    if (!policy_last_step(obs, raw, dd)) {
        printf("no policy step yet (`policy run ... dry` makes some without servos)\n");
        return;
    }
    static const char *const part[] = { "gyro     ", "gravity  ", "command  " };
    for (int k = 0; k < 3; ++k) printf("%s %+.3f %+.3f %+.3f\n", part[k], obs[3 * k], obs[3 * k + 1], obs[3 * k + 2]);
    printf("servo id ");
    for (int i = 0; i < 12; ++i) printf(" %6d", i + 1);
    printf("\n");
    static const char *const rows[] = { "joint rad", "joint vel", "prev act " };
    for (int r = 0; r < 3; ++r) {
        printf("%s", rows[r]);
        for (int i = 0; i < 12; ++i) printf(" %+6.3f", obs[9 + 12 * r + i]);
        printf("\n");
    }
    printf("action   ");
    for (int i = 0; i < 12; ++i) printf(" %+6.3f", raw[i]);
    printf("\nservo dd ");
    for (int i = 0; i < 12; ++i) printf(" %6d", dd[i]);
    printf("\n");
}

static void list_files(void)
{
    DIR *d = opendir(POLICY_DIR);
    printf("built in\n");
    if (!d) {
        printf("(no %s folder)\n", POLICY_DIR);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const size_t n = strlen(e->d_name);
        if (n > 5 && strcasecmp(e->d_name + n - 5, ".mpxp") == 0) printf("%.*s\n", (int)(n - 5), e->d_name);
    }
    closedir(d);
}

static void save_choice(const char *path)
{
    nvs_handle_t h;
    if (nvs_open("policy", NVS_READWRITE, &h) != ESP_OK) return;
    if (path) nvs_set_str(h, "file", path);
    else nvs_erase_key(h, "file");
    nvs_commit(h);
    nvs_close(h);
}

static int usage(void)
{
    printf("usage: policy [status]          what is happening\n"
           "       policy info              the loaded policy\n"
           "       policy test              self-test and timing\n"
           "       policy run <seconds> [vx vy wz] [hold <s>] [force] [dry]\n"
           "                                  seconds 0 = until `policy stop`; hold = stand first (default 2 s)\n"
           "                                  dry: no servo is touched (IMU, network and timing only)\n"
           "       policy imu [seconds]     the IMU as the policy sees it (mounting check)\n"
           "       policy obs               the last step: observation, action, servo targets\n"
           "       policy log on|off|print  record the next runs step by step / print the last one (CSV)\n"
           "       policy getup [force]     get up from wherever it lies (the get-up policy), then stand\n"
           "       policy autoup on|off     never stays down: a walk that falls gets up and walks on\n"
           "       policy stop              ramp to the stand, hold, give the robot back\n"
           "       policy cmd <vx> <vy> <wz>  change the command while it runs (m/s, m/s, rad/s)\n"
           "       policy offsets robot|contract\n"
           "       policy list | load <name> | load builtin   (files in " POLICY_DIR ")\n");
    return 1;
}

/* A number, all of it: "abc" or "1x" is not 0. */
static bool num(const char *s, float *out)
{
    char *end = NULL;
    const float v = strtof(s, &end);
    if (end == s || *end != '\0' || !isfinite(v)) return false;
    *out = v;
    return true;
}

static int policy_cmd(int argc, char **argv)
{
    if (argc <= 1 || strcmp(argv[1], "status") == 0) {
        print_status();
        return 0;
    }
    const char *c = argv[1];
    char why[96] = "";
    if (strcmp(c, "info") == 0) {
        print_info();
    } else if (strcmp(c, "test") == 0) {
        selftest();
    } else if (strcmp(c, "run") == 0) {
        float seconds, hold = 2.0f, cmd[3] = { 0, 0, 0 };
        if (argc < 3 || !num(argv[2], &seconds)) return usage();
        bool force = false, dry = false;
        int ncmd = 0;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "force") == 0) {
                force = true;
            } else if (strcmp(argv[i], "dry") == 0) {
                dry = true;
            } else if (strcmp(argv[i], "hold") == 0 && i + 1 < argc) {
                if (!num(argv[++i], &hold)) return usage();
            } else if (ncmd < 3 && num(argv[i], &cmd[ncmd])) {
                ++ncmd;
            } else {
                return usage();
            }
        }
        mpx_policy_set_deadman_ms(0);   /* the console stays: `policy stop` ends it */
        if (mpx_policy_start(seconds, hold, force, dry, why, sizeof(why)) != ESP_OK) {
            printf("refused: %s\n", why);
            return 1;
        }
        mpx_policy_set_cmd(cmd[0], cmd[1], cmd[2]);   /* only once accepted: RUN is >= 1 s away */
        if (dry) {
            printf("DRY run: no servo is touched. Gyro bias 1 s (keep the board still), then the policy %s;\n"
                   "the joints are taken to be at the stand. `policy` for steps and timing, `policy obs` for\n"
                   "the last step\n", seconds > 0 ? "for the given time" : "until `policy stop`");
            return 0;
        }
        printf("starting: gyro bias 1 s (keep the robot still), ramp 2 s, hold %.1f s, then the policy %s\n"
               "`policy stop` ends it; the robot stops by itself on a tilt, a hot or overloaded servo, "
               "or an IMU dropout\n", hold, seconds > 0 ? "for the given time" : "until `policy stop`");
    } else if (strcmp(c, "imu") == 0) {
        float sec = 0.0f;
        if (argc >= 3 && (!num(argv[2], &sec) || sec < 0.0f || sec > 600.0f)) return usage();
        show_imu(sec);
    } else if (strcmp(c, "obs") == 0) {
        show_obs();
    } else if (strcmp(c, "log") == 0) {
        if (argc < 3) return usage();
        if (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0) {
            mpx_policy_set_log(strcmp(argv[2], "on") == 0);
            printf("run log %s for the next runs\n", argv[2]);
        } else if (strcmp(argv[2], "print") == 0) {
            if (mpx_policy_active()) {
                printf("not while a run is on\n");
                return 1;
            }
            size_t dropped = 0;
            const size_t n = mpx_policy_log_count(&dropped);
            char line[200];
            printf("%s\n", mpx_policy_log_header());
            for (size_t i = 0; i < n && mpx_policy_log_row(i, line, sizeof(line)); ++i) printf("%s\n", line);
            printf("# %u steps logged, %u dropped\n", (unsigned)n, (unsigned)dropped);
        } else {
            return usage();
        }
    } else if (strcmp(c, "getup") == 0) {
        const bool force = argc >= 3 && strcmp(argv[2], "force") == 0;
        if (argc >= 3 && !force) return usage();
        mpx_policy_set_deadman_ms(0);
        if (mpx_policy_getup(force, why, sizeof(why)) != ESP_OK) {
            printf("refused: %s\n", why);
            return 1;
        }
        printf("getting up: gyro bias 1 s (keep it still), then the get-up policy for up to 10 s; standing, it is\n"
               "given back. `policy stop` ends it\n");
    } else if (strcmp(c, "autoup") == 0) {
        if (argc < 3 || (strcmp(argv[2], "on") && strcmp(argv[2], "off"))) return usage();
        if (!mpx_policy_set_auto_getup(strcmp(argv[2], "on") == 0)) {
            printf("no get-up policy is built in\n");
            return 1;
        }
        printf("never stays down %s for the next runs\n", argv[2]);
    } else if (strcmp(c, "stop") == 0) {
        mpx_policy_stop("policy stop");
    } else if (strcmp(c, "cmd") == 0) {
        float c3[3];
        if (argc < 5 || !num(argv[2], &c3[0]) || !num(argv[3], &c3[1]) || !num(argv[4], &c3[2])) return usage();
        mpx_policy_set_cmd(c3[0], c3[1], c3[2]);
        print_status();
    } else if (strcmp(c, "offsets") == 0) {
        if (argc < 3 || (strcmp(argv[2], "robot") && strcmp(argv[2], "contract"))) return usage();
        if (mpx_policy_active()) {
            printf("not while a run is on\n");
            return 1;
        }
        policy_set_robot_offsets(strcmp(argv[2], "robot") == 0);
        print_info();
    } else if (strcmp(c, "list") == 0) {
        list_files();
    } else if (strcmp(c, "load") == 0) {
        if (argc < 3) return usage();
        esp_err_t e;
        if (strcmp(argv[2], "builtin") == 0) {
            e = policy_load_builtin(why, sizeof(why));
            if (e == ESP_OK) save_choice(NULL);
        } else {
            char path[96];
            snprintf(path, sizeof(path), POLICY_DIR "/%s%s", argv[2],
                     strstr(argv[2], ".mpxp") ? "" : ".mpxp");
            e = policy_load_file(path, why, sizeof(why));
            if (e == ESP_OK) save_choice(path);
        }
        if (e != ESP_OK) {
            printf("not loaded: %s\n", why);
            return 1;
        }
        print_info();
    } else {
        return usage();
    }
    return 0;
}

void mpx_policy_register_console(void)
{
    const esp_console_cmd_t cmd = {
        .command = "policy",
        .help = "RL walk policy on the robot: policy [status|info|test|imu|run|getup|autoup|obs|stop|cmd|offsets|"
                "list|load]",
        .hint = NULL,
        .func = &policy_cmd,
    };
    if (esp_console_cmd_register(&cmd) != ESP_OK) ESP_LOGW(TAG, "`policy` command not registered");
}

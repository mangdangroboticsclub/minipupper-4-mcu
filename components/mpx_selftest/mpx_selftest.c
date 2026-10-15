/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * `selftest` -- does the software agree with the hardware?
 *
 * Every pin in boards/mp4_esp32_core/ was read off a schematic and has never
 * carried a signal. This command is how you find out which of those readings
 * were right, in one pass, on a bench, before anything depends on them.
 *
 * ── It does not move the servos ───────────────────────────────────────────
 *
 * That is the point of running it first. MP4_ROBOT_SERVO_BOARD_VARIANT decides
 * which physical joint a logical servo id drives, nothing validates it, and a
 * wrong variant drives the wrong joint -- which on an assembled robot means
 * legs folding the wrong way against their stops. So the servo check only
 * PINGS: it proves the SPI bus, the four chip selects and the driver boards
 * are wired as declared, and moves nothing.
 *
 * Run it with the servo rail unpowered. Everything here works without it.
 *
 * ── Reading the result ────────────────────────────────────────────────────
 *
 *   PASS   the hardware answered as the board files say it should
 *   FAIL   it answered, but not with what was expected -- a wiring or config
 *          mismatch, and the line says which
 *   SKIP   could not be tested here (a subsystem is off, or needs your eyes)
 *   LOOK   nothing is wrong; the panel is showing you something to judge
 *
 * A FAIL is a fact. A SKIP is not a pass -- it means this command could not
 * answer the question and something else has to.
 */

#include "mpx_selftest.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_board_device.h"
#include "esp_board_periph.h"
/* Only on the include path when the board declares a `ledc` peripheral. A
 * board without one still has to build; the backlight check reports SKIP. */
#if defined(__has_include)
#  if __has_include("periph_ledc.h")
#    include "periph_ledc.h"
#    define SELFTEST_HAS_LEDC 1
#  endif
#endif

#include "claw_paths.h"

#if CONFIG_MP4_ROBOT_ENABLE
#include "mpx_robot.h"
#endif
#include "cap_display.h"

static const char *TAG = "selftest";

/* What the board files claim is on the I2C bus. Both are on IO1/IO2 per
 * schematic sheets 2 and 3. The ES7210's 0x41 is the 7-bit form of the 0x82
 * write address the codec driver is configured with. */
#define SELFTEST_I2C_BMI270   0x68
#define SELFTEST_I2C_ES7210   0x41

#define SELFTEST_BTN_BOOT     0
#define SELFTEST_BTN_WAKE     21

/* Gravity should read 1.0 g on a still robot. A tolerance this wide passes a
 * sensor that is merely uncalibrated and fails one that is misconfigured --
 * a wrong full-scale range is off by a factor of two or more, not by 15%. */
#define SELFTEST_G_TOLERANCE  0.15f

static int s_pass, s_fail, s_skip, s_warn;

/* Set by --other-board. The firmware-side checks (partitions, mounts, RAM)
 * are true of any board this is flashed to, so they stay real. The
 * hardware-side ones describe MP4 wiring specifically, and on a different
 * board their failing is the expected answer, not a fault -- reporting them
 * as FAIL would drown the result that was actually being looked for. */
static bool s_other_board;

/* ── Result reporting ──────────────────────────────────────────────────────
 *
 * printf, not ESP_LOGI: this is a report a person reads on a serial monitor,
 * and log level, colour and timestamps get in the way of a results table. */

static void r_pass(const char *what, const char *detail)
{
    s_pass++;
    printf("  PASS  %-22s %s\n", what, detail ? detail : "");
}

static void r_fail(const char *what, const char *detail)
{
    s_fail++;
    printf("  FAIL  %-22s %s\n", what, detail ? detail : "");
}

static void r_skip(const char *what, const char *why)
{
    s_skip++;
    printf("  SKIP  %-22s %s\n", what, why ? why : "");
}

static void r_warn(const char *what, const char *detail)
{
    s_warn++;
    printf("  WARN  %-22s %s\n", what, detail ? detail : "");
}

/* A check that can only pass on MP4 hardware. */
static void r_board(const char *what, const char *detail)
{
    if (s_other_board) {
        r_warn(what, detail);
    } else {
        r_fail(what, detail);
    }
}

static void r_look(const char *what, const char *detail)
{
    printf("  LOOK  %-22s %s\n", what, detail ? detail : "");
}

static void section(const char *name)
{
    printf("\n%s\n", name);
}

/* ── System ────────────────────────────────────────────────────────────────
 *
 * Cheap, and it catches the most embarrassing failure mode: a board flashed
 * with firmware but not with its data partitions, which then fails later in
 * ways that look like bugs. */

static void test_system(void)
{
    section("System");

    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    char detail[96];

    if (psram >= 4u * 1024 * 1024) {
        snprintf(detail, sizeof(detail), "%u MB total, %u KB free",
                 (unsigned)(psram / (1024 * 1024)), (unsigned)(psram_free / 1024));
        r_pass("PSRAM", detail);
    } else {
        snprintf(detail, sizeof(detail), "expected 8 MB, found %u KB -- octal PSRAM not configured?",
                 (unsigned)(psram / 1024));
        r_board("PSRAM", detail);
    }

    snprintf(detail, sizeof(detail), "%u KB free", (unsigned)(internal_free / 1024));
    r_pass("Internal RAM", detail);

    /* The partitions the firmware cannot work without. */
    static const char *const parts[] = { "system", "storage", "model" };
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        const esp_partition_t *p = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, parts[i]);
        char label[32];
        snprintf(label, sizeof(label), "Partition '%s'", parts[i]);
        if (p) {
            snprintf(detail, sizeof(detail), "at 0x%06" PRIX32 ", %u KB",
                     (uint32_t)p->address, (unsigned)(p->size / 1024));
            r_pass(label, detail);
        } else {
            r_fail(label, "not in the partition table");
        }
    }

    /* Mounted, not merely present. */
    const char *roots[2] = { claw_paths_get(CLAW_PATH_SYSTEM), claw_paths_get(CLAW_PATH_DATA) };
    const char *names[2] = { "SYSTEM mount", "DATA mount" };
    for (int i = 0; i < 2; i++) {
        uint64_t total = 0, freeb = 0;
        if (!roots[i]) {
            r_fail(names[i], "claw_paths has no root for it");
            continue;
        }
        if (esp_vfs_fat_info(roots[i], &total, &freeb) == ESP_OK) {
            snprintf(detail, sizeof(detail), "%s  %u KB free of %u KB",
                     roots[i], (unsigned)(freeb / 1024), (unsigned)(total / 1024));
            r_pass(names[i], detail);
        } else {
            r_fail(names[i], roots[i]);
        }
    }
}

/* ── I2C ───────────────────────────────────────────────────────────────────
 *
 * The single most informative test on the board. Probing every address tells
 * you at once whether SDA/SCL are on the pins the board file claims, whether
 * the pull-ups are doing their job, and whether each chip is strapped to the
 * address that was read off the schematic. */

static void test_i2c(void)
{
    section("I2C bus (SDA 1, SCL 2)");

    i2c_master_bus_handle_t bus = NULL;
    if (esp_board_periph_ref_handle("i2c_master", (void **)&bus) != ESP_OK || !bus) {
        r_skip("I2C bus", "board peripheral 'i2c_master' not available");
        return;
    }

    /* The driver logs an error for every address that does not answer. On a
     * board with nothing on the bus that is 112 identical error lines, which
     * buries the report this command exists to print. Silence it for the scan
     * and put the level back afterwards. */
    esp_log_level_t prev = esp_log_level_get("i2c.master");
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    bool found_bmi = false, found_es = false, found_69 = false;
    int others = 0, answered = 0;
    char extra[96] = {0};
    size_t at = 0;

    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        /* A short timeout: a device that is there acknowledges immediately,
         * and one that is not costs this whole budget. At 200 ms -- the
         * driver's default -- a silent bus takes 22 seconds and trips the
         * task watchdog. */
        if (i2c_master_probe(bus, addr, 20) != ESP_OK) {
            if ((addr & 0x0F) == 0x0F) {
                vTaskDelay(1);   /* let the watchdog and the console breathe */
            }
            continue;
        }
        answered++;
        if (addr == SELFTEST_I2C_BMI270) {
            found_bmi = true;
        } else if (addr == 0x69) {
            found_69 = true;
        } else if (addr == SELFTEST_I2C_ES7210) {
            found_es = true;
        } else if (at + 6 < sizeof(extra)) {
            at += (size_t)snprintf(extra + at, sizeof(extra) - at, "%s0x%02X", others ? " " : "", addr);
            others++;
        }
    }
    esp_log_level_set("i2c.master", prev);
    esp_board_periph_unref_handle("i2c_master");

    /* Nothing at all is one finding, not two failures. It means the bus is
     * silent -- no devices, no pull-ups, or the wrong pins -- and saying that
     * once is more use than reporting each expected chip as missing. */
    if (answered == 0) {
        r_board("I2C bus", "no device answered at any address");
        printf("        Either nothing is attached, the pull-ups are absent, or SDA/SCL\n"
               "        are not on GPIO 1 and 2. Expected if this is not the MP4 board.\n");
        return;
    }

    if (found_bmi) {
        r_pass("BMI270 @ 0x68", "IMU answered on the primary address");
    } else if (found_69) {
        /* Worth naming: this is the reading I got wrong twice. */
        r_board("BMI270 @ 0x68",
                "silent, but something answered at 0x69 -- SDO is strapped high, "
                "set i2c_addr: 0x69 in board_devices.yaml");
    } else {
        r_board("BMI270 @ 0x68", "no answer, though other devices are on the bus");
    }

    if (found_es) {
        r_pass("ES7210 @ 0x41", "audio ADC answered");
    } else {
        r_board("ES7210 @ 0x41", "no answer -- microphones will not work");
    }

    if (others > 0) {
        r_look("Other devices", extra);
    }
}

/* ── IMU ───────────────────────────────────────────────────────────────────
 *
 * Answering on I2C only proves the chip is there. This proves it is
 * configured: on a still robot the accelerometer must measure 1 g, and it can
 * only do that if the full-scale range matches what the driver believes. */

static void test_imu(void)
{
#if CONFIG_MP4_ROBOT_ENABLE
    section("IMU");

    if (!mpx_robot_ready()) {
        r_skip("IMU", "mpx_robot did not start -- see the boot log");
        return;
    }

    mpx_robot_imu_t s = {0};
    mpx_robot_imu_read(&s);

    float mag = sqrtf(s.ax * s.ax + s.ay * s.ay + s.az * s.az);
    float spin = fabsf(s.gx) + fabsf(s.gy) + fabsf(s.gz);
    char detail[128];

    snprintf(detail, sizeof(detail), "a=(%+.2f %+.2f %+.2f) g  |a|=%.2f", s.ax, s.ay, s.az, mag);
    if (mag > 0.01f && fabsf(mag - 1.0f) <= SELFTEST_G_TOLERANCE) {
        r_pass("Accelerometer", detail);
    } else if (mag <= 0.01f) {
        r_board("Accelerometer", "reads zero on every axis -- not sampling");
    } else {
        r_board("Accelerometer", detail);
        printf("        |a| should be 1.00 g at rest. Far off usually means the "
               "full-scale range does not match the driver.\n");
    }

    snprintf(detail, sizeof(detail), "g=(%+.1f %+.1f %+.1f) dps", s.gx, s.gy, s.gz);
    if (spin < 15.0f) {
        r_pass("Gyroscope", detail);
    } else {
        r_board("Gyroscope", detail);
        printf("        Expected near zero on a stationary robot. Hold it still and retry.\n");
    }

    /* Which axis holds gravity tells you how the sensor is mounted, which is
     * the thing the gait's tilt correction depends on. Nobody can check that
     * from a schematic. */
    const char *axis = fabsf(s.az) > 0.7f ? "Z" : (fabsf(s.ay) > 0.7f ? "Y" : (fabsf(s.ax) > 0.7f ? "X" : "none"));
    snprintf(detail, sizeof(detail), "gravity is on %s -- expect Z on a level robot", axis);
    r_look("Orientation", detail);

    /* The BMM150 hangs off the BMI270's aux bus, so "answers on I2C" above
     * says nothing about it; the only proof is a plausible field. Earth's
     * is 25-65 uT depending on where you are; a chip reading zero is not
     * being polled, one reading hundreds has a magnet next to it. */
    if (s.mag_valid) {
        const float field = sqrtf(s.mx * s.mx + s.my * s.my + s.mz * s.mz);
        snprintf(detail, sizeof(detail), "m=(%+.0f %+.0f %+.0f) uT  |m|=%.0f", s.mx, s.my, s.mz, field);
        if (field >= 15.0f && field <= 150.0f) {
            r_pass("Magnetometer", detail);
        } else {
            r_board("Magnetometer", detail);
            printf("        Earth's field is 25-65 uT. Much more means something magnetic is "
                   "close (a speaker?); zero means the aux bus is not delivering.\n");
        }
    } else if (mpx_robot_mag_ready()) {
        r_board("Magnetometer", "BMM150 came up but no sample has arrived yet");
    } else {
        r_board("Magnetometer", "BMM150 not found behind the BMI270 -- see the boot log");
    }
#else
    r_skip("IMU", "MP4_ROBOT_ENABLE is off");
#endif
}

/* ── Servo bus ─────────────────────────────────────────────────────────────
 *
 * Read-only. Ping every servo id and report which answered, grouped by the
 * driver board that owns it, so a dead chip select shows up as a whole board
 * of three going quiet rather than as scattered failures. */

static void test_servo(void)
{
#if CONFIG_MP4_ROBOT_ENABLE
    section("Servo bus (SPI3: MOSI 6, CLK 16, MISO 17)");

    if (!mpx_robot_ready()) {
        r_skip("Servo bus", "mpx_robot did not start -- see the boot log");
        return;
    }

    static const struct { const char *conn; int cs; int first; } boards[4] = {
        { "CN3", 15, 1 }, { "CN4", 7, 4 }, { "CN5", 4, 7 }, { "CN6", 5, 10 },
    };
    int total = 0;

    for (int b = 0; b < 4; b++) {
        /* Three ids at most, so "10 11 12" -- nine bytes with the NUL. Sized
         * to that rather than generously, because the compiler reasons about
         * the declared size, not the reachable content: a roomy buffer here
         * makes the worst case below overflow `part` and fail the build. */
        char label[32], detail[16];
        int alive = 0;
        size_t at = 0;

        detail[0] = '\0';
        for (int i = 0; i < 3; i++) {
            int id = boards[b].first + i;
            if (mpx_robot_ping_servo(id) > 0) {
                alive++;
                total++;
                at += (size_t)snprintf(detail + at, sizeof(detail) - at, "%s%d", at ? " " : "", id);
            }
        }
        snprintf(label, sizeof(label), "%s (CS GPIO %d)", boards[b].conn, boards[b].cs);
        if (alive == 3) {
            r_pass(label, "servos ready");
        } else if (alive > 0) {
            char part[128];
            snprintf(part, sizeof(part), "only %d of 3 answered (ids %s)", alive, detail);
            r_board(label, part);
        } else {
            r_board(label, "no servo answered");
        }
    }

    if (total == 0) {
        printf("\n  Nothing answered at all. If the servo rail is unpowered this is\n"
               "  EXPECTED and not a fault -- it is the safe way to run this test.\n"
               "  Power the servos and rerun to check the bus properly.\n");
    } else if (total < 12) {
        printf("\n  A whole connector silent usually means its chip select, not its\n"
               "  servos. All four share MOSI, CLK and MISO, so if one board answers\n"
               "  the bus itself is fine.\n");
    }
#else
    r_skip("Servo bus", "MP4_ROBOT_ENABLE is off");
#endif
}

/* ── Backlight ─────────────────────────────────────────────────────────────
 *
 * The board declares output_invert because the backlight FET is P-channel
 * (Q2, SI2301). If that is wrong the panel is brightest at 0%, which this
 * ramp makes obvious in three seconds. */

static void test_backlight(void)
{
    section("Backlight (LEDC on GPIO 42)");

#if !SELFTEST_HAS_LEDC
    r_skip("Backlight", "no LEDC peripheral on this board");
#else
    void *handle = NULL;
    if (esp_board_device_get_handle("lcd_brightness", &handle) != ESP_OK || !handle) {
        r_skip("Backlight", "device 'lcd_brightness' not available");
        return;
    }
    periph_ledc_handle_t *ledc = (periph_ledc_handle_t *)handle;
    const uint32_t max = (1u << LEDC_TIMER_10_BIT) - 1u;

    printf("        ramping 0 -> 100 -> 80 %%...\n");
    for (int pct = 0; pct <= 100; pct += 5) {
        ledc_set_duty(ledc->speed_mode, ledc->channel, max * (uint32_t)pct / 100u);
        ledc_update_duty(ledc->speed_mode, ledc->channel);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    ledc_set_duty(ledc->speed_mode, ledc->channel, max * 80u / 100u);
    ledc_update_duty(ledc->speed_mode, ledc->channel);

    r_look("Backlight", "did it go dark to bright? if it went bright to dark, "
                        "output_invert is wrong");
#endif /* SELFTEST_HAS_LEDC */
}

/* ── Display ───────────────────────────────────────────────────────────────
 *
 * The three values in board_devices.yaml that a schematic cannot settle --
 * mirror_x, swap_xy and invert_color -- are all decided by looking at the
 * panel. So this draws something whose correct appearance is unambiguous. */

static void test_display(uint32_t hold_ms)
{
    section("Display (ST7789, SPI2)");

    esp_err_t err = cap_display_show_test_pattern(hold_ms);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        r_skip("Display", "no panel on this board");
        return;
    }
    if (err != ESP_OK) {
        r_fail("Display", esp_err_to_name(err));
        return;
    }

    r_look("Test pattern", "check all four points below");
    printf("        1. corner labels read TL TR BL BR, in those corners\n");
    printf("           wrong corners  -> mirror_x / mirror_y\n");
    printf("           rotated 90 deg -> swap_xy\n");
    printf("        2. the bars are RED, GREEN, BLUE in that order\n");
    printf("           inverted colours -> invert_color\n");
    printf("        3. no tearing, noise or wrong-coloured pixels\n");
    printf("           speckle -> lower pclk_hz from 80 MHz to 40 MHz\n");
    printf("           garbage -> try a different spi_mode\n");
    printf("        4. the frame touches all four edges with no offset band\n");
}

/* ── Buttons ───────────────────────────────────────────────────────────────
 *
 * Both are to ground, so a press reads low. Sampling rather than asking the
 * board manager, because nothing in the firmware reads these yet. */

static void test_buttons(uint32_t window_ms)
{
    section("Buttons");

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << SELFTEST_BTN_BOOT) | (1ULL << SELFTEST_BTN_WAKE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        r_skip("Buttons", "could not configure GPIO 0 and 21");
        return;
    }

    printf("        press BOOT and WAKE now (%u s)...\n", (unsigned)(window_ms / 1000));
    bool boot_seen = false, wake_seen = false;
    int64_t deadline = esp_timer_get_time() + (int64_t)window_ms * 1000;
    while (esp_timer_get_time() < deadline && !(boot_seen && wake_seen)) {
        if (gpio_get_level(SELFTEST_BTN_BOOT) == 0) {
            boot_seen = true;
        }
        if (gpio_get_level(SELFTEST_BTN_WAKE) == 0) {
            wake_seen = true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    boot_seen ? r_pass("BOOT (GPIO 0)", "press detected")
              : r_skip("BOOT (GPIO 0)", "no press seen -- untested, not failed");
    wake_seen ? r_pass("WAKE (GPIO 21)", "press detected")
              : r_skip("WAKE (GPIO 21)", "no press seen -- untested, not failed");
}

/* ── The command ───────────────────────────────────────────────────────── */

static struct {
    struct arg_lit *system_;
    struct arg_lit *i2c;
    struct arg_lit *imu;
    struct arg_lit *servo;
    struct arg_lit *display;
    struct arg_lit *backlight;
    struct arg_lit *buttons;
    struct arg_int *hold;
    struct arg_lit *other;
    struct arg_end *end;
} s_args;

static int selftest_cmd(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_args.end, argv[0]);
        return 1;
    }

    s_other_board = s_args.other->count > 0;

    bool any = s_args.system_->count || s_args.i2c->count || s_args.imu->count ||
               s_args.servo->count || s_args.display->count ||
               s_args.backlight->count || s_args.buttons->count;
    bool all = !any;
    uint32_t hold = s_args.hold->count ? (uint32_t)s_args.hold->ival[0] * 1000u : 10000u;

    s_pass = s_fail = s_skip = s_warn = 0;
    printf("\nMP4 ESP32 CORE self-test\n");
    if (s_other_board) {
        printf("\n  ** --other-board: this is NOT MP4 hardware **\n"
               "  Checks that describe MP4 wiring report WARN instead of FAIL.\n"
               "  A WARN here says nothing about the firmware. What is being\n"
               "  tested is whether ESP-Claw itself came up: partitions, mounts,\n"
               "  RAM, console. Those still report PASS or FAIL for real.\n\n");
    } else {
        printf("Servo rail should be UNPOWERED for a first run.\n");
    }

    if (all || s_args.system_->count)   { test_system(); }
    if (all || s_args.i2c->count)       { test_i2c(); }
    if (all || s_args.imu->count)       { test_imu(); }
    if (all || s_args.servo->count)     { test_servo(); }
    if (all || s_args.backlight->count) { test_backlight(); }
    if (all || s_args.display->count)   { test_display(hold); }
    if (all || s_args.buttons->count)   { test_buttons(8000); }

    printf("\n%d passed, %d failed, %d warned, %d skipped\n",
           s_pass, s_fail, s_warn, s_skip);
    if (s_fail == 0 && s_warn == 0) {
        printf("No mismatches found. LOOK items still need your eyes.\n\n");
    } else if (s_fail == 0) {
        printf("Nothing the firmware controls is wrong. The WARN lines are MP4\n"
               "hardware that is not on this board -- expected with --other-board.\n\n");
    } else {
        printf("Each FAIL names what was expected -- board files, wiring, or both.\n\n");
    }
    return s_fail == 0 ? 0 : 1;
}


/* ── `imu` and `button`: live views for bring-up ──────────────────────────
 *
 * `selftest --imu` is a single snapshot with a pass/fail; these two are for
 * watching the hardware while you handle it -- the M5CoreS3IMU style of
 * bring-up, minus the screen: tilt the board and watch pitch/roll follow,
 * press a button and see the edge with a timestamp. */

#define IMU_WATCH_DEFAULT_S    10
#define IMU_WATCH_DEFAULT_HZ   5
#define IMU_CAL_DEFAULT_S      20
#define BTN_WATCH_DEFAULT_S    15
#define RAD2DEG(x)             ((x) * 57.2957795f)

#if CONFIG_MP4_ROBOT_ENABLE
/* Hard-iron offset for the compass, from `imu cal`. RAM only: this is a
 * bring-up aid, and the offset changes with anything magnetic near the board
 * -- the MAX98357A's speaker magnet is centimetres from the BMM150, so the
 * heading without it will be biased, sometimes badly. */
static float s_mag_off[3] = {0, 0, 0};
static bool  s_mag_cal    = false;

/* Tilt-compensated compass heading from the magnetometer, in degrees
 * clockwise from magnetic north with the sensor's +X as "forward".
 *
 * Gravity gives the up vector; the magnetic vector projected onto the plane
 * perpendicular to it is where north is, and the sensor's X axis projected
 * onto the same plane is where the board points. This works at any tilt as
 * long as the board is not accelerating, and assumes the BMM150 and BMI270
 * axes agree -- they are separate chips and may be mounted differently, so
 * check it: a full slow turn on the spot must sweep 0..360. Returns a
 * negative value when the field is too weak to trust (no chip, or saturated
 * next to a magnet). */
static float imu_heading_deg(const mpx_robot_imu_t *s, float *field_ut)
{
    float m[3] = { s->mx - s_mag_off[0], s->my - s_mag_off[1], s->mz - s_mag_off[2] };
    float u[3] = { s->ax, s->ay, s->az };

    const float mn = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    const float un = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (field_ut) *field_ut = mn;
    if (!s->mag_valid || mn < 5.0f || un < 0.3f) {
        return -1.0f;
    }
    for (int i = 0; i < 3; i++) u[i] /= un;

    /* forward = X with its vertical part removed; left = up x forward */
    float f[3] = { 1.0f - u[0] * u[0], -u[0] * u[1], -u[0] * u[2] };
    const float fn = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fn < 0.05f) {
        return -1.0f;   /* X is pointing straight up or down: no heading */
    }
    for (int i = 0; i < 3; i++) f[i] /= fn;
    const float l[3] = { u[1] * f[2] - u[2] * f[1],
                         u[2] * f[0] - u[0] * f[2],
                         u[0] * f[1] - u[1] * f[0] };

    const float mf = m[0] * f[0] + m[1] * f[1] + m[2] * f[2];
    const float ml = m[0] * l[0] + m[1] * l[1] + m[2] * l[2];
    float h = RAD2DEG(atan2f(ml, mf));
    if (h < 0) h += 360.0f;
    return h;
}
#endif

static void imu_print_sample(uint32_t t_ms)
{
#if CONFIG_MP4_ROBOT_ENABLE
    mpx_robot_imu_t s = {0};
    mpx_robot_imu_read(&s);

    const float mag = sqrtf(s.ax * s.ax + s.ay * s.ay + s.az * s.az);
    /* Accelerometer-only attitude: fine for a still or slowly moving body,
     * meaningless while it is being shaken -- which the |a| column shows. */
    const float pitch = RAD2DEG(atan2f(-s.ax, sqrtf(s.ay * s.ay + s.az * s.az)));
    const float roll  = RAD2DEG(atan2f(s.ay, s.az));
    const float spin  = fabsf(s.gx) + fabsf(s.gy) + fabsf(s.gz);

    const char *state;
    if (fabsf(mag - 1.0f) > 0.5f) {
        state = "SHAKE";
    } else if (spin > 60.0f) {
        state = "turning";
    } else if (s.az < -0.7f) {
        state = "UPSIDE DOWN";
    } else if (fabsf(pitch) > 30.0f) {
        state = pitch > 0 ? "nose up" : "nose down";
    } else if (fabsf(roll) > 30.0f) {
        state = roll > 0 ? "roll right" : "roll left";
    } else {
        state = spin > 15.0f ? "moving" : "level, still";
    }

    printf("  %6" PRIu32 " ms  a %+5.2f %+5.2f %+5.2f g |a| %4.2f   g %+7.1f %+7.1f %+7.1f dps"
           "   pitch %+6.1f roll %+6.1f",
           t_ms, s.ax, s.ay, s.az, mag, s.gx, s.gy, s.gz, pitch, roll);

    if (s.mag_valid) {
        float field = 0;
        const float heading = imu_heading_deg(&s, &field);
        printf("   m %+4.0f %+4.0f %+4.0f uT |m| %3.0f", s.mx, s.my, s.mz, field);
        if (heading >= 0) {
            static const char *const dirs[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
            printf("  hdg %5.1f %-2s%s", heading, dirs[((int)(heading + 22.5f) / 45) % 8],
                   s_mag_cal ? "" : "*");
        } else {
            printf("  hdg  ---   ");
        }
    } else if (mpx_robot_mag_ready()) {
        printf("   m  (waiting)");
    } else {
        printf("   m  (no BMM150)");
    }
    printf("   %s\n", state);
#else
    (void)t_ms;
    printf("  MP4_ROBOT_ENABLE is off -- no IMU driver in this build\n");
#endif
}

#if CONFIG_MP4_ROBOT_ENABLE
/* ── `imu orient`: which way is the chip mounted? ──────────────────────────
 *
 * One command, no hand-holding: the robot stands, measures gravity, tilts
 * its own body nose-down and then right-side-down with its legs, and reads
 * which chip axis moved which way. From that it prints, in plain words, which
 * axes are reversed, and the two lines the Pi's ROS bridge needs
 * (imu_axis_map / imu_axis_sign, REP-103: x forward, y left, z up):
 *
 *     body[i] = imu_axis_sign[i] * chip[imu_axis_map[i]]
 *
 * Physics used: the accelerometer reads +1 g on the axis pointing UP. Tilt
 * the nose down and the forward axis starts pointing down, so its reading
 * goes NEGATIVE; tilt the right side down and the left axis starts pointing
 * up, so its reading goes POSITIVE. The two tilts are cross-checked against
 * each other with the right-hand rule (forward x left must equal up).
 *
 * Tilt directions come from the gait code's own convention (firmware degrees:
 * pitch + = nose UP, roll + = right side DOWN; see robot.cc
 * set_body_attitude(rad) and the look moves). If the servos are off, the
 * robot cannot tilt itself; the command then asks for the same tilts by hand. */

#define ORIENT_TILT_DEG     14.0f
#define ORIENT_MIN_DELTA_G  0.10f

static void orient_measure(float out[3], int ms)
{
    double acc[3] = {0, 0, 0};
    int n = 0;
    const int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < end) {
        mpx_robot_imu_t s = {0};
        mpx_robot_imu_read(&s);
        acc[0] += s.ax; acc[1] += s.ay; acc[2] += s.az;
        n++;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    for (int i = 0; i < 3; i++) {
        out[i] = n ? (float)(acc[i] / n) : 0.0f;
    }
}

/* Index of the axis (not `skip`) that changed most; its signed change. */
static int orient_biggest(const float d[3], int skip, float *val)
{
    int best = -1;
    for (int i = 0; i < 3; i++) {
        if (i == skip) continue;
        if (best < 0 || fabsf(d[i]) > fabsf(d[best])) best = i;
    }
    *val = d[best];
    return best;
}

static bool orient_tilt(const char *what, float roll, float pitch, bool by_hand,
                        const float level[3], float delta[3])
{
    float tilted[3];
    if (by_hand) {
        printf("  >> now hold it with the %s, about 20 degrees ... ", what);
        fflush(stdout);
        for (int i = 3; i > 0; i--) {
            printf("%d ", i);
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        printf("measuring\n");
    } else {
        printf("  robot tilts: %s ...\n", what);
        mpx_robot_set_body_attitude(roll, pitch, 0.0f);
        vTaskDelay(pdMS_TO_TICKS(1800));
    }
    orient_measure(tilted, 1000);
    if (!by_hand) {
        mpx_robot_set_body_attitude(0.0f, 0.0f, 0.0f);
        vTaskDelay(pdMS_TO_TICKS(1500));
    } else {
        printf("  >> put it back level\n");
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
    for (int i = 0; i < 3; i++) {
        delta[i] = tilted[i] - level[i];
    }
    printf("     change  x %+.2f  y %+.2f  z %+.2f g\n", delta[0], delta[1], delta[2]);
    return true;
}

static int imu_orient(bool force_hand)
{
    static const char AX[3] = { 'X', 'Y', 'Z' };
    const bool robot = mpx_robot_ready() && !force_hand;
    float level[3], d_pitch[3], d_roll[3];

    printf("\n  IMU orientation test -- %s\n",
           robot ? "the robot tilts itself; keep it on the floor and keep hands off"
                 : "follow the prompts and tilt the robot by hand");
    if (robot) {
        mpx_robot_gait_by_name("init");
        mpx_robot_set_attitude_speed(40.0f);
        mpx_robot_set_body_attitude(0.0f, 0.0f, 0.0f);
        vTaskDelay(pdMS_TO_TICKS(2000));
    } else {
        printf("  >> stand it level on the table ... ");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(3000));
        printf("measuring\n");
    }

    /* 1. Gravity at rest: which chip axis points up (or down). */
    orient_measure(level, 1200);
    printf("     level   x %+.2f  y %+.2f  z %+.2f g\n", level[0], level[1], level[2]);
    int up = 0;
    for (int i = 1; i < 3; i++) {
        if (fabsf(level[i]) > fabsf(level[up])) up = i;
    }
    const float g = sqrtf(level[0] * level[0] + level[1] * level[1] + level[2] * level[2]);
    if (fabsf(level[up]) < 0.85f || g < 0.85f || g > 1.15f) {
        printf("  FAIL: not level/still (|a| %.2f g) -- put it on a flat table and run again\n", g);
        return 1;
    }
    const int up_sign = level[up] > 0 ? 1 : -1;

    /* 2. Nose down: the forward axis reading goes negative. */
    orient_tilt("NOSE DOWN", 0.0f, -ORIENT_TILT_DEG, !robot, level, d_pitch);
    float dv;
    int fwd = orient_biggest(d_pitch, up, &dv);
    if (robot && fabsf(dv) < ORIENT_MIN_DELTA_G) {
        printf("  the robot did not tilt enough (servos off?) -- doing it by hand instead\n");
        return imu_orient(true);
    }
    if (fabsf(dv) < ORIENT_MIN_DELTA_G) {
        printf("  FAIL: hardly any change -- tilt further and run `imu orient hand` again\n");
        return 1;
    }
    const int fwd_sign = dv < 0 ? 1 : -1;

    /* 3. Right side down: the left axis reading goes positive. */
    orient_tilt("RIGHT SIDE DOWN", ORIENT_TILT_DEG, 0.0f, !robot, level, d_roll);
    int left = -1, left_sign = 0;
    float lv;
    int l2 = orient_biggest(d_roll, up, &lv);
    if (l2 != fwd && fabsf(lv) >= ORIENT_MIN_DELTA_G) {
        left = l2;
        left_sign = lv > 0 ? 1 : -1;
    }

    if (robot) {
        mpx_robot_gait_by_name("init");
    }

    /* Right-hand rule: up = forward x left. The remaining axis is "left";
     * its sign follows from up and forward. */
    const int calc_left = 3 - up - fwd;
    float f[3] = {0, 0, 0}, u[3] = {0, 0, 0};
    f[fwd] = (float)fwd_sign;
    u[up] = (float)up_sign;
    /* left = up x forward */
    const float lc[3] = { u[1] * f[2] - u[2] * f[1],
                          u[2] * f[0] - u[0] * f[2],
                          u[0] * f[1] - u[1] * f[0] };
    const int calc_left_sign = lc[calc_left] > 0 ? 1 : -1;
    const bool agrees = (left == calc_left && left_sign == calc_left_sign);

    printf("\n  RESULT (robot axes, ROS REP-103: x forward, y left, z up)\n");
    const struct { const char *name; int axis, sign, want; } res[3] = {
        { "forward", fwd,       fwd_sign,       0 },
        { "left   ", calc_left, calc_left_sign, 1 },
        { "up     ", up,        up_sign,        2 },
    };
    for (int i = 0; i < 3; i++) {
        const bool same_axis = res[i].axis == res[i].want;
        printf("    %s = chip %c%c   %s%s%s\n", res[i].name,
               res[i].sign > 0 ? '+' : '-', AX[res[i].axis],
               (same_axis && res[i].sign > 0) ? "OK" : "",
               same_axis ? "" : "SWAPPED axis",
               res[i].sign < 0 ? (same_axis ? "REVERSED" : " + REVERSED") : "");
    }
    if (up == 2 && up_sign < 0) {
        printf("    (the chip is mounted upside down)\n");
    }
    if (!agrees) {
        printf("    (the side-tilt check %s -- the result above is computed from\n"
               "     up + forward, which are the reliable two; run again to confirm)\n",
               left < 0 ? "was too small to read" : "disagreed");
    } else {
        printf("    side-tilt check agrees\n");
    }
    printf("\n  For the Pi (mini_pupper_esp_bridge/config/esp_bridge.yaml):\n"
           "    imu_axis_map:  [%d, %d, %d]\n"
           "    imu_axis_sign: [%d, %d, %d]\n\n",
           fwd, calc_left, up, fwd_sign, calc_left_sign, up_sign);
    return 0;
}
#endif

static int imu_cmd(int argc, char **argv)
{
    /* Deliberately not gated on mpx_robot_ready(): that is false with no
     * servo boards attached, but the IMU is brought up regardless (the boot
     * log says "IMU initialised"). All-zero readings are the tell if it is
     * not. */
    if (argc >= 2 && strcmp(argv[1], "watch") == 0) {
        const uint32_t secs = (argc >= 3) ? (uint32_t)atoi(argv[2]) : IMU_WATCH_DEFAULT_S;
        uint32_t hz = (argc >= 4) ? (uint32_t)atoi(argv[3]) : IMU_WATCH_DEFAULT_HZ;
        if (hz < 1) hz = 1;
        if (hz > 20) hz = 20;   /* the driver polls at 20 Hz; faster just repeats */
        printf("  watching the BMI270 for %" PRIu32 " s at %" PRIu32 " Hz -- tilt it, turn it, shake it\n"
               "  (axes are the chip's: +Z should read +1 g with the board flat and face up;\n"
               "   hdg is the compass, 0 = magnetic north, * = no `imu cal` yet)\n",
               secs, hz);
        const int64_t t0 = esp_timer_get_time();
        const int64_t end = t0 + (int64_t)secs * 1000000;
        while (esp_timer_get_time() < end) {
            imu_print_sample((uint32_t)((esp_timer_get_time() - t0) / 1000));
            vTaskDelay(pdMS_TO_TICKS(1000 / hz));
        }
        return 0;
    }
#if CONFIG_MP4_ROBOT_ENABLE
    if (argc >= 2 && strcmp(argv[1], "orient") == 0) {
        return imu_orient(argc >= 3 && strcmp(argv[2], "hand") == 0);
    }
    if (argc >= 2 && strcmp(argv[1], "cal") == 0) {
        /* Hard-iron calibration: the field the chip sees is the Earth's plus
         * a fixed offset from whatever is magnetised on the board. Turn the
         * board through every orientation and the Earth part sweeps a
         * sphere while the offset stays put -- so the centre of the min/max
         * box on each axis is the offset. Crude but it is what every phone
         * does with its figure-eight dance, and it turns a heading that is
         * off by 30-90 degrees into one that is off by a few. */
        if (!mpx_robot_mag_ready()) {
            printf("  no BMM150 -- see the boot log for 'BMM150' lines\n");
            return 1;
        }
        const uint32_t secs = (argc >= 3) ? (uint32_t)atoi(argv[2]) : IMU_CAL_DEFAULT_S;
        float lo[3] = {  1e9f,  1e9f,  1e9f };
        float hi[3] = { -1e9f, -1e9f, -1e9f };
        int n = 0;
        printf("  calibrating the compass for %" PRIu32 " s: slowly turn the board through EVERY\n"
               "  orientation -- flat, on edge, upside down, spin it -- like a figure eight\n", secs);
        const int64_t end = esp_timer_get_time() + (int64_t)secs * 1000000;
        while (esp_timer_get_time() < end) {
            mpx_robot_imu_t s = {0};
            mpx_robot_imu_read(&s);
            if (s.mag_valid) {
                const float m[3] = { s.mx, s.my, s.mz };
                for (int i = 0; i < 3; i++) {
                    if (m[i] < lo[i]) lo[i] = m[i];
                    if (m[i] > hi[i]) hi[i] = m[i];
                }
                n++;
            }
            if ((n % 20) == 1) {
                printf("  x %+5.0f..%+5.0f  y %+5.0f..%+5.0f  z %+5.0f..%+5.0f uT\r",
                       lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
                fflush(stdout);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        printf("\n");
        if (n < 10) {
            printf("  no magnetometer samples arrived\n");
            return 1;
        }
        float span_min = 1e9f;
        for (int i = 0; i < 3; i++) {
            s_mag_off[i] = (hi[i] + lo[i]) * 0.5f;
            if (hi[i] - lo[i] < span_min) span_min = hi[i] - lo[i];
        }
        s_mag_cal = true;
        printf("  offset  x %+5.0f  y %+5.0f  z %+5.0f uT   (span x %.0f y %.0f z %.0f)\n",
               s_mag_off[0], s_mag_off[1], s_mag_off[2],
               hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
        if (span_min < 40.0f) {
            printf("  one axis barely moved -- the board was not turned enough on that axis;\n"
                   "  the Earth's field is ~40-60 uT, so each span should be ~80-120. Run it again.\n");
        } else {
            printf("  applied (RAM only, lost at reboot). `imu watch` heading is now corrected.\n");
        }
        return 0;
    }
#endif
    if (argc >= 2 && strcmp(argv[1], "help") == 0) {
        printf("\n  imu                 one reading\n"
               "  imu watch [s] [hz]  live readings, default %d s at %d Hz\n"
               "  imu cal [s]         compass hard-iron calibration, default %d s\n"
               "  imu orient          robot tilts itself; prints which axes are reversed\n"
               "                      and the ROS imu_axis_map / imu_axis_sign\n"
               "  imu orient hand     same, but you tilt it by hand (servos off)\n\n"
               "  a = accelerometer in g, |a| its magnitude (1.00 at rest);\n"
               "  g = gyroscope in deg/s; pitch/roll from the accelerometer,\n"
               "  so only meaningful while the board is not being shaken.\n"
               "  m = BMM150 magnetometer in uT (through the BMI270 aux bus),\n"
               "  |m| its magnitude (Earth: ~40-60 uT, more near a magnet);\n"
               "  hdg = tilt-compensated heading, 0 = magnetic north, +X forward.\n\n",
               IMU_WATCH_DEFAULT_S, IMU_WATCH_DEFAULT_HZ, IMU_CAL_DEFAULT_S);
        return 0;
    }
    imu_print_sample(0);
    return 0;
}

static int button_cmd(int argc, char **argv)
{
    const uint32_t secs = (argc >= 2) ? (uint32_t)atoi(argv[1]) : BTN_WATCH_DEFAULT_S;

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << SELFTEST_BTN_BOOT) | (1ULL << SELFTEST_BTN_WAKE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        printf("  could not configure GPIO %d and %d\n", SELFTEST_BTN_BOOT, SELFTEST_BTN_WAKE);
        return 1;
    }

    printf("  watching BOOT (GPIO %d) and WAKE (GPIO %d) for %" PRIu32 " s -- press them\n"
           "  (a press reads 0: both are momentary to ground with pull-ups)\n",
           SELFTEST_BTN_BOOT, SELFTEST_BTN_WAKE, secs);

    const int64_t t0 = esp_timer_get_time();
    const int64_t end = t0 + (int64_t)secs * 1000000;
    int last_boot = gpio_get_level(SELFTEST_BTN_BOOT);
    int last_wake = gpio_get_level(SELFTEST_BTN_WAKE);
    int64_t boot_down = 0, wake_down = 0;
    int presses = 0;
    printf("  now: BOOT %s, WAKE %s\n", last_boot ? "released" : "PRESSED",
           last_wake ? "released" : "PRESSED");

    while (esp_timer_get_time() < end) {
        vTaskDelay(pdMS_TO_TICKS(10));   /* 10 ms poll: a bounce shorter than that is invisible */
        const int64_t now = esp_timer_get_time();
        const int boot = gpio_get_level(SELFTEST_BTN_BOOT);
        const int wake = gpio_get_level(SELFTEST_BTN_WAKE);
        if (boot != last_boot) {
            if (boot == 0) {
                boot_down = now;
                printf("  %6" PRIu32 " ms  BOOT pressed\n", (uint32_t)((now - t0) / 1000));
                presses++;
            } else {
                printf("  %6" PRIu32 " ms  BOOT released  (held %" PRIu32 " ms)\n",
                       (uint32_t)((now - t0) / 1000), (uint32_t)((now - boot_down) / 1000));
            }
            last_boot = boot;
        }
        if (wake != last_wake) {
            if (wake == 0) {
                wake_down = now;
                printf("  %6" PRIu32 " ms  WAKE pressed\n", (uint32_t)((now - t0) / 1000));
                presses++;
            } else {
                printf("  %6" PRIu32 " ms  WAKE released  (held %" PRIu32 " ms)\n",
                       (uint32_t)((now - t0) / 1000), (uint32_t)((now - wake_down) / 1000));
            }
            last_wake = wake;
        }
    }
    printf("  done: %d press(es) seen\n", presses);
    return 0;
}

static void register_bringup_commands(void)
{
    const esp_console_cmd_t imu = {
        .command = "imu",
        .help = "BMI270+BMM150: `imu`, `imu watch [s] [hz]`, `imu orient [hand]`, `imu cal [s]`, `imu help`",
        .hint = NULL,
        .func = imu_cmd,
    };
    const esp_console_cmd_t button = {
        .command = "button",
        .help = "Watch BOOT (GPIO 0) and WAKE (GPIO 21) press/release: `button [s]`",
        .hint = NULL,
        .func = button_cmd,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&imu));
    ESP_ERROR_CHECK(esp_console_cmd_register(&button));
}

void register_selftest_command(void)
{
    s_args.system_   = arg_lit0(NULL, "system",    "RAM, partitions and mounts");
    s_args.i2c       = arg_lit0(NULL, "i2c",       "Probe the I2C bus for the IMU and audio ADC");
    s_args.imu       = arg_lit0(NULL, "imu",       "Read the BMI270 and check it measures 1 g");
    s_args.servo     = arg_lit0(NULL, "servo",     "Ping the servo boards (does NOT move anything)");
    s_args.display   = arg_lit0(NULL, "display",   "Show a test pattern for orientation and colour");
    s_args.backlight = arg_lit0(NULL, "backlight", "Ramp the backlight to check output_invert");
    s_args.buttons   = arg_lit0(NULL, "buttons",   "Wait for BOOT and WAKE presses");
    s_args.hold      = arg_int0(NULL, "hold", "<s>", "Seconds to hold the test pattern (default 10)");
    s_args.other     = arg_lit0(NULL, "other-board", "Not MP4 hardware: report wiring checks as WARN, not FAIL");
    s_args.end       = arg_end(8);

    const esp_console_cmd_t cmd = {
        .command = "selftest",
        .help = "Check the board against what the firmware believes is wired.\n"
                "With no options every check runs. Nothing here moves a servo.\n"
                "Examples:\n"
                "  selftest                 everything\n"
                "  selftest --i2c --imu     just the sensors\n"
                "  selftest --display --hold 30\n"
                "  selftest --other-board   on a different ESP32-S3, to check ESP-Claw itself\n",
        .func = selftest_cmd,
        .argtable = &s_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    register_bringup_commands();
    ESP_LOGI(TAG, "'selftest', 'imu' and 'button' console commands registered");
}

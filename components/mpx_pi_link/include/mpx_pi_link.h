/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * mpx_pi_link -- the UART link to the Raspberry Pi (ROS 2 host) on J-PI.
 *
 * This is mpv2's Pi link (main/pi/pi_link.cc, pi_protocol.h, pi_audio.cc,
 * pi_video.cc), moved into a component. The wire format is mpv2's
 * src/pi_protocol.h, byte for byte, so the ROS bridge that drives mpv2
 * (ros2jazzympv2/mini_pupper_esp_bridge) drives this firmware unchanged.
 *
 * The Pi is OPTIONAL. With nothing on J-PI the robot is exactly the robot it
 * was: the app, the agent, voice and skills drive it. When a ROS host holds
 * HOST_PRESENT (IO3) high and heartbeats with WANT_CONTROL, the robot goes
 * into slave mode (robot::master::begin()) and from then on only the Pi
 * moves it. Losing either signal for CONFIG_MP4_PI_LINK_TIMEOUT_MS hands the
 * robot back (robot::master::end(): stop, stand, standalone).
 *
 *   J-PI pin 2  IO18  ESP TX  -> Pi GPIO15 RXD (header pin 10)
 *   J-PI pin 3  IO8   ESP RX  <- Pi GPIO14 TXD (header pin 8)
 *   J-PI pin 4  IO3   HOST_PRESENT, Pi -> ESP (Pi GPIO17, pin 11)
 *   J-PI pin 5  IO11  ESP_SLAVE,    ESP -> Pi (Pi GPIO27, pin 13)
 *   8N1, CONFIG_MP4_PI_LINK_UART_BAUD (default 921600).
 *
 * What the Pi gets: STATE (servo angles, IMU, velocity; 50 Hz default),
 * INFO (1 Hz), DIAG (5 Hz), EVENTs. What it can send: HEARTBEAT, CMD_VEL,
 * SET_MODE, BODY_POSE, FOOT_TARGETS, AUDIO_CTRL/DATA (music through the
 * speaker, slave mode only), HOST_INFO (its IP under the eyes) and
 * VIDEO_DATA (a camera picture on the screen).
 *
 * Who reacts to slave mode is wired in main.c through
 * mpx_robot_set_master_observer(): voice and music stop, the screen says
 * ROS. The app's /v1/robot routes, the agent's robot tools and WASM skills
 * refuse to move the robot while mpx_robot_pi_in_control() is true.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MPX_PI_LINK_STANDALONE = 0,   /* no host: the robot drives itself       */
    MPX_PI_LINK_MONITOR    = 1,   /* host heard, not in control             */
    MPX_PI_LINK_SLAVE      = 2,   /* host in control of all motion          */
} mpx_pi_link_state_t;

typedef struct {
    mpx_pi_link_state_t state;
    bool     host_pin;            /* IO3 level right now                    */
    bool     heartbeat;           /* a heartbeat is fresh                   */
    bool     want_control;        /* the last heartbeat asked for control   */
    uint32_t ms_since_rx;         /* since the last valid frame, UINT32_MAX = never */
    uint32_t rx_frames;
    uint32_t rx_crc_err;
    uint32_t rx_bad;
    uint32_t tx_frames;
    uint32_t tx_drop;
    uint32_t slave_entries;       /* how many times slave mode was entered  */
    uint16_t state_rate_hz;
    char     last_exit_reason[32];
    /* The Pi's speaker stream and camera picture. */
    bool     audio_playing;
    uint32_t audio_buffered_ms;
    uint32_t audio_underruns;
    uint32_t video_frames_shown;
    uint32_t video_frames_dropped;
    uint32_t video_decode_errors;
    /* The RL host link (mp4-link protocol, rl_link.cc) on the same UART. */
    bool     rl_session;          /* an RL policy is talking to the robot   */
    bool     rl_robot;            /* ...and owns it (not the ROS bridge)    */
    bool     rl_armed;
    uint16_t rl_flags;            /* the last STATE flags (mp4-link ST_*)   */
    uint8_t  rl_fault_servo;
    uint32_t rl_frames;
    uint32_t rl_crc_err;
    uint32_t rl_sessions;
    uint32_t rl_ticks;
} mpx_pi_link_status_t;

/* Open UART1 on the J-PI pins, configure IO3 (input, pulled down) and IO11
 * (output, low) and start the link task on core 0. Call once, straight after
 * mpx_robot_init(): a Pi that took control must always be able to lose it. */
esp_err_t mpx_pi_link_init(void);

/* True while the Pi is in control of the robot. */
bool mpx_pi_link_is_slave(void);

/* Snapshot for the app and the console. Thread-safe. */
void mpx_pi_link_get_status(mpx_pi_link_status_t *out);

/* "standalone", "monitor" or "slave". */
const char *mpx_pi_link_state_name(mpx_pi_link_state_t state);

/* The current state's name (for GET /v1/robot/status "pi_link"). */
const char *mpx_pi_link_state_str(void);

/* The Pi's IP and hostname from its last HOST_INFO (the bridge sends one
 * every ~2 s, pi_announce too). False, with "" in both, when nothing
 * arrived for 6 s. Any link state: HOST_INFO alone shows the address. Either pointer may be NULL. */
bool mpx_pi_link_host_info(char *ip, size_t ip_len, char *hostname, size_t hostname_len);

/* Just the IP (used by GET /v1/wifi/status and the face's network line). */
bool mpx_pi_link_ip(char *out, size_t out_size);

/* Called when the Pi's address appears, changes or goes away (ip = "").
 * Runs on the link task: must not block (post it somewhere). */
void mpx_pi_link_set_ip_observer(void (*cb)(const char *ip));

/* Where complete camera pictures go: a baseline JPEG up to 320x240. Called
 * from the link's own picture task, never the link task, so it may take the
 * display lock. NULL (the default) drops VIDEO_DATA. */
void mpx_pi_link_set_video_sink(esp_err_t (*sink)(const uint8_t *jpeg, size_t len));

/* GET /v1/pi/status body (the same fields mpv2 sends). Returns the length
 * written, or a negative number if `size` was too small. */
int mpx_pi_link_status_json(char *buf, size_t size);

/* `pi status` on the console. */
void register_pi_command(void);

#ifdef __cplusplus
}
#endif

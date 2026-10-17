/* pi_link.h - the UART link to the Raspberry Pi (ROS 2 host) on J-PI.
 *
 * mpv2's Pi link (mpv2-claw components/mpx_pi_link), brought back into the
 * single-app layout of this firmware as `namespace pi_link`. The wire format
 * is pi_protocol.h, byte for byte, so the ROS bridge that drives mpv2
 * (ros2jazzympv2/mini_pupper_esp_bridge) drives this firmware unchanged.
 *
 * The Pi is OPTIONAL. With nothing on J-PI the robot is exactly the robot it
 * was: the app, Lua and WASM skills drive it. When a ROS host holds
 * HOST_PRESENT (IO3) high and heartbeats with WANT_CONTROL, the robot goes
 * into slave mode (robot::master::begin()) and from then on only the Pi moves
 * it. Losing either signal for CONFIG_APP_PI_LINK_TIMEOUT_MS hands the robot
 * back (robot::master::end(): stop, stand, standalone).
 *
 *   J-PI pin 2  IO18  ESP TX  -> Pi GPIO15 RXD (header pin 10)
 *   J-PI pin 3  IO8   ESP RX  <- Pi GPIO14 TXD (header pin 8)
 *   J-PI pin 4  IO3   HOST_PRESENT, Pi -> ESP (Pi GPIO17, pin 11)
 *   J-PI pin 5  IO11  ESP_SLAVE,    ESP -> Pi (Pi GPIO27, pin 13)
 *   8N1, CONFIG_APP_PI_LINK_UART_BAUD (default 921600).
 *
 * What the Pi gets: STATE (servo angles, IMU, velocity; 50 Hz default),
 * INFO (1 Hz), DIAG (5 Hz), EVENTs. What it can send: HEARTBEAT, CMD_VEL,
 * SET_MODE, BODY_POSE, FOOT_TARGETS, AUDIO_CTRL/DATA (music through the
 * speaker, slave mode only), HOST_INFO (its IP, shown under the eyes) and
 * VIDEO_DATA (a camera picture on the screen).
 *
 * The same UART also carries mp4-link's RL host protocol (pi/rl_link.cc,
 * pi/RL_LINK.md): a policy on the Pi (python -m mp3link) drives all twelve
 * joints directly at 200 Hz. Whoever takes the robot first, the ROS bridge
 * or an RL session, keeps it.
 *
 * Who reacts to slave mode is wired in main.cc through
 * robot::set_master_observer(): voice stops, the screen says ROS. The
 * /v1/robot routes answer 423 pi_in_control while robot::master_active().
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace pi_link {

enum class State : uint8_t {
    Standalone = 0,   // no host: the robot drives itself
    Monitor    = 1,   // host heard, not in control
    Slave      = 2,   // host in control of all motion
};

struct Status {
    State    state;
    bool     host_pin;            // IO3 level right now
    bool     heartbeat;           // a heartbeat is fresh
    bool     want_control;        // the last heartbeat asked for control
    uint32_t ms_since_rx;         // since the last valid frame, UINT32_MAX = never
    uint32_t rx_frames;
    uint32_t rx_crc_err;
    uint32_t rx_bad;
    uint32_t tx_frames;
    uint32_t tx_drop;
    uint32_t slave_entries;       // how many times slave mode was entered
    uint16_t state_rate_hz;
    char     last_exit_reason[32];
    // The Pi's speaker stream and camera picture.
    bool     audio_playing;
    uint32_t audio_buffered_ms;
    uint32_t audio_underruns;
    uint32_t video_frames_shown;
    uint32_t video_frames_dropped;
    uint32_t video_decode_errors;
    // The RL host link (mp4-link protocol, rl_link.cc) on the same UART.
    bool     rl_session;          // an RL policy is talking to the robot
    bool     rl_robot;            // ...and owns it (not the ROS bridge)
    bool     rl_armed;
    uint16_t rl_flags;            // the last STATE flags (mp4-link ST_*)
    uint8_t  rl_fault_servo;
    uint32_t rl_frames;
    uint32_t rl_crc_err;
    uint32_t rl_sessions;
    uint32_t rl_ticks;
};

/* Open UART1 on the J-PI pins, configure IO3 (input, pulled down) and IO11
 * (output, low) and start the link task on core 0. Call once, straight after
 * robot::init(): a Pi that took control must always be able to lose it. */
esp_err_t init();

/* True while the Pi is in control of the robot. */
bool is_slave();

/* Snapshot for the app. Thread-safe. */
void get_status(Status *out);

/* "standalone", "monitor" or "slave". */
const char *state_name(State state);

/* The current state's name (GET /v1/robot/status "pi_link"). */
const char *state_str();

/* The Pi's IP and hostname from its last HOST_INFO (the bridge sends one
 * every ~2 s, tools/pi/pi_announce.py too). False, with "" in both, when
 * nothing arrived for 6 s. Any link state: HOST_INFO alone shows the address.
 * Either pointer may be nullptr. */
bool host_info(char *ip, size_t ip_len, char *hostname, size_t hostname_len);

/* Just the IP (GET /v1/wifi/status "pi" and the face's network line). */
bool ip(char *out, size_t out_size);

/* Called when the Pi's address appears, changes or goes away (ip = "").
 * Runs on the link task: must not block (post it somewhere). */
void set_ip_observer(void (*cb)(const char *ip));

/* Where complete camera pictures go: a baseline JPEG up to 320x240. Called
 * from the link's own picture task, never the link task, so it may take the
 * display lock. nullptr (the default) drops VIDEO_DATA. */
void set_video_sink(esp_err_t (*sink)(const uint8_t *jpeg, size_t len));

/* GET /v1/pi/status body (the same fields mpv2 sends). Returns the length
 * written, or a negative number if `size` was too small or the link was
 * never started. */
int status_json(char *buf, size_t size);

}  // namespace pi_link

/* pi_link.cc -- see include/mpx_pi_link.h and pi_protocol.h.
 *
 * Ported from mpv2 main/pi/pi_link.cc. The protocol, the state machine, the
 * safety model and every timing are unchanged; what differs is only how it
 * reaches the rest of this firmware:
 *   - Kconfig names are CONFIG_MP4_PI_LINK_* (main/Kconfig.projbuild);
 *   - "is a skill running" comes from mpx_wasm's C API;
 *   - camera pictures go to a sink main.c registers (cap_display), and the
 *     Pi's IP to an observer (the face's network line);
 *   - INFO.fw says "mpv2-claw".
 *
 * One task, pinned to core 0 next to the network stack (the gait owns core 1),
 * does everything: drains UART1, parses frames, runs the link state machine
 * and sends telemetry. One task means no locking around the parser or the
 * TX sequence number, and the only state shared with other tasks is the
 * status snapshot (behind a spinlock) and the is_slave flag.
 *
 * SAFETY MODEL
 *   - Slave mode needs the HOST_PRESENT line AND a fresh WANT_CONTROL
 *     heartbeat. A crashed ROS node stops heartbeating; a Pi that powers off
 *     or reboots drops the line; a cable that falls out does both.
 *   - Leaving slave mode always goes through robot::master::end(), which
 *     zeroes the velocity and returns the robot to the neutral stand before
 *     handing control back to the app.
 *   - Inside slave mode, CMD_VEL itself also times out (robot.cc,
 *     PI_VEL_TIMEOUT_MS): a ROS node that keeps heartbeating but stops
 *     publishing cmd_vel gets a robot that steps in place, not one that keeps
 *     walking into a wall.
 */
#include "mpx_pi_link.h"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "sdkconfig.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mpx_wasm.h"
#include "pi_audio.h"
#include "pi_protocol.h"
#include "pi_video.h"
#include "rl_link.h"
#include "robot/board_config.h"
#include "robot/robot.h"

/* Fallbacks so this file still builds against an sdkconfig generated before
 * the Kconfig entries existed. menuconfig is where these are meant to be set. */
#ifndef CONFIG_MP4_PI_LINK_UART_BAUD
#define CONFIG_MP4_PI_LINK_UART_BAUD 921600
#endif
#ifndef CONFIG_MP4_PI_LINK_TIMEOUT_MS
#define CONFIG_MP4_PI_LINK_TIMEOUT_MS 300
#endif
#ifndef CONFIG_MP4_PI_LINK_STATE_HZ
#define CONFIG_MP4_PI_LINK_STATE_HZ 50
#endif
#ifndef CONFIG_MP4_ROBOT_UUID
#define CONFIG_MP4_ROBOT_UUID "MPX-DOG-01"
#endif

namespace pi_link {
namespace {

const char *TAG = "pi_link";

constexpr int      RX_BUF_SIZE     = 4096;   // ~44 ms at 921600: music + video frames
constexpr int      TX_BUF_SIZE     = 2048;
constexpr int64_t  TIMEOUT_US      = int64_t(CONFIG_MP4_PI_LINK_TIMEOUT_MS) * 1000;
constexpr int64_t  INFO_PERIOD_US  = 1000000;   // 1 Hz
constexpr int64_t  DIAG_PERIOD_US  = 200000;    // 5 Hz
constexpr int64_t  REJECT_GAP_US   = 500000;    // at most 2 "rejected" events/s
constexpr int64_t  HOST_INFO_STALE_US = 6000000;
constexpr int      PIN_LOW_SAMPLES = 3;         // ~30 ms of LOW before it counts
constexpr int      RATE_MIN_HZ     = 5;
constexpr int      RATE_MAX_HZ     = 100;
/* mpv2 ran FreeRTOS at 100 Hz and read with a 1-tick timeout, i.e. 10 ms.
 * This firmware runs at 1000 Hz, so the period is spelled out to keep the
 * same loop rate instead of waking a thousand times a second. */
constexpr TickType_t RX_WAIT       = pdMS_TO_TICKS(10) ? pdMS_TO_TICKS(10) : 1;

/* ── Shared with other tasks ─────────────────────────────────────────── */
portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;
mpx_pi_link_status_t s_status = {};
volatile bool s_is_slave = false;
bool          s_started  = false;
void        (*s_ip_cb)(const char *ip) = nullptr;

/* ── Link task state (only the link task touches these) ──────────────── */
mpx_pi_link_state_t s_state = MPX_PI_LINK_STANDALONE;
int64_t   s_last_hb_us     = 0;         // 0 = never
int64_t   s_last_rx_us     = 0;
bool      s_want_control   = false;
uint16_t  s_req_rate_hz    = 0;
uint8_t   s_last_rx_seq    = 0;
uint8_t   s_tx_seq         = 0;
int       s_pin_low_count  = PIN_LOW_SAMPLES;
int64_t   s_last_reject_us = 0;
int64_t   s_last_mismatch_us = 0;

uint32_t  s_rx_frames = 0, s_rx_crc_err = 0, s_rx_bad = 0;
uint32_t  s_tx_frames = 0, s_tx_drop = 0, s_slave_entries = 0;
char      s_last_exit[32] = "";
char      s_host_ip[16] = "";           // HOST_INFO, under s_mux
char      s_host_name[32] = "";
int64_t   s_host_info_us = 0;
char      s_ip_told[16] = "";           // what the IP observer last heard

/* ── Frame parser ────────────────────────────────────────────────────── */
enum class Ps : uint8_t { Sync0, Sync1, Len, Type, Seq, Payload, CrcL, CrcH };

struct Parser {
    Ps       st  = Ps::Sync0;
    uint8_t  len = 0, type = 0, seq = 0;
    uint16_t idx = 0;
    uint16_t crc_rx = 0;
    uint8_t  buf[PI_MAX_PAYLOAD];
};
Parser s_p;

/* ── TX ──────────────────────────────────────────────────────────────── */
void send_frame(uint8_t type, const void *payload, uint8_t len)
{
    static uint8_t frame[PI_MAX_PAYLOAD + PI_FRAME_OVERHEAD];
    frame[0] = PI_SYNC0;
    frame[1] = PI_SYNC1;
    frame[2] = len;
    frame[3] = type;
    frame[4] = s_tx_seq++;
    if (len) std::memcpy(&frame[5], payload, len);
    const uint16_t crc = pi_crc16(&frame[2], 3u + len, 0xFFFF);
    frame[5 + len] = static_cast<uint8_t>(crc & 0xFF);
    frame[6 + len] = static_cast<uint8_t>(crc >> 8);

    const int n = uart_write_bytes(PI_UART_PORT, frame, len + PI_FRAME_OVERHEAD);
    if (n == len + PI_FRAME_OVERHEAD) s_tx_frames++;
    else                              s_tx_drop++;
}

void send_event(uint8_t code, uint8_t ref_type, uint8_t ref_seq, const char *text)
{
    pi_event_t ev = {};
    ev.code     = code;
    ev.ref_type = ref_type;
    ev.ref_seq  = ref_seq;
    std::snprintf(ev.text, sizeof(ev.text), "%s", text ? text : "");
    send_frame(PI_MSG_EVENT, &ev, sizeof(ev));
}

void reject(uint8_t type, uint8_t seq, const char *why)
{
    const int64_t now = esp_timer_get_time();
    if (now - s_last_reject_us < REJECT_GAP_US) return;
    s_last_reject_us = now;
    ESP_LOGW(TAG, "rejected msg 0x%02x: %s", type, why);
    send_event(PI_EV_REJECTED, type, seq, why);
}

uint16_t state_rate_hz()
{
    int hz = s_req_rate_hz ? s_req_rate_hz : CONFIG_MP4_PI_LINK_STATE_HZ;
    if (hz < RATE_MIN_HZ) hz = RATE_MIN_HZ;
    if (hz > RATE_MAX_HZ) hz = RATE_MAX_HZ;
    return static_cast<uint16_t>(hz);
}

int16_t clamp_i16(float v)
{
    if (!std::isfinite(v)) return PI_CDEG_INVALID;
    if (v >  32767.0f) return  32767;
    if (v < -32767.0f) return -32767;
    return static_cast<int16_t>(std::lrintf(v));
}

void send_info()
{
    pi_info_t info = {};
    info.proto         = PI_PROTO_VERSION;
    info.link_state    = static_cast<uint8_t>(s_state);
    info.state_rate_hz = state_rate_hz();
    std::snprintf(info.fw,   sizeof(info.fw),   "%s", "mpv2-claw");
    std::snprintf(info.uuid, sizeof(info.uuid), "%s", CONFIG_MP4_ROBOT_UUID);
    send_frame(PI_MSG_INFO, &info, sizeof(info));
}

void send_state(bool host_pin)
{
    pi_state_t st = {};
    st.esp_ms      = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    st.link_state  = static_cast<uint8_t>(s_state);
    const robot::GaitCmd g = robot::current_gait_cmd();
    st.gait        = static_cast<uint8_t>(g);
    st.last_rx_seq = s_last_rx_seq;

    robot::servo_snapshot(st.servo_cmd_cdeg, st.servo_meas_cdeg);

    const robot::ImuData d = robot::imu_read();
    st.acc_g[0] = d.ax;  st.acc_g[1] = d.ay;  st.acc_g[2] = d.az;
    st.gyro_dps[0] = d.gx;  st.gyro_dps[1] = d.gy;  st.gyro_dps[2] = d.gz;
    st.mag_ut[0] = d.mx;  st.mag_ut[1] = d.my;  st.mag_ut[2] = d.mz;

    robot::executed_velocity(st.vx, st.vy, st.wz);

    uint8_t f = 0;
    if (d.ax != 0.0f || d.ay != 0.0f || d.az != 0.0f) f |= PI_ST_IMU_OK;
    if (d.mag_valid)                                   f |= PI_ST_MAG_VALID;
    if (host_pin)                                      f |= PI_ST_HOST_PIN;
    if (robot::studio_mode())                          f |= PI_ST_STUDIO;
    if (mpx_wasm_skill_running())                      f |= PI_ST_SKILL;
    if (g == robot::GaitCmd::StanfordWalk)             f |= PI_ST_WALKING;
    st.flags = f;

    send_frame(PI_MSG_STATE, &st, sizeof(st));
}

void send_diag()
{
    pi_diag_t dg = {};
    for (int i = 0; i < 12; ++i) {
        const float c = robot::read_temperature_c(i + 1);
        dg.temp_dc[i]    = std::isnan(c) ? INT16_MIN : clamp_i16(c * 10.0f);
        dg.current_ma[i] = clamp_i16(static_cast<float>(robot::read_current(i + 1)));
    }
    dg.rx_frames  = s_rx_frames;
    dg.rx_crc_err = s_rx_crc_err;
    dg.rx_bad     = s_rx_bad;
    dg.tx_frames  = s_tx_frames;
    dg.tx_drop    = static_cast<uint16_t>(s_tx_drop > 0xFFFF ? 0xFFFF : s_tx_drop);
    send_frame(PI_MSG_DIAG, &dg, sizeof(dg));
}

/* ── Slave mode transitions ──────────────────────────────────────────── */
void enter_slave()
{
    ESP_LOGW(TAG, "Pi took control - SLAVE mode (app/agent/skills can no longer move servos)");
    robot::master::begin();          // also tells main.c's observer: voice off, ROS on screen
    s_is_slave = true;
    gpio_set_level(PI_SLAVE_OUT_PIN, 1);
    s_slave_entries++;
    send_event(PI_EV_LINK_UP, 0, 0, "slave");
}

void exit_slave(const char *why)
{
    ESP_LOGW(TAG, "Pi released control (%s) - back to STANDALONE, robot standing", why);
    s_is_slave = false;
    gpio_set_level(PI_SLAVE_OUT_PIN, 0);
    pi_audio::stop();                  // the speaker goes back to voice
    robot::master::end();
    std::snprintf(s_last_exit, sizeof(s_last_exit), "%s", why);
    send_event(PI_EV_LINK_DOWN, 0, 0, why);
}

/* ── Message handlers ────────────────────────────────────────────────── */
bool finite_all(const float *v, int n)
{
    for (int i = 0; i < n; ++i) if (!std::isfinite(v[i])) return false;
    return true;
}

void on_frame(uint8_t type, uint8_t seq, const uint8_t *p, uint8_t len)
{
    const int64_t now = esp_timer_get_time();

    // Anything with a good CRC proves the wire is alive; the heartbeat alone
    // decides whether the host is in control.
    s_last_rx_us  = now;
    s_last_rx_seq = seq;

    if (type == PI_MSG_HEARTBEAT) {
        if (len != sizeof(pi_heartbeat_t)) { s_rx_bad++; return; }
        pi_heartbeat_t hb;
        std::memcpy(&hb, p, sizeof(hb));
        s_rx_frames++;
        if (hb.proto != PI_PROTO_VERSION) {
            // Do not let a host that speaks another protocol version steer the
            // robot - every float after the header could mean something else.
            if (now - s_last_mismatch_us > 1000000) {
                s_last_mismatch_us = now;
                char t[48];
                std::snprintf(t, sizeof(t), "host proto %u, esp proto %u",
                              hb.proto, PI_PROTO_VERSION);
                ESP_LOGE(TAG, "%s", t);
                send_event(PI_EV_PROTO_MISMATCH, type, seq, t);
            }
            return;
        }
        s_last_hb_us   = now;
        s_want_control = (hb.flags & PI_HB_WANT_CONTROL) != 0;
        s_req_rate_hz  = hb.state_rate_hz;
        return;
    }

    if (type == PI_MSG_HOST_INFO) {    // display only - any link state
        if (len != sizeof(pi_host_info_t)) { s_rx_bad++; return; }
        s_rx_frames++;
        pi_host_info_t h;
        std::memcpy(&h, p, sizeof(h));
        portENTER_CRITICAL(&s_mux);
        std::memcpy(s_host_ip, h.ip, sizeof(s_host_ip));
        s_host_ip[sizeof(s_host_ip) - 1] = '\0';
        std::memcpy(s_host_name, h.hostname, sizeof(s_host_name));
        s_host_name[sizeof(s_host_name) - 1] = '\0';
        s_host_info_us = now;
        portEXIT_CRITICAL(&s_mux);
        return;
    }

    if (type == PI_MSG_VIDEO_DATA) {   // display only - any link state
        if (len <= sizeof(pi_video_hdr_t)) { s_rx_bad++; return; }
        s_rx_frames++;
#if CONFIG_MP4_RL_LINK_ENABLE
        rl_link::note_bulk();
#endif
        pi_video::chunk(p, len);
        return;
    }

    // Every other message moves the robot, and only the master may do that.
    const bool slave = (s_state == MPX_PI_LINK_SLAVE);

    switch (type) {
    case PI_MSG_CMD_VEL: {
        if (len != sizeof(pi_cmd_vel_t)) { s_rx_bad++; return; }
        s_rx_frames++;
        if (!slave) { reject(type, seq, "not in control (CMD_VEL)"); return; }
        pi_cmd_vel_t c;
        std::memcpy(&c, p, sizeof(c));
        const float v[3] = { c.vx, c.vy, c.wz };
        if (!finite_all(v, 3)) { reject(type, seq, "non-finite velocity"); return; }
        robot::master::set_velocity(c.vx, c.vy, c.wz);
        return;
    }
    case PI_MSG_SET_MODE: {
        if (len != sizeof(pi_set_mode_t)) { s_rx_bad++; return; }
        s_rx_frames++;
        if (!slave) { reject(type, seq, "not in control (SET_MODE)"); return; }
        pi_set_mode_t m;
        std::memcpy(&m, p, sizeof(m));
        switch (m.mode) {
        case PI_MODE_STAND: robot::master::send_gait_cmd(robot::GaitCmd::None);         break;
        case PI_MODE_WALK:  robot::master::send_gait_cmd(robot::GaitCmd::StanfordWalk); break;
        case PI_MODE_INIT:  robot::master::send_gait_cmd(robot::GaitCmd::Init);         break;
        case PI_MODE_GAIT: {
            char name[sizeof(m.name) + 1];
            std::memcpy(name, m.name, sizeof(m.name));
            name[sizeof(m.name)] = '\0';
            robot::GaitCmd g;
            if (!robot::gait_from_name(name, g)) {
                char t[48];
                std::snprintf(t, sizeof(t), "unknown gait '%.23s'", name);
                reject(type, seq, t);
                return;
            }
            robot::master::send_gait_cmd(g);
            break;
        }
        default:
            reject(type, seq, "unknown mode");
            return;
        }
        return;
    }
    case PI_MSG_BODY_POSE: {
        if (len != sizeof(pi_body_pose_t)) { s_rx_bad++; return; }
        s_rx_frames++;
        if (!slave) { reject(type, seq, "not in control (BODY_POSE)"); return; }
        pi_body_pose_t b;
        std::memcpy(&b, p, sizeof(b));
        const float v[4] = { b.roll, b.pitch, b.yaw, b.height };
        if (!finite_all(v, 4)) { reject(type, seq, "non-finite pose"); return; }
        if (b.height > 0.0f) robot::master::set_height_mm(b.height * 1000.0f);
        robot::master::set_body_attitude(b.roll, b.pitch, b.yaw);
        return;
    }
    case PI_MSG_AUDIO_CTRL: {
        if (len != sizeof(pi_audio_ctrl_t)) { s_rx_bad++; return; }
        s_rx_frames++;
        if (!slave) { reject(type, seq, "not in control (AUDIO)"); return; }
        pi_audio_ctrl_t a;
        std::memcpy(&a, p, sizeof(a));
        switch (a.op) {
        case PI_AUDIO_START:
            if (!pi_audio::start(a.sample_rate, a.volume)) reject(type, seq, "audio: need 16000 Hz / no speaker");
            break;
        case PI_AUDIO_STOP:   pi_audio::stop(); break;
        case PI_AUDIO_END:    pi_audio::end(); break;
        case PI_AUDIO_VOLUME: pi_audio::set_volume(a.volume); break;
        default: reject(type, seq, "audio: unknown op"); break;
        }
        return;
    }
    case PI_MSG_AUDIO_DATA: {
        if (len == 0 || (len & 1)) { s_rx_bad++; return; }
        s_rx_frames++;
#if CONFIG_MP4_RL_LINK_ENABLE
        rl_link::note_bulk();
#endif
        if (!slave) return;            // quietly: up to 170 of these a second
        pi_audio::push(p, len);
        return;
    }
    case PI_MSG_FOOT_TARGETS: {
        if (len != sizeof(pi_foot_targets_t)) { s_rx_bad++; return; }
        s_rx_frames++;
        if (!slave) { reject(type, seq, "not in control (FOOT_TARGETS)"); return; }
        pi_foot_targets_t ft;
        std::memcpy(&ft, p, sizeof(ft));
        if (!finite_all(&ft.xyz[0][0], 12)) { reject(type, seq, "non-finite feet"); return; }
        robot::master::set_foot_targets(ft.xyz);
        return;
    }
    default:
        s_rx_bad++;
        return;
    }
}

void feed(uint8_t b)
{
    Parser &p = s_p;
    switch (p.st) {
    case Ps::Sync0:
        if (b == PI_SYNC0) p.st = Ps::Sync1;
        break;
    case Ps::Sync1:
        if (b == PI_SYNC1)      p.st = Ps::Len;
        else if (b != PI_SYNC0) p.st = Ps::Sync0;   // A5 A5 5A still syncs
        break;
    case Ps::Len:
        if (b > PI_MAX_PAYLOAD) { s_rx_bad++; p.st = Ps::Sync0; break; }
        p.len = b;
        p.st  = Ps::Type;
        break;
    case Ps::Type:
        p.type = b;
        p.st   = Ps::Seq;
        break;
    case Ps::Seq:
        p.seq = b;
        p.idx = 0;
        p.st  = p.len ? Ps::Payload : Ps::CrcL;
        break;
    case Ps::Payload:
        p.buf[p.idx++] = b;
        if (p.idx >= p.len) p.st = Ps::CrcL;
        break;
    case Ps::CrcL:
        p.crc_rx = b;
        p.st     = Ps::CrcH;
        break;
    case Ps::CrcH: {
        p.crc_rx |= static_cast<uint16_t>(b) << 8;
        const uint8_t hdr[3] = { p.len, p.type, p.seq };
        uint16_t crc = pi_crc16(hdr, 3, 0xFFFF);
        crc = pi_crc16(p.buf, p.len, crc);
        if (crc == p.crc_rx) on_frame(p.type, p.seq, p.buf, p.len);
        else                 s_rx_crc_err++;
        p.st = Ps::Sync0;
        break;
    }
    }
}

/* ── The Pi's IP for the face: tell the observer when it changes ─────── */
/* Fresh HOST_INFO alone is enough, in any link state. pi_announce on the Pi
 * sends nothing else (no heartbeat), so it can show the Pi's address without
 * ever touching who controls the robot: a heartbeat of its own, interleaved
 * with the ROS bridge's, flipped the robot in and out of slave mode. */
bool host_info_fresh_locked(int64_t now)
{
    return s_host_info_us != 0 && now - s_host_info_us < HOST_INFO_STALE_US;
}

void tell_ip(int64_t now)
{
    char ip[16];
    portENTER_CRITICAL(&s_mux);
    std::snprintf(ip, sizeof(ip), "%s", host_info_fresh_locked(now) ? s_host_ip : "");
    portEXIT_CRITICAL(&s_mux);
    if (std::strcmp(ip, s_ip_told) == 0) return;
    std::snprintf(s_ip_told, sizeof(s_ip_told), "%s", ip);
    if (ip[0]) ESP_LOGI(TAG, "Pi is at %s", ip);
    else       ESP_LOGI(TAG, "Pi address gone");
    if (s_ip_cb) s_ip_cb(ip);
}

/* ── State machine ───────────────────────────────────────────────────── */
bool update_state(int64_t now)
{
    const bool pin = gpio_get_level(PI_HOST_PRESENT_PIN) != 0;
    if (pin) s_pin_low_count = 0;
    else if (s_pin_low_count < 1000) s_pin_low_count++;

#if CONFIG_MP4_PI_LINK_REQUIRE_HOST_PIN
    // Entering needs the line high now; staying needs it not to have been low
    // for PIN_LOW_SAMPLES loops, so one glitch does not drop a walking robot.
    const bool pin_ok = (s_state == MPX_PI_LINK_SLAVE)
                        ? (s_pin_low_count < PIN_LOW_SAMPLES)
                        : pin;
#else
    const bool pin_ok = true;
#endif

    const bool hb = s_last_hb_us != 0 && (now - s_last_hb_us) < TIMEOUT_US;

    mpx_pi_link_state_t want = MPX_PI_LINK_STANDALONE;
    if (hb) want = (s_want_control && pin_ok) ? MPX_PI_LINK_SLAVE : MPX_PI_LINK_MONITOR;
#if CONFIG_MP4_RL_LINK_ENABLE
    // An RL policy driving the joints (rl_link.cc) and the ROS bridge cannot
    // both own the robot. Whoever was first keeps it: a running RL session
    // keeps the bridge in monitor, and a slave robot refuses the policy's ARM.
    if (want == MPX_PI_LINK_SLAVE && s_state != MPX_PI_LINK_SLAVE && rl_link::session_active())
        want = MPX_PI_LINK_MONITOR;
#endif

    if (want == MPX_PI_LINK_SLAVE && s_state != MPX_PI_LINK_SLAVE) {
        s_state = want;            // before enter_slave(): the event reports it
        enter_slave();
    } else if (want != MPX_PI_LINK_SLAVE && s_state == MPX_PI_LINK_SLAVE) {
        s_state = want;
        exit_slave(!hb ? "heartbeat timeout"
                       : !pin_ok ? "host pin low"
                                 : "host released control");
    } else {
        s_state = want;
    }

    portENTER_CRITICAL(&s_mux);
    s_status.state         = s_state;
    s_status.host_pin      = pin;
    s_status.heartbeat     = hb;
    s_status.want_control  = s_want_control;
    s_status.ms_since_rx   = s_last_rx_us ? static_cast<uint32_t>((now - s_last_rx_us) / 1000)
                                          : 0xFFFFFFFFu;
    s_status.rx_frames     = s_rx_frames;
    s_status.rx_crc_err    = s_rx_crc_err;
    s_status.rx_bad        = s_rx_bad;
    s_status.tx_frames     = s_tx_frames;
    s_status.tx_drop       = s_tx_drop;
    s_status.slave_entries = s_slave_entries;
    s_status.state_rate_hz = hb ? state_rate_hz() : 0;
    std::memcpy(s_status.last_exit_reason, s_last_exit, sizeof(s_last_exit));
    portEXIT_CRITICAL(&s_mux);

    return pin;
}

void link_task(void *)
{
    static uint8_t rx[256];
    int64_t next_state = 0, next_info = 0, next_diag = 0;

    for (;;) {
        // Wait for one byte, then take whatever else has arrived. Asking for a
        // full buffer would hold a 36-byte RL SET_ALL until the 10 ms timeout:
        // two of the policy's 5 ms ticks late (mp4-link learned this too).
        int n = uart_read_bytes(PI_UART_PORT, rx, 1, RX_WAIT);
        if (n > 0) {
            size_t more = 0;
            uart_get_buffered_data_len(PI_UART_PORT, &more);
            if (more > sizeof(rx) - 1) more = sizeof(rx) - 1;
            if (more) {
                const int m = uart_read_bytes(PI_UART_PORT, rx + 1, more, 0);
                if (m > 0) n += m;
            }
        }
        for (int i = 0; i < n; ++i) feed(rx[i]);
#if CONFIG_MP4_RL_LINK_ENABLE
        if (n > 0) rl_link::feed(rx, n);
#endif

        const int64_t now = esp_timer_get_time();
        const bool pin = update_state(now);
        tell_ip(now);

        // Only talk while someone is listening: before the Pi has configured
        // its UART, Linux may still have a login console on it, and streaming
        // binary into a getty is how you get a terminal full of garbage.
        if (s_state == MPX_PI_LINK_STANDALONE) {
            next_state = next_info = next_diag = now;
            continue;
        }
#if CONFIG_MP4_RL_LINK_ENABLE
        // The RL policy's STATE owns the wire while it runs: no ROS telemetry
        // in between (pi_announce, say, may still be heartbeating for a moment).
        if (rl_link::driving()) {
            next_state = next_info = next_diag = now;
            continue;
        }
#endif

        if (now >= next_state) {
            send_state(pin);
            next_state += 1000000 / state_rate_hz();
            if (now - next_state > 100000) next_state = now;   // resync, don't burst
        }
        if (now >= next_info) {
            send_info();
            next_info = now + INFO_PERIOD_US;
        }
        if (now >= next_diag) {
            send_diag();
            next_diag = now + DIAG_PERIOD_US;
        }
    }
}

}  // namespace
}  // namespace pi_link

using namespace pi_link;

extern "C" esp_err_t mpx_pi_link_init(void)
{
    if (s_started) return ESP_ERR_INVALID_STATE;

    uart_config_t cfg = {};
    cfg.baud_rate  = CONFIG_MP4_PI_LINK_UART_BAUD;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(PI_UART_PORT, RX_BUF_SIZE, TX_BUF_SIZE, 0, nullptr, 0);
    if (err == ESP_OK) err = uart_param_config(PI_UART_PORT, &cfg);
    if (err == ESP_OK) err = uart_set_pin(PI_UART_PORT, PI_UART_TX_PIN, PI_UART_RX_PIN,
                                          UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART%d setup failed: %s", (int)PI_UART_PORT, esp_err_to_name(err));
        return err;
    }
    // Idle-high RX when nothing is plugged in, so a floating wire does not
    // read as a stream of break characters.
    gpio_pullup_en(PI_UART_RX_PIN);

    // HOST_PRESENT: input, pulled DOWN - no Pi (or a Pi that has not set the
    // line up yet) reads as "not present".
    gpio_config_t in = {};
    in.pin_bit_mask = 1ULL << PI_HOST_PRESENT_PIN;
    in.mode         = GPIO_MODE_INPUT;
    in.pull_up_en   = GPIO_PULLUP_DISABLE;
    in.pull_down_en = GPIO_PULLDOWN_ENABLE;
    in.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&in);

    // ESP_SLAVE: output, low until the Pi actually has control.
    gpio_config_t out = {};
    out.pin_bit_mask = 1ULL << PI_SLAVE_OUT_PIN;
    out.mode         = GPIO_MODE_OUTPUT;
    out.pull_up_en   = GPIO_PULLUP_DISABLE;
    out.pull_down_en = GPIO_PULLDOWN_DISABLE;
    out.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&out);
    gpio_set_level(PI_SLAVE_OUT_PIN, 0);

    s_status.ms_since_rx = 0xFFFFFFFFu;
#if CONFIG_MP4_RL_LINK_ENABLE
    rl_link::init();   // before the link task can feed it
#endif
    if (xTaskCreatePinnedToCore(link_task, "pi_link", 4096, nullptr, 9, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "failed to start the link task");
        return ESP_ERR_NO_MEM;
    }
    s_started = true;

    ESP_LOGI(TAG, "Pi link on UART%d TX=IO%d RX=IO%d @%d baud, host-present IO%d, slave-out IO%d, timeout %d ms",
             (int)PI_UART_PORT, (int)PI_UART_TX_PIN, (int)PI_UART_RX_PIN,
             CONFIG_MP4_PI_LINK_UART_BAUD, (int)PI_HOST_PRESENT_PIN, (int)PI_SLAVE_OUT_PIN,
             CONFIG_MP4_PI_LINK_TIMEOUT_MS);
    return ESP_OK;
}

extern "C" bool mpx_pi_link_is_slave(void)
{
    return s_is_slave;
}

extern "C" void mpx_pi_link_get_status(mpx_pi_link_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_status;
    portEXIT_CRITICAL(&s_mux);
    if (!s_started) out->ms_since_rx = 0xFFFFFFFFu;
    const pi_audio::Stats a = pi_audio::stats();
    out->audio_playing     = a.playing;
    out->audio_buffered_ms = a.buffered_ms;
    out->audio_underruns   = a.underruns;
    const pi_video::Stats v = pi_video::stats();
    out->video_frames_shown   = v.frames_shown;
    out->video_frames_dropped = v.frames_dropped;
    out->video_decode_errors  = v.decode_errors;
#if CONFIG_MP4_RL_LINK_ENABLE
    const rl_link::Stats r = rl_link::stats();
    out->rl_session     = r.session;
    out->rl_robot       = r.robot;
    out->rl_armed       = r.armed;
    out->rl_flags       = r.flags;
    out->rl_fault_servo = r.fault_servo;
    out->rl_frames      = r.frames;
    out->rl_crc_err     = r.crc_err;
    out->rl_sessions    = r.sessions;
    out->rl_ticks       = r.ticks;
#endif
}

extern "C" const char *mpx_pi_link_state_name(mpx_pi_link_state_t s)
{
    switch (s) {
    case MPX_PI_LINK_STANDALONE: return "standalone";
    case MPX_PI_LINK_MONITOR:    return "monitor";
    case MPX_PI_LINK_SLAVE:      return "slave";
    }
    return "?";
}

extern "C" const char *mpx_pi_link_state_str(void)
{
    mpx_pi_link_state_t st;
    portENTER_CRITICAL(&s_mux);
    st = s_status.state;
    portEXIT_CRITICAL(&s_mux);
    return mpx_pi_link_state_name(st);
}

extern "C" bool mpx_pi_link_host_info(char *ip, size_t ip_len, char *hostname, size_t hostname_len)
{
    const int64_t now = esp_timer_get_time();
    bool fresh;
    portENTER_CRITICAL(&s_mux);
    fresh = host_info_fresh_locked(now);
    if (ip && ip_len) std::snprintf(ip, ip_len, "%s", fresh ? s_host_ip : "");
    if (hostname && hostname_len) std::snprintf(hostname, hostname_len, "%s", fresh ? s_host_name : "");
    portEXIT_CRITICAL(&s_mux);
    return fresh;
}

extern "C" bool mpx_pi_link_ip(char *out, size_t out_size)
{
    char ip[16] = "";
    const bool fresh = mpx_pi_link_host_info(ip, sizeof(ip), nullptr, 0);
    if (out && out_size) std::snprintf(out, out_size, "%s", ip);
    return fresh && ip[0];
}

extern "C" void mpx_pi_link_set_ip_observer(void (*cb)(const char *ip))
{
    s_ip_cb = cb;
}

extern "C" void mpx_pi_link_set_video_sink(esp_err_t (*sink)(const uint8_t *jpeg, size_t len))
{
    pi_video::set_sink(sink);
}

extern "C" int mpx_pi_link_status_json(char *buf, size_t size)
{
    mpx_pi_link_status_t st;
    char pi_ip[16], pi_name[32];

    mpx_pi_link_get_status(&st);
    mpx_pi_link_host_info(pi_ip, sizeof(pi_ip), pi_name, sizeof(pi_name));
    for (char &c : pi_name) if (c == '"' || c == '\\' || (c > 0 && c < 0x20)) c = '_';
    for (char &c : st.last_exit_reason) if (c == '"' || c == '\\' || (c > 0 && c < 0x20)) c = '_';

    // mpv2's GET /v1/pi/status fields first, then this firmware's extras.
    const int n = std::snprintf(buf, size,
        R"({"state":"%s","in_control":%s,"host_pin":%s,"heartbeat":%s,"want_control":%s,)"
        R"("ms_since_rx":%ld,"state_rate_hz":%u,"rx_frames":%lu,"rx_crc_err":%lu,"rx_bad":%lu,)"
        R"("tx_frames":%lu,"tx_drop":%lu,"slave_entries":%lu,"last_exit_reason":"%s",)"
        R"("pi_ip":"%s","pi_hostname":"%s",)"
        R"("audio":{"playing":%s,"buffered_ms":%lu,"underruns":%lu},)"
        R"("video":{"shown":%lu,"dropped":%lu,"errors":%lu},)"
        R"("rl":{"session":%s,"robot":%s,"armed":%s,"flags":%u,"fault_servo":%u,)"
        R"("frames":%lu,"crc_err":%lu,"sessions":%lu,"ticks":%lu}})",
        mpx_pi_link_state_name(st.state),
        robot::master_active() ? "true" : "false",
        st.host_pin ? "true" : "false",
        st.heartbeat ? "true" : "false",
        st.want_control ? "true" : "false",
        st.ms_since_rx == 0xFFFFFFFFu ? -1L : static_cast<long>(st.ms_since_rx),
        static_cast<unsigned>(st.state_rate_hz),
        static_cast<unsigned long>(st.rx_frames),
        static_cast<unsigned long>(st.rx_crc_err),
        static_cast<unsigned long>(st.rx_bad),
        static_cast<unsigned long>(st.tx_frames),
        static_cast<unsigned long>(st.tx_drop),
        static_cast<unsigned long>(st.slave_entries),
        st.last_exit_reason, pi_ip, pi_name,
        st.audio_playing ? "true" : "false",
        static_cast<unsigned long>(st.audio_buffered_ms),
        static_cast<unsigned long>(st.audio_underruns),
        static_cast<unsigned long>(st.video_frames_shown),
        static_cast<unsigned long>(st.video_frames_dropped),
        static_cast<unsigned long>(st.video_decode_errors),
        st.rl_session ? "true" : "false",
        st.rl_robot ? "true" : "false",
        st.rl_armed ? "true" : "false",
        static_cast<unsigned>(st.rl_flags),
        static_cast<unsigned>(st.rl_fault_servo),
        static_cast<unsigned long>(st.rl_frames),
        static_cast<unsigned long>(st.rl_crc_err),
        static_cast<unsigned long>(st.rl_sessions),
        static_cast<unsigned long>(st.rl_ticks));
    return (n < 0 || static_cast<size_t>(n) >= size) ? -1 : n;
}

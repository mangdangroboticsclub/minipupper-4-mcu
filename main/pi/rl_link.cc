/* rl_link.cc -- mp4-link's host link (the mp3-link protocol) in mpx-dog.
 *
 * Ported verbatim from mpv2-claw components/mpx_pi_link/src/rl_link.cc;
 * only the include paths and the Kconfig prefix (APP_ for MP4_) differ.
 *
 * A policy on the Raspberry Pi (python -m mp3link, or anything built on
 * mp3link.Link) drives all twelve joints itself: it sends SET_ALL frames with
 * raw AT32 targets and reads a STATE frame back every tick - positions,
 * currents, housing temperatures, the IMU and the link's flags. The frames,
 * the flags, the watchdog and the guard are mp4-link's (PROTOCOL.md there,
 * RL_LINK.md here), byte for byte, so a policy that walks on mp4-link walks on
 * this firmware without a change beyond the port and the baud rate.
 *
 * What differs from mp4-link, and why:
 *   - One port. The frames share UART1 with mpv2's ROS link (pi_link.cc reads
 *     the UART and hands every byte to feed() as well as to its own parser;
 *     the two framings cannot be mistaken for each other once the CRC is
 *     checked). The USB port is this firmware's console, so no second host.
 *   - A session. mp4-link is nothing but the link; here the robot also walks,
 *     talks and runs skills. The first RL frame starts a session:
 *     robot::host::begin() parks the gait and fences the app, the agent and
 *     skills off, exactly as the ROS bridge's slave mode does. With no RL frame
 *     for APP_RL_RELEASE_MS the session ends and the legs glide back into the
 *     stand (robot::host::end()). While the ROS bridge holds the robot, RL
 *     frames are ignored and unanswered; while an RL session runs, the bridge
 *     cannot take the robot (pi_link.cc keeps it in monitor).
 *   - Frames must make sense as well as pass the CRC (a request's command and
 *     servo, SET_ALL's version, flags and enable mask): the ROS link's audio
 *     and video share these bytes, and 1 in 65536 of its windows will match a
 *     CRC.
 *   - Requests that need the boards, and ARM, wait (up to 300 ms) until the
 *     session holds the robot and the boards have answered one of its frames;
 *     a session that is ending accepts no ARM.
 *   - HOLD keeps positions only: a channel a skill left in torque mode goes
 *     limp instead of being replayed outside the guard.
 *   - Before the first ARM of a session the servos HOLD the pose the gait last
 *     sent instead of going limp, so pinging the link does not drop a standing
 *     robot. Any ARM, DISARM, IDLE, watchdog or guard trip ends the hold; from
 *     then on it is mp4-link's rule: disarmed means every servo IDLE.
 *   - The IMU is the robot's (robot/imu.cc, 200 Hz, raw BMI270 axes like
 *     mp4-link's 400 Hz sampler). A sample counts as current for 30 ms, not
 *     20, so a late I2C read never clears IMU_OK (a policy runner stops on
 *     the first frame without it).
 *   - The servo gains are saved when a session takes the robot and written
 *     back when it ends: `mp3link config` changes them in the boards' RAM and
 *     the walk was tuned on the others.
 *   - Calibration offsets are the gait's own offset[] (degrees, the same
 *     511 + offset x 10240 / scale stand as minipupperesp). SAVE_OFFSETS
 *     writes them here and into minipupperesp's "parameter" namespace.
 *   - The tick does not reconfigure the task watchdog to reboot on a 500 ms
 *     stall: in a firmware with an agent, Wi-Fi and audio that is not a
 *     call one module gets to make. A stalled tick leaves the boards holding
 *     their last frame, as mp4-link's boards do between a stall and its
 *     reboot.
 *
 * Tasks: feed() runs in the pi_link task (core 0) and only parses, answers
 * per-servo requests and copies SET_ALL into s_ctl. The tick runs in its own
 * task on core 1, woken by an esp_timer at APP_RL_TICK_HZ (between sessions
 * it returns at once), and is the only writer of the servo frames and STATE.
 */
#include "pi/rl_link.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "sdkconfig.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "robot/board_config.h"
#include "robot/driver_board.h"
#include "robot/robot.h"

/* Fallbacks for an sdkconfig generated before these options existed. */
#ifndef CONFIG_APP_RL_TICK_HZ
#define CONFIG_APP_RL_TICK_HZ 200
#endif
#ifndef CONFIG_APP_RL_WATCHDOG_MS
#define CONFIG_APP_RL_WATCHDOG_MS 100
#endif
#ifndef CONFIG_APP_RL_RELEASE_MS
#define CONFIG_APP_RL_RELEASE_MS 2000
#endif
#ifndef CONFIG_APP_RL_GUARD_CUR_MA
#define CONFIG_APP_RL_GUARD_CUR_MA 500
#endif
#ifndef CONFIG_APP_RL_GUARD_CUR_MS
#define CONFIG_APP_RL_GUARD_CUR_MS 500
#endif
#ifndef CONFIG_APP_RL_GUARD_TEMP_DC
#define CONFIG_APP_RL_GUARD_TEMP_DC 600
#endif
#ifndef CONFIG_APP_RL_GUARD_TEMP_MS
#define CONFIG_APP_RL_GUARD_TEMP_MS 1000
#endif

namespace rl_link {
namespace {

const char *TAG = "rl_link";

constexpr int64_t TICK_US          = 1000000 / CONFIG_APP_RL_TICK_HZ;
constexpr int64_t WDOG_US          = int64_t(CONFIG_APP_RL_WATCHDOG_MS) * 1000;
constexpr int64_t RELEASE_US       = int64_t(CONFIG_APP_RL_RELEASE_MS) * 1000;
constexpr int     GUARD_CUR_MA     = CONFIG_APP_RL_GUARD_CUR_MA;
constexpr int64_t GUARD_CUR_US     = int64_t(CONFIG_APP_RL_GUARD_CUR_MS) * 1000;
constexpr int     GUARD_TEMP_DC    = CONFIG_APP_RL_GUARD_TEMP_DC;
constexpr int64_t GUARD_TEMP_US    = int64_t(CONFIG_APP_RL_GUARD_TEMP_MS) * 1000;
constexpr int     BOARD_LOST_TICKS = 20;      // an enabled servo's board silent this long -> fault
constexpr int64_t IMU_MAX_AGE_US   = 30000;   // see the header
constexpr int64_t SESSION_WAIT_US  = 1500000; // a request waits this long for the robot (taking it reads the gains, ~0.2 s)
constexpr float   OFFSET_MAX_DEG   = 45.0f;

/* ── Frames (mp4-link PROTOCOL.md) ───────────────────────────────────── */
constexpr uint8_t MAGIC_REQ     = 0x42;   // 'B'
constexpr uint8_t MAGIC_RSP     = 0x62;   // 'b'
constexpr uint8_t MAGIC_SET_ALL = 0x43;   // 'C'
constexpr uint8_t MAGIC_STATE   = 0x63;   // 'c'
constexpr uint8_t PROTO_VERSION = 1;

#pragma pack(push, 1)
struct bam_req_t { uint8_t magic, cmd, servo, pad; uint16_t arg1, arg2; uint16_t crc; };
struct bam_rsp_t { uint8_t magic, cmd, servo, status; uint16_t pos_dd; int16_t cur_mA; float val; uint16_t crc; };
struct set_all_t {
    uint8_t  magic, version;
    uint16_t seq;
    uint16_t flags;          // SA_*
    uint16_t enable;         // bit i: servo i+1 in position mode, else IDLE
    uint16_t cur_cap_mA;
    uint16_t pos_dd[12];     // raw AT32 deci-degrees, by servo id
    uint16_t crc;
};
struct state_t {
    uint8_t  magic, version;
    uint16_t seq;            // last SET_ALL received
    uint32_t tick;
    uint32_t t_us;           // ESP32 time of the exchange the feedback belongs to
    uint16_t flags;          // ST_*
    uint8_t  fault_servo;
    uint8_t  boards;         // bit b: board b gave fresh feedback this tick
    uint16_t pos_dd[12];
    int16_t  cur_mA[12];
    int16_t  temp_dC[12];
    int16_t  acc_mg[3];
    int16_t  gyr_ddps[3];
    uint32_t imu_t_us;
    uint16_t crc;
};
#pragma pack(pop)
static_assert(sizeof(bam_req_t) == 10, "bam_req_t size");
static_assert(sizeof(bam_rsp_t) == 14, "bam_rsp_t size");
static_assert(sizeof(set_all_t) == 36, "set_all_t size");
static_assert(sizeof(state_t) == 106, "state_t size");

constexpr uint16_t SA_ARM    = 1u << 0;
constexpr uint16_t SA_DISARM = 1u << 1;
constexpr uint16_t SA_CLEAR  = 1u << 2;

constexpr uint16_t ST_ARMED         = 1u << 0;
constexpr uint16_t ST_WATCHDOG      = 1u << 1;
constexpr uint16_t ST_FAULT_CUR     = 1u << 2;
constexpr uint16_t ST_FAULT_TEMP    = 1u << 3;
constexpr uint16_t ST_FAULT_BOARD   = 1u << 4;
constexpr uint16_t ST_IMU_OK        = 1u << 5;
constexpr uint16_t ST_BOARD_MISSING = 1u << 6;
constexpr uint16_t ST_REJECTED      = 1u << 7;
constexpr uint16_t ST_FAULTS        = ST_FAULT_CUR | ST_FAULT_TEMP | ST_FAULT_BOARD;

enum : uint8_t {
    CMD_PING = 0, CMD_SET_POS = 1, CMD_IDLE = 2, CMD_TORQUE = 3, CMD_SET_KP = 4,
    CMD_SET_KD = 5, CMD_GET_PARAM = 6, CMD_GET_LIVE = 7, CMD_SET_SCALE = 8,
    CMD_RAW_REPLY = 9, CMD_SET_PARAM = 10, CMD_GET_STATUS = 11,
    CMD_GET_OFFSET = 12, CMD_SET_OFFSET = 13, CMD_SAVE_OFFSETS = 14,
};
constexpr uint8_t BAM_OK          = 0;
constexpr uint8_t BAM_ERR_SERVO   = 1;
constexpr uint8_t BAM_ERR_ARG     = 2;
constexpr uint8_t BAM_ERR_BOARD   = 3;
constexpr uint8_t BAM_ERR_CMD     = 5;
constexpr uint8_t BAM_ERR_REFUSED = 6;   // armed, a motion command, or the ROS bridge holds the robot
constexpr uint8_t BAM_ERR_NVS     = 8;

/* ── Shared state (s_mux) ────────────────────────────────────────────── */
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
struct {
    bool     armed;
    bool     hold;           // this session has not armed yet: servos hold the parked pose
    uint16_t latched;        // ST_WATCHDOG | ST_FAULTS | ST_REJECTED
    uint8_t  fault_servo;
    uint16_t seq, enable, cur_cap;
    uint16_t pos_dd[12];
    int64_t  last_set_all_us;
} s_ctl = {};

int64_t  s_last_frame_us = 0;    // any valid RL frame
bool     s_want      = false;    // a frame arrived since the last session ended
volatile bool s_session = false; // written by the tick task only
volatile bool s_robot   = false; // robot::host::begin() succeeded for this session
/* The robot is ours AND the boards have answered a host frame, so feedback is
 * this session's. Set by the tick after its second sync; cleared under s_mux
 * in the same critical section that decides a session is over, so an ARM
 * cannot land on a robot that is being handed back. */
volatile bool s_ready   = false;
int      s_syncs = 0;            // tick task: host frames sent this session
int64_t  s_last_bulk_us = -1000000000;   // pi_link task: last ROS audio/video frame
constexpr int64_t BULK_QUIET_US = 1000000;
uint16_t s_last_flags = 0;
uint32_t s_frames = 0, s_crc_err = 0, s_sessions = 0, s_ticks = 0;

TaskHandle_t       s_tick_task = nullptr;
esp_timer_handle_t s_timer     = nullptr;

/* Offsets set over the link wait here until SAVE_OFFSETS (s_mux). */
float s_off_pending[13];
bool  s_off_dirty[13];

/* Session state between sessions: what a new session starts from. Set at
 * init and when a session ends, never when one begins, so the frames that
 * open a session (an IDLE, a DISARM, a SET_OFFSET) are not undone by it. */
void reset_ctl_locked()
{
    s_ctl.armed = false;
    s_ctl.hold  = true;
    s_ctl.latched = 0;
    s_ctl.fault_servo = 0;
    s_ctl.enable = 0;
    s_ctl.cur_cap = 0;
    for (int i = 1; i <= 12; i++) s_off_dirty[i] = false;
}

/* The ROS bridge (robot::master::begin()) owns the robot. RL frames are then
 * ignored outright, unanswered: the bridge's own traffic on this UART is what
 * they would most likely be, a 1-in-65536 CRC match in its audio or video. */
bool ros_holds()
{
    return robot::master_active() && !robot::host::active();
}

/* ── CRC16-CCITT-FALSE (init 0xFFFF, poly 0x1021, MSB first) ─────────── */
uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= static_cast<uint16_t>(p[i]) << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000u) ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
                                  : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

void disarm_locked(uint16_t why, uint8_t servo)
{
    s_ctl.armed = false;
    s_ctl.hold  = false;
    s_ctl.latched |= why;
    if (servo && !s_ctl.fault_servo) s_ctl.fault_servo = servo;
}

bool is_armed()
{
    portENTER_CRITICAL(&s_mux);
    const bool a = s_ctl.armed;
    portEXIT_CRITICAL(&s_mux);
    return a;
}

/* A frame arrived: keep (or start, on the next tick) the session. */
void note_frame()
{
    portENTER_CRITICAL(&s_mux);
    s_last_frame_us = esp_timer_get_time();
    s_want = true;
    s_frames++;
    portEXIT_CRITICAL(&s_mux);
}

/* The first frame of a session reaches us before the tick has taken the
 * robot and read the boards. Anything that needs them waits for that (once,
 * briefly): a reply built on an empty feedback cache would say "board did not
 * answer" for a board that is fine. */
bool wait_robot()
{
    const int64_t until = esp_timer_get_time() + SESSION_WAIT_US;
    while (!s_ready && esp_timer_get_time() < until) vTaskDelay(pdMS_TO_TICKS(2));
    return s_ready;
}

/* ── Per-servo requests ──────────────────────────────────────────────── */
void attach_feedback(bam_rsp_t *rsp, int servo, bool with_temp)
{
    uint16_t pos = 0;
    int16_t cur = 0, temp = 0;
    if (!driver_board_raw_feedback(servo, &pos, &cur, &temp, nullptr)) {
        if (rsp->status == BAM_OK) rsp->status = BAM_ERR_BOARD;
        return;
    }
    rsp->pos_dd = pos;
    rsp->cur_mA = cur;
    if (with_temp) {
        // mp4-link puts the board's raw res word here; its low 16 bits are the
        // housing NTC in 0.1 degC, which is all that word carries.
        const uint32_t res = static_cast<uint16_t>(temp);
        std::memcpy(&rsp->val, &res, sizeof(rsp->val));
    }
}

float f32_from(uint16_t lo, uint16_t hi)
{
    const uint32_t bits = static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 16);
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

bam_rsp_t exec_req(const bam_req_t &req)
{
    bam_rsp_t rsp = {};
    rsp.magic  = MAGIC_RSP;
    rsp.cmd    = req.cmd;
    rsp.servo  = req.servo;
    rsp.status = BAM_OK;
    const bool ok_servo = req.servo >= 1 && req.servo <= 12;

    if (req.cmd == CMD_IDLE) {
        // Any IDLE stops everything: the tick sends IDLE from its next frame.
        portENTER_CRITICAL(&s_mux);
        s_ctl.armed = false;
        s_ctl.hold  = false;
        portEXIT_CRITICAL(&s_mux);
        if (!wait_robot()) { rsp.status = BAM_ERR_REFUSED; return rsp; }   // not ours to idle
        if (ok_servo) attach_feedback(&rsp, req.servo, true);
        return rsp;
    }
    if (req.cmd == CMD_PING) {
        wait_robot();
        if (ok_servo) attach_feedback(&rsp, req.servo, true);
        return rsp;
    }
    if (req.cmd == CMD_SET_POS || req.cmd == CMD_TORQUE || is_armed()) {
        rsp.status = BAM_ERR_REFUSED;   // motion goes through SET_ALL, under the watchdog
        return rsp;
    }

    switch (req.cmd) {
    /* The offset commands need neither the boards nor the robot: they work
     * with the servos unpowered, as on mp4-link. */
    case CMD_GET_OFFSET: {
        if (!ok_servo) { rsp.status = BAM_ERR_SERVO; return rsp; }
        portENTER_CRITICAL(&s_mux);
        const bool dirty = s_off_dirty[req.servo];
        const float pend = s_off_pending[req.servo];
        portEXIT_CRITICAL(&s_mux);
        rsp.val = dirty ? pend : robot::get_offset(req.servo);
        return rsp;
    }
    case CMD_SET_OFFSET: {
        if (!ok_servo) { rsp.status = BAM_ERR_SERVO; return rsp; }
        const float v = f32_from(req.arg1, req.arg2);
        if (!(v >= -OFFSET_MAX_DEG && v <= OFFSET_MAX_DEG)) { rsp.status = BAM_ERR_ARG; return rsp; }
        portENTER_CRITICAL(&s_mux);
        s_off_pending[req.servo] = v;
        s_off_dirty[req.servo]   = true;
        portEXIT_CRITICAL(&s_mux);
        rsp.val = v;
        return rsp;
    }
    case CMD_SAVE_OFFSETS: {
        // Every offset, as mp4-link writes every one that is set (here all
        // twelve always are): the ones set over the link, the rest as in use.
        float v[13];
        portENTER_CRITICAL(&s_mux);
        for (int i = 1; i <= 12; i++) v[i] = s_off_dirty[i] ? s_off_pending[i] : -1000.0f;
        portEXIT_CRITICAL(&s_mux);
        int n = 0;
        bool ok = true;
        for (int i = 1; i <= 12; i++) {
            const float deg = v[i] > -999.0f ? v[i] : robot::get_offset(i);
            if (robot::host::store_offset(i, deg)) n++;
            else ok = false;
        }
        portENTER_CRITICAL(&s_mux);
        for (int i = 1; i <= 12; i++) s_off_dirty[i] = false;
        portEXIT_CRITICAL(&s_mux);
        if (!ok) rsp.status = BAM_ERR_NVS;
        rsp.val = static_cast<float>(n);
        return rsp;
    }
    case CMD_SET_SCALE: {
        // The scale is the boards' own range_position_deg, read at boot; the
        // gait depends on it, so the host may read it but not change it.
        const float now = driver_board_get_scale_dd();
        rsp.val = now;
        if (req.arg1 != 0 && std::fabs(static_cast<float>(req.arg1) - now) > 0.5f)
            rsp.status = BAM_ERR_REFUSED;
        return rsp;
    }
    case CMD_RAW_REPLY:
        rsp.status = BAM_ERR_CMD;       // a bench diagnostic this driver does not keep
        return rsp;
    default:
        break;
    }

    // Everything below talks to the boards' configuration.
    if (!wait_robot()) { rsp.status = BAM_ERR_REFUSED; return rsp; }

    switch (req.cmd) {
    case CMD_SET_KP:
    case CMD_SET_KD: {
        if (!ok_servo) { rsp.status = BAM_ERR_SERVO; break; }
        const float v = f32_from(req.arg1, req.arg2);
        const int pid = (req.cmd == CMD_SET_KP) ? DB_PARAM_KP_POSITION : DB_PARAM_KD_POSITION;
        if (!driver_board_set_param(req.servo, pid, v)) rsp.status = BAM_ERR_BOARD;
        else rsp.val = v;
        break;
    }
    case CMD_GET_PARAM: {
        if (!ok_servo) { rsp.status = BAM_ERR_SERVO; break; }
        if (req.arg1 >= DB_PARAM_COUNT) { rsp.status = BAM_ERR_ARG; break; }
        float v = 0.0f;
        if (!driver_board_get_param(req.servo, req.arg1, &v)) rsp.status = BAM_ERR_BOARD;
        else rsp.val = v;
        break;
    }
    case CMD_SET_PARAM: {
        // RAM only, nothing saved to the AT32 flash. The id travels in the pad
        // byte so both float halves fit in arg1 and arg2.
        if (!ok_servo) { rsp.status = BAM_ERR_SERVO; break; }
        if (req.pad >= DB_PARAM_COUNT) { rsp.status = BAM_ERR_ARG; break; }
        const float v = f32_from(req.arg1, req.arg2);
        float rb = 0.0f;
        if (!driver_board_set_param(req.servo, req.pad, v) ||
            !driver_board_get_param(req.servo, req.pad, &rb))
            rsp.status = BAM_ERR_BOARD;
        else
            rsp.val = rb;
        break;
    }
    case CMD_GET_LIVE: {
        if (!ok_servo) { rsp.status = BAM_ERR_SERVO; break; }
        if (req.arg1 > DB_LIVE_SUPPLY_CURRENT_MA) { rsp.status = BAM_ERR_ARG; break; }
        float v = 0.0f;
        if (!driver_board_get_live(req.servo, req.arg1, &v)) rsp.status = BAM_ERR_BOARD;
        else rsp.val = v;
        break;
    }
    case CMD_GET_STATUS: {
        if (!ok_servo) { rsp.status = BAM_ERR_SERVO; break; }
        uint16_t st = 0;
        if (!driver_board_raw_feedback(req.servo, nullptr, nullptr, nullptr, &st)) {
            rsp.status = BAM_ERR_BOARD;
            break;
        }
        rsp.val = static_cast<float>(st);
        break;
    }
    default:
        rsp.status = BAM_ERR_CMD;
        return rsp;
    }
    if (ok_servo && rsp.status == BAM_OK) attach_feedback(&rsp, req.servo, false);
    return rsp;
}

void handle_set_all(const set_all_t &f)
{
    // An ARM needs the robot, which the tick takes on the session's first
    // tick; wait for that rather than reject the very first frame.
    if ((f.flags & SA_ARM) && !(f.flags & SA_DISARM)) wait_robot();

    portENTER_CRITICAL(&s_mux);
    s_ctl.last_set_all_us = esp_timer_get_time();
    s_ctl.seq     = f.seq;
    s_ctl.enable  = f.enable & 0x0FFF;
    s_ctl.cur_cap = f.cur_cap_mA;
    std::memcpy(s_ctl.pos_dd, f.pos_dd, sizeof(s_ctl.pos_dd));
    if (f.flags & SA_CLEAR) {
        s_ctl.latched = 0;
        s_ctl.fault_servo = 0;
    }
    if (f.flags & SA_DISARM) {
        s_ctl.armed = false;
        s_ctl.hold  = false;
    } else if (f.flags & SA_ARM) {
        if ((s_ctl.latched & ST_FAULTS) || !s_ready) {
            s_ctl.latched |= ST_REJECTED;
        } else {
            s_ctl.armed = true;
            s_ctl.hold  = false;
            s_ctl.latched &= static_cast<uint16_t>(~(ST_WATCHDOG | ST_REJECTED));
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

/* ── Receive: resync on magic + CRC ──────────────────────────────────── */
struct Rx { uint8_t buf[128]; size_t len; };
Rx s_rx = {};

size_t frame_len(uint8_t magic)
{
    if (magic == MAGIC_REQ)     return sizeof(bam_req_t);
    if (magic == MAGIC_SET_ALL) return sizeof(set_all_t);
    return 0;
}

void rx_drop(size_t n)
{
    std::memmove(s_rx.buf, s_rx.buf + n, s_rx.len - n);
    s_rx.len -= n;
}

void rx_process()
{
    while (s_rx.len > 0) {
        const size_t need = frame_len(s_rx.buf[0]);
        if (!need) { rx_drop(1); continue; }
        if (s_rx.len < need) return;
        uint16_t crc;
        std::memcpy(&crc, s_rx.buf + need - 2, 2);
        if (crc16(s_rx.buf, need - 2) != crc) {   // resync one byte on
            s_crc_err++;
            rx_drop(1);
            continue;
        }
        // Opening a session takes more than a CRC: about 1 in 65536 windows of
        // the ROS link's own bytes (its video and audio share this UART) will
        // match one. Inside a session frames are taken as mp4-link takes them,
        // with its status codes and its trimming.
        const bool open = session_active();
        if (s_rx.buf[0] == MAGIC_REQ) {
            bam_req_t req;
            std::memcpy(&req, s_rx.buf, sizeof(req));
            if (!open && (req.cmd > CMD_SAVE_OFFSETS || req.servo > 12)) { rx_drop(1); continue; }
            rx_drop(need);
            if (!open && esp_timer_get_time() - s_last_bulk_us < BULK_QUIET_US) continue;
            if (ros_holds()) continue;
            note_frame();
            bam_rsp_t rsp = exec_req(req);
            rsp.crc = crc16(reinterpret_cast<const uint8_t *>(&rsp), sizeof(rsp) - 2);
            uart_write_bytes(PI_UART_PORT, &rsp, sizeof(rsp));
        } else {
            set_all_t f;
            std::memcpy(&f, s_rx.buf, sizeof(f));
            if (!open && ((f.flags & ~0x7u) || (f.enable & ~0x0FFFu))) { rx_drop(1); continue; }
            rx_drop(need);
            if (f.version != PROTO_VERSION) continue;   // as mp4-link: taken, ignored
            if (ros_holds()) continue;
            note_frame();
            handle_set_all(f);
        }
    }
}

/* ── Sessions (tick task only) ───────────────────────────────────────── */
void begin_session()
{
    // The session's state was reset when the last one ended (reset_ctl_locked),
    // and the frames that opened this one may already have changed it.
    const bool robot = robot::host::begin();
    driver_board_raw_invalidate();   // nothing from an earlier session is current
    s_syncs = 0;
    s_ready = false;
    s_robot = robot;
    s_session = true;
    s_sessions++;
    if (robot) {
        ESP_LOGW(TAG, "RL host session started: the Pi drives the joints (%d Hz)",
                 CONFIG_APP_RL_TICK_HZ);
    } else {
        ESP_LOGW(TAG, "RL host session started, but the ROS bridge holds the robot: "
                      "STATE only, ARM refused");   // ROS took it between frame and tick
    }
}

/* Returns false (and keeps the session) if a frame raced in. */
bool end_session(int64_t now)
{
    bool stop;
    portENTER_CRITICAL(&s_mux);
    stop = now - s_last_frame_us > RELEASE_US;
    if (stop) {
        s_want  = false;
        s_ready = false;          // from here no ARM is accepted
        reset_ctl_locked();       // the next session starts disarmed and holding
    }
    portEXIT_CRITICAL(&s_mux);
    if (!stop) return false;

    if (s_robot) {
        // Limp first, as the disarm rule says, then hand the robot back: its
        // wake path reads where the legs are and glides them up.
        uint16_t mode[12], pos[12];
        int16_t cur[12] = {};
        for (int i = 0; i < 12; i++) {
            mode[i] = DB_MODE_IDLE;
            uint16_t p = 0;
            pos[i] = driver_board_raw_feedback(i + 1, &p, nullptr, nullptr, nullptr) ? p : 1550;
        }
        driver_board_sync_raw(mode, pos, cur, nullptr, nullptr);
        robot::host::end();
    }
    s_robot = false;
    s_session = false;
    ESP_LOGW(TAG, "RL host session ended (no frame for %d ms)", CONFIG_APP_RL_RELEASE_MS);
    return true;
}

int16_t sat16(float v)
{
    if (!std::isfinite(v)) return 0;
    if (v >  32767.0f) return  32767;
    if (v < -32767.0f) return -32767;
    return static_cast<int16_t>(std::lrintf(v));
}

/* ── The tick ────────────────────────────────────────────────────────── */
void tick_cb(void *)
{
    if (s_tick_task) xTaskNotifyGive(s_tick_task);
}

void tick_task(void *)
{
    uint16_t mode[12], pos[12], target[12];
    int16_t  cur[12];
    int64_t  over_cur_us[12] = {}, over_temp_us[12] = {};
    uint16_t lost_ticks[4] = {};
    uint32_t tick = 0;
    int64_t  t_prev = esp_timer_get_time();
    state_t  st;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const int64_t now = esp_timer_get_time();

        if (!s_session) {
            bool want;
            portENTER_CRITICAL(&s_mux);
            want = s_want;
            portEXIT_CRITICAL(&s_mux);
            if (!want) continue;
            begin_session();
            std::memset(over_cur_us, 0, sizeof(over_cur_us));
            std::memset(over_temp_us, 0, sizeof(over_temp_us));
            std::memset(lost_ticks, 0, sizeof(lost_ticks));
            t_prev = esp_timer_get_time();
            continue;   // the boards answer this tick's frame on the next one
        }
        if (end_session(now)) continue;
        const int64_t dt = now - t_prev;   // real time: a late tick counts in full

        // Snapshot the command; the watchdog.
        portENTER_CRITICAL(&s_mux);
        if (s_ctl.armed && now - s_ctl.last_set_all_us > WDOG_US) disarm_locked(ST_WATCHDOG, 0);
        bool           armed  = s_ctl.armed;
        const bool     hold   = s_ctl.hold;
        const uint16_t enable = s_ctl.enable, cap = s_ctl.cur_cap, seq = s_ctl.seq;
        std::memcpy(target, s_ctl.pos_dd, sizeof(target));
        portEXIT_CRITICAL(&s_mux);

        uint8_t answered = 0, fresh = 0;
        if (s_robot) {
            for (int i = 0; i < 12; i++) {
                if (armed && (enable >> i & 1u)) {
                    mode[i] = DB_MODE_POSITION;
                    pos[i]  = target[i];
                    cur[i]  = static_cast<int16_t>(cap);
                } else if (hold) {
                    mode[i] = DB_MODE_HOLD;
                    pos[i]  = 0;
                    cur[i]  = 0;
                } else {
                    uint16_t p = 0;
                    mode[i] = DB_MODE_IDLE;
                    pos[i]  = driver_board_raw_feedback(i + 1, &p, nullptr, nullptr, nullptr) ? p : 1550;
                    cur[i]  = 0;
                }
            }
            driver_board_sync_raw(mode, pos, cur, &answered, &fresh);
            // The first frame's reply can still be a config echo (raw_skip);
            // by the second the boards have answered one of ours.
            if (!s_ready && ++s_syncs >= 2) s_ready = true;
        }

        // The guard: only servos driven this tick.
        uint16_t trip = 0;
        uint8_t  trip_servo = 0;
        for (int i = 0; i < 12; i++) {
            const bool driven = armed && (enable >> i & 1u);
            if (!driven) { over_cur_us[i] = 0; over_temp_us[i] = 0; continue; }
            int16_t c = 0, t = 0;
            const bool valid = driver_board_raw_feedback(i + 1, nullptr, &c, &t, nullptr) &&
                               (fresh >> (i / 3) & 1u);
            if (!valid) continue;
            const int ac = c < 0 ? -c : c;
            over_cur_us[i] = (ac > GUARD_CUR_MA) ? over_cur_us[i] + dt : 0;
            const bool hot = t > GUARD_TEMP_DC && t < 1500;   // above 150 degC is not a temperature
            over_temp_us[i] = hot ? over_temp_us[i] + dt : 0;
            if (!trip && over_cur_us[i] > GUARD_CUR_US)   { trip = ST_FAULT_CUR;  trip_servo = static_cast<uint8_t>(i + 1); }
            if (!trip && over_temp_us[i] > GUARD_TEMP_US) { trip = ST_FAULT_TEMP; trip_servo = static_cast<uint8_t>(i + 1); }
        }
        for (int b = 0; b < 4; b++) {
            const bool any_driven = armed && (enable >> (3 * b) & 0x7u);
            lost_ticks[b] = (any_driven && !(answered >> b & 1u)) ? lost_ticks[b] + 1 : 0;
            if (!trip && lost_ticks[b] > BOARD_LOST_TICKS) {
                trip = ST_FAULT_BOARD;
                for (int j = 0; j < 3; j++)
                    if (enable >> (3 * b + j) & 1u) { trip_servo = static_cast<uint8_t>(3 * b + j + 1); break; }
            }
        }

        // The IMU: raw BMI270 axes, mg and 0.1 deg/s, as mp4-link sends them.
        const robot::ImuData d = robot::imu_read();
        const bool imu_ok = d.seq != 0 && now - d.t_us <= IMU_MAX_AGE_US;
        if (imu_ok) {
            st.acc_mg[0]   = sat16(d.ax * 1000.0f);
            st.acc_mg[1]   = sat16(d.ay * 1000.0f);
            st.acc_mg[2]   = sat16(d.az * 1000.0f);
            st.gyr_ddps[0] = sat16(d.gx * 10.0f);
            st.gyr_ddps[1] = sat16(d.gy * 10.0f);
            st.gyr_ddps[2] = sat16(d.gz * 10.0f);
        } else {
            std::memset(st.acc_mg, 0, sizeof(st.acc_mg));
            std::memset(st.gyr_ddps, 0, sizeof(st.gyr_ddps));
        }
        st.imu_t_us = static_cast<uint32_t>(d.t_us > 0 ? d.t_us : 0);

        uint16_t latched;
        uint8_t  fault_servo;
        portENTER_CRITICAL(&s_mux);
        if (trip && s_ctl.armed) disarm_locked(trip, trip_servo);
        armed       = s_ctl.armed;
        latched     = s_ctl.latched;
        fault_servo = s_ctl.fault_servo;
        portEXIT_CRITICAL(&s_mux);
        if (trip) ESP_LOGW(TAG, "guard: %s on servo %d - disarmed",
                           trip == ST_FAULT_CUR ? "current" : trip == ST_FAULT_TEMP ? "temperature" : "board silent",
                           trip_servo);

        // STATE: what the boards latched at the previous exchange, so it
        // carries that exchange's time.
        st.magic   = MAGIC_STATE;
        st.version = PROTO_VERSION;
        st.seq     = seq;
        st.tick    = tick++;
        st.t_us    = static_cast<uint32_t>(t_prev);
        st.flags   = static_cast<uint16_t>(latched
                   | (armed ? ST_ARMED : 0)
                   | (imu_ok ? ST_IMU_OK : 0)
                   | (answered != 0x0F ? ST_BOARD_MISSING : 0));
        st.fault_servo = fault_servo;
        st.boards      = fresh;
        for (int i = 0; i < 12; i++) {
            uint16_t p = 0;
            int16_t c = 0, t = 0;
            driver_board_raw_feedback(i + 1, &p, &c, &t, nullptr);   // 0s when not valid
            st.pos_dd[i]  = p;
            st.cur_mA[i]  = c;
            st.temp_dC[i] = t;
        }
        st.crc = crc16(reinterpret_cast<const uint8_t *>(&st), sizeof(st) - 2);
        uart_write_bytes(PI_UART_PORT, &st, sizeof(st));

        s_last_flags = st.flags;
        s_ticks++;
        t_prev = now;
    }
}

}  // namespace

void init()
{
    if (s_tick_task) return;
    portENTER_CRITICAL(&s_mux);
    reset_ctl_locked();
    portEXIT_CRITICAL(&s_mux);
    // Core 1 with the gait, one priority above it: while a session runs the
    // gait is parked and only plans, so the tick keeps its 5 ms.
    if (xTaskCreatePinnedToCore(tick_task, "rl_tick", 4096, nullptr, 23, &s_tick_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "could not start the RL tick task");
        s_tick_task = nullptr;
        return;
    }
    // The timer runs for good. Starting it on a session's first frame and
    // stopping it when the session ends raced: a frame landing while a session
    // closed found the timer still running (so did not restart it), and the
    // close then stopped it under the new session, which never ticked again.
    // Between sessions a tick is a flag test and a return.
    esp_timer_create_args_t ta = {};
    ta.callback = tick_cb;
    ta.name     = "rl_tick";
    if (esp_timer_create(&ta, &s_timer) != ESP_OK ||
        esp_timer_start_periodic(s_timer, TICK_US) != ESP_OK) {
        ESP_LOGE(TAG, "could not start the RL tick timer");
        s_timer = nullptr;
        return;
    }
    ESP_LOGI(TAG, "RL host link ready (mp4-link protocol, %d Hz tick, watchdog %d ms, "
                  "session ends %d ms after the last frame)",
             CONFIG_APP_RL_TICK_HZ, CONFIG_APP_RL_WATCHDOG_MS, CONFIG_APP_RL_RELEASE_MS);
}

void feed(const uint8_t *data, int n)
{
    if (!s_timer) return;
    for (int i = 0; i < n; i++) {
        if (s_rx.len == sizeof(s_rx.buf)) rx_drop(1);   // only junk with no frame in it
        s_rx.buf[s_rx.len++] = data[i];
        // Process byte by byte: a chunk from the UART can hold several frames
        // and up to 256 bytes, more than the buffer, interleaved with pi_link
        // frames that are junk to this parser.
        if (frame_len(s_rx.buf[0]) && s_rx.len >= frame_len(s_rx.buf[0])) rx_process();
        else if (!frame_len(s_rx.buf[0])) rx_process();
    }
}

void note_bulk()
{
    s_last_bulk_us = esp_timer_get_time();
}

bool driving()
{
    return s_session && s_robot;
}

bool session_active()
{
    bool want;
    portENTER_CRITICAL(&s_mux);
    want = s_want;
    portEXIT_CRITICAL(&s_mux);
    return s_session || want;
}

Stats stats()
{
    Stats s = {};
    s.session = s_session;
    s.robot   = s_robot;
    portENTER_CRITICAL(&s_mux);
    s.armed       = s_ctl.armed;
    s.fault_servo = s_ctl.fault_servo;
    s.frames      = s_frames;
    portEXIT_CRITICAL(&s_mux);
    s.flags    = s_last_flags;
    s.crc_err  = s_crc_err;
    s.sessions = s_sessions;
    s.ticks    = s_ticks;
    return s;
}

}  // namespace rl_link

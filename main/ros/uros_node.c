/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The micro-ROS session: wait for Wi-Fi and the agent, create the node and
 * its topics, publish on a 1 kHz tick, and start over whenever the agent
 * goes away or a setting changes.
 *
 * Everything micro-ROS touches lives on this one task: rcl/rclc are not
 * thread-safe, and nothing else in the firmware calls them.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <rcl/error_handling.h>
#include <rcl/rcl.h>
#include <rclc/executor.h>
#include <rclc/rclc.h>
#include <rmw_microros/rmw_microros.h>
#include <sensor_msgs/msg/imu.h>
#include <sensor_msgs/msg/joint_state.h>
#include <std_msgs/msg/header.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "uros_joint_map.h"
#include "uros_priv.h"
#include "uros_robot.h"

static const char *TAG = "uros";

/* rcl's *_fini() are warn_unused_result, and a cast to void does not
 * silence that in GCC. On teardown there is nothing to do about a failure. */
static inline void ignore_rc(rcl_ret_t rc) { (void)rc; }

#define NODE_NAME        "mpv2"
#define IMU_FRAME        "imu_link"
#define G_MS2            9.80665
#define DEG2RAD          0.017453292519943295

#define AGENT_PING_PERIOD_US   1000000   /* liveness check while connected  */
#define AGENT_PING_TIMEOUT_MS  50        /* it stalls the tick, but only when
                                            the agent is slow to answer     */
#define AGENT_LOST_AFTER       5         /* consecutive failed pings: home
                                            Wi-Fi stalls for 1-2 s at times,
                                            and a reconnect costs ~2 s more  */
#define SYNC_TIMEOUT_MS        50
#define RESYNC_PERIOD_US       30000000  /* re-sync the clock to the agent  */
#define TICK_LATE_US           10000     /* a tick this late counts as late */

/*
 * BMI270 chip axes -> REP-103 body axes (x forward, y left, z up):
 *     body[i] = sign[i] * chip[map[i]]
 * Measured on the robot (omniRL's tilt_mount.py: "swap_xy"): body x = chip y,
 * body y = chip x, body z = -chip z. Flat on the floor, the chip reads
 * az = -1 g and imu/data_raw reads +9.8 m/s^2 on z.
 */
static const int   kImuMap[3]  = {1, 0, 2};
static const float kImuSign[3] = {1.0f, 1.0f, -1.0f};

/* ── Wi-Fi power save ─────────────────────────────────────────────────────
 *
 * The robot idles in WIFI_PS_MIN_MODEM: the radio sleeps between DTIM
 * beacons and the router holds packets FOR the robot until it wakes. That is
 * invisible to the robot's own traffic, but every PC -> robot message waits
 * up to a DTIM interval, and a burst of them can overflow the router's
 * buffer: measured on the robot, pings went from 8 ms to 31 ms and then
 * stopped coming back. So the radio stays awake for as long as the agent is
 * connected, exactly as mpx_voice_ws.c does for a live conversation.
 *
 * On the way out, modem sleep comes back only if no voice conversation is
 * live (it set WIFI_PS_NONE for itself). mpx_voice_state() is weak: this
 * builds with or without mpx_voice_link. */
int mpx_voice_state(void) __attribute__((weak));   /* MPX_VOICE_IDLE == 0 */

static void wifi_awake(bool on)
{
    wifi_ps_type_t cur;
    if (esp_wifi_get_ps(&cur) != ESP_OK) {
        return;
    }
    if (on) {
        if (cur != WIFI_PS_NONE) {
            (void)esp_wifi_set_ps(WIFI_PS_NONE);
        }
    } else if (cur == WIFI_PS_NONE && !(mpx_voice_state && mpx_voice_state() != 0)) {
        (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    }
}

/* ── messages (static: nothing is allocated per publish) ─────────────── */

static sensor_msgs__msg__Imu        s_imu;
static sensor_msgs__msg__JointState s_js;
static rosidl_runtime_c__String     s_js_names[UROS_JOINTS];
static double                       s_js_pos[UROS_JOINTS];
static std_msgs__msg__Header        s_ping;
static std_msgs__msg__Header        s_pong;
static char s_ping_frame[64];
static char s_pong_frame[64];
static char s_imu_frame[] = IMU_FRAME;
static char s_js_frame[]  = "";

/* ── session state (this task only) ──────────────────────────────────── */

static rcl_publisher_t    s_pub_imu, s_pub_js, s_pub_pong;
static rcl_subscription_t s_sub_ping;
static rcl_timer_t        s_tick;
static uint32_t s_imu_last_seq;
static int64_t  s_tick_last_us;
static int64_t  s_imu_period_us, s_imu_due_us;   /* period 0 = topic off */
static int64_t  s_js_period_us, s_js_due_us;

static void messages_init(void)
{
    memset(&s_imu, 0, sizeof(s_imu));
    s_imu.header.frame_id.data = s_imu_frame;
    s_imu.header.frame_id.size = strlen(s_imu_frame);
    s_imu.header.frame_id.capacity = sizeof(s_imu_frame);
    s_imu.orientation_covariance[0] = -1.0;   /* REP-145: no orientation here */

    memset(&s_js, 0, sizeof(s_js));
    s_js.header.frame_id.data = s_js_frame;
    s_js.header.frame_id.size = 0;
    s_js.header.frame_id.capacity = sizeof(s_js_frame);
    for (int i = 0; i < UROS_JOINTS; ++i) {
        s_js_names[i].data = (char *)uros_joint_names[i];
        s_js_names[i].size = strlen(uros_joint_names[i]);
        s_js_names[i].capacity = s_js_names[i].size + 1;
    }
    s_js.name.data = s_js_names;
    s_js.name.size = s_js.name.capacity = UROS_JOINTS;
    s_js.position.data = s_js_pos;
    s_js.position.size = s_js.position.capacity = UROS_JOINTS;

    memset(&s_ping, 0, sizeof(s_ping));
    s_ping.frame_id.data = s_ping_frame;
    s_ping.frame_id.capacity = sizeof(s_ping_frame);
    memset(&s_pong, 0, sizeof(s_pong));
    s_pong.frame_id.data = s_pong_frame;
    s_pong.frame_id.capacity = sizeof(s_pong_frame);
}

/* ── the PC's clock ───────────────────────────────────────────────────────
 *
 * Stamps are the agent's (the PC's) time. micro-ROS can sync to it
 * (rmw_uros_sync_session), but its offset comes from ONE request/reply,
 * NTP-style, so it is only as good as half that round trip -- and a reply
 * that limps in during a Wi-Fi stall still sets it. Measured on the robot:
 * stamps 340 ms in the future. It also rides on CLOCK_REALTIME, which SNTP
 * steps under it.
 *
 * So the offset kept here is our own, against esp_timer (monotonic), and it
 * is taken only from a sync whose whole round trip was quick: then the error
 * is under SYNC_GOOD_RTT_US / 2. A slow one is ignored and the last good
 * offset stays; the ESP's crystal drifts ~1 ms a minute at worst, and a
 * re-sync every 30 s takes that out. */
#define SYNC_GOOD_RTT_US  20000
#define SYNC_TRIES_START  20

static int64_t s_epoch_off_ns;   /* ROS time = esp_timer us * 1000 + this */
static bool    s_epoch_ok;
static int32_t s_sync_rtt_us;

static bool clock_sync(int tries)
{
    for (int i = 0; i < tries; ++i) {
        const int64_t t0 = esp_timer_get_time();
        if (rmw_uros_sync_session(SYNC_TIMEOUT_MS) != RMW_RET_OK) {
            continue;
        }
        const int64_t t1 = esp_timer_get_time();
        if (t1 - t0 > SYNC_GOOD_RTT_US) {
            continue;   /* too slow to trust */
        }
        const int64_t now_us = esp_timer_get_time();
        s_epoch_off_ns = rmw_uros_epoch_nanos() - now_us * 1000;
        s_epoch_ok = true;
        s_sync_rtt_us = (int32_t)(t1 - t0);
        return true;
    }
    return false;
}

/* ROS time of an esp_timer instant: the PC's clock once synced. */
static void stamp_at(builtin_interfaces__msg__Time *t, int64_t at_us)
{
    const int64_t ns = at_us * 1000 + (s_epoch_ok ? s_epoch_off_ns : 0);
    t->sec = (int32_t)(ns / 1000000000LL);
    t->nanosec = (uint32_t)(ns % 1000000000LL);
}

/* True if a new sample went out (or failed to); false if there was none. */
static bool publish_imu(void)
{
    uros_imu_t d;
    if (!uros_robot_imu(&d) || d.seq == s_imu_last_seq) {
        return false;   /* nothing new since the last one */
    }
    s_imu_last_seq = d.seq;
    stamp_at(&s_imu.header.stamp, d.t_us);
    double *a = &s_imu.linear_acceleration.x;
    double *w = &s_imu.angular_velocity.x;
    for (int i = 0; i < 3; ++i) {
        a[i] = kImuSign[i] * d.acc_g[kImuMap[i]] * G_MS2;
        w[i] = kImuSign[i] * d.gyro_dps[kImuMap[i]] * DEG2RAD;
    }
    if (rcl_publish(&s_pub_imu, &s_imu, NULL) == RCL_RET_OK) {
        uros_status_count(UROS_CNT_IMU_SENT);
    } else {
        rcl_reset_error();
        uros_status_count(UROS_CNT_IMU_FAIL);
    }
    return true;
}

static void publish_joints(void)
{
    float deg[12];
    const uint16_t measured = uros_robot_servo_deg(deg);
    if (measured != 0x0FFF) {
        uros_status_count(UROS_CNT_JOINT_FALLBACK);
    }
    uros_joint_angles(deg, s_js_pos);
    stamp_at(&s_js.header.stamp, esp_timer_get_time());
    if (rcl_publish(&s_pub_js, &s_js, NULL) == RCL_RET_OK) {
        uros_status_count(UROS_CNT_JOINT_SENT);
    } else {
        rcl_reset_error();
        uros_status_count(UROS_CNT_JOINT_FAIL);
    }
}

static void tick_cb(rcl_timer_t *timer, int64_t last_call_time)
{
    (void)last_call_time;
    if (!timer) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (s_tick_last_us && now - s_tick_last_us > TICK_LATE_US) {
        uros_status_count(UROS_CNT_OVERRUNS);
    }
    s_tick_last_us = now;

    /* Due, and a sample newer than the last one: send it. Not due yet, or
     * no new sample: try again on the next tick. */
    if (s_imu_period_us && now >= s_imu_due_us && publish_imu()) {
        s_imu_due_us += s_imu_period_us;
        if (now - s_imu_due_us > s_imu_period_us) {
            s_imu_due_us = now;   /* fell behind: catch up, do not burst */
        }
    }
    if (s_js_period_us && now >= s_js_due_us) {
        publish_joints();
        s_js_due_us += s_js_period_us;
        if (now - s_js_due_us > s_js_period_us) {
            s_js_due_us = now;
        }
    }
}

/* uros/ping -> uros/pong, unchanged: the PC measures the round trip. */
static void ping_cb(const void *msg_in)
{
    const std_msgs__msg__Header *in = (const std_msgs__msg__Header *)msg_in;
    s_pong.stamp = in->stamp;
    const size_t n = in->frame_id.size < sizeof(s_pong_frame) - 1
                         ? in->frame_id.size : sizeof(s_pong_frame) - 1;
    memcpy(s_pong_frame, in->frame_id.data, n);
    s_pong_frame[n] = '\0';
    s_pong.frame_id.size = n;
    if (rcl_publish(&s_pub_pong, &s_pong, NULL) == RCL_RET_OK) {
        uros_status_count(UROS_CNT_PINGS);
    } else {
        rcl_reset_error();
    }
}

static int64_t period_us(uint16_t hz)
{
    return hz ? 1000000 / hz : 0;
}

static bool set_agent(rmw_init_options_t *rmw_opts, const uros_cfg_t *c)
{
    char port[8];
    snprintf(port, sizeof(port), "%u", (unsigned)c->agent_port);
    return rmw_uros_options_set_udp_address(c->agent_ip, port, rmw_opts) == RMW_RET_OK;
}

/* Is an agent answering at the configured address? (no session needed) */
static bool agent_answers(const uros_cfg_t *c)
{
    rcl_allocator_t alloc = rcl_get_default_allocator();
    rcl_init_options_t opts = rcl_get_zero_initialized_init_options();
    if (rcl_init_options_init(&opts, alloc) != RCL_RET_OK) {
        rcl_reset_error();
        return false;
    }
    bool ok = set_agent(rcl_init_options_get_rmw_init_options(&opts), c) &&
              rmw_uros_ping_agent_options(200, 2, rcl_init_options_get_rmw_init_options(&opts)) ==
                  RMW_RET_OK;
    ignore_rc(rcl_init_options_fini(&opts));
    rcl_reset_error();
    return ok;
}

#define TRY(what, call)                                                     \
    do {                                                                    \
        rcl_ret_t rc_ = (call);                                             \
        if (rc_ != RCL_RET_OK) {                                            \
            ESP_LOGW(TAG, "%s failed (%d): %s", what, (int)rc_,             \
                     rcl_get_error_string().str);                           \
            rcl_reset_error();                                              \
            goto teardown;                                                  \
        }                                                                   \
    } while (0)

/* One session, from creating the node to tearing it down. Returns whether
 * it got as far as publishing. */
static bool run_session(const uros_cfg_t *c, uint32_t gen)
{
    rcl_allocator_t alloc = rcl_get_default_allocator();
    rcl_init_options_t opts = rcl_get_zero_initialized_init_options();
    rclc_support_t support;
    rcl_node_t node = rcl_get_zero_initialized_node();
    rclc_executor_t exec = rclc_executor_get_zero_initialized_executor();
    bool have_opts = false, have_support = false, have_node = false, have_exec = false;
    bool have_imu = false, have_js = false, have_pong = false, have_ping = false,
         have_tick = false;
    bool lost = false;
    bool ran = false;   /* got as far as publishing */

    memset(&support, 0, sizeof(support));
    s_pub_imu = rcl_get_zero_initialized_publisher();
    s_pub_js = rcl_get_zero_initialized_publisher();
    s_pub_pong = rcl_get_zero_initialized_publisher();
    s_sub_ping = rcl_get_zero_initialized_subscription();
    s_tick = rcl_get_zero_initialized_timer();

    TRY("init options", rcl_init_options_init(&opts, alloc));
    have_opts = true;
    TRY("domain id", rcl_init_options_set_domain_id(&opts, c->domain_id));
    if (!set_agent(rcl_init_options_get_rmw_init_options(&opts), c)) {
        ESP_LOGW(TAG, "bad agent address %s:%u", c->agent_ip, (unsigned)c->agent_port);
        goto teardown;
    }
    /* A fresh client key every session. rmw_microxrcedds picks one with an
     * unseeded rand(), so every boot starts with the SAME key: a robot that
     * reboots while the agent keeps running walks into its own previous
     * session, still open on the agent with all its entities. */
    (void)rmw_uros_options_set_client_key(esp_random() | 1u, rcl_init_options_get_rmw_init_options(&opts));

    /* Awake before the first request: creating the node and its topics is a
     * dozen round trips, each waiting up to 1 s for the agent's reply, and
     * with modem sleep every reply waits for the next DTIM beacon (~300 ms
     * on a typical router) -- enough to time a publisher out. */
    wifi_awake(true);
    TRY("session", rclc_support_init_with_options(&support, 0, NULL, &opts, &alloc));
    have_support = true;
    /* rcl_init() keeps its own copy of the options: ours must go now. They
     * come from a small static pool in rmw_microxrcedds, and keeping one per
     * session emptied it -- after the second lost agent every reconnect
     * failed at once ("session failed (1)"), for good. */
    ignore_rc(rcl_init_options_fini(&opts));
    have_opts = false;
    TRY("node", rclc_node_init_default(&node, NODE_NAME, "", &support));
    have_node = true;

    TRY("imu/data_raw", rclc_publisher_init_best_effort(
            &s_pub_imu, &node, ROSIDL_GET_MSG_TYPE_SUPPORT(sensor_msgs, msg, Imu), "imu/data_raw"));
    have_imu = true;
    TRY("joint_states", rclc_publisher_init_best_effort(
            &s_pub_js, &node, ROSIDL_GET_MSG_TYPE_SUPPORT(sensor_msgs, msg, JointState),
            "joint_states"));
    have_js = true;
    TRY("uros/pong", rclc_publisher_init_best_effort(
            &s_pub_pong, &node, ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Header), "uros/pong"));
    have_pong = true;
    TRY("uros/ping", rclc_subscription_init_best_effort(
            &s_sub_ping, &node, ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Header), "uros/ping"));
    have_ping = true;
    TRY("tick", rclc_timer_init_default2(&s_tick, &support,
                                         RCL_S_TO_NS(1) / UROS_TICK_HZ, tick_cb, true));
    have_tick = true;

    TRY("executor", rclc_executor_init(&exec, &support.context, 2, &alloc));
    have_exec = true;
    TRY("executor timer", rclc_executor_add_timer(&exec, &s_tick));
    TRY("executor ping", rclc_executor_add_subscription(&exec, &s_sub_ping, &s_ping, ping_cb,
                                                        ON_NEW_DATA));

    s_epoch_ok = false;   /* a new agent may run on another PC's clock */
    const bool synced = clock_sync(SYNC_TRIES_START);
    s_imu_period_us = period_us(c->imu_hz > UROS_IMU_HZ_MAX ? UROS_IMU_HZ_MAX : c->imu_hz);
    s_js_period_us = period_us(c->joint_hz > UROS_JOINT_HZ_MAX ? UROS_JOINT_HZ_MAX : c->joint_hz);
    s_imu_due_us = s_js_due_us = esp_timer_get_time();
    s_tick_last_us = 0;
    s_imu_last_seq = 0;
    uros_status_count(UROS_CNT_SESSIONS);
    uros_status_session(true, synced);
    uros_status_set_state(MPX_UROS_CONNECTED);
    ran = true;
    if (synced) {
        ESP_LOGI(TAG, "connected to the agent at %s:%u (domain %u), clock synced to the PC "
                      "(round trip %ld ms)", c->agent_ip, (unsigned)c->agent_port,
                 (unsigned)c->domain_id, (long)(s_sync_rtt_us / 1000));
    } else {
        ESP_LOGW(TAG, "connected to the agent at %s:%u (domain %u), clock NOT synced yet "
                      "(no quick round trip); retrying every 30 s", c->agent_ip,
                 (unsigned)c->agent_port, (unsigned)c->domain_id);
    }

    int64_t next_ping = esp_timer_get_time() + AGENT_PING_PERIOD_US;
    int64_t next_sync = esp_timer_get_time() + RESYNC_PERIOD_US;
    int misses = 0;
    for (;;) {
        (void)rclc_executor_spin_some(&exec, RCL_MS_TO_NS(1));
        rcl_reset_error();
        if (uros_cfg_generation() != gen) {
            ESP_LOGI(TAG, "settings changed: reconnecting");
            break;
        }
        const int64_t now = esp_timer_get_time();
        if (now >= next_ping) {
            next_ping = now + AGENT_PING_PERIOD_US;
            wifi_awake(true);   /* the voice link puts modem sleep back when it hangs up */
            const int64_t t0 = esp_timer_get_time();
            if (rmw_uros_ping_agent(AGENT_PING_TIMEOUT_MS, 1) == RMW_RET_OK) {
                misses = 0;
                uros_status_ping_ms((int32_t)((esp_timer_get_time() - t0) / 1000));
            } else if (++misses >= AGENT_LOST_AFTER) {
                ESP_LOGW(TAG, "the agent stopped answering: reconnecting");
                uros_status_count(UROS_CNT_AGENT_LOST);
                lost = true;
                break;
            }
        }
        if (now >= next_sync) {
            next_sync = now + RESYNC_PERIOD_US;
            if (clock_sync(1)) {
                uros_status_session_clock(true);
            }
        }
    }

teardown:
    uros_status_session(false, false);
    wifi_awake(false);
    if (have_support) {
        /* With the agent gone, every delete would wait for a reply. */
        rmw_context_t *rmw_ctx = rcl_context_get_rmw_context(&support.context);
        (void)rmw_uros_set_context_entity_destroy_session_timeout(rmw_ctx, lost ? 0 : 100);
    }
    if (have_exec)   ignore_rc(rclc_executor_fini(&exec));
    if (have_tick)   ignore_rc(rcl_timer_fini(&s_tick));
    if (have_ping)   ignore_rc(rcl_subscription_fini(&s_sub_ping, &node));
    if (have_pong)   ignore_rc(rcl_publisher_fini(&s_pub_pong, &node));
    if (have_js)     ignore_rc(rcl_publisher_fini(&s_pub_js, &node));
    if (have_imu)    ignore_rc(rcl_publisher_fini(&s_pub_imu, &node));
    if (have_node)   ignore_rc(rcl_node_fini(&node));
    if (have_support) {
        ignore_rc(rclc_support_fini(&support));
    }
    if (have_opts) {
        ignore_rc(rcl_init_options_fini(&opts));
    }
    rcl_reset_error();
    return ran;
}

void uros_node_task(void *arg)
{
    (void)arg;
    messages_init();
    bool said_searching = false;

    for (;;) {
        uros_cfg_t c;
        const uint32_t gen = uros_cfg_snapshot(&c);

        if (!c.enabled) {
            uros_status_set_state(MPX_UROS_OFF);
        } else if (!c.agent_ip[0]) {
            uros_status_set_state(MPX_UROS_NO_AGENT_IP);
        } else if (!uros_wifi_ready()) {
            uros_status_set_state(MPX_UROS_NO_WIFI);
        } else {
            uros_status_set_state(MPX_UROS_SEARCHING);
            if (!said_searching) {
                ESP_LOGI(TAG, "looking for the micro-ROS agent at %s:%u", c.agent_ip,
                         (unsigned)c.agent_port);
                said_searching = true;
            }
            if (agent_answers(&c)) {
                if (run_session(&c, gen)) {
                    said_searching = false;
                    continue;   /* it ran: straight back to searching */
                }
                /* It failed while setting up: wait as after a missed ping,
                 * never retry in a tight loop. */
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

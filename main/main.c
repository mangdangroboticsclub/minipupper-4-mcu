/* main.c -- Raspberry Pi link of the Mini Pupper v4 (MP4 ESP32 CORE board).
 *
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The ESP32-S3 is a bridge between a controller on the Raspberry Pi and the
 * four AT32 servo boards. The Pi speaks the binary protocol in
 * docs/PROTOCOL.md over UART1 on the J-PI connector (pins in board_config.h,
 * baud APP_PI_LINK_UART_BAUD). With APP_RL_USB_LINK the same protocol also
 * runs on UART0 (USB, CH340K); otherwise UART0 is the log console.
 *
 * Frames are little-endian with CRC16-CCITT-FALSE over every byte before the
 * crc field:
 *   request  10 B  42 cmd servo pad arg1 arg2 crc   per-servo setup commands
 *   response 14 B  62 cmd servo status pos cur val crc
 *   SET_ALL  36 B  43 ...  twelve targets, enable mask, arm / disarm / clear
 *   STATE   106 B  63 ...  every tick: twelve positions, currents and
 *                          temperatures, the IMU, flags
 *
 * The tick (APP_RL_TICK_HZ, 200 Hz by default) sends one frame to each of
 * the four AT32 boards: the latest SET_ALL targets while armed, IDLE
 * otherwise. STATE goes to the port that sent the last SET_ALL (UART1 until
 * one arrives). Every channel is IDLE at boot. The link watchdog and the
 * guards disarm on their own (all IDLE) and latch a flag until a SET_ALL
 * clears it. If the tick itself stops for 500 ms, the task watchdog reboots
 * the ESP32, and the boot path idles every board. The IMU is sampled in its
 * own task: its driver waits on I2C with no timeout, and nothing may block
 * the tick. The tick never logs.
 *
 * Per-servo commands run only while disarmed; SET_POS and TORQUE are refused
 * (motion goes through SET_ALL, under the watchdog). IDLE disarms everything.
 *
 * Calibration offsets live in NVS namespace "parameter", keys
 * offset1..offset12 (4-byte float blobs, degrees), the namespace the `main`
 * branch's firmware imports its calibration from. This firmware only
 * stores and serves them (GET_OFFSET, SET_OFFSET, SAVE_OFFSETS): the host adds
 * them when it builds a pose, and SET_ALL positions stay raw. NVS also holds
 * other firmwares' data, so it is never erased here.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "board_config.h"
#include "driver_board.h"
#include "imu.h"

static const char *TAG = "mp4link";

/* ---- ports ---------------------------------------------------------------- */
#define HOST_PORT     PI_UART_PORT
#define HOST_TX       PI_UART_TX_PIN
#define HOST_RX       PI_UART_RX_PIN
#define HOST_BAUD     CONFIG_APP_PI_LINK_UART_BAUD
#if CONFIG_APP_RL_USB_LINK
#define USB_PORT      UART_NUM_0
#define USB_TX        43    /* U0TXD through the CH340K */
#define USB_RX        44    /* U0RXD */
#define USB_BAUD      CONFIG_APP_RL_USB_BAUD
#endif

#define TICK_US       (1000000 / CONFIG_APP_RL_TICK_HZ)
#define WDOG_US       ((int64_t)CONFIG_APP_RL_WATCHDOG_MS * 1000)
#define GUARD_CUR_MA  CONFIG_APP_RL_GUARD_CUR_MA
#define GUARD_CUR_US  ((int64_t)CONFIG_APP_RL_GUARD_CUR_MS * 1000)
#define GUARD_TEMP_DC CONFIG_APP_RL_GUARD_TEMP_DC
#define GUARD_TEMP_US ((int64_t)CONFIG_APP_RL_GUARD_TEMP_MS * 1000)
#define BOARD_LOST_TICKS 20   /* an enabled servo's board silent this long -> fault */
#define IMU_MAX_AGE_US   20000 /* an older IMU sample is reported as absent */
#define TICK_WDT_MS      500   /* the tick silent this long -> reboot (boot idles every board) */

/* ---- frames ---------------------------------------------------------------- */
#define MAGIC_REQ     0x42   /* 'B' */
#define MAGIC_RSP     0x62   /* 'b' */
#define MAGIC_SET_ALL 0x43   /* 'C' */
#define MAGIC_STATE   0x63   /* 'c' */
#define PROTO_VERSION 1

#pragma pack(push,1)
typedef struct { uint8_t magic, cmd, servo, pad; uint16_t arg1, arg2; uint16_t crc; } req_t;
typedef struct { uint8_t magic, cmd, servo, status; uint16_t pos_dd; int16_t cur_mA; float val; uint16_t crc; } rsp_t;
typedef struct {
    uint8_t  magic, version;
    uint16_t seq;
    uint16_t flags;          /* SA_* */
    uint16_t enable;         /* bit i: servo i+1 in position mode, else IDLE */
    uint16_t cur_cap_mA;     /* frame cap; in AT32 control_mode 1 a duty cap of kff x cap */
    uint16_t pos_dd[12];     /* raw AT32 deci-degrees, by servo id */
    uint16_t crc;
} set_all_t;
typedef struct {
    uint8_t  magic, version;
    uint16_t seq;            /* last SET_ALL received */
    uint32_t tick;
    uint32_t t_us;           /* ESP32 time of the exchange whose state the feedback is */
    uint16_t flags;          /* ST_* */
    uint8_t  fault_servo;    /* 1..12 when a fault names a servo, else 0 */
    uint8_t  boards;         /* bit b: board b gave fresh feedback this tick */
    uint16_t pos_dd[12];
    int16_t  cur_mA[12];
    int16_t  temp_dC[12];    /* housing NTC, 0.1 degC (low half of the feedback res word) */
    int16_t  acc_mg[3];
    int16_t  gyr_ddps[3];    /* 0.1 deg/s */
    uint32_t imu_t_us;
    uint16_t crc;
} state_t;
#pragma pack(pop)
_Static_assert(sizeof(req_t) == 10, "req_t size");
_Static_assert(sizeof(rsp_t) == 14, "rsp_t size");
_Static_assert(sizeof(set_all_t) == 36, "set_all_t size");
_Static_assert(sizeof(state_t) == 106, "state_t size");

/* SET_ALL flags */
#define SA_ARM     (1u << 0)
#define SA_DISARM  (1u << 1)
#define SA_CLEAR   (1u << 2)   /* clear latched faults and the watchdog flag */

/* STATE flags */
#define ST_ARMED         (1u << 0)
#define ST_WATCHDOG      (1u << 1)   /* latched: no SET_ALL for the watchdog time */
#define ST_FAULT_CUR     (1u << 2)   /* latched: sustained current */
#define ST_FAULT_TEMP    (1u << 3)   /* latched: housing temperature */
#define ST_FAULT_BOARD   (1u << 4)   /* latched: an enabled servo's board went silent */
#define ST_IMU_OK        (1u << 5)
#define ST_BOARD_MISSING (1u << 6)   /* this tick: a board did not answer */
#define ST_REJECTED      (1u << 7)   /* latched: an ARM refused while a fault was latched */
#define ST_HOST_USB      (1u << 8)   /* STATE goes to UART0 (APP_RL_USB_LINK) */
#define ST_LATCHED (ST_WATCHDOG | ST_FAULT_CUR | ST_FAULT_TEMP | ST_FAULT_BOARD | ST_REJECTED)
#define ST_FAULTS  (ST_FAULT_CUR | ST_FAULT_TEMP | ST_FAULT_BOARD)

/* per-servo commands */
enum {
    CMD_PING = 0, CMD_SET_POS = 1, CMD_IDLE = 2, CMD_TORQUE = 3, CMD_SET_KP = 4,
    CMD_SET_KD = 5, CMD_GET_PARAM = 6, CMD_GET_LIVE = 7, CMD_SET_SCALE = 8,
    CMD_RAW_REPLY = 9, CMD_SET_PARAM = 10, CMD_GET_STATUS = 11,
    CMD_GET_OFFSET = 12, CMD_SET_OFFSET = 13, CMD_SAVE_OFFSETS = 14,
};
#define RSP_OK          0
#define RSP_ERR_SERVO   1
#define RSP_ERR_ARG     2
#define RSP_ERR_BOARD   3
#define RSP_ERR_CMD     5
#define RSP_ERR_REFUSED 6   /* armed, or a motion command: use SET_ALL */
#define RSP_ERR_UNSET   7   /* no offset stored for this servo (val 0) */
#define RSP_ERR_NVS     8   /* NVS unavailable, or a write failed */

/* ---- calibration offsets (see the header) ----------------------------------- */
#define OFFSET_MAX_DEG  45.0f
static SemaphoreHandle_t s_off_lock;
static nvs_handle_t s_nvs;
static bool  s_nvs_ok;
static float s_offset[13];
static bool  s_offset_set[13];

static void offsets_load(void)
{
    s_off_lock = xSemaphoreCreateMutex();
    if (s_off_lock == NULL) return;
    esp_err_t e = nvs_flash_init();
    if (e != ESP_OK) {                 /* full or newer format: leave it alone */
        ESP_LOGE(TAG, "nvs_flash_init: %s; offsets unavailable", esp_err_to_name(e));
        return;
    }
    e = nvs_open("parameter", NVS_READWRITE, &s_nvs);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open parameter: %s; offsets unavailable", esp_err_to_name(e));
        return;
    }
    s_nvs_ok = true;
    for (int i = 1; i <= 12; i++) {
        char key[12];
        float v;
        size_t sz = sizeof v;
        snprintf(key, sizeof key, "offset%d", i);
        if (nvs_get_blob(s_nvs, key, &v, &sz) == ESP_OK && sz == sizeof v) {
            s_offset[i] = v;
            s_offset_set[i] = true;
        }
    }
}

/* Writes every offset that is set; returns how many, or -1 on a failure. */
static int offsets_save(void)
{
    if (!s_nvs_ok) return -1;
    int n = 0;
    bool ok = true;
    xSemaphoreTake(s_off_lock, portMAX_DELAY);
    for (int i = 1; i <= 12 && ok; i++) {
        if (!s_offset_set[i]) continue;
        char key[12];
        snprintf(key, sizeof key, "offset%d", i);
        ok = nvs_set_blob(s_nvs, key, &s_offset[i], sizeof s_offset[i]) == ESP_OK;
        if (ok) n++;
    }
    if (ok) ok = nvs_commit(s_nvs) == ESP_OK;
    xSemaphoreGive(s_off_lock);
    return ok ? n : -1;
}

/* ---- shared control state (rx tasks write, the tick reads) ----------------- */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool        armed;
    uint16_t    latched;        /* ST_LATCHED bits */
    uint8_t     fault_servo;
    uint16_t    seq, enable, cur_cap;
    uint16_t    pos_dd[12];
    int64_t     last_set_all_us;
    uart_port_t state_port;
} s_ctl = { .state_port = HOST_PORT };

static void disarm_locked(uint16_t why, uint8_t servo)
{
    s_ctl.armed = false;
    s_ctl.latched |= why;
    if (servo && !s_ctl.fault_servo) s_ctl.fault_servo = servo;
}

static bool is_armed(void)
{
    portENTER_CRITICAL(&s_mux);
    bool a = s_ctl.armed;
    portEXIT_CRITICAL(&s_mux);
    return a;
}

/* ---- CRC16-CCITT-FALSE (init 0xFFFF, poly 0x1021, MSB-first) --------------- */
static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U)
                                  : (uint16_t)(crc << 1);
    }
    return crc;
}

/* ---- per-servo commands ---------------------------------------------------- */
static void attach_feedback(rsp_t *rsp, uint16_t servo, bool with_res)
{
    if (!driver_board_feedback_valid(servo)) {
        if (rsp->status == RSP_OK) rsp->status = RSP_ERR_BOARD;
        return;
    }
    rsp->pos_dd = driver_board_present_pos_dd(servo);
    rsp->cur_mA = driver_board_present_cur_mA(servo);
    if (with_res) {
        uint32_t res = driver_board_present_res(servo);
        memcpy(&rsp->val, &res, sizeof rsp->val);
    }
}

static float f32_from(uint16_t lo, uint16_t hi)
{
    uint32_t bits = (uint32_t)lo | ((uint32_t)hi << 16);
    float v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

static rsp_t exec_req(const req_t *req)
{
    rsp_t rsp;
    memset(&rsp, 0, sizeof rsp);
    rsp.magic  = MAGIC_RSP;
    rsp.cmd    = req->cmd;
    rsp.servo  = req->servo;
    rsp.status = RSP_OK;

    const bool ok_servo = (req->servo >= 1 && req->servo <= 12);

    if (req->cmd == CMD_IDLE) {
        /* any IDLE stops everything: the tick sends IDLE from the next frame */
        portENTER_CRITICAL(&s_mux);
        s_ctl.armed = false;
        portEXIT_CRITICAL(&s_mux);
        if (ok_servo) attach_feedback(&rsp, req->servo, true);
        return rsp;
    }
    if (req->cmd == CMD_PING) {
        /* the tick refreshes the feedback cache every frame */
        if (ok_servo) attach_feedback(&rsp, req->servo, true);
        return rsp;
    }
    if (req->cmd == CMD_SET_POS || req->cmd == CMD_TORQUE || is_armed()) {
        rsp.status = RSP_ERR_REFUSED;
        return rsp;
    }

    switch (req->cmd) {
    case CMD_SET_KP:
    case CMD_SET_KD: {
        if (!ok_servo) { rsp.status = RSP_ERR_SERVO; break; }
        float v = f32_from(req->arg1, req->arg2);
        uint16_t pid = (req->cmd == CMD_SET_KP) ? DB_PARAM_KP_POSITION : DB_PARAM_KD_POSITION;
        if (!driver_board_set_param(req->servo, pid, v)) rsp.status = RSP_ERR_BOARD;
        else rsp.val = v;
        break;
    }
    case CMD_GET_PARAM: {
        if (!ok_servo) { rsp.status = RSP_ERR_SERVO; break; }
        if (req->arg1 >= DB_PARAM_COUNT) { rsp.status = RSP_ERR_ARG; break; }
        float v = 0.0f;
        if (!driver_board_get_param(req->servo, req->arg1, &v)) rsp.status = RSP_ERR_BOARD;
        else rsp.val = v;
        break;
    }
    case CMD_SET_PARAM: {
        /* RAM only: nothing is saved to the AT32 flash. The id travels in the
         * pad byte so both float halves fit in arg1/arg2. */
        if (!ok_servo) { rsp.status = RSP_ERR_SERVO; break; }
        if (req->pad >= DB_PARAM_COUNT) { rsp.status = RSP_ERR_ARG; break; }
        float v = f32_from(req->arg1, req->arg2), rb = 0.0f;
        if (!driver_board_set_param(req->servo, req->pad, v) ||
            !driver_board_get_param(req->servo, req->pad, &rb))
            rsp.status = RSP_ERR_BOARD;
        else
            rsp.val = rb;
        break;
    }
    case CMD_GET_LIVE: {
        if (!ok_servo) { rsp.status = RSP_ERR_SERVO; break; }
        if (req->arg1 >= DB_LIVE_COUNT) { rsp.status = RSP_ERR_ARG; break; }
        float v = 0.0f;
        if (!driver_board_get_live(req->servo, req->arg1, &v)) rsp.status = RSP_ERR_BOARD;
        else rsp.val = v;
        break;
    }
    case CMD_GET_STATUS:
        if (!ok_servo) { rsp.status = RSP_ERR_SERVO; break; }
        if (!driver_board_feedback_valid(req->servo)) { rsp.status = RSP_ERR_BOARD; break; }
        rsp.val = (float)driver_board_present_status(req->servo);
        break;
    case CMD_SET_SCALE:
        driver_board_set_scale_dd((float)req->arg1);
        rsp.val = driver_board_get_scale_dd();
        break;
    /* The offset commands answer without the boards' feedback: they work with
     * the servos unpowered and right after a boot. */
    case CMD_GET_OFFSET:
        if (!ok_servo) { rsp.status = RSP_ERR_SERVO; return rsp; }
        if (!s_nvs_ok) { rsp.status = RSP_ERR_NVS; return rsp; }
        xSemaphoreTake(s_off_lock, portMAX_DELAY);
        rsp.val = s_offset[req->servo];
        if (!s_offset_set[req->servo]) rsp.status = RSP_ERR_UNSET;
        xSemaphoreGive(s_off_lock);
        return rsp;
    case CMD_SET_OFFSET: {
        /* RAM only until SAVE_OFFSETS */
        if (!ok_servo) { rsp.status = RSP_ERR_SERVO; return rsp; }
        if (!s_nvs_ok) { rsp.status = RSP_ERR_NVS; return rsp; }
        float v = f32_from(req->arg1, req->arg2);
        if (!(v >= -OFFSET_MAX_DEG && v <= OFFSET_MAX_DEG)) { rsp.status = RSP_ERR_ARG; return rsp; }
        xSemaphoreTake(s_off_lock, portMAX_DELAY);
        s_offset[req->servo] = v;
        s_offset_set[req->servo] = true;
        xSemaphoreGive(s_off_lock);
        rsp.val = v;
        return rsp;
    }
    case CMD_SAVE_OFFSETS: {
        int n = offsets_save();
        if (n < 0) rsp.status = RSP_ERR_NVS;
        else rsp.val = (float)n;
        return rsp;
    }
    case CMD_RAW_REPLY: {
        if (!ok_servo) { rsp.status = RSP_ERR_SERVO; break; }
        uint8_t raw[36];
        if (!driver_board_raw_reply(req->servo, raw)) { rsp.status = RSP_ERR_BOARD; break; }
        int ones = 0;
        for (int i = 0; i < 36; i++) if (raw[i] == 0xFF) ones++;
        uint32_t packed = (uint32_t)ones | ((uint32_t)raw[0] << 8)
                        | ((uint32_t)raw[1] << 16) | ((uint32_t)raw[2] << 24);
        memcpy(&rsp.val, &packed, sizeof rsp.val);
        return rsp;   /* no feedback attached: the raw bytes are the answer */
    }
    default:
        rsp.status = RSP_ERR_CMD;
        return rsp;
    }
    if (ok_servo && rsp.status == RSP_OK) attach_feedback(&rsp, req->servo, false);
    return rsp;
}

static void handle_set_all(uart_port_t port, const set_all_t *f)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    s_ctl.state_port      = port;
    s_ctl.last_set_all_us = now;
    s_ctl.seq             = f->seq;
    s_ctl.enable          = f->enable & 0x0FFF;
    s_ctl.cur_cap         = f->cur_cap_mA;
    memcpy(s_ctl.pos_dd, f->pos_dd, sizeof s_ctl.pos_dd);
    if (f->flags & SA_CLEAR) {
        s_ctl.latched = 0;
        s_ctl.fault_servo = 0;
    }
    if (f->flags & SA_DISARM) {
        s_ctl.armed = false;
    } else if (f->flags & SA_ARM) {
        if (s_ctl.latched & ST_FAULTS) {
            s_ctl.latched |= ST_REJECTED;
        } else {
            s_ctl.armed = true;
            s_ctl.latched &= (uint16_t)~(ST_WATCHDOG | ST_REJECTED);
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

/* ---- receive: one parser per port, resyncing on magic + CRC ---------------- */
/* A chunk (64 B) lands on at most a partial frame (35 B), so 128 B never overflows. */
typedef struct { uart_port_t port; uint8_t buf[128]; size_t len; } rx_t;

static size_t frame_len(uint8_t magic)
{
    if (magic == MAGIC_REQ)     return sizeof(req_t);
    if (magic == MAGIC_SET_ALL) return sizeof(set_all_t);
    return 0;
}

static void rx_drop(rx_t *r, size_t n)
{
    memmove(r->buf, r->buf + n, r->len - n);
    r->len -= n;
}

static void rx_process(rx_t *r)
{
    while (r->len > 0) {
        size_t need = frame_len(r->buf[0]);
        if (!need) { rx_drop(r, 1); continue; }
        if (r->len < need) return;
        uint16_t crc;
        memcpy(&crc, r->buf + need - 2, 2);
        if (crc16(r->buf, need - 2) != crc) { rx_drop(r, 1); continue; }  /* resync */

        if (r->buf[0] == MAGIC_REQ) {
            req_t req;
            memcpy(&req, r->buf, sizeof req);
            rsp_t rsp = exec_req(&req);
            rsp.crc = crc16((const uint8_t *)&rsp, sizeof rsp - 2);
            uart_write_bytes(r->port, &rsp, sizeof rsp);
        } else {
            set_all_t f;
            memcpy(&f, r->buf, sizeof f);
            if (f.version == PROTO_VERSION) handle_set_all(r->port, &f);
        }
        rx_drop(r, need);
    }
}

static void rx_task(void *arg)
{
    rx_t *r = (rx_t *)arg;
    uint8_t chunk[64];
    for (;;) {
        /* Wait for one byte, then take what else has arrived. Asking for a
         * full chunk would hold a 36-byte SET_ALL until the next one came. */
        int n = uart_read_bytes(r->port, chunk, 1, portMAX_DELAY);
        if (n <= 0) continue;
        size_t more = 0;
        uart_get_buffered_data_len(r->port, &more);
        if (more > sizeof chunk - 1) more = sizeof chunk - 1;
        if (more) {
            int m = uart_read_bytes(r->port, chunk + 1, more, 0);
            if (m > 0) n += m;
        }
        for (int i = 0; i < n; i++) {
            if (r->len == sizeof r->buf) rx_drop(r, 1);   /* defensive: cannot happen (see rx_t) */
            r->buf[r->len++] = chunk[i];
        }
        if (n > 0) rx_process(r);
    }
}

/* ---- the servo tick -------------------------------------------------------- */
static TaskHandle_t s_tick_task;

static void tick_timer_cb(void *arg)
{
    (void)arg;
    xTaskNotifyGive(s_tick_task);
}

static void tick_task(void *arg)
{
    (void)arg;
    uint16_t mode[12], pos[12], target[12];
    int16_t  cur[12];
    int64_t  over_cur_us[12] = { 0 }, over_temp_us[12] = { 0 };
    uint16_t lost_ticks[4] = { 0 };
    uint32_t tick = 0;
    int64_t  t_prev = esp_timer_get_time();
    bool     imu_ok = false;
    state_t  st;

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        esp_task_wdt_reset();
        int64_t now = esp_timer_get_time();
        int64_t dt  = now - t_prev;   /* real time: a late tick counts in full */

        /* snapshot the command; the watchdog */
        portENTER_CRITICAL(&s_mux);
        if (s_ctl.armed && now - s_ctl.last_set_all_us > WDOG_US)
            disarm_locked(ST_WATCHDOG, 0);
        bool        armed  = s_ctl.armed;
        uint16_t    enable = s_ctl.enable, cap = s_ctl.cur_cap, seq = s_ctl.seq;
        uart_port_t port   = s_ctl.state_port;
        memcpy(target, s_ctl.pos_dd, sizeof target);
        portEXIT_CRITICAL(&s_mux);

        for (int i = 0; i < 12; i++) {
            if (armed && (enable >> i & 1u)) {
                mode[i] = DB_MODE_POSITION;
                pos[i]  = target[i];
                cur[i]  = (int16_t)cap;
            } else {
                mode[i] = DB_MODE_IDLE;
                pos[i]  = driver_board_feedback_valid(i + 1) ? driver_board_present_pos_dd(i + 1) : 1550;
                cur[i]  = 0;
            }
        }
        uint8_t answered = 0, fresh = 0;
        driver_board_sync(mode, pos, cur, &answered, &fresh);

        /* the guard: only servos driven this tick */
        uint16_t trip = 0;
        uint8_t  trip_servo = 0;
        for (int i = 0; i < 12; i++) {
            bool driven = armed && (enable >> i & 1u);
            bool valid  = driven && driver_board_feedback_valid(i + 1) && (fresh >> (i / 3) & 1u);
            if (!driven) { over_cur_us[i] = 0; over_temp_us[i] = 0; continue; }
            if (!valid) continue;
            int c = driver_board_present_cur_mA(i + 1);
            if (c < 0) c = -c;
            over_cur_us[i] = (c > GUARD_CUR_MA) ? over_cur_us[i] + dt : 0;
            int16_t t = (int16_t)(driver_board_present_res(i + 1) & 0xFFFFu);
            bool hot = t > GUARD_TEMP_DC && t < 1500;   /* above 150 degC is not a temperature */
            over_temp_us[i] = hot ? over_temp_us[i] + dt : 0;
            if (!trip && over_cur_us[i] > GUARD_CUR_US)   { trip = ST_FAULT_CUR;  trip_servo = (uint8_t)(i + 1); }
            if (!trip && over_temp_us[i] > GUARD_TEMP_US) { trip = ST_FAULT_TEMP; trip_servo = (uint8_t)(i + 1); }
        }
        for (int b = 0; b < 4; b++) {
            bool any_driven = armed && (enable >> (3 * b) & 0x7u);
            lost_ticks[b] = (any_driven && !(answered >> b & 1u)) ? lost_ticks[b] + 1 : 0;
            if (!trip && lost_ticks[b] > BOARD_LOST_TICKS) {
                trip = ST_FAULT_BOARD;
                for (int j = 0; j < 3; j++)
                    if (enable >> (3 * b + j) & 1u) { trip_servo = (uint8_t)(3 * b + j + 1); break; }
            }
        }

        uint32_t t_imu = 0;
        imu_ok = imu_latest(st.acc_mg, st.gyr_ddps, &t_imu, IMU_MAX_AGE_US);

        uint16_t latched;
        uint8_t  fault_servo;
        portENTER_CRITICAL(&s_mux);
        if (trip && s_ctl.armed) disarm_locked(trip, trip_servo);
        armed       = s_ctl.armed;
        latched     = s_ctl.latched;
        fault_servo = s_ctl.fault_servo;
        portEXIT_CRITICAL(&s_mux);

        /* STATE: the feedback of this exchange is what the AT32s latched at
         * the previous one, so it carries that exchange's time. */
        st.magic   = MAGIC_STATE;
        st.version = PROTO_VERSION;
        st.seq     = seq;
        st.tick    = tick++;
        st.t_us    = (uint32_t)t_prev;
        st.flags   = latched
                   | (armed ? ST_ARMED : 0)
                   | (imu_ok ? ST_IMU_OK : 0)
                   | (answered != 0x0F ? ST_BOARD_MISSING : 0)
#if CONFIG_APP_RL_USB_LINK
                   | (port == USB_PORT ? ST_HOST_USB : 0)
#endif
                   ;
        st.fault_servo = fault_servo;
        st.boards      = fresh;
        for (int i = 0; i < 12; i++) {
            bool v = driver_board_feedback_valid(i + 1);
            st.pos_dd[i]  = v ? driver_board_present_pos_dd(i + 1) : 0;
            st.cur_mA[i]  = v ? driver_board_present_cur_mA(i + 1) : 0;
            st.temp_dC[i] = v ? (int16_t)(driver_board_present_res(i + 1) & 0xFFFFu) : 0;
        }
        st.imu_t_us = t_imu;
        st.crc = crc16((const uint8_t *)&st, sizeof st - 2);
        uart_write_bytes(port, &st, sizeof st);

        t_prev = now;
    }
}

/* ---- start-up --------------------------------------------------------------- */
static void uart_open(uart_port_t port, int tx, int rx, int baud)
{
    uart_config_t c = {
        .baud_rate  = baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(port, 1024, 2048, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(port, &c));
    ESP_ERROR_CHECK(uart_set_pin(port, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

#if CONFIG_APP_RL_USB_LINK
static rx_t s_rx_usb  = { .port = USB_PORT };
#endif
static rx_t s_rx_host = { .port = HOST_PORT };

void app_main(void)
{
    /* Every channel IDLE before anything else: after a reset the boards may
     * still hold the last command of the previous run. */
    driver_board_init();
    uint16_t mode[12], pos[12];
    int16_t  cur[12];
    for (int i = 0; i < 12; i++) { mode[i] = DB_MODE_IDLE; pos[i] = 1550; cur[i] = 0; }
    driver_board_sync(mode, pos, cur, NULL, NULL);

    offsets_load();

#if CONFIG_APP_RL_USB_LINK
    /* USB: a readable line at the bootloader's 115200, then the link baud */
    uart_open(USB_PORT, USB_TX, USB_RX, 115200);
    char line[96];
    int n = snprintf(line, sizeof line,
                     "\r\n# mp4link: USB %d baud, Pi UART%d %d baud (TX IO%d, RX IO%d)\r\n",
                     USB_BAUD, (int)HOST_PORT, HOST_BAUD, (int)HOST_TX, (int)HOST_RX);
    uart_write_bytes(USB_PORT, line, n);
    uart_wait_tx_done(USB_PORT, pdMS_TO_TICKS(200));
    uart_set_baudrate(USB_PORT, USB_BAUD);
#else
    ESP_LOGI(TAG, "Pi link UART%d %d baud (TX IO%d, RX IO%d), tick %d Hz, guard %d mA / %d ms",
             (int)HOST_PORT, HOST_BAUD, (int)HOST_TX, (int)HOST_RX, CONFIG_APP_RL_TICK_HZ,
             CONFIG_APP_RL_GUARD_CUR_MA, CONFIG_APP_RL_GUARD_CUR_MS);
#endif

    uart_open(HOST_PORT, HOST_TX, HOST_RX, HOST_BAUD);
    gpio_pullup_en(HOST_RX);   /* idle-high while the Pi is off */

    if (imu_init()) imu_start();

    /* the tick subscribes to the task watchdog: stalled 500 ms -> panic -> reboot */
    const esp_task_wdt_config_t wdt = {
        .timeout_ms     = TICK_WDT_MS,
        .idle_core_mask = (1u << 0) | (1u << 1),
        .trigger_panic  = true,
    };
    if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) ESP_ERROR_CHECK(esp_task_wdt_init(&wdt));

#if CONFIG_APP_RL_USB_LINK
    xTaskCreatePinnedToCore(rx_task, "rx_usb",  4096, &s_rx_usb,  12, NULL, 0);
#endif
    xTaskCreatePinnedToCore(rx_task, "rx_host", 4096, &s_rx_host, 12, NULL, 0);
    xTaskCreatePinnedToCore(tick_task, "tick", 4096, NULL, 20, &s_tick_task, 1);

    const esp_timer_create_args_t ta = { .callback = tick_timer_cb, .name = "tick" };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&ta, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, TICK_US));
}

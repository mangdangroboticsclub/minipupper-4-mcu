/* driver_board.c - talks to the 4 AT32 servo boards over SPI.
 *
 * Each leg has an AT32 chip that runs its 3 servos (control loop + pot reading).
 * The ESP32 swaps a 36-byte message with one board at a time. Two kinds:
 *
 *   SERVO message  (starts 0xA5A5)  mode, angle and current limit for the 3
 *                                    servos. The 36 bytes that come back at the
 *                                    same time are their angle, current, temp.
 *   CONFIG message (starts 0xC0DE)  read/write one setting or live value. The
 *                                    AT32 puts the answer in the NEXT reply, so:
 *                                    send the request, wait 0.5 ms, send an
 *                                    empty config message to collect the answer.
 *
 * Because of that, the first SERVO message after a CONFIG one can still carry
 * a config answer instead of feedback (it is marked 0xC0DE) - we skip those.
 */
#include "driver_board.h"
#include <string.h>
#include "driver/spi_master.h"
#include "esp_rom_sys.h"            /* esp_rom_delay_us */

/* ---- wiring (MangDang MP4 ESP32 CORE) ---- */
#define PIN_MOSI 6
#define PIN_MISO 17
#define PIN_CLK  16
static const int cs_pin[4] = { 7, 4, 15, 5 };     /* board 0 FR (CN4), 1 FL (CN5), 2 RR (CN3), 3 RL (CN6) */

/* servo id -> channel on the boards (1..12) for the 3 wirings in driver_board.h */
static int channel(int id){
    static const int8_t swap_hip_calf[13] = { 0, 3, 2, 1, 6, 5, 4, 9, 8, 7, 12, 11, 10 };
    static const int8_t purple[13]        = { 0, 4, 6, 5, 1, 3, 2, 10, 12, 11, 7, 9, 8 };
    if(SERVO_BOARD == 2) return swap_hip_calf[id];
    if(SERVO_BOARD == 3) return purple[id];
    return id;
}

/* ---- the 36-byte messages ---- */
#define SERVO_MSG   0xA5A5
#define CONFIG_MSG  0xC0DE
enum { CFG_NOP = 0, CFG_SET = 1, CFG_GET = 2, CFG_SAVE = 3, CFG_GET_LIVE = 5 };

#pragma pack(push, 1)
typedef struct { uint16_t mode, deg10; int16_t mA; uint16_t kp, kd; } servo_cmd_t;          /* 10 bytes */
typedef struct { uint16_t start, kind; servo_cmd_t s[3]; uint16_t check; } servo_msg_t;     /* 36 bytes */
typedef struct { uint16_t start, op, slot, id; float value; uint8_t pad[24]; } config_msg_t;  /* 36 bytes */
typedef struct { uint16_t status, deg10; int16_t mA; uint32_t extra; } servo_fb_t;          /* 10 bytes */
typedef struct { uint16_t start, status; servo_fb_t s[3]; uint16_t check; } reply_t;        /* 36 bytes */
#pragma pack(pop)
_Static_assert(sizeof(servo_msg_t) == 36 && sizeof(config_msg_t) == 36 && sizeof(reply_t) == 36, "36-byte messages");

/* ---- what we last told each channel, and what it last reported ---- */
static uint16_t cmd_mode[12], cmd_deg10[12];
static int16_t  cmd_mA[12];
static uint16_t fb_deg10[12];
static float    scale_dd = 3100;                    /* 310.0 deg full range */

static spi_device_handle_t board_dev[4];

void driver_board_init(void){
    for(int c = 0; c < 12; c++){ cmd_mode[c] = DB_MODE_POSITION; cmd_deg10[c] = 1550; cmd_mA[c] = 0; }
    spi_bus_config_t bus = { .mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO, .sclk_io_num = PIN_CLK,
                             .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 128 };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = { .mode = 0, .duty_cycle_pos = 128,
                                          .clock_speed_hz = 12 * 1000 * 1000, .queue_size = 2 };
    for(int b = 0; b < 4; b++){
        dev.spics_io_num = cs_pin[b];
        ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &board_dev[b]));
    }
}

/* swap 36 bytes with one board */
static bool exchange(int board, const void *out, reply_t *in){
    spi_transaction_t t = { .length = 36 * 8, .tx_buffer = out, .rx_buffer = in };
    return spi_device_transmit(board_dev[board], &t) == ESP_OK;
}

/* send a board the commands for its 3 servos and keep the feedback.
 * retry = send once more if the reply was a left-over config answer */
static bool send_servo_msg(int board, bool retry){
    servo_msg_t m = { .start = SERVO_MSG, .kind = 1 };
    for(int k = 0; k < 3; k++){
        int c = board * 3 + k;
        m.s[k] = (servo_cmd_t){ .mode = cmd_mode[c], .deg10 = cmd_deg10[c], .mA = cmd_mA[c] };
    }
    reply_t r;
    if(!exchange(board, &m, &r)) return false;
    if(r.status == CONFIG_MSG){
        if(!retry) return false;
        if(!exchange(board, &m, &r)) return false;
        if(r.status == CONFIG_MSG) return true;       /* still no feedback - keep the old one */
    }
    for(int k = 0; k < 3; k++) fb_deg10[board * 3 + k] = r.s[k].deg10;
    return true;
}

/* ---- one servo ---- */
bool driver_board_direct(int id, uint16_t mode, float deg, int16_t mA){
    if(id < 1 || id > 12) return false;
    if(deg < 0) deg = 0;
    if(deg > scale_dd / 10) deg = scale_dd / 10;
    int c = channel(id) - 1;
    cmd_mode[c] = mode; cmd_deg10[c] = (uint16_t)(deg * 10); cmd_mA[c] = mA;
    return send_servo_msg(c / 3, true);
}
bool driver_board_poll(int id){
    if(id < 1 || id > 12) return false;
    return send_servo_msg((channel(id) - 1) / 3, true);
}
uint16_t driver_board_present_posdd(int id){
    if(id < 1 || id > 12) return 0;
    return fb_deg10[channel(id) - 1];
}

/* ---- all 12 (walking). pos 0..1023 counts the other way round to the
 *      servo angle, so 0 = 310 deg and 1023 = 0 deg ---- */
void driver_board_sync_write(const uint16_t pos[12], const uint16_t mA[12]){
    for(int board = 0; board < 4; board++){
        for(int k = 0; k < 3; k++){
            int c = board * 3 + k, id = channel(c + 1) - 1;
            cmd_mode[c]  = DB_MODE_POSITION;
            cmd_deg10[c] = (uint16_t)(scale_dd - (float)pos[id] * scale_dd / 1024.0f);
            cmd_mA[c]    = (int16_t)mA[id];
        }
        send_servo_msg(board, false);
    }
}

/* ---- settings and live values ---- */
static bool config(int board, uint16_t op, uint16_t slot, uint16_t id, float value, float *answer){
    config_msg_t ask = { .start = CONFIG_MSG, .op = op, .slot = slot, .id = id, .value = value };
    config_msg_t collect = { .start = CONFIG_MSG, .op = CFG_NOP };
    reply_t r;
    if(!exchange(board, &ask, &r)) return false;
    esp_rom_delay_us(500);                                   /* the AT32 prepares the answer */
    if(!exchange(board, &collect, &r)) return false;
    if(r.status != CONFIG_MSG || r.s[0].deg10 != id) return false;    /* not the answer to our question */
    if(answer) memcpy(answer, &r.s[0].extra, sizeof(float));
    return true;
}
static bool servo_config(int id, uint16_t op, int what, int count, float value, float *answer){
    if(id < 1 || id > 12 || what < 0 || what >= count) return false;
    int c = channel(id) - 1;
    return config(c / 3, op, (uint16_t)(c % 3), (uint16_t)what, value, answer);
}
bool driver_board_get_param(int id, int p, float *v){ return servo_config(id, CFG_GET, p, DB_PARAM_COUNT, 0, v); }
bool driver_board_set_param(int id, int p, float v) { return servo_config(id, CFG_SET, p, DB_PARAM_COUNT, v, NULL); }
bool driver_board_get_live(int id, int w, float *v) { return servo_config(id, CFG_GET_LIVE, w, DB_LIVE_COUNT, 0, v); }

bool driver_board_save_config_n(int board, int *ok, int *failed){
    int n_ok = 0, n_bad = 0;
    for(int b = 0; b < 4; b++){
        if(board >= 0 && board <= 3 && b != board) continue;
        if(config(b, CFG_SAVE, 0, 0, 0, NULL)) n_ok++; else n_bad++;
    }
    if(ok) *ok = n_ok;
    if(failed) *failed = n_bad;
    return n_ok > 0;
}

static const char *const param_names[DB_PARAM_COUNT] = {
    "reverse_position_sensor", "min_position_adc", "max_position_adc", "range_position_deg",
    "reverse_motor", "kp_position", "kd_position", "kp_current", "kff_current",
    "max_pwm_duty_cycle", "ki_position", "control_mode", "pwm_scale",
};
const char *driver_board_param_name(int p){ return p >= 0 && p < DB_PARAM_COUNT ? param_names[p] : NULL; }
int driver_board_param_id(const char *name){
    for(int p = 0; p < DB_PARAM_COUNT; p++) if(!strcmp(name, param_names[p])) return p;
    return -1;
}

float driver_board_get_scale_dd(void){ return scale_dd; }
void  driver_board_set_scale_dd(float dd){ if(dd > 100 && dd <= 3600) scale_dd = dd; }

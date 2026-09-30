/* driver_board.c  -  SPI backend for the Mini Pupper controller driver board.
 *
 * Ported from minipupper2pro/esp32 (SERVOS_BY_SPI path of mini_pupper_servos.cpp),
 * stripped down to exactly what the ESP-only gait code needs:
 *   - one-shot init (bus + 4 devices + power pin)
 *   - sync write of 12 positions + 12 current limits
 *   - cached feedback (present position + present current)
 *
 * Imported into the mangdang firmware from the mpxesp test project. Two things
 * differ from that build and both are load-bearing — see the bus-sharing note
 * in driver_board_init() and the locking note just below.
 */
#include "driver_board.h"

#include <string.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"   /* esp_rom_delay_us */
#include "esp_timer.h"     /* esp_timer_get_time — config-op pacing */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "DRVBOARD"

/* ---- bus arbitration -----------------------------------------------------
 * In the mpxesp test firmware the gait loop and the web CLI were kept apart by
 * the CliMode flag alone. Here the gait task runs pinned to core 1 while the
 * HTTP server runs on core 0, so a Servo Studio parameter read can genuinely
 * land in the middle of a gait tick's four-frame burst.
 *
 * That matters more than it looks: a config request is a request/NOP PAIR, and
 * splitting it does not merely lose the reply — it leaves a config frame
 * pending in the AT32's tx buffer, which the next feedback decode would read as
 * a temperature (a real 31.03 degC reply decodes as 1540.8 degC; see
 * fb_frame_valid). One mutex held across each whole public operation prevents
 * the interleave.
 *
 * Nothing else is on this bus: MP4-ESP32-CORE moved the IMU to I2C, so unlike
 * the Mini Pupper 2 build there is no second device whose transfers could land
 * inside a request/NOP pair. The mutex still earns its place between the gait
 * task and the HTTP handlers, which are on different cores.
 */
static SemaphoreHandle_t s_lock;

/* ── SCS full scale, in AT32 deci-degrees ────────────────────────────────
 * This was the literal 2700 in three places. It is not a constant: the AT32
 * boards have a range_position_deg of their own, and minipupperesp's "guard"
 * command trims it (310 electrical minus 2x5 of pot guard gives 300, so 3000
 * here) and writes the trimmed value into the boards' flash, where it stays
 * across any amount of ESP reflashing.
 *
 * Assuming 270 against boards trimmed to 300 puts every commanded angle out
 * by 11%. Standing, that is a slouch. Through a backflip it is the difference
 * between landing and not. driver_board_probe_scale() now asks the boards what
 * their range actually is instead of asserting one.
 *
 * The default below only matters when no board answers that query, so it is
 * 3100 - the same figure minipupperesp declares, and what an unguarded board
 * ships with. It was 2700, which is not a number any board here has ever
 * held. */
static float db_scale_dd = 3100.0f;   /* stock range_position_deg, pre-guard */

void  driver_board_set_scale_dd(float v){ if(v >= 1000.0f && v <= 4000.0f) db_scale_dd = v; }
float driver_board_get_scale_dd(void){ return db_scale_dd; }

#define DB_LOCK()    do { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); } while (0)
#define DB_UNLOCK()  do { if (s_lock) xSemaphoreGive(s_lock); } while (0)

/* ---- pin map ----
 * Taken from the board header rather than repeated here, so a schematic
 * change lands in one file. MP4-ESP32-CORE moved every one of these: the
 * Mini Pupper 2 board used MOSI=11 MISO=13 CLK=12 with CS on 10/9/14/21 and a
 * power-enable on GPIO8, all of which are now audio, LCD or Pi-header pins.
 */
#include "robot/board_config.h"

#define SPI_MASTER_ID    SERVO_SPI_HOST
#define SPI_MASTER_MOSI  SERVO_SPI_MOSI_PIN
#define SPI_MASTER_MISO  SERVO_SPI_MISO_PIN
#define SPI_MASTER_CLK   SERVO_SPI_CLK_PIN
/* Which connector each leg is on lives in the board header, not here — see
 * the note above SERVO_SPI_CS_FR_PIN in robot/board_config.h for why these were
 * wrong. Board index follows the device order below, not the CS number:
 *   CS0 = FL = board 1 (servos 4-6)    CS2 = RL = board 3 (servos 10-12)
 *   CS1 = FR = board 0 (servos 1-3)    CS3 = RR = board 2 (servos 7-9)  */
#define SPI_MASTER_CS0   SERVO_SPI_CS_FL_PIN   /* CN5 - left  front */
#define SPI_MASTER_CS1   SERVO_SPI_CS_FR_PIN   /* CN4 - right front */
#define SPI_MASTER_CS2   SERVO_SPI_CS_RL_PIN   /* CN6 - left  rear  */
#define SPI_MASTER_CS3   SERVO_SPI_CS_RR_PIN   /* CN3 - right rear  */
#define POWER_EN_GPIO    SERVO_POWER_EN_PIN

/* ---- on-the-wire protocol (identical to AT32 spi_command_frame) ---- */
#define START_FIELD  0xA5A5
#define MODE_FIELD   0x0001
/* Frame markers the AT32 sends back. Both feedback and config replies carry
 * start = 0xFEED; only `status` tells them apart. Defined up here (not down
 * in the config section) because fb_frame_valid() below needs it.         */
#define FB_START_FIELD   0xFEED   /* AT32 -> host, both reply kinds        */
#define FB_STATUS_SERVO  0x0003   /* ordinary servo feedback               */
#define START_CONFIG     0xC0DE   /* config-frame request AND reply marker */
#define MODE_POSITION 0x0001   /* AT32: position control, "torque" field = max current mA */

#pragma pack(push,1)
typedef struct { uint16_t mode, position; int16_t torque; uint16_t kp, kd; } servo_cmd_sub_t;
typedef struct { uint16_t start, mode; servo_cmd_sub_t s1, s2, s3; uint16_t check_sum; } host_SMS_t;

/* res1 = NTC temperature, SIGNED, 0.1 degC (AT32 "reserved1").
 * res2 = still unused, sent as 0.  They were one uint32_t "res" before the
 * AT32 firmware started filling in the temperature; the config-frame reply
 * path packs a float across BOTH halves, which still works because the
 * struct is #pragma pack(1) and the two words are adjacent. */
typedef struct { uint16_t status, position; int16_t torque; uint16_t res1, res2; } servo_fb_sub_t;
typedef struct { uint16_t start, status; servo_fb_sub_t s1, s2, s3; uint16_t check_sum; } SMS_host_t;
#pragma pack(pop)

static spi_device_handle_t dev_left_front, dev_right_front, dev_left_rear, dev_right_rear;

/* cached feedback, indexed by PHYSICAL channel (index 0 == physical chan 1) */
static uint16_t fb_position[12];
static int16_t  fb_current[12];
/* NTC temperature in 0.1 degC, signed. INT16_MIN = never received. */
#define FB_TEMP_NONE  ((int16_t)-32768)
static int16_t  fb_temp_dc[12] = { [0 ... 11] = FB_TEMP_NONE };

/* True only for a REAL servo feedback frame.
 *
 * This guard matters. The AT32's config_frame_handler() builds its reply in
 * the SAME tx buffer as the feedback frame and also stamps start = 0xFEED;
 * the only thing separating the two is status (0x0003 feedback vs 0xC0DE
 * config reply). Worse, the reply is clocked out on the transaction AFTER
 * the request and the NOP path deliberately does NOT clear it, so a config
 * reply can still be pending when the next ordinary servo frame goes out.
 *
 * In a config reply servo1.reserved1/reserved2 hold a FLOAT, not a
 * temperature, and servo2/servo3 are left untouched (stale). Storing one
 * would decode the low half of that float as deci-degC - e.g. a real
 * 31.03 degC reply (0x41F83C30) reads back as 0x3C30 = 15408 = 1540.8 degC,
 * and only ever on servo1 of that board. Reject it instead.
 *
 * It also rejects an all-zero / all-ones frame from a board that is absent
 * or not powered, which previously landed in the cache as position 0.     */
static inline bool fb_frame_valid(const SMS_host_t *rx)
{
    return rx->start == FB_START_FIELD && rx->status != START_CONFIG;
}

/* Pull position / current / temperature out of one feedback frame into the
 * caches. b = index of the board's first physical channel (0,3,6,9). */
static void fb_store(int b, const SMS_host_t *rx)
{
    const servo_fb_sub_t *fb[3] = { &rx->s1, &rx->s2, &rx->s3 };
    for (int j = 0; j < 3; j++) {
        fb_position[b + j] = (uint16_t)((float)fb[j]->position * 1024.0f / db_scale_dd);
        fb_current[b + j]  = fb[j]->torque;          /* present motor current, mA */
        fb_temp_dc[b + j]  = (int16_t)fb[j]->res1;   /* NTC temperature, 0.1 degC */
    }
}

/* shadow of the last commanded state per servo (deci-degrees), so a direct
 * single-servo write can resend the board frame without disturbing the
 * other two servos on the same board */
static uint16_t sh_mode[12]  = { [0 ... 11] = MODE_POSITION };
static uint16_t sh_posdd[12] = { [0 ... 11] = 1350 };   /* 135.0 deg centre */
static int16_t  sh_cur[12]   = { 0 };
/* Per-frame gains. Zero everywhere means "use the board's stored sms_config
 * gains", which is what the stock AT32 firmware does and what every path here
 * sent before the SDK gained a low-level command API. */
static uint16_t sh_fkp[12]   = { 0 };
static uint16_t sh_fkd[12]   = { 0 };

/* board index (0..3) -> SPI device. Matches reference spi_read_write_bytes(). */
static esp_err_t spi_xfer(uint8_t board, uint8_t size, uint8_t *tx, uint8_t *rx)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length    = (size_t)size * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    switch (board) {
        case 0: return spi_device_transmit(dev_right_front, &t); /* servos 1-3  FR */
        case 1: return spi_device_transmit(dev_left_front,  &t); /* servos 4-6  FL */
        case 2: return spi_device_transmit(dev_right_rear,  &t); /* servos 7-9  RR */
        case 3: return spi_device_transmit(dev_left_rear,   &t); /* servos 10-12 RL */
        default: return ESP_FAIL;
    }
}

/*
 * MP4-ESP32-CORE feeds the servo connectors straight off Vbat+ — there is no
 * switch to close, so this is a no-op on this board. It is kept (rather than
 * deleted) because the gait code and the Servo Studio page both call it, and
 * a board revision that adds a load switch only has to define
 * SERVO_POWER_EN_PIN for this to start working again.
 */
void driver_board_power(bool on)
{
#if SERVO_HAS_POWER_EN
    gpio_set_level((gpio_num_t)POWER_EN_GPIO, on ? 1 : 0);
#else
    (void)on;
#endif
}

bool driver_board_init(void)
{
    if (s_lock) {
        ESP_LOGW(TAG, "driver_board_init() called twice — ignoring");
        return true;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "mutex alloc failed");
        return false;
    }

    /* power-enable pin (absent on MP4-ESP32-CORE, see driver_board_power) */
#if SERVO_HAS_POWER_EN
    gpio_config_t io = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << POWER_EN_GPIO),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    driver_board_power(false);
#endif

    /* ---- SPI bus --------------------------------------------------------
     * On the Mini Pupper 2 board the QMI8658C IMU shared this bus, so init
     * order decided which caller created it. MP4-ESP32-CORE puts the IMU on
     * I2C instead, leaving SPI2 entirely to the servo boards — but the
     * tolerant handling of ESP_ERR_INVALID_STATE stays, because it costs
     * nothing and a double init should log rather than abort the boot.
     */
    spi_bus_config_t bus = {
        .mosi_io_num   = SPI_MASTER_MOSI,
        .miso_io_num   = SPI_MASTER_MISO,
        .sclk_io_num   = SPI_MASTER_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 128,
    };
    esp_err_t err = spi_bus_initialize(SPI_MASTER_ID, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "SPI%d bus already initialised — attaching",
                 SPI_MASTER_ID + 1);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return false;
    }

    /* 4 driver-board devices (12 MHz, mode 0) */
    spi_device_interface_config_t dev = {
        .mode           = 0,
        .duty_cycle_pos = 128,
        .clock_speed_hz = 12 * 1000 * 1000,   /* 15 MHz max */
        .queue_size     = 2,
    };
    struct { int cs; spi_device_handle_t *out; const char *name; } devs[4] = {
        { SPI_MASTER_CS0, &dev_left_front,  "FL (CN5)" },
        { SPI_MASTER_CS1, &dev_right_front, "FR (CN4)" },
        { SPI_MASTER_CS2, &dev_left_rear,   "RL (CN6)" },
        { SPI_MASTER_CS3, &dev_right_rear,  "RR (CN3)" },
    };
    for (int i = 0; i < 4; i++) {
        dev.spics_io_num = devs[i].cs;
        err = spi_bus_add_device(SPI_MASTER_ID, &dev, devs[i].out);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "spi_bus_add_device %s failed: %s",
                     devs[i].name, esp_err_to_name(err));
            return false;
        }
    }

    driver_board_power(true);
    ESP_LOGI(TAG, "driver board SPI init OK (4 boards, 12 servos, variant %d)",
             SERVO_BOARD);
    return true;
}

static bool db_probe_answered = false;

bool driver_board_probe_answered(void){ return db_probe_answered; }

float driver_board_probe_scale(void)
{
    /* Ask each board in turn and take the first sane answer. One board is
     * enough: guard_apply() writes the same range to all twelve servos, and
     * a robot with two different ranges on it has a bigger problem than this
     * function can fix. Servo 1, 4, 7, 10 is one servo per board, so a robot
     * with one leg unplugged still finds the answer. */
    static const int PROBE[4] = { 1, 4, 7, 10 };
    for (int i = 0; i < 4; ++i) {
        float deg = 0.0f;
        if (!driver_board_get_param(PROBE[i], DB_PARAM_RANGE_POSITION_DEG, &deg))
            continue;
        if (deg < 100.0f || deg > 400.0f) continue;   /* not a range */
        float dd = deg * 10.0f;
        if (dd < db_scale_dd - 0.5f || dd > db_scale_dd + 0.5f) {
            ESP_LOGW(TAG, "servo %d reports range_position_deg = %.1f, not %.1f — adopting it. Every commanded angle was %.1f%% out.",
                     PROBE[i], deg, db_scale_dd / 10.0f,
                     100.0f * (dd - db_scale_dd) / db_scale_dd);
        } else {
            ESP_LOGI(TAG, "servo %d confirms range_position_deg = %.1f", PROBE[i], deg);
        }
        driver_board_set_scale_dd(dd);
        db_probe_answered = true;
        return db_scale_dd;
    }
    ESP_LOGW(TAG, "no board answered a range query — assuming %.1f deg. If this robot was ever guarded on minipupperesp, that is wrong.", db_scale_dd / 10.0f);
    return db_scale_dd;
}

void driver_board_sync_write(const uint16_t pos[12], const uint16_t cur_mA[12])
{
    host_SMS_t frame;
    SMS_host_t rx;

    DB_LOCK();
    for (int i = 0; i < 4; i++) {
        const int b = i * 3;   /* first servo index of this board */
        frame.start = START_FIELD;
        frame.mode  = MODE_FIELD;

        servo_cmd_sub_t *sub[3] = { &frame.s1, &frame.s2, &frame.s3 };
        for (int j = 0; j < 3; j++) {
            int idx = b + j;                     /* PHYSICAL channel index */
            int L   = db_phys_inv(idx + 1) - 1;      /* LOGICAL servo feeding this physical chan */
            sub[j]->mode = MODE_POSITION;
            /* SCS 0..1023 -> AT32 deci-degrees 0..db_scale_dd, with the same global
             * direction flip the reference uses. Position already carries the
             * gait's per-servo calibration offset (applied in servo_write).
             * pos[]/cur_mA[] are indexed by logical servo, so pull the logical
             * servo mapped to this physical channel (board-variant swap). */
            sub[j]->position = (uint16_t)(db_scale_dd - (float)pos[L] * db_scale_dd / 1024.0f);
            sub[j]->torque   = (int16_t)cur_mA[L];   /* MODE_POSITION => max current (mA) */
            sub[j]->kp = 0;
            sub[j]->kd = 0;
            sh_mode[idx]  = MODE_POSITION;             /* keep shadow in sync */
            sh_posdd[idx] = sub[j]->position;
            sh_cur[idx]   = sub[j]->torque;
        }
        frame.check_sum = 0;

        if (spi_xfer((uint8_t)i, sizeof(host_SMS_t), (uint8_t *)&frame, (uint8_t *)&rx) == ESP_OK
            && fb_frame_valid(&rx))
            fb_store(b, &rx);   /* position + current + NTC temperature */
        /* an invalid frame is simply skipped - this runs at ~200 Hz, so the
         * cache is refreshed on the very next tick */
    }
    DB_UNLOCK();
}

/* ---- AT32 sms_config parameter access (config frames, start 0xC0DE) ---- */
/* START_CONFIG is defined near the top, next to the other wire constants */
#define CFG_OP_NOP      0
#define CFG_OP_SET      1
#define CFG_OP_GET      2
#define CFG_OP_SAVE     3
#define CFG_OP_RESTORE  4
#define CFG_OP_GET_LIVE 5

#pragma pack(push,1)
typedef struct {
    uint16_t start, op, servo_index, param_id;
    float    value;
    uint8_t  padding[24];   /* pad to sizeof(host_SMS_t) = 36 bytes */
} cfg_frame_t;
#pragma pack(pop)
_Static_assert(sizeof(cfg_frame_t) == sizeof(host_SMS_t), "cfg frame size");

static const char *param_names[DB_PARAM_COUNT] = {
    "reverse_position_sensor", "min_position_adc", "max_position_adc",
    "range_position_deg", "reverse_motor", "kp_position", "kd_position",
    "kp_current", "kff_current", "max_pwm_duty_cycle",
};

const char *driver_board_param_name(int param_id)
{
    if (param_id < 0 || param_id >= DB_PARAM_COUNT) return NULL;
    return param_names[param_id];
}

int driver_board_param_id(const char *name)
{
    for (int i = 0; i < DB_PARAM_COUNT; i++)
        if (strcmp(name, param_names[i]) == 0) return i;
    return -1;
}

static bool cfg_xfer(uint8_t board, uint16_t op, uint16_t servo_index,
                     uint16_t param_id, float value, SMS_host_t *rx_out)
{
    cfg_frame_t f;
    SMS_host_t rx;
    memset(&f, 0, sizeof f);
    f.start = START_CONFIG;
    f.op = op; f.servo_index = servo_index;
    f.param_id = param_id; f.value = value;
    if (spi_xfer(board, sizeof f, (uint8_t *)&f, (uint8_t *)&rx) != ESP_OK)
        return false;
    if (rx_out) *rx_out = rx;
    return true;
}

/* The AT32 loads its reply into the frame clocked out on the NEXT
 * transaction, so every request is a request/NOP transaction pair. */
/* The reference firmware reads the reply once, 500 us after the request, and
 * that is enough there. It is not enough here: on the Mini Pupper 2 build the
 * IMU shared this bus and a config request could be delayed by a transfer the
 * reference never saw, so the read became a bounded retry loop instead.
 *
 * The IMU is on I2C on this board, which removes that particular delay, but
 * the loop stays: it costs nothing in the normal case (the first attempt
 * succeeds and returns), it is bounded, and it can never hang the bus. Reply
 * timing is AT32 firmware behaviour, not something this file controls.
 *
 * The param_id echo does double duty: it proves the reply belongs to THIS
 * request, and it is how a stale reply to the PREVIOUS one gets skipped
 * instead of being returned as the answer.
 */
#define CFG_REPLY_TRIES     6
#define CFG_REPLY_GAP_US  600

/* ── Minimum spacing between config operations on one board ─────────────────
 *
 * The AT32 needs a breath between config exchanges. Fire them back to back
 * and it simply stops answering: the reply poll runs out of tries and every
 * write in the burst fails with "board never sent a config reply", one after
 * another, for boards 0, 2 and 3 while whichever board happened to catch the
 * timing right sails through.
 *
 * This was already known, in the wrong place. studio_setall_handler() sleeps
 * between servos with the comment "give the AT32 time between writes", which
 * fixed Servo Studio and left the constraint undocumented everywhere else.
 * So mpx_gains_all() and mpx_current_all() — twelve joints, two parameters
 * each, in a tight loop with no HTTP round trip to slow them down — walked
 * straight into it.
 *
 * A timing requirement of the hardware belongs next to the hardware, not
 * duplicated into every caller that happens to discover it. Enforced here,
 * per board, so a burst across different boards still interleaves at full
 * speed and only consecutive ops on the SAME board wait.
 */
#define CFG_MIN_GAP_US   2000
#define CFG_BOARDS          4    /* four AT32 driver boards, three servos each */

static int64_t s_cfg_last_us[CFG_BOARDS];

/* ── A board that is not there should cost one attempt, not every attempt ────
 *
 * A config request to an absent board is not free: it sends, then polls six
 * times at 600 us before concluding nobody is home, and writes a warning. On
 * a bench robot with one board plugged in, a single "set this gain on all
 * twelve joints" sweep pays that 45 times over — about 900 ms of dead bus
 * time and 90 log lines, per sweep, for boards whose absence was established
 * on the first one.
 *
 * So establish it once. After three consecutive failures a board is muted:
 * further requests fail immediately, silently, with no retries. Any success
 * un-mutes it. A muted board is re-probed every 5 s, which is what makes this
 * self-healing rather than sticky — plug a board in and it comes back on its
 * own, and a board that drops out mid-session stops costing a second of every
 * sweep within three requests.
 *
 * Deliberately NOT a configured list of "which boards exist". A robot should
 * not need to be told what it is plugged into, and a setting like that is
 * wrong exactly when it matters most: when something has come loose.
 */
#define CFG_FAILS_TO_MUTE      3
#define CFG_REPROBE_US   5000000   /* 5 s */

static uint8_t s_cfg_fails[CFG_BOARDS];
static bool    s_cfg_muted[CFG_BOARDS];
static int64_t s_cfg_probe_us[CFG_BOARDS];

/* Called on every completed config exchange. */
static void cfg_note_result(uint8_t board, bool ok)
{
    if (board >= CFG_BOARDS) return;

    if (ok) {
        if (s_cfg_muted[board]) {
            ESP_LOGI(TAG, "board %u is answering again", (unsigned)board);
        }
        s_cfg_muted[board] = false;
        s_cfg_fails[board] = 0;
        return;
    }

    if (s_cfg_fails[board] < 255) s_cfg_fails[board]++;
    if (!s_cfg_muted[board] && s_cfg_fails[board] >= CFG_FAILS_TO_MUTE) {
        s_cfg_muted[board]   = true;
        s_cfg_probe_us[board] = esp_timer_get_time();
        ESP_LOGW(TAG, "board %u not responding after %d config requests — "
                      "skipping it until it answers (re-probed every %d s)",
                 (unsigned)board, CFG_FAILS_TO_MUTE, CFG_REPROBE_US / 1000000);
    }
}

static bool cfg_request(uint8_t board, uint16_t op, uint16_t servo_index,
                        uint16_t param_id, float value, float *out)
{
    /* Muted: fail now rather than spending 3.6 ms discovering the same thing
       again. One request every CFG_REPROBE_US is let through to check. */
    if (board < CFG_BOARDS && s_cfg_muted[board]) {
        const int64_t now = esp_timer_get_time();
        if (now - s_cfg_probe_us[board] < CFG_REPROBE_US) return false;
        s_cfg_probe_us[board] = now;   /* this attempt is the probe */
    }

    if (board < CFG_BOARDS) {
        const int64_t since = esp_timer_get_time() - s_cfg_last_us[board];
        if (since >= 0 && since < CFG_MIN_GAP_US) {
            esp_rom_delay_us((uint32_t)(CFG_MIN_GAP_US - since));
        }
    }

    SMS_host_t rx;
    if (!cfg_xfer(board, op, servo_index, param_id, value, NULL)) {
        if (board < CFG_BOARDS) s_cfg_last_us[board] = esp_timer_get_time();
        cfg_note_result(board, false);
        return false;
    }

    bool saw_frame = false;
    for (int attempt = 0; attempt < CFG_REPLY_TRIES; attempt++) {
        esp_rom_delay_us(CFG_REPLY_GAP_US);      /* let the AT32 IRQ run */
        if (!cfg_xfer(board, CFG_OP_NOP, 0, 0, 0, &rx)) {
            if (board < CFG_BOARDS) s_cfg_last_us[board] = esp_timer_get_time();
            cfg_note_result(board, false);
            return false;
        }

        if (rx.status != START_CONFIG) continue; /* not a config reply yet */
        saw_frame = true;
        if (rx.s1.position != param_id) continue;/* stale reply; keep looking */

        /* the AT32 packs the float across reserved1+reserved2 (res1/res2
         * here, adjacent in a packed struct), little endian */
        if (out) memcpy(out, &rx.s1.res1, sizeof(float));
        if (board < CFG_BOARDS) s_cfg_last_us[board] = esp_timer_get_time();
        cfg_note_result(board, true);
        return true;
    }

    if (board < CFG_BOARDS) s_cfg_last_us[board] = esp_timer_get_time();

    /* Name which failure it was. They need different fixes, and one message
     * covering both is why this took several attempts to find.
     *
     * Only while the board is still believed to be there: once muted, the
     * summary above has said it, and repeating it per parameter per joint
     * buries everything else in the log. */
    const bool first_time = (board >= CFG_BOARDS) || !s_cfg_muted[board];
    cfg_note_result(board, false);
    if (first_time) {
        ESP_LOGW(TAG, "cfg op %u param %u board %u: %s after %d tries",
                 (unsigned)op, (unsigned)param_id, (unsigned)board,
                 saw_frame ? "reply never echoed the parameter"
                           : "board never sent a config reply",
                 CFG_REPLY_TRIES);
    }
    return false;
}


/* The same mutex driver_board.c uses internally, for the other SPI2 users.
 * Exposed rather than duplicated: two locks guarding one bus is not a lock. */
void driver_board_bus_lock(void)   { DB_LOCK(); }
void driver_board_bus_unlock(void) { DB_UNLOCK(); }

bool driver_board_set_param(int servo, int param_id, float value)
{
    if (servo < 1 || servo > 12 || param_id < 0 || param_id >= DB_PARAM_COUNT)
        return false;
    int p = db_phys(servo) - 1;                 /* logical -> physical */
    DB_LOCK();
    bool ok = cfg_request((uint8_t)(p / 3), CFG_OP_SET,
                          (uint16_t)(p % 3), (uint16_t)param_id,
                          value, NULL);
    DB_UNLOCK();
    return ok;
}

bool driver_board_get_param(int servo, int param_id, float *out)
{
    if (servo < 1 || servo > 12 || param_id < 0 || param_id >= DB_PARAM_COUNT)
        return false;
    int p = db_phys(servo) - 1;                 /* logical -> physical */
    DB_LOCK();
    bool ok = cfg_request((uint8_t)(p / 3), CFG_OP_GET,
                          (uint16_t)(p % 3), (uint16_t)param_id,
                          0, out);
    DB_UNLOCK();
    return ok;
}

bool driver_board_get_live(int servo, int live_id, float *out)
{
    if (servo < 1 || servo > 12 || live_id < 0 || live_id >= DB_LIVE_COUNT)
        return false;
    int p = db_phys(servo) - 1;                 /* logical -> physical */
    DB_LOCK();
    bool ok = cfg_request((uint8_t)(p / 3), CFG_OP_GET_LIVE,
                          (uint16_t)(p % 3), (uint16_t)live_id,
                          0, out);
    DB_UNLOCK();
    return ok;
}

/* SAVE and RESTORE, for one board or all four.
 *
 * servo_index 0 and param_id 0 are placeholders: these ops address the board,
 * not a servo. Identical to the reference firmware, which flashes reliably --
 * an earlier guess here that the op needed repeating per servo index was
 * wrong, and mpxesp is the evidence.
 */
static bool cfg_board_op(int board, uint16_t op)
{
    DB_LOCK();
    bool ok;
    if (board >= 0 && board <= 3) {
        ok = cfg_request((uint8_t)board, op, 0, 0, 0, NULL);
    } else {
        ok = true;                               /* board == -1: all boards */
        for (int b = 0; b < 4; b++)
            ok &= cfg_request((uint8_t)b, op, 0, 0, 0, NULL);
    }
    DB_UNLOCK();
    return ok;
}

bool driver_board_save_config(int board)    { return cfg_board_op(board, CFG_OP_SAVE); }
bool driver_board_factory_restore(int board){ return cfg_board_op(board, CFG_OP_RESTORE); }

/* Send one board's frame rebuilt from the shadow, refresh feedback cache. */
static bool board_resend(int board)
{
    int b = board * 3;
    host_SMS_t frame;
    SMS_host_t rx;
    frame.start = START_FIELD;
    frame.mode  = MODE_FIELD;
    servo_cmd_sub_t *sub[3] = { &frame.s1, &frame.s2, &frame.s3 };
    for (int j = 0; j < 3; j++) {
        sub[j]->mode     = sh_mode[b + j];
        sub[j]->position = sh_posdd[b + j];
        sub[j]->torque   = sh_cur[b + j];
        sub[j]->kp       = sh_fkp[b + j];
        sub[j]->kd       = sh_fkd[b + j];
    }
    frame.check_sum = 0;

    /* Two attempts. If a config reply was still pending in the AT32's tx
     * buffer (cfg_request's NOP leaves it there on purpose), the FIRST frame
     * we clock out is that reply, not feedback. Sending this servo frame is
     * what makes the AT32 rebuild a proper feedback frame, so the second
     * attempt always gets it - one repeat is provably enough, never a loop. */
    for (int attempt = 0; attempt < 2; attempt++) {
        if (spi_xfer((uint8_t)board, sizeof frame, (uint8_t *)&frame, (uint8_t *)&rx) != ESP_OK)
            return false;
        if (fb_frame_valid(&rx)) {
            fb_store(b, &rx);   /* position + current + NTC temperature */
            return true;
        }
        esp_rom_delay_us(200);   /* let the AT32 IRQ refill its tx buffer */
    }
    return false;   /* board absent, or still not answering with feedback */
}

bool driver_board_poll_board(int board)
{
    if (board < 0 || board > 3) return false;
    DB_LOCK();
    bool ok = board_resend(board);
    DB_UNLOCK();
    return ok;
}

bool driver_board_direct(int servo, uint16_t mode, float pos_deg, int16_t current_mA)
{
    if (servo < 1 || servo > 12) return false;
    /* Clamp to the boards' OWN travel, not to 270. range_position_deg is 310
     * on a stock board and whatever 'guard' left behind after that; a fixed
     * 270 silently threw away the top of the range on every board wider than
     * that, and let a narrower one be commanded past its end stop. */
    const float full_deg = db_scale_dd / 10.0f;
    if (pos_deg < 0)        pos_deg = 0;
    if (pos_deg > full_deg) pos_deg = full_deg;

    int idx = db_phys(servo) - 1;                  /* logical -> physical */
    DB_LOCK();
    sh_mode[idx]  = mode;
    sh_posdd[idx] = (uint16_t)(pos_deg * 10.0f);   /* RAW angle, no flip */
    sh_cur[idx]   = current_mA;
    bool ok = board_resend(idx / 3);
    DB_UNLOCK();
    return ok;
}

bool driver_board_poll(int servo)
{
    if (servo < 1 || servo > 12) return false;
    DB_LOCK();
    bool ok = board_resend((db_phys(servo) - 1) / 3);
    DB_UNLOCK();
    return ok;
}

void driver_board_stage(int servo, uint16_t mode, float pos_deg,
                        int16_t current_mA, uint16_t kp, uint16_t kd)
{
    if (servo < 1 || servo > 12) return;
    /* Clamp to the boards' OWN travel, not to 270. range_position_deg is 310
     * on a stock board and whatever 'guard' left behind after that; a fixed
     * 270 silently threw away the top of the range on every board wider than
     * that, and let a narrower one be commanded past its end stop. */
    const float full_deg = db_scale_dd / 10.0f;
    if (pos_deg < 0)        pos_deg = 0;
    if (pos_deg > full_deg) pos_deg = full_deg;

    const int idx = db_phys(servo) - 1;              /* logical -> physical */
    DB_LOCK();
    sh_mode[idx]  = mode;
    sh_posdd[idx] = (uint16_t)(pos_deg * 10.0f);     /* RAW angle, no flip */
    sh_cur[idx]   = current_mA;
    sh_fkp[idx]   = kp;
    sh_fkd[idx]   = kd;
    DB_UNLOCK();
}

bool driver_board_commit(void)
{
    bool ok = true;
    DB_LOCK();
    for (int b = 0; b < 4; b++) {
        ok &= board_resend(b);
    }
    DB_UNLOCK();
    return ok;
}

int16_t driver_board_present_current(int ch)
{
    if (ch < 1 || ch > 12) return 0;
    return fb_current[db_phys(ch) - 1];
}

uint16_t driver_board_present_position(int ch)
{
    if (ch < 1 || ch > 12) return 0;
    return fb_position[db_phys(ch) - 1];
}

float driver_board_present_temperature(int ch)
{
    if (ch < 1 || ch > 12) return DB_TEMP_INVALID;
    int16_t dc = fb_temp_dc[db_phys(ch) - 1];
    if (dc == FB_TEMP_NONE) return DB_TEMP_INVALID;
    return (float)dc * 0.1f;
}

/* driver_board.h
 *
 * SPI backend for the Mini Pupper "controller driver board" (4x AT32F413).
 *
 * This replaces the direct Feetech serial-bus servo control (SCServo / ftservo)
 * used previously. Instead of bit-banging the SCS bus, the ESP32 now talks to
 * four AT32F413 servo-driver boards over SPI (one board per leg, 3 servos each).
 *
 * Each SPI frame (host_SMS_t) carries, per servo: { mode, position, torque, kp, kd }.
 * IMPORTANT: there is NO "speed" field. In MODE_POSITION the AT32 firmware reads
 * the "torque" field as a MAX CURRENT LIMIT in milliamps (max_current_mA), NOT a
 * speed. So the legacy "goal_speed" value is translated into a current limit.
 *
 * Wiring: MangDang MP4 ESP32 CORE (MP4ESP32_CORE_SCH_0827.pdf). The pin
 * numbers themselves live in robot/board_config.h — this comment
 * described the older Mini Pupper 2 board (MOSI=11 MISO=13 CLK=12, CS on
 * 9/10/21/14, power-enable on GPIO 8) long after the code had stopped using
 * any of it, which is how the leg-to-connector table below came to be read
 * off the wrong schematic.
 *
 *   SPI2_HOST  MOSI=6  MISO=17  CLK=16
 *   CS: CN3=15  CN4=7  CN5=4  CN6=5
 *   Leg <-> connector:  FR=CN4  FL=CN5  RR=CN3  RL=CN6
 *   No power enable -- the servo rail is tied to Vbat+ unconditionally.
 *
 * Servo ID -> leg mapping (unchanged from the gait code):
 *   1,2,3   = Front Right (board 0)
 *   4,5,6   = Front Left  (board 1)
 *   7,8,9   = Rear  Right (board 2)
 *   10,11,12= Rear  Left  (board 3)
 */
#ifndef DRIVER_BOARD_H
#define DRIVER_BOARD_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------
 * SERVO BOARD VARIANT SELECT
 *   1 = normal  (black board - this robot, as wired now)
 *   2 = swapped (the other build: in each leg the HIP and CALF servos are
 *                plugged the other way round -> physical channels swapped
 *                1<->3, 4<->6, 7<->9, 10<->12; thighs 2,5,8,11 unchanged)
 *   3 = purple board  (same design as board 1, but the 1-2-3 group sits where
 *                4-5-6 is and 7-8-9 where 10-11-12 is. The leg groups swap
 *                straight across with NO within-leg reversal. Net remap,
 *                verified on hardware: 1<->4, 2<->5, 3<->6, 7<->10, 8<->11,
 *                9<->12.)
 *
 * db_phys() maps a LOGICAL servo id (what the gait / IK / CLI / calibration
 * all use, 1..12) to the PHYSICAL channel on the driver boards. Doing the
 * remap HERE - at the single hardware boundary - means every path agrees:
 * the walk, the `pos` command, sweep/swalk, position feedback and the
 * offset[] calibration all address the same servo by the same id. Set
 * SERVO_BOARD and re-flash to switch builds.
 * ---------------------------------------------------------------------- */
#ifndef SERVO_BOARD
#define SERVO_BOARD 1
#endif

static inline int db_phys(int logical){
#if SERVO_BOARD == 2
    switch(logical){
        case 1:  return 3;   case 3:  return 1;
        case 4:  return 6;   case 6:  return 4;
        case 7:  return 9;   case 9:  return 7;
        case 10: return 12;  case 12: return 10;
        default: return logical;
    }
#elif SERVO_BOARD == 3
    /* purple board: the 1-2-3 group sits where 4-5-6 is and 7-8-9 where
     * 10-11-12 is, AND the thigh/calf are reversed within each leg. Net,
     * verified on hardware: 1<->4, 2<->6, 3<->5, 7<->10, 8<->12, 9<->11.
     *
     * This said 2<->5, 3<->6, 8<->11, 9<->12 — the leg groups swapping
     * straight across with no within-leg reversal. minipupperesp, which is
     * the build these movements were taught on and which has run a backflip
     * on this hardware, has the reversal. A straight-across map drives each
     * thigh with its own calf's angle, so the leg folds the wrong way at
     * every frame — survivable standing, not survivable inverted. */
    switch(logical){
        case 1:  return 4;   case 4:  return 1;
        case 2:  return 6;   case 6:  return 2;
        case 3:  return 5;   case 5:  return 3;
        case 7:  return 10;  case 10: return 7;
        case 8:  return 12;  case 12: return 8;
        case 9:  return 11;  case 11: return 9;
        default: return logical;
    }

#else
    return logical;
#endif
}

/* Inverse: given a PHYSICAL channel (1..12), return the LOGICAL servo (1..12)
 * wired to it. Used by driver_board_sync_write() to route pos[] data.
 *
 * For every variant this file ships (1, 2 and 3) the map is its own inverse —
 * each is built from disjoint two-element swaps — so this is just db_phys()
 * again. That is worth stating rather than assuming: a variant with a
 * cross-board channel (one leg's servo on another leg's board) would break the
 * symmetry, and this function would then need a table of its own instead. The
 * two white-pupper variants that used to live here were exactly that case, and
 * they shipped a db_phys_inv that silently fell through to the wrong map.
 */
static inline int db_phys_inv(int physical)
{
    return db_phys(physical);
}

/* Initialise the SPI bus and the 4 driver-board devices. Call once at
 * start-up, before the gait task exists.
 *
 * The bus pins and the four chip selects come from robot/board_config.h.
 * There is no power-enable pin on this board — see SERVO_HAS_POWER_EN there —
 * so the call to driver_board_power() below is a no-op.
 *
 * SPI2 is used only by these four boards. The IMU moved to I2C with this
 * board (BMI270/BMM150), so unlike the Mini Pupper 2 build there is no second
 * device on the bus and no QMI8658C on CS=38 to share it with.
 *
 * Returns false if the bus or any of the four devices could not be brought up.
 *
 * All entry points below are internally serialised by one mutex, so the gait
 * task (core 1) and the Servo Studio HTTP handlers (core 0) may call them
 * concurrently without corrupting a config request/reply pair. */
bool driver_board_init(void);

/* Enable / disable servo bus power.
 *
 * A no-op on this board: CN3..CN6 sit on Vbat+ through OR-ing diodes, so the
 * rail is live as soon as the battery is. Kept so that callers do not have to
 * know which board they are on — the pin is GPIO_NUM_NC and the body compiles
 * out. (On the Mini Pupper 2 board this drove GPIO 8.) */
void driver_board_power(bool on);

/* Push all 12 setpoints to the four driver boards in one pass (4 SPI frames).
 *   pos[12]   : per-servo goal position, SCS scale 0..1023 (511 = centre),
 *               index 0 == servo ID 1 ... index 11 == servo ID 12.
 *   cur_mA[12]: per-servo current/torque limit in milliamps (the "torque" field).
 * Feedback (present position + present current) is captured on the same
 * transaction and cached for driver_board_present_*().
 */
void driver_board_sync_write(const uint16_t pos[12], const uint16_t cur_mA[12]);

/* Every servo to IDLE -- motor off, the joint goes limp -- in one pass, with
 * feedback read on the way (so present positions are fresh). The next
 * driver_board_sync_write() puts them back in position control. Returns how
 * many of the four boards answered. */
int driver_board_sync_idle(void);

/* Did this servo's board answer the LAST driver_board_sync_idle()? Its
 * present position is only worth anything if it did. ch = 1..12. */
bool driver_board_present_fresh(int ch);

/* ---- SCS full scale, AT32 deci-degrees (default 3100 = 310.0 deg) -------
 * Keep equal to the boards' own range_position_deg x 10. The boards keep
 * that in their flash, so it outlives any ESP reflash and is NOT safe to
 * assume: minipupperesp's 'guard' command trims it (typically to 300 deg
 * => 3000) and a robot that has been guarded stays guarded. */
void  driver_board_set_scale_dd(float v);
float driver_board_get_scale_dd(void);

/* Ask the boards what their range_position_deg actually is and adopt it.
 * Call once after driver_board_init(), before the gait task starts — the
 * config path needs the SPI bus to itself. Returns the scale now in use,
 * which is the 3100 default if no board answered. */
float driver_board_probe_scale(void);

/* Did a board actually answer that query, or is the scale still the built-in
 * default? A skill carrying recorded tick tables needs to know the difference
 * -- replaying them against a guessed scale is how a taught pose comes out the
 * wrong size. */
bool driver_board_probe_answered(void);

/* Cached feedback from the last sync_write. ch = 1..12. */
int16_t  driver_board_present_current(int ch);   /* motor current, mA (signed) */
uint16_t driver_board_present_position(int ch);  /* SCS scale 0..1023          */

/* ---- NTC servo temperature ---------------------------------------------
 * The AT32 firmware samples one 10k NTC per servo (MTA10103F3380F00, 10k
 * pull-up to 3V3, NTC to GND) at ~22 Hz and ships the reading in the
 * feedback frame's reserved1 field as a SIGNED value in 0.1 degC
 * (253 => 25.3 degC).
 *
 * That field rides along on EVERY feedback frame, so this cache is
 * refreshed FOR FREE by driver_board_sync_write() while the gait runs -
 * zero extra SPI traffic. When the gait is parked (CLI mode) refresh it
 * with driver_board_poll() / driver_board_poll_board().
 *
 * Returns degC, or DB_TEMP_INVALID if that servo has never answered.     */
#define DB_TEMP_INVALID  (-273.0f)

float driver_board_present_temperature(int ch);  /* degC, ch = 1..12 */

/* Refresh the whole feedback cache (position / current / temperature) for
 * ONE board by resending its last commanded frame. board = 0..3 in
 * PHYSICAL board order (FR, FL, RR, RL). Setpoints are unchanged, so this
 * is safe while parked in CLI mode - an idle servo stays idle. */
bool driver_board_poll_board(int board /*0..3*/);

/* ---- AT32 sms_config parameter access over SPI (web CLI) ----------------
 * Parameter ids match the AT32 UART CLI table order.
 * NOTE: do not run these concurrently with driver_board_sync_write() —
 * pause the gait loop first (CLI mode).                                    */
enum {
    DB_PARAM_REVERSE_POSITION_SENSOR = 0,
    DB_PARAM_MIN_POSITION_ADC,      /* 1 */
    DB_PARAM_MAX_POSITION_ADC,      /* 2 */
    DB_PARAM_RANGE_POSITION_DEG,    /* 3 */
    DB_PARAM_REVERSE_MOTOR,         /* 4 */
    DB_PARAM_KP_POSITION,           /* 5 */
    DB_PARAM_KD_POSITION,           /* 6 */
    DB_PARAM_KP_CURRENT,            /* 7 */
    DB_PARAM_KFF_CURRENT,           /* 8 */
    DB_PARAM_MAX_PWM_DUTY_CYCLE,    /* 9 */
    DB_PARAM_KI_POSITION,           /* 10  AT32 fw >= 0.0.7: position integral gain (0 = off)   */
    DB_PARAM_CONTROL_MODE,          /* 11  AT32 fw >= 0.0.8: DB_CONTROL_MODE_CURRENT / _PWM      */
    DB_PARAM_PWM_SCALE,             /* 12  AT32 fw >= 0.0.9: PWM mode duty per PID output        */
    DB_PARAM_COUNT
};

/* Values of DB_PARAM_CONTROL_MODE (from minipupperesp): which algorithm runs
 * inside the AT32 - switched at run time, no reflash. */
#define DB_CONTROL_MODE_CURRENT  0   /* position PD -> current loop -> PWM (AT32 fw 0.0.6 loop) */
#define DB_CONTROL_MODE_PWM      1   /* position PID -> PWM directly                          */

/* Parameter name for id, or NULL if out of range. */
const char *driver_board_param_name(int param_id);
/* Name -> id, or -1 if unknown. */
int driver_board_param_id(const char *name);

/* servo = 1..12 (global id). Return true on success. */
/* ── The SPI2 bus lock ────────────────────────────────────────────────────
 *
 * SPI2 carries the four AT32 driver boards and nothing else on this board.
 * ESP-IDF serialises individual transactions, which is enough for anything
 * that talks in single transfers.
 *
 * A config request is NOT a single transfer. It is a request followed by a NOP
 * that clocks the reply back out, and the two must not be separated — split
 * them and the reply is left pending in the AT32's tx buffer, where the next
 * decode reads it as garbage (a 31.03 degC reply becomes 1540.8 degC).
 *
 * driver_board.c already holds its own mutex across each whole operation, and
 * because the IMU no longer shares this bus that is sufficient on its own.
 * These two calls are kept for any other task that later needs to hold the bus
 * across more than one transfer (a display, a second SPI sensor): take this
 * around the whole sequence, not around each frame.
 */
void driver_board_bus_lock(void);
void driver_board_bus_unlock(void);

bool driver_board_set_param(int servo, int param_id, float value);
bool driver_board_get_param(int servo, int param_id, float *out);

/* board = 0..3, or -1 for all boards. */
bool driver_board_save_config(int board);      /* commit sms_config to flash */
bool driver_board_factory_restore(int board);  /* factory defaults (RAM only) */

/* ---- direct single-servo control (web CLI pos/tor/stop) -----------------
 * Sends one servo command frame to the servo's board; the OTHER two servos
 * on that board keep their last commanded values (shadow of the last
 * sync_write / direct write). Angle is the RAW AT32 angle in degrees,
 * 0..270 (135 = centre) - same units as the AT32 UART CLI 'pos' command. */
#define DB_MODE_IDLE     0
#define DB_MODE_POSITION 1
#define DB_MODE_TORQUE   2

bool driver_board_direct(int servo /*1..12*/, uint16_t mode,
                         float pos_deg, int16_t current_mA);

/* Refresh feedback for this servo's board by resending the last commanded
 * frame (setpoints unchanged). Use driver_board_present_*() afterwards.
 * Used by the web CLI 'trace' live view.                                  */
bool driver_board_poll(int servo /*1..12*/);

/* ---- staged batch write (Unitree-style low-level command) ---------------
 * driver_board_stage() updates ONE servo's shadow without touching the bus;
 * driver_board_commit() then pushes all four boards in a single pass, so a
 * whole-robot command costs 4 SPI frames rather than 12.
 *
 * pos_deg is the RAW AT32 angle 0..270 (135 = centre) — the same units as
 * driver_board_direct(), NOT the gait's calibrated frame.
 *
 * kp/kd ride in the per-servo command frame. The stock AT32 firmware ignores
 * them and uses its stored sms_config gains instead, which is why every other
 * path here sends 0. Pass 0 to keep that behaviour; pass non-zero only if your
 * board firmware is known to honour per-frame gains.
 */
void driver_board_stage(int servo /*1..12*/, uint16_t mode,
                        float pos_deg, int16_t current_mA,
                        uint16_t kp, uint16_t kd);

/* Push the staged state of all four boards. Returns false if any board did
 * not answer with a valid feedback frame. */
bool driver_board_commit(void);

/* ---- live control-loop values (web trace, needs new AT32 firmware) ------
 * Same set the AT32 uart_trace() prints:                                   */
enum {
    DB_LIVE_POS_ADC = 0,        /* position sensor, raw ADC     */
    DB_LIVE_CUR_ADC,            /* current sense, raw ADC       */
    DB_LIVE_SETPOINT_POS_DEG,   /* commanded position, deg      */
    DB_LIVE_PRESENT_POS_DEG,    /* measured position, deg       */
    DB_LIVE_ERROR_POS_DEG,      /* position error, deg          */
    DB_LIVE_MAX_CURRENT_MA,     /* current cap (position mode)  */
    DB_LIVE_SETPOINT_CUR_MA,    /* current setpoint (torque)    */
    DB_LIVE_PRESENT_CUR_MA,     /* measured motor current, mA   */
    DB_LIVE_ERROR_CUR_MA,       /* current error, mA            */
    DB_LIVE_PWM_DUTY,           /* PWM duty cycle 0..1          */
    DB_LIVE_MODE,               /* 0 idle 1 position 2 torque   */
    DB_LIVE_LOOP_COUNTER,       /* control loop tick counter    */
    DB_LIVE_TEMPERATURE_C,      /* NTC servo temperature, degC  */
    DB_LIVE_COUNT
};

/* One live value past DB_LIVE_COUNT, outside the enum on purpose: Servo
 * Studio sizes its key table by DB_LIVE_COUNT and reads every entry, and this
 * one needs AT32 fw >= 0.0.9. driver_board_get_live() accepts it. */
#define DB_LIVE_SUPPLY_CURRENT_MA  13   /* battery-side current |I| x duty, mA */

bool driver_board_get_live(int servo /*1..12*/, int live_id, float *out);

/* ---- raw host link: the Pi's RL policy (mp4-link's mp3-link protocol) ----
 * The policy commands every joint itself at 200 Hz in RAW AT32 deci-degrees
 * (0..scale, no direction flip, no calibration offset): the units mp4-link's
 * SET_ALL frame carries. Everything here is indexed by LOGICAL servo id and
 * mapped through db_phys() like the rest of this file.
 *
 * mode[i] is DB_MODE_IDLE, DB_MODE_POSITION or DB_MODE_TORQUE, or
 * DB_MODE_HOLD: keep the position that channel was last commanded (the parked
 * gait pose, until the host first arms). A channel last left in any other
 * mode (a skill's torque command, say) goes IDLE instead, and per-frame gains
 * are dropped. In MODE_POSITION cur_mA is the current cap, as everywhere
 * else.
 *
 * *answered and *fresh, either may be NULL: bit L is logical leg L (servos
 * 3L+1..3L+3). answered = its board replied; fresh = the reply was servo
 * feedback and the caches were refreshed. A config reply, and the first
 * ordinary frame after a config exchange (which still carries the parameter
 * in the board's first channel), are answered but not fresh.
 *
 * The SCS-scale caches the gait reads (driver_board_present_position() and
 * friends) are refreshed too, so the app and Servo Studio keep seeing the
 * legs while the host drives them. */
#define DB_MODE_HOLD 0xFFFFu
void driver_board_sync_raw(const uint16_t mode[12], const uint16_t pos_dd[12],
                           const int16_t cur_mA[12], uint8_t *answered, uint8_t *fresh);

/* What the last driver_board_sync_raw() read for a logical servo: position in
 * RAW deci-degrees, motor current in mA, NTC temperature in 0.1 degC and the
 * board's status word. Returns false, with every value 0, when that servo's
 * board gave no feedback on the last sync_raw. Any pointer may be NULL. */
bool driver_board_raw_feedback(int servo, uint16_t *pos_dd, int16_t *cur_mA,
                               int16_t *temp_dc, uint16_t *status);

/* Forget the raw feedback: every servo reads "no feedback" until the next
 * driver_board_sync_raw(). Called when a host session starts, so nothing from
 * an earlier session is reported as current. */
void driver_board_raw_invalidate(void);

#ifdef __cplusplus
}
#endif

#endif /* DRIVER_BOARD_H */

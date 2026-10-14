/* driver_board.h - the 4 AT32 servo boards (one per leg), reached over SPI.
 *
 * Servo ids 1..12:   1  2  3 = front right   (hip, thigh, calf)
 *                    4  5  6 = front left
 *                    7  8  9 = rear right
 *                   10 11 12 = rear left
 * Angles are the servo's own angle: 0 .. 310 deg, centre 155 deg.
 */
#ifndef DRIVER_BOARD_H
#define DRIVER_BOARD_H

#include <stdint.h>
#include <stdbool.h>

/* Which wiring this robot has (see driver_board.c):
 *   1 = normal (black board)   2 = hip and calf plugs swapped   3 = purple board */
#define SERVO_BOARD 1

/* servo modes */
#define DB_MODE_IDLE     0     /* limp                                   */
#define DB_MODE_POSITION 1     /* hold an angle, current limited to mA   */
#define DB_MODE_TORQUE   2     /* push with a fixed current (mA)         */

/* servo settings (ids as the AT32 numbers them) */
enum {
    DB_PARAM_REVERSE_POSITION_SENSOR, DB_PARAM_MIN_POSITION_ADC, DB_PARAM_MAX_POSITION_ADC,
    DB_PARAM_RANGE_POSITION_DEG, DB_PARAM_REVERSE_MOTOR, DB_PARAM_KP_POSITION,
    DB_PARAM_KD_POSITION, DB_PARAM_KP_CURRENT, DB_PARAM_KFF_CURRENT,
    DB_PARAM_MAX_PWM_DUTY_CYCLE, DB_PARAM_KI_POSITION, DB_PARAM_CONTROL_MODE,
    DB_PARAM_PWM_SCALE,
    DB_PARAM_COUNT
};

/* live values (what the servo is doing right now) */
enum {
    DB_LIVE_POS_ADC,            /* raw pot reading (ADC counts)  */
    DB_LIVE_CUR_ADC,            /* raw current reading           */
    DB_LIVE_SETPOINT_POS_DEG,   /* angle it was told to go to    */
    DB_LIVE_PRESENT_POS_DEG,    /* angle it is at                */
    DB_LIVE_ERROR_POS_DEG,      /* difference                    */
    DB_LIVE_MAX_CURRENT_MA,     /* current limit                 */
    DB_LIVE_SETPOINT_CUR_MA,
    DB_LIVE_PRESENT_CUR_MA,     /* motor current now             */
    DB_LIVE_ERROR_CUR_MA,
    DB_LIVE_PWM_DUTY,           /* motor drive 0..1              */
    DB_LIVE_MODE,
    DB_LIVE_LOOP_COUNTER,
    DB_LIVE_TEMPERATURE_C,      /* motor temperature             */
    DB_LIVE_SUPPLY_CURRENT_MA,  /* only on AT32 firmware >= 0.0.9 */
    DB_LIVE_COUNT
};

void driver_board_init(void);                      /* start SPI, once at boot */

/* --- one servo --- */
bool     driver_board_direct(int id, uint16_t mode, float deg, int16_t mA);   /* command it; also refreshes its feedback */
bool     driver_board_poll(int id);                                          /* refresh its feedback only */
uint16_t driver_board_present_posdd(int id);                                 /* angle from the last feedback, 0.1 deg */

/* --- all 12 servos at once (walking): pos 0..1023 (511 = centre), current limit mA --- */
void driver_board_sync_write(const uint16_t pos[12], const uint16_t mA[12]);

/* --- settings and live values (slower: request + answer) --- */
bool driver_board_get_param(int id, int param, float *value);
bool driver_board_set_param(int id, int param, float value);   /* until power-off ... */
bool driver_board_save_config_n(int board, int *ok, int *failed); /* ... unless saved: board 0..3, -1 = all */
bool driver_board_get_live(int id, int what, float *value);
const char *driver_board_param_name(int param);
int  driver_board_param_id(const char *name);                  /* -1 if unknown */

/* --- full angle range in 0.1 deg (3100 = 310 deg); must match range_position_deg on the boards --- */
float driver_board_get_scale_dd(void);
void  driver_board_set_scale_dd(float dd);

#endif

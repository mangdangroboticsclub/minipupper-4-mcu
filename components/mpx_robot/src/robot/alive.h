/* alive.h - IMU reflexes for the Mini Pupper: "picked up -> cartoon running" and the
 * "push reflex". Pure C (math only): main.c feeds it the IMU every control tick and
 * turns what it returns into servo angles, so the same file runs on the ESP32 and in
 * the host test (tools/alive_test.c).
 *
 * FRAMES
 *   IMU input: the BMI270 as mounted on this robot (checked with the sit-up recordings:
 *   standing reads az = -1 g, nose-up 70 deg reads ay = +0.89 g, nose-up = -gx):
 *       +x = robot's LEFT, +y = FORWARD, +z = DOWN        (acc in g, gyro in deg/s)
 *   Output (body pose + paw offsets), robot frame:
 *       x = forward (mm), y = left (mm), z = up (mm)
 *       roll  + = left side UP, pitch + = nose UP, yaw + = nose LEFT   (deg)
 *   paw offsets are in the "ground" frame (x fwd, y left, z up, mm) from each paw's stand
 *   spot; the body pose moves the body over them. Leg order FR, FL, RR, RL.
 *
 * STATES
 *   STAND   standing still, breathing; watching for a lift or a push
 *   PUSHED  the push reflex is playing (brace, maybe a catch step, look at the pusher, huff)
 *   SURPRISE  it was lifted: legs stretch down and splay ("whoa!")
 *   RUN     cartoon running in the air, faster and faster
 *   DANGLE  ran for too long: the legs hang and kick now and then
 *   LANDED  put back down: paws under it, frozen
 *   LOOK    looks left, looks right ("what just happened?")
 *   SHAKE   shakes it off, then STAND
 */
#ifndef ALIVE_H
#define ALIVE_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float acc_g[3];      /* IMU accelerometer, g (sensor frame above)          */
    float gyro_dps[3];   /* IMU gyro, deg/s                                    */
    float cur_mA;        /* sum of |current| of the 8 thigh + knee servos, mA (0 = unknown) */
    int   imu_ok;        /* 0 = no fresh IMU sample: the reflexes hold still   */
} alive_in_t;

typedef struct {
    float body[6];       /* x, y, z (mm), roll, pitch, yaw (deg)               */
    float paw[4][3];     /* per-paw offsets (mm), ground frame                  */
} alive_out_t;

typedef struct {
    float lift_mm;       /* raised this far (IMU double integral) -> picked up        [30]  */
    float lift_v;        /* ... and it rose at least this fast, m/s                   [0.15]*/
    float land_mm;       /* came down at least this far since it was last held still  [25]  */
    float land_g;        /* ... and hit something: vertical force above this, g       [1.30]*/
    float push_g;        /* horizontal kick above this (g) = a push                   [0.18]*/
    float push_dps;      /* ... or the body rolled / pitched faster than this, deg/s  [45]  */
    float push_lean;     /* ... or it leans this far from how it stands, deg          [1.5] */
    float cur_ground_mA; /* > 0: while running, this much leg current = on the floor  [0]   */
    float run_hz;        /* top running speed, steps per second (per leg)             [4.0] */
    float land_jolt_g;   /* while running: two jolts bigger than this in 0.3 s = paws on the floor [0.6] */
    int   enable_lift;   /* 1 = picked-up reflex on                                   [1]   */
    int   enable_push;   /* 1 = push reflex on                                        [1]   */
} alive_param_t;

extern alive_param_t alive_p;

void        alive_reset(void);
/* one control tick: dt in seconds (~0.005) */
void        alive_step(const alive_in_t *in, float dt, alive_out_t *out);
int         alive_state(void);
const char *alive_state_name(void);
/* the live numbers, as JSON, for /alive/status (tuning the thresholds) */
int         alive_status_json(char *buf, int n);
/* set one parameter by name ("lift_mm", "push_g", ...). 1 = known name */
int         alive_set(const char *name, float value);

enum { AL_STAND = 0, AL_PUSHED, AL_SURPRISE, AL_RUN, AL_DANGLE, AL_LANDED, AL_LOOK, AL_SHAKE };

#ifdef __cplusplus
}
#endif
#endif

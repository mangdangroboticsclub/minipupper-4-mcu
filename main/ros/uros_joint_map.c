/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 */
#include "uros_joint_map.h"

#include <math.h>

/* servo_map.yaml, firmware section */
#define TH1N_DEG (-52.472448271569206)
#define TH2N_DEG (44.920296480443625)

typedef struct {
    int    servo[3];        /* hip, thigh, knee (1-based) */
    double ik_thigh, ik_knee, hip_dir;
    double ax[3];           /* axis_sign */
    double z_thigh, z_shin; /* CAD zero pose, degrees */
} leg_map_t;

static const leg_map_t s_legs[4] = {
    /* FR */ { {1, 2, 3},    -1.0,  1.0,  1.0, {-1.0,  1.0, -1.0}, -45.0, 45.0 },
    /* FL */ { {4, 5, 6},     1.0, -1.0,  1.0, {-1.0, -1.0,  1.0}, -45.0, 45.0 },
    /* RR */ { {7, 8, 9},    -1.0,  1.0, -1.0, { 1.0,  1.0, -1.0}, -45.0, 45.0 },
    /* RL */ { {10, 11, 12},  1.0, -1.0, -1.0, { 1.0, -1.0,  1.0}, -45.0, 45.0 },
};

const char *const uros_joint_names[UROS_JOINTS] = {
    "front_right_hip_joint", "front_right_leg_upper_joint", "front_right_leg_lower_joint",
    "front_left_hip_joint",  "front_left_leg_upper_joint",  "front_left_leg_lower_joint",
    "back_right_hip_joint",  "back_right_leg_upper_joint",  "back_right_leg_lower_joint",
    "back_left_hip_joint",   "back_left_leg_upper_joint",   "back_left_leg_lower_joint",
};

static double rad(double deg) { return deg * (M_PI / 180.0); }

void uros_joint_angles(const float servo_deg[12], double q[UROS_JOINTS])
{
    for (int l = 0; l < 4; ++l) {
        const leg_map_t *L = &s_legs[l];
        const double s_hip = servo_deg[L->servo[0] - 1];
        const double s_th  = servo_deg[L->servo[1] - 1];
        const double s_kn  = servo_deg[L->servo[2] - 1];

        const double a1  = TH1N_DEG + L->ik_thigh * s_th;      /* thigh from vertical */
        const double th2 = TH2N_DEG + L->ik_knee * s_kn;
        const double a2  = 90.0 - th2;                          /* shin from vertical  */

        q[3 * l + 0] = rad(L->ax[0] * L->hip_dir * s_hip);
        q[3 * l + 1] = rad(-L->ax[1] * (a1 - L->z_thigh));
        q[3 * l + 2] = rad(-L->ax[2] * ((a2 - a1) - (L->z_shin - L->z_thigh)));
    }
}

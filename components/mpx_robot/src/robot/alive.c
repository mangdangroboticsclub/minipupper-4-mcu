/* alive.c - IMU reflexes: picked up -> cartoon running, and the push reflex.
 * See alive.h for the frames, the states and the parameters. No ESP-IDF in here. */
#include "alive.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI_F 3.14159265f
#define G    9.81f

alive_param_t alive_p = {
    .lift_mm = 30.0f, .lift_v = 0.12f, .land_mm = 25.0f, .land_g = 1.30f,
    .push_g = 0.18f, .push_dps = 45.0f, .push_lean = 1.5f, .cur_ground_mA = 0.0f, .run_hz = 4.0f, .land_jolt_g = 0.6f,
    .enable_lift = 1, .enable_push = 1,
};

static const char *NAMES[] = { "stand", "pushed", "surprise", "run", "dangle", "landed", "look", "shake" };

/* ---- estimator ---------------------------------------------------------- */
static float up[3];          /* world "up" in the robot frame (unit)            */
static float g0;             /* |acc| when still (the IMU's own 1 g, ~1.01)     */
static float s_lp;           /* vertical specific force, low-passed (g)         */
static float v, h;           /* vertical velocity (m/s) and height (m), IMU     */
static float v_peak;         /* fastest rise since it was last still            */
static float h_ref;          /* height when it was last held still (in the air) */
static float still_t;        /* how long it has been still (s)                  */
static float ah_lp[2];       /* horizontal specific force, fwd / left (g)       */
static float vh[2];          /* horizontal velocity, leaky (m/s): WHICH WAY it was pushed,
                                even while it is already slowing down again     */
static float cur_lp;         /* leg current, low-passed (mA)                    */
#define AH_N 96              /* the last ~0.5 s of the horizontal kick, to find where a push STARTED */
static float ah_hist[AH_N][2]; static int ah_i;
static float jolt;           /* biggest jolt this tick: |vertical - 1 g| or horizontal (g) */
static float jolt_t[2];      /* when the last two big jolts came (s)            */
static float tilt_deg, roll_deg, pitch_deg;
static float roll0, pitch0;  /* how it stands when nothing happens (slow average)  */
static float gyro_b[3];      /* roll / pitch / yaw rate, robot frame (deg/s)    */
static int   est_ok;

/* ---- state machine ------------------------------------------------------ */
static int   st;
static float st_t;           /* time in this state                              */
static float cool;           /* push cooldown (s)                               */
static float al_clock;        /* running time (breathing)                        */
static alive_out_t cur, from;
static float run_ph, run_f;
static float push_m[2], push_sig; static int push_step;
static float dangle_next;
static int   n_lifts, n_lands, n_pushes;
static float trig[6]; static int trig_by;    /* what set off the last push: ah fwd/left, vh fwd/left, gyro roll/pitch */

static float clampf(float x, float a, float b){ return x < a ? a : x > b ? b : x; }
static float ease(float u){ u = clampf(u, 0, 1); return u * u * (3 - 2 * u); }
static float lerp(float a, float b, float u){ return a + (b - a) * u; }

static void zero_out(alive_out_t *o){ memset(o, 0, sizeof *o); }
static void blend(alive_out_t *o, const alive_out_t *a, const alive_out_t *b, float u){
    for(int i = 0; i < 6; i++) o->body[i] = lerp(a->body[i], b->body[i], u);
    for(int l = 0; l < 4; l++) for(int k = 0; k < 3; k++) o->paw[l][k] = lerp(a->paw[l][k], b->paw[l][k], u);
}
static void enter(int s){ st = s; st_t = 0; from = cur; }

void alive_reset(void){
    up[0] = 0; up[1] = 0; up[2] = 1; g0 = 1.0f; s_lp = 1.0f;
    v = h = v_peak = h_ref = 0; still_t = 0; ah_lp[0] = ah_lp[1] = 0; vh[0] = vh[1] = 0; cur_lp = 0; jolt = 0; jolt_t[0] = jolt_t[1] = 0; memset(ah_hist, 0, sizeof ah_hist); ah_i = 0;
    tilt_deg = roll_deg = pitch_deg = 0; roll0 = pitch0 = 0; est_ok = 0;
    zero_out(&cur); zero_out(&from);
    st = AL_STAND; st_t = 0; cool = 1.0f; al_clock = 0;
    run_ph = 0; run_f = 0; push_sig = 0; push_step = 0; dangle_next = 0;
}

int alive_state(void){ return st; }
const char *alive_state_name(void){ return NAMES[st]; }

/* ---- the IMU -> up, vertical motion, horizontal kick ---------------------- */
static void estimate(const alive_in_t *in, float dt){
    /* sensor (x left, y fwd, z down) -> robot (x fwd, y left, z up) */
    const float a[3] = { in->acc_g[1], in->acc_g[0], -in->acc_g[2] };
    const float w[3] = { in->gyro_dps[1], in->gyro_dps[0], -in->gyro_dps[2] };   /* roll, pitch-about-left, yaw */
    for(int i = 0; i < 3; i++) gyro_b[i] = w[i];
    const float an = sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]);
    /* 1) carry "up" with the gyro: a world-fixed vector turns by -w x up */
    const float wr[3] = { w[0] * PI_F / 180, w[1] * PI_F / 180, w[2] * PI_F / 180 };
    float c[3] = { wr[1]*up[2] - wr[2]*up[1], wr[2]*up[0] - wr[0]*up[2], wr[0]*up[1] - wr[1]*up[0] };
    for(int i = 0; i < 3; i++) up[i] -= c[i] * dt;
    /* 2) pull it toward the accelerometer, faster when the reading is ~1 g (not shaking) */
    if(an > 0.2f){
        const float tau = fabsf(an - g0) < 0.08f ? 0.25f : 2.0f;
        for(int i = 0; i < 3; i++) up[i] += (a[i] / an - up[i]) * clampf(dt / tau, 0, 1);
    }
    float un = sqrtf(up[0]*up[0] + up[1]*up[1] + up[2]*up[2]);
    if(un < 1e-3f){ up[0] = up[1] = 0; up[2] = 1; un = 1; }
    for(int i = 0; i < 3; i++) up[i] /= un;
    tilt_deg  = acosf(clampf(up[2], -1, 1)) * 180 / PI_F;
    pitch_deg = atan2f(up[0], up[2]) * 180 / PI_F;        /* nose up -> up has a +fwd part */
    roll_deg  = atan2f(-up[1], up[2]) * 180 / PI_F;       /* left side up -> up leans right */
    /* 3) vertical and horizontal specific force */
    const float s = a[0]*up[0] + a[1]*up[1] + a[2]*up[2];
    const float ah[2] = { a[0] - s * up[0], a[1] - s * up[1] };
    s_lp += (s - s_lp) * clampf(dt / 0.03f, 0, 1);
    { const float hm = sqrtf(ah[0]*ah[0] + ah[1]*ah[1]); jolt = fabsf(s - g0) > hm ? fabsf(s - g0) : hm; }
    for(int i = 0; i < 2; i++){
        ah_lp[i] += (ah[i] - ah_lp[i]) * clampf(dt / 0.012f, 0, 1);
        vh[i] += ah[i] * G * dt;
        vh[i] -= vh[i] * dt / 0.25f;
    }
    ah_hist[ah_i][0] = ah_lp[0]; ah_hist[ah_i][1] = ah_lp[1]; ah_i = (ah_i + 1) % AH_N;
    /* 4) still? then this is 1 g, and nothing is moving */
    const float wn = sqrtf(w[0]*w[0] + w[1]*w[1] + w[2]*w[2]);
    const int held = st == AL_RUN || st == AL_DANGLE || st == AL_SURPRISE;
    const int still = held ? (fabsf(s_lp - g0) < 0.06f) : (fabsf(s - g0) < 0.04f && wn < 8.0f);
    still_t = still ? still_t + dt : 0;
    if(!held && still_t > 0.3f){
        g0 += (an - g0) * clampf(dt / 2.0f, 0, 1);
        roll0 += (roll_deg - roll0) * clampf(dt / 1.0f, 0, 1);
        pitch0 += (pitch_deg - pitch0) * clampf(dt / 1.0f, 0, 1);
    }
    /* 5) vertical velocity / height (leaky double integral, zeroed when still) */
    v += (s - g0) * G * dt;
    v -= v * dt / (held ? 3.0f : 1.5f);
    h += v * dt;
    if(!held) h -= h * dt / 3.0f;
    if(v > v_peak) v_peak = v;
    if(still_t > (held ? 0.08f : 0.10f)){
        v = 0; v_peak = 0;
        if(held) h_ref = h; else h *= 0.9f;
    }
    if(held && h > h_ref) h_ref = h;           /* it only counts coming DOWN from the highest */
    cur_lp += (in->cur_mA - cur_lp) * clampf(dt / 0.08f, 0, 1);
    est_ok = 1;
}

/* ---- the moves ---------------------------------------------------------- */
static void stand_pose(alive_out_t *o){
    zero_out(o);
    o->body[2] = 0.8f * sinf(2 * PI_F * al_clock / 1.8f);      /* breathing */
    o->body[4] = 0.4f * sinf(2 * PI_F * al_clock / 1.8f - 0.5f);
}

static void run_legs(alive_out_t *o, float f_target, float dt, float amp){
    run_f += (f_target - run_f) * clampf(dt / 0.35f, 0, 1);
    run_ph += 2 * PI_F * run_f * dt;
    zero_out(o);
    for(int l = 0; l < 4; l++){
        /* trot pairs: FR+RL and FL+RR half a cycle apart (the reactions cancel) */
        const float th = run_ph + ((l == 0 || l == 3) ? 0.0f : PI_F);
        o->paw[l][0] = -20.0f * amp * cosf(th);               /* sweeps back while low */
        o->paw[l][2] = amp * (8.0f + 13.0f * sinf(th)) - 4.0f;
        o->paw[l][1] = ((l == 1 || l == 3) ? 1.0f : -1.0f) * 3.0f;   /* a little wide */
    }
}

static void push_pose(alive_out_t *o, float t){
    /* m = where the push moved it (fwd, left); s = where the pusher is */
    const float m0 = push_m[0], m1 = push_m[1], s0 = -m0, s1 = -m1, sg = push_sig;
    const float brace = 6.0f + 8.0f * sg, lean = 4.0f + 4.0f * sg;
    const float d = push_step ? 14.0f * sg + 6.0f : 0.0f;
    zero_out(o);
    /* 1) brace into the push: body toward the pusher, a bit lower, leaning into it (0 - 0.10 s) */
    float wb = ease(t / 0.10f) * (1 - ease((t - (push_step ? 0.40f : 0.25f)) / 0.35f));
    o->body[0] = -m0 * brace * wb; o->body[1] = -m1 * brace * wb;
    o->body[2] = -(3.0f + 5.0f * sg) * wb;
    o->body[3] = -s1 * lean * wb;                    /* pusher on the left -> left side down */
    o->body[4] = -s0 * lean * wb;                    /* pusher in front -> nose down        */
    /* 2) a big push: a quick catch step with each diagonal pair, the way it was pushed */
    if(push_step){
        for(int l = 0; l < 4; l++){
            const float t0 = (l == 0 || l == 3) ? 0.10f : 0.25f;
            const float u = ease((t - t0) / 0.15f);
            o->paw[l][0] = m0 * d * u; o->paw[l][1] = m1 * d * u;
            o->paw[l][2] = 12.0f * sinf(PI_F * clampf((t - t0) / 0.15f, 0, 1));
        }
        const float wc = ease((t - 0.25f) / 0.35f);  /* the body follows its paws */
        o->body[0] += m0 * d * wc; o->body[1] += m1 * d * wc;
    }
    /* 3) turn to look at who did that: yaw toward the pusher, head tilt, chin up */
    float yaw = atan2f(s1, s0) * 180 / PI_F;
    yaw = clampf(yaw, -28.0f, 28.0f);
    if(s0 < -0.2f) yaw = (s1 >= 0 ? 28.0f : -28.0f);   /* from behind: over the shoulder */
    const float wl = ease((t - 0.45f) / 0.25f) * (1 - ease((t - 1.75f) / 0.35f));
    o->body[5] += yaw * wl;
    o->body[3] += (yaw >= 0 ? -5.0f : 5.0f) * wl;
    o->body[4] += 5.0f * wl;
    /* 4) "hmph": a quick little head shake */
    const float wh = (t > 1.3f && t < 1.7f) ? sinf(PI_F * (t - 1.3f) / 0.4f) : 0.0f;
    o->body[5] += 4.0f * wh * sinf(2 * PI_F * 6.0f * (t - 1.3f));
}
#define PUSH_T 2.15f

void alive_step(const alive_in_t *in, float dt, alive_out_t *out){
    al_clock += dt; st_t += dt;
    if(cool > 0) cool -= dt;
    if(in->imu_ok) estimate(in, dt);
    alive_out_t tgt;
    switch(st){
    case AL_STAND: {
        stand_pose(&tgt);
        blend(&cur, &from, &tgt, ease(st_t / 0.4f));
        if(!est_ok || cool > 0) break;
        if(alive_p.enable_lift && h > alive_p.lift_mm / 1000.0f && v_peak > alive_p.lift_v){
            n_lifts++; h_ref = h; enter(AL_SURPRISE); break;
        }
        const float ahm = sqrtf(ah_lp[0]*ah_lp[0] + ah_lp[1]*ah_lp[1]);
        const float rr = fabsf(gyro_b[0]) > fabsf(gyro_b[1]) ? fabsf(gyro_b[0]) : fabsf(gyro_b[1]);
        const float lr = roll_deg - roll0, lp = pitch_deg - pitch0;
        static float lean_t = 0;
        const int calm_z = fabsf(s_lp - g0) < 0.10f && fabsf(v) < 0.04f && fabsf(h) < 0.006f;   /* not being lifted */
        lean_t = (sqrtf(lr*lr + lp*lp) > alive_p.push_lean && calm_z) ? lean_t + dt : 0;
        const int leaned = lean_t > 0.08f;
        if(alive_p.enable_push && calm_z && (ahm > alive_p.push_g || rr > alive_p.push_dps || leaned)){
            lean_t = 0;
            trig[0] = ah_lp[0]; trig[1] = ah_lp[1]; trig[2] = vh[0]; trig[3] = vh[1]; trig[4] = gyro_b[0]; trig[5] = gyro_b[1];
            trig_by = ahm > alive_p.push_g ? 1 : rr > alive_p.push_dps ? 2 : 3;
            /* WHICH WAY: the lean says it best - a push leans the body away from the pusher and
             * it is still leaning that way while it springs back (when the acceleration already
             * points the other way). Leaning right = left side up = +roll. */
            const float dr = roll_deg - roll0, dp = pitch_deg - pitch0, dn = sqrtf(dr*dr + dp*dp);
            const float vhm = sqrtf(vh[0]*vh[0] + vh[1]*vh[1]);
            trig[4] = dr; trig[5] = dp;
            /* the FIRST big kick in the last half second is the push; what comes after it can
             * be the body springing back (pointing the other way) */
            float amax = 0; for(int j = 0; j < AH_N; j++){ const float q = hypotf(ah_hist[j][0], ah_hist[j][1]); if(q > amax) amax = q; }
            int first = -1;
            for(int j = 0; j < AH_N && amax > 0.08f; j++){
                const int q = (ah_i + j) % AH_N;                  /* oldest first */
                if(hypotf(ah_hist[q][0], ah_hist[q][1]) > 0.6f * amax){ first = q; break; }
            }
            if(first >= 0){
                const float q = hypotf(ah_hist[first][0], ah_hist[first][1]);
                push_m[0] = ah_hist[first][0] / q; push_m[1] = ah_hist[first][1] / q; trig_by += 20;
            }
            else if(dn > 0.6f){ push_m[0] = -dp / dn; push_m[1] = -dr / dn; trig_by += 10; }
            else if(vhm > 0.01f){ push_m[0] = vh[0] / vhm; push_m[1] = vh[1] / vhm; }
            else if(ahm > 0.05f){ push_m[0] = ah_lp[0] / ahm; push_m[1] = ah_lp[1] / ahm; }
            else{                                           /* only the gyro saw it: tipping direction */
                const float mx = -gyro_b[1], my = -gyro_b[0];
                const float mn = sqrtf(mx*mx + my*my) + 1e-6f;
                push_m[0] = mx / mn; push_m[1] = my / mn;
            }
            float k = ahm / alive_p.push_g;
            if(rr / alive_p.push_dps > k) k = rr / alive_p.push_dps;
            if(sqrtf(lr*lr + lp*lp) / alive_p.push_lean > k) k = sqrtf(lr*lr + lp*lp) / alive_p.push_lean;
            push_sig = clampf((k - 1.0f) / 1.5f, 0, 1);
            push_step = 0;   /* (a catch step made the sim robot fall more often - off) */
            n_pushes++; enter(AL_PUSHED);
        }
        break; }
    case AL_PUSHED:
        push_pose(&tgt, st_t);
        blend(&cur, &from, &tgt, ease(st_t / 0.05f));
        if(st_t < 0.08f){                            /* the push is still growing: size it by its peak */
            const float ahm = sqrtf(ah_lp[0]*ah_lp[0] + ah_lp[1]*ah_lp[1]);
            const float sg = clampf((ahm / alive_p.push_g - 1.0f) / 1.5f, 0, 1);
            if(sg > push_sig) push_sig = sg;
        }
        if(st_t >= PUSH_T){
            /* it may have stepped: the new spot is home now (paws and body both moved) */
            zero_out(&cur); from = cur; cool = 0.6f; v = h = v_peak = h_ref = 0; vh[0] = vh[1] = 0; memset(ah_hist, 0, sizeof ah_hist); enter(AL_STAND);
        }
        break;
    case AL_SURPRISE:                                   /* "whoa!": legs stretch down + splay */
        zero_out(&tgt);
        for(int l = 0; l < 4; l++){
            tgt.paw[l][2] = -9.0f;
            tgt.paw[l][1] = ((l == 1 || l == 3) ? 1.0f : -1.0f) * 9.0f;
            tgt.paw[l][0] = (l < 2 ? 6.0f : -6.0f);
        }
        blend(&cur, &from, &tgt, ease(st_t / 0.12f));
        if(st_t > 0.35f){ run_ph = 0; run_f = 1.2f; enter(AL_RUN); }
        break;
    case AL_RUN: {
        const float f = st_t < 0.9f ? lerp(1.5f, alive_p.run_hz, st_t / 0.9f) : alive_p.run_hz;
        run_legs(&tgt, f, dt, 1.0f);
        blend(&cur, &from, &tgt, ease(st_t / 0.2f));
        if(st_t > 8.0f){ dangle_next = 1.2f; enter(AL_DANGLE); }
        goto check_land; }
    case AL_DANGLE: {
        /* tired: legs hang; every ~1.5 s a short burst of kicking */
        const float tb = st_t - dangle_next;
        if(tb > 0 && tb < 0.7f) run_legs(&tgt, alive_p.run_hz, dt, 0.8f * sinf(PI_F * tb / 0.7f));
        else { zero_out(&tgt); for(int l = 0; l < 4; l++) tgt.paw[l][2] = -8.0f; }
        if(tb > 1.6f) dangle_next = st_t + 0.9f;
        blend(&cur, &from, &tgt, ease(st_t / 0.4f));
        if(st_t > 0.4f) from = tgt, cur = tgt;
        goto check_land; }
    check_land: {
        if(st_t < 0.3f) break;
        const float desc = (h_ref - h) * 1000.0f;
        /* running legs that hit the floor kick the body around (sim: 1-4 g jolts, a hand
         * holding it: < 0.05 g) - two big jolts within 0.3 s = its paws are on the floor */
        if(jolt > alive_p.land_jolt_g){ jolt_t[0] = jolt_t[1]; jolt_t[1] = al_clock; }
        const int shaken = alive_p.land_jolt_g > 0 && jolt_t[0] > 0 && al_clock - jolt_t[0] < 0.3f && al_clock - jolt_t[1] < 0.05f;
        const int imu_land = (desc > alive_p.land_mm && s_lp > alive_p.land_g) || shaken;
        static float cur_t = 0;
        cur_t = (alive_p.cur_ground_mA > 0 && cur_lp > alive_p.cur_ground_mA) ? cur_t + dt : 0;
        if((imu_land || cur_t > 0.15f) && tilt_deg < 35.0f){ n_lands++; cur_t = 0; jolt_t[0] = jolt_t[1] = 0; enter(AL_LANDED); }
        break; }
    case AL_LANDED:                                     /* paws down fast, then frozen */
        zero_out(&tgt); tgt.body[2] = -3.0f;
        blend(&cur, &from, &tgt, ease(st_t / 0.12f));
        if(st_t > 0.6f) enter(AL_LOOK);
        break;
    case AL_LOOK: {                                     /* "what just happened?" left ... right */
        static const float kt[] = { 0.0f, 0.30f, 0.65f, 1.10f, 1.45f, 1.75f };
        static const float ky[] = { 0.0f, 18.0f, 18.0f, -18.0f, -18.0f, 0.0f };
        float yaw = 0;
        for(int i = 0; i < 5; i++) if(st_t >= kt[i] && st_t < kt[i+1]) yaw = lerp(ky[i], ky[i+1], ease((st_t - kt[i]) / (kt[i+1] - kt[i])));
        zero_out(&tgt); tgt.body[2] = -3.0f * (1 - ease(st_t / 1.7f));
        tgt.body[5] = yaw; tgt.body[4] = 4.0f * sinf(PI_F * clampf(st_t / 1.75f, 0, 1));
        tgt.body[3] = -yaw * 0.15f;
        blend(&cur, &from, &tgt, ease(st_t / 0.1f));
        if(st_t > 1.75f) enter(AL_SHAKE);
        break; }
    case AL_SHAKE: {                                    /* shake it off (decaying roll wobble) */
        const float e = expf(-st_t / 0.28f) * (st_t < 0.9f ? 1.0f : 0.0f);
        zero_out(&tgt);
        tgt.body[3] = 7.0f * e * sinf(2 * PI_F * 7.0f * st_t);
        tgt.body[5] = 2.0f * e * sinf(2 * PI_F * 7.0f * st_t + 1.2f);
        tgt.body[2] = 1.5f * e * sinf(2 * PI_F * 14.0f * st_t);
        blend(&cur, &from, &tgt, ease(st_t / 0.05f));
        if(st_t > 1.1f){ cool = 0.8f; v = h = v_peak = h_ref = 0; vh[0] = vh[1] = 0; memset(ah_hist, 0, sizeof ah_hist); enter(AL_STAND); }
        break; }
    }
    *out = cur;
}

int alive_status_json(char *b, int n){
    return snprintf(b, n,
        "{\"state\":\"%s\",\"t\":%.2f,\"tilt\":%.1f,\"roll\":%.1f,\"pitch\":%.1f,"
        "\"vert_g\":%.3f,\"g0\":%.3f,\"v\":%.3f,\"h_mm\":%.1f,\"desc_mm\":%.1f,\"vpeak\":%.3f,"
        "\"push_g\":%.3f,\"gyro\":[%.0f,%.0f,%.0f],\"cur_mA\":%.0f,"
        "\"lifts\":%d,\"lands\":%d,\"pushes\":%d,\"last_push\":{\"fwd\":%.2f,\"left\":%.2f,\"size\":%.2f,\"step\":%d,\"by\":%d,\"trig\":[%.2f,%.2f,%.3f,%.3f,%.0f,%.0f]},"
        "\"p\":{\"lift_mm\":%.0f,\"lift_v\":%.2f,\"land_mm\":%.0f,\"land_g\":%.2f,\"push_g\":%.2f,\"push_dps\":%.0f,\"push_lean\":%.1f,"
        "\"cur_ground_mA\":%.0f,\"run_hz\":%.1f,\"land_jolt_g\":%.2f,\"enable_lift\":%d,\"enable_push\":%d}}",
        NAMES[st], (double)st_t, (double)tilt_deg, (double)roll_deg, (double)pitch_deg,
        (double)s_lp, (double)g0, (double)v, (double)(h * 1000), (double)((h_ref - h) * 1000), (double)v_peak,
        (double)sqrtf(ah_lp[0]*ah_lp[0] + ah_lp[1]*ah_lp[1]),
        (double)gyro_b[0], (double)gyro_b[1], (double)gyro_b[2], (double)cur_lp,
        n_lifts, n_lands, n_pushes, (double)push_m[0], (double)push_m[1], (double)push_sig, push_step, trig_by,
        (double)trig[0], (double)trig[1], (double)trig[2], (double)trig[3], (double)trig[4], (double)trig[5],
        (double)alive_p.lift_mm, (double)alive_p.lift_v, (double)alive_p.land_mm, (double)alive_p.land_g,
        (double)alive_p.push_g, (double)alive_p.push_dps, (double)alive_p.push_lean, (double)alive_p.cur_ground_mA, (double)alive_p.run_hz, (double)alive_p.land_jolt_g,
        alive_p.enable_lift, alive_p.enable_push);
}

int alive_set(const char *k, float x){
    if(!strcmp(k, "lift_mm"))       { alive_p.lift_mm = x; return 1; }
    if(!strcmp(k, "lift_v"))        { alive_p.lift_v = x; return 1; }
    if(!strcmp(k, "land_mm"))       { alive_p.land_mm = x; return 1; }
    if(!strcmp(k, "land_g"))        { alive_p.land_g = x; return 1; }
    if(!strcmp(k, "push_g"))        { alive_p.push_g = x; return 1; }
    if(!strcmp(k, "push_dps"))      { alive_p.push_dps = x; return 1; }
    if(!strcmp(k, "push_lean"))     { alive_p.push_lean = x; return 1; }
    if(!strcmp(k, "cur_ground_mA")) { alive_p.cur_ground_mA = x; return 1; }
    if(!strcmp(k, "land_jolt_g"))   { alive_p.land_jolt_g = x; return 1; }
    if(!strcmp(k, "run_hz"))        { alive_p.run_hz = clampf(x, 1.0f, 6.0f); return 1; }
    if(!strcmp(k, "enable_lift"))   { alive_p.enable_lift = x != 0; return 1; }
    if(!strcmp(k, "enable_push"))   { alive_p.enable_push = x != 0; return 1; }
    return 0;
}

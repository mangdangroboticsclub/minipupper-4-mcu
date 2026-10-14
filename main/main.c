/* ============================================================================
 *  factorytest - Mini Pupper factory test firmware            (main/main.c)
 * ============================================================================
 *
 *  Everything the factory page can do is in THIS file, one section each:
 *
 *    1. Settings              board number, Wi-Fi names, current limits
 *    2. Small helpers         time, reading ?id=..&deg=.. from a web address
 *    3. CLI mode / walk mode  who may talk to the servos right now
 *    4. Walking (Left)        leg IK + the 4-phase "Left" step
 *    5. Servo basics          Go + the saved angle buttons  /api/direct /api/presets
 *    6. RPM test              time a move                  /api/speed
 *    7. Spin test             spin + count turns (Dynamixel style) /api/spin
 *    8. Torque test           read angle, move 100 deg at 1400 mA, hold  /api/hold /api/angle
 *    9. Wi-Fi                 hotspot + router
 *   10. Web server            address -> function table
 *   11. Start-up              app_main()
 *
 *  HOW A SERVO IS REACHED
 *  ----------------------
 *  The ESP32 does not drive the servos itself. Each leg has an AT32 chip
 *  ("driver board") that runs the servo's control loop and reads its pot.
 *  The ESP32 talks to the 4 AT32 boards over SPI - all of that is in
 *  driver_board.c. In this file a servo is just (section 5):
 *
 *      servo_go(id, 155.5, 300)          go to 155.5 deg, at most 300 mA
 *      servo_angle(id)                   where is it now, in degrees
 *      servo_limp(id)                    let go
 *
 *  Walking sends all 12 servos at once with driver_board_sync_write().
 *
 *  Angles in this file are the RAW servo angle, 0 .. 310 deg, centre 155.
 * ========================================================================== */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"            /* esp_rom_delay_us: wait a few microseconds */
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver_board.h"

/* ============================================================================
 *  1. SETTINGS
 * ========================================================================== */
#define TESTBOARD_NUM   1              /* 1 = hotspot "testboard 1" at 192.168.3.1, 2 = .4.1 ... */
#define WIFI_SSID       "Mangdang"     /* router it joins (change on the Wi-Fi page) */
#define WIFI_PASS       "mangdang"
#define AP_PASS_DEFAULT ""             /* hotspot password: "" = open, else 8+ characters */

#define GO_MA           300            /* current limit for Go (mA) */
#define RPM_MA          1500           /* current limit for the RPM test - full power */
#define WALK_MA         1500           /* current limit while walking */

#define TB_STR_(x) #x
#define TB_STR(x)  TB_STR_(x)
#define AP_SSID_DEFAULT "testboard " TB_STR(TESTBOARD_NUM)
#if   TESTBOARD_NUM == 1
#define AP_IP "192.168.3.1"
#elif TESTBOARD_NUM == 2
#define AP_IP "192.168.4.1"
#elif TESTBOARD_NUM == 3
#define AP_IP "192.168.5.1"
#elif TESTBOARD_NUM == 4
#define AP_IP "192.168.6.1"
#elif TESTBOARD_NUM == 5
#define AP_IP "192.168.7.1"
#elif TESTBOARD_NUM == 6
#define AP_IP "192.168.8.1"
#else
#error "TESTBOARD_NUM must be 1..6"
#endif

#define PI 3.14159265f
static const char *TAG = "factory";
static nvs_handle_t nvs;               /* namespace "parameter": calibration + walk settings */

/* ============================================================================
 *  2. SMALL HELPERS
 * ========================================================================== */
static uint32_t millis(void){ return (uint32_t)(esp_timer_get_time() / 1000); }

/* read ?key=value from the web address, with a default */
static int q_int(httpd_req_t *r, const char *key, int def){
    char q[256], v[32];
    if(httpd_req_get_url_query_str(r, q, sizeof q) == ESP_OK &&
       httpd_query_key_value(q, key, v, sizeof v) == ESP_OK) return atoi(v);
    return def;
}
static float q_float(httpd_req_t *r, const char *key, float def){
    char q[256], v[32];
    if(httpd_req_get_url_query_str(r, q, sizeof q) == ESP_OK &&
       httpd_query_key_value(q, key, v, sizeof v) == ESP_OK) return strtof(v, NULL);
    return def;
}
static void q_str(httpd_req_t *r, const char *key, char *out, int n){
    char q[300];
    if(httpd_req_get_url_query_str(r, q, sizeof q) == ESP_OK)
        httpd_query_key_value(q, key, out, n);
}
static esp_err_t send_json(httpd_req_t *r, const char *json){
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, json);
}
#define NEED_CLI(r) do{ if(!cli_mode) return send_json(r, "{\"ok\":false,\"err\":\"climode\"}"); }while(0)
#define NEED_ID(r, id) do{ if((id) < 1 || (id) > 12) return send_json(r, "{\"ok\":false,\"err\":\"bad id\"}"); }while(0)

/* ============================================================================
 *  3. CLI MODE / WALK MODE
 *  Only one side may talk to the servo boards at a time:
 *    CLI mode  (the normal state) - the page and the tests move single servos.
 *    walk mode                    - the walk task sends all 12 servos 100x a second.
 *  The robot starts in CLI mode, so nothing moves at power-on.
 * ========================================================================== */
static volatile bool cli_mode    = true;
static volatile bool walk_parked = true;    /* the walk task is not using the servos */
static volatile int  walk_dir    = 0;       /* 0 = stand still, 1 = walk left */
static volatile bool test_busy   = false;   /* an RPM / spin test is running */

static void enter_cli(void){
    cli_mode = true;
    while(!walk_parked) vTaskDelay(pdMS_TO_TICKS(10));   /* let the walk task finish its frame */
}
static void leave_cli(void){ cli_mode = false; }

/* ============================================================================
 *  4. WALKING (Left)  - the same leg maths and step as minipupperesp
 * ========================================================================== */
static uint16_t goal[13];             /* target per servo, 0..1023 (511 = centre) */
static float    offset[13];           /* calibration per servo (deg), from flash  */
static int      period = 80, height = 70, upHeight = 10, tilt = 10;   /* walk settings */
static const float L1 = 50, L2 = 56;  /* thigh and calf length (mm) */
#define NEUTRAL_Z 70.0f               /* stand height that means "all servos centred" */
static float th1_neutral, th2_neutral;

static void servo_write(int id, float deg){          /* deg from centre -> 0..1023 */
    int sig = 511 + (int)(deg * 10240.0f / driver_board_get_scale_dd());
    goal[id] = (uint16_t)(sig < 0 ? 0 : sig > 1023 ? 1023 : sig);
}
static void ik_neutral_init(void){
    float ld = NEUTRAL_Z;
    th1_neutral = -acosf((L1*L1 + ld*ld - L2*L2) / (2*L1*ld));
    th2_neutral = asinf((ld*ld - L1*L1 - L2*L2) / (2*L1*L2)) - th1_neutral;
}
/* one leg: foot at x (mm forward), hip at th0 (deg), height z (mm).
 * hip_id = 1 FR, 4 FL, 7 RR, 10 RL. Right legs mirror the thigh, left legs the calf. */
static void leg_ik(int hip_id, bool right, float x, float th0, float z){
    float zd  = z / cosf(th0 / 180.0f * PI);
    float ld  = sqrtf(x*x + zd*zd);
    float phi = atan2f(x, zd);
    float th1 = phi - acosf((L1*L1 + ld*ld - L2*L2) / (2*L1*ld));
    float th2 = asinf((ld*ld - L1*L1 - L2*L2) / (2*L1*L2)) - th1;
    float thigh = (th1 - th1_neutral) * 180.0f / PI;
    float calf  = (th2 - th2_neutral) * 180.0f / PI;
    if(right) thigh = -thigh; else calf = -calf;
    servo_write(hip_id,     th0   + offset[hip_id]);
    servo_write(hip_id + 1, thigh + offset[hip_id + 1]);
    servo_write(hip_id + 2, calf  + offset[hip_id + 2]);
}
#define fRIK(x, th0, z) leg_ik(1,  true,  x, th0, z)   /* front right */
#define fLIK(x, th0, z) leg_ik(4,  false, x, th0, z)   /* front left  */
#define rRIK(x, th0, z) leg_ik(7,  true,  x, th0, z)   /* rear right  */
#define rLIK(x, th0, z) leg_ik(10, false, x, th0, z)   /* rear left   */

/* send goal[] to all 12 servos (at most every 10 ms). false = CLI mode took over. */
static bool flush_servos(void){
    static int64_t last = 0;
    while(esp_timer_get_time() - last < 5000) vTaskDelay(1);
    last = esp_timer_get_time();
    if(cli_mode) return false;
    uint16_t pos[12], cur[12];
    for(int i = 0; i < 12; i++){ pos[i] = goal[i + 1]; cur[i] = WALK_MA; }
    driver_board_sync_write(pos, cur);
    return true;
}

/* the 4 phases of one Left step; tt goes 0 .. pi/2 during each phase */
static void left_1(float tt){
    fRIK(0,  tilt - 2*tilt*sinf(tt), height - upHeight*sinf(tt));
    rLIK(0, -tilt + 2*tilt*sinf(tt), height - upHeight*sinf(tt));
    rRIK(0,  tilt*cosf(tt),          height - upHeight*cosf(tt));
    fLIK(0, -tilt*cosf(tt),          height - upHeight*cosf(tt));
}
static void left_2(float tt){
    rRIK(0, -tilt*sinf(tt), height);
    fLIK(0,  tilt*sinf(tt), height);
}
static void left_3(float tt){
    fRIK(0, -tilt*cosf(tt),          height - upHeight*cosf(tt));
    rLIK(0,  tilt*cosf(tt),          height - upHeight*cosf(tt));
    rRIK(0, -tilt + 2*tilt*sinf(tt), height - upHeight*sinf(tt));
    fLIK(0,  tilt - 2*tilt*sinf(tt), height - upHeight*sinf(tt));
}
static void left_4(float tt){
    fRIK(0,  tilt*sinf(tt), height);
    rLIK(0, -tilt*sinf(tt), height);
}
/* run one phase for `period` ms. false = CLI mode took over. */
static bool phase(void (*pose)(float tt)){
    uint32_t t0 = millis(), tim = 0;
    while(tim < (uint32_t)period){
        tim = millis() - t0;
        pose((float)tim * PI / 2 / period);
        if(!flush_servos()) return false;
    }
    return true;
}
static void stand(void){
    fRIK(0, 0, height); fLIK(0, 0, height); rRIK(0, 0, height); rLIK(0, 0, height);
}

static void walk_task(void *arg){
    (void)arg;
    for(int i = 1; i <= 12; i++) goal[i] = 511;
    for(;;){
        walk_parked = false;
        if(cli_mode){                         /* CLI mode: hands off the servos */
            walk_parked = true;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if(walk_dir == 1){
            if(phase(left_1) && phase(left_2) && phase(left_3)) phase(left_4);
        }else{
            stand();
            flush_servos();
        }
    }
}

/* /api/walk?go=left | stop   - stop = stand, then back to CLI mode */
static esp_err_t api_walk(httpd_req_t *r){
    char go[8] = "";
    q_str(r, "go", go, sizeof go);
    if(!strcmp(go, "left")){
        if(test_busy) return send_json(r, "{\"ok\":false,\"err\":\"a test is running\"}");
        walk_dir = 1;
        leave_cli();
        return send_json(r, "{\"ok\":true,\"walking\":\"left\",\"climode\":0}");
    }
    walk_dir = 0;                             /* stop: stand ... */
    if(!cli_mode) vTaskDelay(pdMS_TO_TICKS(1200));
    enter_cli();                              /* ... then hold the stand in CLI mode */
    return send_json(r, "{\"ok\":true,\"walking\":\"\",\"climode\":1}");
}

/* ============================================================================
 *  5. SERVO BASICS
 *  The three things you do with one servo. Everything below uses these.
 * ========================================================================== */
static bool servo_go(int id, float deg, int mA){          /* go to an angle */
    return driver_board_direct(id, DB_MODE_POSITION, deg, (int16_t)mA);
} //angle
static bool servo_limp(int id){                            /* let go */
    return driver_board_direct(id, DB_MODE_IDLE, 0, 0);
}
static float servo_angle(int id){                          /* where is it now (deg), NAN = no answer */
    if(!driver_board_poll(id)) return NAN;
    float deg = driver_board_present_posdd(id) / 10.0f;
    if(deg > driver_board_get_scale_dd() / 10 + 5) return NAN;    /* garbled reply */
    return deg;
}

/* /api/direct?id=N&deg=155.5&cur=300    Go: the Go button and the angle buttons */
static esp_err_t api_direct(httpd_req_t *r){
    int   id  = q_int(r, "id", 0);
    float deg = q_float(r, "deg", 155.0f), mA = q_float(r, "cur", GO_MA);
    NEED_CLI(r); NEED_ID(r, id);
    bool ok = servo_go(id, deg, (int)mA);
    return send_json(r, ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"spi\"}");
}

/* ---- saved angle buttons --------------------------------------------------
 * 6 angles shown as buttons under Go. They are kept in the ESP32's flash
 * (key "presets"), so they are still there after power-off.
 *   /api/presets                         -> {"angles":[5,60,120,155,200,305]}
 *   /api/presets?set=5,60,120,155,200,305  saves new ones                     */
#define N_PRESETS 6
static float presets[N_PRESETS] = { 5, 60, 120, 155, 200, 305 };   /* until you save your own */

static void presets_load(void){
    float saved[N_PRESETS];
    size_t size = sizeof saved;
    if(nvs_get_blob(nvs, "presets", saved, &size) == ESP_OK && size == sizeof saved)
        memcpy(presets, saved, sizeof presets);
}

static esp_err_t api_presets(httpd_req_t *r){
    char text[100] = "";
    q_str(r, "set", text, sizeof text);
    if(text[0]){                                      /* "5,60,120,155,200,305" -> 6 numbers */
        float fresh[N_PRESETS];
        char *p = text;
        for(int i = 0; i < N_PRESETS; i++){
            char *end;
            fresh[i] = strtof(p, &end);
            if(end == p || fresh[i] < 0 || fresh[i] > 310)
                return send_json(r, "{\"ok\":false,\"err\":\"need 6 angles between 0 and 310\"}");
            p = (*end == ',') ? end + 1 : end;
        }
        memcpy(presets, fresh, sizeof presets);
        nvs_set_blob(nvs, "presets", presets, sizeof presets);   /* into flash */
        nvs_commit(nvs);
    }
    char b[160];
    int n = snprintf(b, sizeof b, "{\"ok\":true,\"angles\":[");
    for(int i = 0; i < N_PRESETS; i++) n += snprintf(b + n, sizeof b - n, "%s%g", i ? "," : "", presets[i]);
    snprintf(b + n, sizeof b - n, "]}");
    return send_json(r, b);
}

/* ============================================================================
 *  6. RPM TEST   /api/speed?id=N&a=5&b=305      (the page calls it out, then back)
 *
 *     1. go to the start angle a, wait 2 s
 *     2. send it to b at full power
 *     3. note the TIME when it passes 10% of the way (35 deg)
 *        and the TIME when it passes 90% of the way (275 deg)
 *     4. speed = 240 deg / (time90 - time10)       RPM = speed / 6
 *
 *  Why 10% and 90%, not 5 and 305: at the start the servo is still speeding
 *  up and at the end it is slowing down. The middle 80% is its real speed.
 * ========================================================================== */
static bool passed(float deg, float mark, float a, float b){    /* has it passed `mark` going from a to b? */
    return b > a ? deg >= mark : deg <= mark;
}

static esp_err_t api_speed(httpd_req_t *r){
    int   id = q_int(r, "id", 0);
    float a  = q_float(r, "a", 5), b = q_float(r, "b", 305);
    NEED_CLI(r); NEED_ID(r, id);
    if(fabsf(b - a) < 20) return send_json(r, "{\"ok\":false,\"err\":\"move under 20 deg\"}");
    if(test_busy)         return send_json(r, "{\"ok\":false,\"err\":\"a test is running\"}");

    /* 1. start angle */
    servo_go(id, a, RPM_MA);
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* 2. go */
    float mark10 = a + 0.1f * (b - a);                  /* e.g.  35 deg */
    float mark90 = a + 0.9f * (b - a);                  /* e.g. 275 deg */
    int64_t t_start = esp_timer_get_time(), t10 = 0, t90 = 0;
    servo_go(id, b, RPM_MA);

    /* 3. read the angle every 2 ms until it has passed 90% (give up after 3 s) */
    while(t90 == 0 && esp_timer_get_time() - t_start < 3000000){
        float deg = servo_angle(id);
        int64_t now = esp_timer_get_time();
        if(!isnan(deg)){
            if(t10 == 0 && passed(deg, mark10, a, b)) t10 = now;
            if(t10 != 0 && passed(deg, mark90, a, b)) t90 = now;
        }
        esp_rom_delay_us(2000);                         /* not faster: asking too often makes the servo stutter */
    }
    if(t90 == 0) return send_json(r, "{\"ok\":false,\"err\":\"servo did not get there in 3 s\"}");

    /* 4. speed */
    float seconds   = (t90 - t10) / 1e6f; //convert to seconds
    float deg_per_s = fabsf(mark90 - mark10) / seconds; //convert to deg/s
    float rpm       = deg_per_s / 6; //convert to RPM

    char out[200];
    snprintf(out, sizeof out,
        "{\"ok\":true,\"id\":%d,\"a\":%.1f,\"b\":%.1f,\"rpm\":%.2f,\"deg_per_s\":%.1f,"
        "\"ms_10_90\":%.1f,\"ms_to_90\":%.1f}",
        id, a, b, rpm, deg_per_s, (t90 - t10) / 1000.0f, (t90 - t_start) / 1000.0f);
    return send_json(r, out);
}

/* ============================================================================
 *  7. SPIN TEST   /api/spin?id=N&ma=1400&turns=10&dir=1      (like Dynamixel "no load speed")
 *
 *     1. torque mode: the motor pushes with a fixed current and ignores the
 *        angle, so with nothing attached it spins round and round, flat out
 *     2. wait 1 s - now it turns at its steady top speed
 *     3. count turns: every turn the angle passes 155 deg once.
 *        note the time of the first pass and of the pass `turns` later
 *     4. RPM = turns / minutes
 *     5. limp
 *
 *  ONLY with the horn free / leg off, and only on a servo that turns 360 deg
 *  by hand. If it does not pass 155 deg for 2 s (blocked, or it cannot turn
 *  all the way round) it stops and goes limp at once.
 *  The pot cannot read about 310..360 deg (its blind spot) - those readings
 *  are ignored; 155 deg is always readable.
 * ========================================================================== */
static esp_err_t api_spin(httpd_req_t *r){
    int id    = q_int(r, "id", 0);
    int ma    = q_int(r, "ma", 1400);
    int turns = q_int(r, "turns", 10);
    int dir   = q_int(r, "dir", 1) < 0 ? -1 : 1;              /* 1 = angle going up, -1 = down */
    NEED_CLI(r); NEED_ID(r, id);
    if(test_busy) return send_json(r, "{\"ok\":false,\"err\":\"a test is running\"}");
    if(ma < 100) ma = 100;
    if(ma > 1500) ma = 1500;
    if(turns < 1) turns = 1;
    if(turns > 50) turns = 50;
    test_busy = true;

    /* 1 + 2. spin, and give it 1 s to reach top speed */
    driver_board_direct(id, DB_MODE_TORQUE, 0, (int16_t)(dir * ma));
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* 3. count passes of 155 deg. A pass counts only when the last 8 readings
     *    each moved smoothly the right way (0..20 deg) - a spinning shaft does
     *    that, random readings in the pot's blind spot do not. */
    int64_t t_first = 0, t_last = 0, t_seen = esp_timer_get_time();
    int passes = -1;                                          /* the first pass starts the clock */
    int smooth = 0;                                           /* readings in a row that moved smoothly */
    float before = NAN;
    const char *err = NULL;
    while(passes < turns){
        float now_deg = servo_angle(id);
        int64_t now = esp_timer_get_time();
        float step = dir * (now_deg - before);                /* how far it moved the right way */
        smooth = (step > -0.5f && step < 20) ? smooth + 1 : 0;   /* (NAN gives 0) */
        bool crossed = dir > 0 ? (before < 155 && now_deg >= 155) : (before > 155 && now_deg <= 155);
        if(smooth >= 8 && crossed){
            passes++;
            if(passes == 0) t_first = now;
            t_last = now;
            t_seen = now;
        }
        before = now_deg;
        if(now - t_seen > 2000000){ err = "not turning - blocked, or it cannot turn 360 deg"; break; }
        esp_rom_delay_us(2000);
    }
    servo_limp(id);                                           /* 5. stop */
    test_busy = false;
    if(err){
        char e[120];
        snprintf(e, sizeof e, "{\"ok\":false,\"err\":\"%s\"}", err);
        return send_json(r, e);
    }

    /* 4. RPM = turns / minutes */
    float minutes = (t_last - t_first) / 60e6f;
    float rpm     = turns / minutes;
    char out[200];
    snprintf(out, sizeof out,
        "{\"ok\":true,\"id\":%d,\"ma\":%d,\"dir\":%d,\"turns\":%d,\"seconds\":%.3f,\"rpm\":%.2f}",
        id, ma, dir, turns, minutes * 60, rpm);
    return send_json(r, out);
}

/* ============================================================================
 *  8. TORQUE TEST   /api/hold?id=N&move=100&ma=1400&secs=2     (position mode)
 *
 *     1. read where the servo is now                 e.g. 120 deg
 *     2. pick the target `move` deg away, up if it fits in 0..310,
 *        else down                                  120 + 100 = 220 deg
 *     3. go there in position mode with `ma` - the most current it may use
 *     4. hold for `secs` seconds   (hang a weight / push it by hand)
 *     5. note the angle it really got to
 *     6. back to the start angle gently (300 mA), wait till it is there (max 3 s)
 *     7. limp - you can turn the horn by hand again, like after power-on
 * ========================================================================== */
/* the angle now. The board's answer to a request is what it measured at the
 * request BEFORE, so after a pause the first answer is old - throw it away.
 * Then read twice, 2 ms apart: in the blind spot (310..360 deg) the readings
 * jump about, so they must be within 20 deg. NAN = cannot read it */
static float angle_now(int id){
    servo_angle(id);                                   /* old answer - throw away */
    float a = servo_angle(id);
    esp_rom_delay_us(2000);
    float b = servo_angle(id);
    if(isnan(a) || isnan(b) || a > 310 || b > 310 || fabsf(a - b) > 20) return NAN;
    return b;
}

/* the angle, checked 5 times 10 ms apart. In the pot's blind spot (310..360 deg)
 * the readings jump about at random, so each one must be within 20 deg of the
 * one before (a moving servo only goes about 10 deg in 10 ms).
 * Gives the last reading, or NAN = cannot read it */
static float steady_angle(int id){
    float before = NAN, a = NAN;
    servo_angle(id);                                   /* old answer - throw away */
    for(int k = 0; k < 5; k++){
        if(k) vTaskDelay(pdMS_TO_TICKS(10));
        a = servo_angle(id);
        if(isnan(a) || a > 310) return NAN;
        if(k && fabsf(a - before) > 20) return NAN;
        before = a;
    }
    return a;
}

/* /api/angle?id=N    live angle on the page (asked about 3 times a second) */
static esp_err_t api_angle(httpd_req_t *r){
    int id = q_int(r, "id", 0);
    NEED_CLI(r); NEED_ID(r, id);
    float a = angle_now(id);
    if(isnan(a)) return send_json(r, "{\"ok\":false,\"err\":\"blind spot\"}");
    char out[64];
    snprintf(out, sizeof out, "{\"ok\":true,\"angle\":%.1f}", a);
    return send_json(r, out);
}

#define BACK_MA 300
static esp_err_t api_hold(httpd_req_t *r){
    int   id   = q_int(r, "id", 0);
    float move = q_float(r, "move", 100);
    int   ma   = q_int(r, "ma", 1400);
    int   secs = q_int(r, "secs", 2);
    NEED_CLI(r); NEED_ID(r, id);
    if(test_busy) return send_json(r, "{\"ok\":false,\"err\":\"a test is running\"}");
    if(move < 1 || move > 300) return send_json(r, "{\"ok\":false,\"err\":\"move 1..300 deg\"}");
    if(ma < 100) ma = 100;
    if(ma > 2000) ma = 2000;                     /* torque test limit (default 1400) */
    if(secs < 1) secs = 1;
    if(secs > 10) secs = 10;

    float start = steady_angle(id);                              /* 1. where is it */
    if(isnan(start)) return send_json(r, "{\"ok\":false,\"err\":\"cannot read the angle - turn it into 0..310\"}");
    float target;                                                /* 2. where to */
    if(start + move <= 310)    target = start + move;
    else if(start - move >= 0) target = start - move;
    else return send_json(r, "{\"ok\":false,\"err\":\"no room to move that far\"}");

    test_busy = true;
    servo_go(id, target, ma);                                    /* 3. go, with full current */
    vTaskDelay(pdMS_TO_TICKS(secs * 1000));                      /* 4. hold */
    float reached = servo_angle(id);                             /* 5. where did it get to */
    servo_go(id, start, BACK_MA);                                /* 6. back, gently ... */
    for(int k = 0; k < 30; k++){                                 /*    ... up to 3 s     */
        vTaskDelay(pdMS_TO_TICKS(100));
        float now_deg = angle_now(id);
        if(!isnan(now_deg) && fabsf(now_deg - start) < 3) break;
    }
    servo_limp(id);                                              /* 7. let go */
    test_busy = false;

    char out[128];
    snprintf(out, sizeof out, "{\"ok\":true,\"start\":%.1f,\"target\":%.1f,\"reached\":%.1f}",
             start, target, isnan(reached) ? -1.0 : reached);
    return send_json(r, out);
}

/* ============================================================================
 *  9. WI-FI  - hotspot always on at AP_IP, and joins the router if it can.
 *  Names live in flash (namespaces "wifi_sta" / "wifi_ap", same as minipupperesp).
 * ========================================================================== */
static char sta_ssid[32], sta_pass[64], ap_ssid[32], ap_pass[64], sta_ip[16];
static int  sta_retries;

static void wifi_get(const char *ns, const char *key, char *out, size_t n, const char *def){
    nvs_handle_t h; size_t sz = n;
    snprintf(out, n, "%s", def);
    if(nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return;
    if(nvs_get_str(h, key, out, &sz) != ESP_OK) snprintf(out, n, "%s", def);
    nvs_close(h);
}
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data){
    (void)arg;
    if(base == WIFI_EVENT && id == WIFI_EVENT_STA_START && sta_ssid[0]) esp_wifi_connect();
    else if(base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED){
        sta_ip[0] = 0;
        if(sta_ssid[0] && sta_retries++ < 20) esp_wifi_connect();      /* then give up; hotspot stays */
    }else if(base == IP_EVENT && id == IP_EVENT_STA_GOT_IP){
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(sta_ip, sizeof sta_ip, IPSTR, IP2STR(&e->ip_info.ip));
        sta_retries = 0;
        ESP_LOGI(TAG, "router IP %s   (hotspot " AP_IP ")", sta_ip);
    }
}
static void wifi_start(void){
    wifi_get("wifi_sta", "ssid",     sta_ssid, sizeof sta_ssid, WIFI_SSID);
    wifi_get("wifi_sta", "password", sta_pass, sizeof sta_pass, WIFI_PASS);
    wifi_get("wifi_ap",  "ssid",     ap_ssid,  sizeof ap_ssid,  AP_SSID_DEFAULT);
    wifi_get("wifi_ap",  "password", ap_pass,  sizeof ap_pass,  AP_PASS_DEFAULT);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    esp_netif_ip_info_t ip = {0};                        /* fixed hotspot address */
    ip.ip.addr = ip.gw.addr = esp_ip4addr_aton(AP_IP);
    ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    esp_netif_dhcps_stop(ap);
    esp_netif_set_ip_info(ap, &ip);
    esp_netif_dhcps_start(ap);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event, NULL);

    wifi_config_t sc = {0}, ac = {0};
    snprintf((char *)sc.sta.ssid, sizeof sc.sta.ssid, "%s", sta_ssid);
    snprintf((char *)sc.sta.password, sizeof sc.sta.password, "%s", sta_pass);
    sc.sta.threshold.authmode = sta_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    snprintf((char *)ac.ap.ssid, sizeof ac.ap.ssid, "%s", ap_ssid);
    ac.ap.ssid_len = strlen(ap_ssid);
    snprintf((char *)ac.ap.password, sizeof ac.ap.password, "%s", ap_pass);
    ac.ap.channel = 1;
    ac.ap.max_connection = 4;
    ac.ap.authmode = strlen(ap_pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sc));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ac));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "hotspot '%s' at http://" AP_IP "   router '%s'", ap_ssid, sta_ssid);
}

/* ============================================================================
 *  10. WEB SERVER
 * ========================================================================== */
extern const char factory_html[] asm("_binary_factory_html_start");   /* main/factory.html */

static esp_err_t page_factory(httpd_req_t *r){
    httpd_resp_set_type(r, "text/html");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, factory_html);
}

static const httpd_uri_t pages[] = {
    { .uri = "/",            .method = HTTP_GET, .handler = page_factory },   /* the factory page  */
    { .uri = "/api/walk",    .method = HTTP_GET, .handler = api_walk     },   /* Left / Stop       */
    { .uri = "/api/direct",  .method = HTTP_GET, .handler = api_direct   },   /* Go                */
    { .uri = "/api/presets", .method = HTTP_GET, .handler = api_presets  },   /* angle buttons     */
    { .uri = "/api/speed",   .method = HTTP_GET, .handler = api_speed    },   /* RPM test          */
    { .uri = "/api/spin",    .method = HTTP_GET, .handler = api_spin     },   /* spin test         */
    { .uri = "/api/hold",    .method = HTTP_GET, .handler = api_hold     },   /* torque test       */
    { .uri = "/api/angle",   .method = HTTP_GET, .handler = api_angle    },   /* Read angle        */
};

static void web_start(void){
    httpd_handle_t s = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = sizeof pages / sizeof pages[0];
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&s, &cfg));
    for(size_t i = 0; i < sizeof pages / sizeof pages[0]; i++) httpd_register_uri_handler(s, &pages[i]);
}

/* ============================================================================
 *  11. START-UP
 * ========================================================================== */
static float nvs_float(const char *key, float def){      /* minipupperesp stores floats as blobs */
    float v = def; size_t sz = sizeof v;
    if(nvs_get_blob(nvs, key, &v, &sz) != ESP_OK) v = def;
    return v;
}

void app_main(void){
    esp_err_t e = nvs_flash_init();
    if(e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND){ nvs_flash_erase(); nvs_flash_init(); }
    nvs_open("parameter", NVS_READWRITE, &nvs);

    /* calibration + walk settings saved by minipupperesp (same flash) */
    int32_t v;
    if(nvs_get_i32(nvs, "period", &v) == ESP_OK) period = v;
    if(nvs_get_i32(nvs, "height", &v) == ESP_OK) height = v;
    for(int i = 1; i <= 12; i++){
        char k[12]; snprintf(k, sizeof k, "offset%d", i);
        offset[i] = nvs_float(k, 0);
    }
    presets_load();                                       /* the saved angle buttons */

    driver_board_init();                                  /* SPI + the 4 AT32 boards */
    vTaskDelay(pdMS_TO_TICKS(1000));                      /* servo power settles */

    /* a dead-band saved by minipupperesp shrinks the angle scale - apply it the same way */
    float full = nvs_float("fullrng", 310), guard = nvs_float("guard", 0);
    if(guard > 0 && 2 * guard < full){
        float cpd = 4096 / full;
        for(int id = 1; id <= 12; id++){
            driver_board_set_param(id, DB_PARAM_MIN_POSITION_ADC, guard * cpd);
            driver_board_set_param(id, DB_PARAM_MAX_POSITION_ADC, 4096 - guard * cpd);
            driver_board_set_param(id, DB_PARAM_RANGE_POSITION_DEG, full - 2 * guard);
        }
        driver_board_set_scale_dd((full - 2 * guard) * 10);
    }

    ik_neutral_init();
    wifi_start();
    web_start();
    xTaskCreatePinnedToCore(walk_task, "walk", 4096, NULL, 22, NULL, 1);
    ESP_LOGI(TAG, "factory test ready - open http://" AP_IP);
}

/* pi_protocol.h — the wire format between the ESP32-S3 and the Raspberry Pi.
 *
 * ONE FILE, TWO IMPLEMENTATIONS. The Pi side lives in
 *   ros2jazzympv2/mini_pupper_esp_bridge/mini_pupper_esp_bridge/protocol.py
 * and mirrors every number below. Change one, change the other, and bump
 * PI_PROTO_VERSION so a mismatched pair says so instead of misreading floats.
 *
 * ── Physical link ───────────────────────────────────────────────────────
 *   J-PI pin 2  IO18  ESP TX  -> Pi GPIO15 RXD (header pin 10)   blue
 *   J-PI pin 3  IO8   ESP RX  <- Pi GPIO14 TXD (header pin 8)    green
 *   J-PI pin 4  IO3   HOST_PRESENT, Pi -> ESP (Pi GPIO17, pin 11) yellow
 *   J-PI pin 5  IO11  ESP_SLAVE,    ESP -> Pi (Pi GPIO27, pin 13) white
 *   J-PI pin 1  GND (black)          J-PI pin 6  VBUS 5 V (red)
 *   8N1, CONFIG_APP_PI_LINK_BAUD (default 921600), no flow control.
 *
 * ── Frame ───────────────────────────────────────────────────────────────
 *   0xA5 0x5A | len | type | seq | payload[len] | crc16 (LE)
 *
 *   len   payload length only, 0..PI_MAX_PAYLOAD
 *   seq   sender's rolling counter; the ESP echoes the last one it accepted
 *   crc16 CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no
 *         xorout) over len, type, seq and the payload - NOT the two sync bytes
 *
 *   All multi-byte fields are little-endian, floats are IEEE-754 binary32,
 *   structs are packed. Both ends are little-endian machines, so the packed
 *   structs below ARE the payloads.
 *
 * ── Who is in charge ────────────────────────────────────────────────────
 *   The ESP is the master of its own servos UNTIL the Pi takes over. It takes
 *   over only while BOTH are true:
 *     1. HOST_PRESENT (IO3) is held high by the Pi, and
 *     2. a HEARTBEAT with PI_HB_WANT_CONTROL arrived within the link timeout.
 *   Lose either and the ESP stops walking, stands, and goes back to running
 *   itself (PWA joystick, Lua, skills). A heartbeat without WANT_CONTROL is a
 *   read-only "monitor" link: telemetry flows, nothing moves.
 *
 *   Units on the wire are SI and REP-103 (x forward, y left, z up,
 *   counter-clockwise positive) so the Pi never has to know the ESP's
 *   internal millimetres or its mirrored leg roles.
 */
#ifndef PI_PROTOCOL_H
#define PI_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PI_PROTO_VERSION   1

#define PI_SYNC0           0xA5
#define PI_SYNC1           0x5A
#define PI_MAX_PAYLOAD     200
#define PI_FRAME_OVERHEAD  7          /* 2 sync + len + type + seq + 2 crc */

/* ── Pi -> ESP ─────────────────────────────────────────────────────────── */
#define PI_MSG_HEARTBEAT     0x01     /* pi_heartbeat_t      every <= 100 ms */
#define PI_MSG_CMD_VEL       0x10     /* pi_cmd_vel_t        walk velocity   */
#define PI_MSG_SET_MODE      0x11     /* pi_set_mode_t       stand/walk/gait */
#define PI_MSG_BODY_POSE     0x12     /* pi_body_pose_t      standing pose   */
#define PI_MSG_FOOT_TARGETS  0x13     /* pi_foot_targets_t   ESP runs the IK */
#define PI_MSG_AUDIO_CTRL    0x14     /* pi_audio_ctrl_t     speaker on/off  */
#define PI_MSG_AUDIO_DATA    0x15     /* raw PCM, s16le mono at the rate given
                                         by AUDIO_CTRL START; even length,
                                         1..PI_MAX_PAYLOAD bytes. The Pi paces
                                         it in real time (about 0.3 s ahead);
                                         the ESP buffers ~2 s and plays.    */
#define PI_MSG_HOST_INFO     0x16     /* pi_host_info_t      every ~2 s: the
                                         Pi's IP, shown on the robot's screen.
                                         Accepted in MONITOR and SLAVE.     */
#define PI_MSG_VIDEO_DATA    0x17     /* pi_video_hdr_t + up to PI_VIDEO_CHUNK
                                         bytes of one baseline JPEG (<= 320x240):
                                         the camera / tracking picture for the
                                         robot's screen. Chunks of a frame come
                                         in order (index 0..count-1); a gap
                                         drops that frame. MONITOR and SLAVE. */

/* ── ESP -> Pi ─────────────────────────────────────────────────────────── */
#define PI_MSG_INFO          0x81     /* pi_info_t           1 Hz            */
#define PI_MSG_STATE         0x82     /* pi_state_t          state rate      */
#define PI_MSG_DIAG          0x83     /* pi_diag_t           5 Hz            */
#define PI_MSG_EVENT         0x8F     /* pi_event_t          on occurrence   */

/* HEARTBEAT.flags */
#define PI_HB_WANT_CONTROL   0x01

/* Link states (INFO.link_state, STATE.link_state) */
#define PI_LINK_STANDALONE   0        /* no host: the ESP drives itself      */
#define PI_LINK_MONITOR      1        /* host heard, not in control          */
#define PI_LINK_SLAVE        2        /* host in control of all motion       */

/* SET_MODE.mode */
#define PI_MODE_STAND        0        /* hold the neutral stand              */
#define PI_MODE_WALK         1        /* Stanford trot, velocity from CMD_VEL */
#define PI_MODE_GAIT         2        /* named move; name[] = robot gait name */
#define PI_MODE_INIT         3        /* all servos centred (calibration pose)*/

/* STATE.flags */
#define PI_ST_IMU_OK         0x01
#define PI_ST_MAG_VALID      0x02
#define PI_ST_HOST_PIN       0x04     /* IO3 currently reads high            */
#define PI_ST_STUDIO         0x08     /* Servo Studio holds the bus          */
#define PI_ST_SKILL          0x10     /* a WASM skill is running             */
#define PI_ST_WALKING        0x20     /* the Stanford walk loop is active    */

/* EVENT.code */
#define PI_EV_LINK_UP        1        /* entered SLAVE                       */
#define PI_EV_LINK_DOWN      2        /* left SLAVE (text says why)          */
#define PI_EV_REJECTED       3        /* a command was refused (text: why)   */
#define PI_EV_PROTO_MISMATCH 4        /* heartbeat proto != PI_PROTO_VERSION */

/* AUDIO_CTRL.op - music from the Pi through the robot's speaker. Accepted
 * only while the Pi is in control (SLAVE): voice is off then, so nothing else
 * is using the speaker. Added without a PI_PROTO_VERSION bump - an older ESP
 * counts these as unknown frames and stays silent. */
#define PI_AUDIO_START       1        /* flush, set rate + volume, start buffering */
#define PI_AUDIO_STOP        2        /* stop now, drop what is buffered     */
#define PI_AUDIO_END         3        /* play what is buffered, then stop    */
#define PI_AUDIO_VOLUME      4        /* change volume only                  */
#define PI_AUDIO_VOLUME_KEEP 255

/* Servo angle that has never been measured. */
#define PI_CDEG_INVALID      ((int16_t)-32768)

#pragma pack(push, 1)

typedef struct {
    uint8_t  proto;          /* PI_PROTO_VERSION                             */
    uint8_t  flags;          /* PI_HB_*                                      */
    uint16_t state_rate_hz;  /* wanted STATE rate, 0 = firmware default      */
} pi_heartbeat_t;

typedef struct {
    float vx;                /* m/s, + forward                               */
    float vy;                /* m/s, + left                                  */
    float wz;                /* rad/s, + counter-clockwise (turn left)       */
} pi_cmd_vel_t;

typedef struct {
    uint8_t mode;            /* PI_MODE_*                                    */
    char    name[23];        /* PI_MODE_GAIT only, NUL-padded                */
} pi_set_mode_t;

typedef struct {
    float roll;              /* rad, + right side down                       */
    float pitch;             /* rad, + nose down (REP-103)                   */
    float yaw;               /* rad, + head left                             */
    float height;            /* m, hip-to-foot; <= 0 keeps the current one   */
} pi_body_pose_t;

typedef struct {
    /* Body-frame foot positions in metres, Stanford/REP-103 axes, the body
     * centre at the origin and z NEGATIVE below the body. Legs in Stanford
     * order: 0 FR, 1 FL, 2 RR, 3 RL - the same columns as the old
     * Command.legs_location Matrix3x4. */
    float xyz[4][3];
} pi_foot_targets_t;

typedef struct {
    uint8_t  op;             /* PI_AUDIO_*                                   */
    uint8_t  volume;         /* 0..100 %, PI_AUDIO_VOLUME_KEEP = unchanged   */
    uint16_t sample_rate;    /* Hz, START only (the ESP plays 16000)         */
} pi_audio_ctrl_t;

typedef struct {
    uint16_t frame_id;       /* rolling; all chunks of a frame share it      */
    uint8_t  index;          /* 0 .. count-1                                 */
    uint8_t  count;          /* chunks in this frame, 1..255                 */
} pi_video_hdr_t;
#define PI_VIDEO_CHUNK       (PI_MAX_PAYLOAD - 4)
#define PI_VIDEO_MAX_FRAME   (255 * PI_VIDEO_CHUNK)

typedef struct {
    char ip[16];             /* dotted IPv4, NUL-padded ("" = no network)    */
    char hostname[32];       /* NUL-padded                                   */
} pi_host_info_t;

typedef struct {
    uint8_t  proto;
    uint8_t  link_state;     /* PI_LINK_*                                    */
    uint16_t state_rate_hz;  /* rate STATE is actually being sent at         */
    char     fw[16];         /* firmware name, NUL-padded                    */
    char     uuid[24];       /* mpx_identity_robot_id(), NUL-padded        */
} pi_info_t;

typedef struct {
    uint32_t esp_ms;               /* ms since ESP boot                      */
    uint8_t  link_state;           /* PI_LINK_*                              */
    uint8_t  gait;                 /* robot::GaitCmd numeric value           */
    uint8_t  flags;                /* PI_ST_*                                */
    uint8_t  last_rx_seq;          /* seq of the last frame the ESP accepted */
    /* Servo angles in the firmware's own terms: centidegrees from the stand
     * pose, calibration offset removed, servo id order 1..12
     * (FR hip/thigh/knee, FL, RR, RL). The Pi turns these into URDF joint
     * angles with servo_map.yaml. */
    int16_t  servo_cmd_cdeg[12];   /* commanded (what the IK asked for)      */
    int16_t  servo_meas_cdeg[12];  /* measured, PI_CDEG_INVALID if unknown   */
    float    acc_g[3];             /* BMI270 chip axes, g                    */
    float    gyro_dps[3];          /* BMI270 chip axes, deg/s                */
    float    mag_ut[3];            /* BMM150, uT (valid if PI_ST_MAG_VALID)  */
    float    vx, vy, wz;           /* velocity the gait is executing, REP-103*/
} pi_state_t;

typedef struct {
    int16_t  temp_dc[12];          /* servo NTC, 0.1 degC; INT16_MIN unknown */
    int16_t  current_ma[12];       /* servo motor current, mA                */
    uint32_t rx_frames;            /* frames accepted                        */
    uint32_t rx_crc_err;           /* frames dropped for a bad CRC           */
    uint32_t rx_bad;               /* bad length / unknown type / bad size   */
    uint32_t tx_frames;
    uint16_t tx_drop;              /* frames not sent: TX buffer full        */
    uint16_t reserved;
} pi_diag_t;

typedef struct {
    uint8_t code;                  /* PI_EV_*                                */
    uint8_t ref_type;              /* message type this is about, or 0       */
    uint8_t ref_seq;               /* its seq, or 0                          */
    char    text[61];              /* human-readable, NUL-padded             */
} pi_event_t;

#pragma pack(pop)

/* Sizes are part of the protocol; protocol.py asserts the same numbers. */
#ifdef __cplusplus
#define PI_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define PI_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif
PI_STATIC_ASSERT(sizeof(pi_heartbeat_t)    == 4,   "pi_heartbeat_t");
PI_STATIC_ASSERT(sizeof(pi_cmd_vel_t)      == 12,  "pi_cmd_vel_t");
PI_STATIC_ASSERT(sizeof(pi_set_mode_t)     == 24,  "pi_set_mode_t");
PI_STATIC_ASSERT(sizeof(pi_body_pose_t)    == 16,  "pi_body_pose_t");
PI_STATIC_ASSERT(sizeof(pi_foot_targets_t) == 48,  "pi_foot_targets_t");
PI_STATIC_ASSERT(sizeof(pi_audio_ctrl_t)   == 4,   "pi_audio_ctrl_t");
PI_STATIC_ASSERT(sizeof(pi_host_info_t)    == 48,  "pi_host_info_t");
PI_STATIC_ASSERT(sizeof(pi_video_hdr_t)    == 4,   "pi_video_hdr_t");
PI_STATIC_ASSERT(sizeof(pi_info_t)         == 44,  "pi_info_t");
PI_STATIC_ASSERT(sizeof(pi_state_t)        == 104, "pi_state_t");
PI_STATIC_ASSERT(sizeof(pi_diag_t)         == 68,  "pi_diag_t");
PI_STATIC_ASSERT(sizeof(pi_event_t)        == 64,  "pi_event_t");

/* CRC-16/CCITT-FALSE. Bitwise: the link moves ~10 kB/s, a table buys nothing. */
static inline uint16_t pi_crc16(const uint8_t *data, unsigned len, uint16_t crc)
{
    for (unsigned i = 0; i < len; ++i) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

#ifdef __cplusplus
}
#endif

#endif /* PI_PROTOCOL_H */

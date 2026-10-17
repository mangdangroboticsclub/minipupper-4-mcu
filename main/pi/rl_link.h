/* rl_link.h -- mp4-link's host link (the mp3-link protocol) on the Pi UART.
 *
 * Ported verbatim from mpv2-claw components/mpx_pi_link (Kconfig names
 * CONFIG_APP_RL_* -> CONFIG_APP_RL_*). Internal to the Pi link: pi_link.cc
 * owns UART1 and hands every received
 * chunk to feed(); this module answers with its own frames on the same port.
 * See rl_link.cc for the behaviour and RL_LINK.md for the protocol.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

namespace rl_link {

/** Create the tick task and its timer. Call once, after the UART is up. */
void init();

/** Bytes from the Pi UART (called by the pi_link task only). */
void feed(const uint8_t *data, int n);

/** True from the first RL frame until APP_RL_RELEASE_MS after the last one:
 *  the ROS bridge cannot enter slave mode meanwhile. */
bool session_active();

/** A session holds the robot and its STATE stream owns the wire. */
bool driving();

/** pi_link received a bulk frame (audio or video data). For the next second
 *  an RL request cannot open a session: those streams are where a false CRC
 *  match would come from. A SET_ALL still can (its structure rules that out). */
void note_bulk();

struct Stats {
    bool     session;       // a session is running
    bool     robot;         // ...and robot::host owns the robot
    bool     armed;
    uint16_t flags;         // the last STATE flags
    uint8_t  fault_servo;
    uint32_t frames;        // valid RL frames received
    uint32_t crc_err;       // RL-shaped frames with a bad CRC
    uint32_t sessions;
    uint32_t ticks;
};
Stats stats();

}  // namespace rl_link

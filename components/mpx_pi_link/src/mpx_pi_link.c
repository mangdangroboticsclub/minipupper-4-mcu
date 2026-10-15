/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * `pi` on the serial console: the state of the Raspberry Pi link.
 *
 * The link itself is pi_link.cc (mpv2's protocol); this file only reads its
 * status. It replaces the old text-line bring-up link (PING/HELLO/IP at
 * 115200) that lived here before mpv2's ROS link was ported.
 */

#include <stdio.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_log.h"

#include "mpx_pi_link.h"

static void print_status(void)
{
    mpx_pi_link_status_t st;
    char ip[16], name[32];

    mpx_pi_link_get_status(&st);
    mpx_pi_link_host_info(ip, sizeof(ip), name, sizeof(name));

    printf("state          %s%s\n", mpx_pi_link_state_name(st.state),
           st.state == MPX_PI_LINK_SLAVE ? "  (the Pi is driving the robot)" : "");
    printf("host pin IO3   %s\n", st.host_pin ? "HIGH (Pi present)" : "low");
    printf("heartbeat      %s%s\n", st.heartbeat ? "fresh" : "none",
           st.heartbeat ? (st.want_control ? ", wants control" : ", monitor only") : "");
    if (st.ms_since_rx == UINT32_MAX) {
        printf("last frame     never\n");
    } else {
        printf("last frame     %lu ms ago\n", (unsigned long)st.ms_since_rx);
    }
    printf("STATE rate     %u Hz\n", (unsigned)st.state_rate_hz);
    printf("rx             %lu frames, %lu CRC errors, %lu bad\n",
           (unsigned long)st.rx_frames, (unsigned long)st.rx_crc_err, (unsigned long)st.rx_bad);
    printf("tx             %lu frames, %lu dropped\n",
           (unsigned long)st.tx_frames, (unsigned long)st.tx_drop);
    printf("slave entries  %lu%s%s\n", (unsigned long)st.slave_entries,
           st.last_exit_reason[0] ? ", last exit: " : "", st.last_exit_reason);
    printf("Pi             %s%s%s\n", ip[0] ? ip : "(no address yet)",
           name[0] ? "  " : "", name);
    printf("Pi music       %s, %lu ms buffered, %lu underruns\n",
           st.audio_playing ? "playing" : "idle",
           (unsigned long)st.audio_buffered_ms, (unsigned long)st.audio_underruns);
    printf("Pi camera      %lu shown, %lu dropped, %lu decode errors\n",
           (unsigned long)st.video_frames_shown, (unsigned long)st.video_frames_dropped,
           (unsigned long)st.video_decode_errors);
    printf("RL host        %s%s, %lu frames, %lu CRC errors, %lu sessions, %lu ticks\n",
           st.rl_session ? (st.rl_robot ? "driving" : "talking (ROS holds the robot)") : "idle",
           st.rl_session ? (st.rl_armed ? ", armed" : ", disarmed") : "",
           (unsigned long)st.rl_frames, (unsigned long)st.rl_crc_err,
           (unsigned long)st.rl_sessions, (unsigned long)st.rl_ticks);
    if (st.rl_flags & 0x9E) {
        printf("RL latched     %s%s%s%s%s (servo %u)\n",
               (st.rl_flags & 0x02) ? "watchdog " : "", (st.rl_flags & 0x04) ? "current " : "",
               (st.rl_flags & 0x08) ? "temperature " : "", (st.rl_flags & 0x10) ? "board " : "",
               (st.rl_flags & 0x80) ? "rejected " : "", (unsigned)st.rl_fault_servo);
    }

    if (st.rx_frames == 0 && st.rx_crc_err == 0) {
        printf("\nNothing heard from the Pi. Check: the ROS bridge is running "
               "(mini_pupper_esp_bridge), its baud matches CONFIG_MP4_PI_LINK_UART_BAUD, "
               "and TX/RX are crossed (ESP IO18 -> Pi pin 10, ESP IO8 <- Pi pin 8).\n");
    } else if (st.rx_crc_err > st.rx_frames) {
        printf("\nMostly CRC errors: the baud rates on the two sides probably differ.\n");
    } else if (st.heartbeat && st.want_control && !st.host_pin) {
        printf("\nThe Pi wants control but IO3 (HOST_PRESENT, yellow wire, Pi GPIO17) "
               "is low, so the robot stays in monitor mode.\n");
    }
}

static int pi_cmd(int argc, char **argv)
{
    if (argc <= 1 || strcmp(argv[1], "status") == 0) {
        print_status();
        return 0;
    }
    if (strcmp(argv[1], "json") == 0) {
        char buf[1024];
        if (mpx_pi_link_status_json(buf, sizeof(buf)) > 0) {
            printf("%s\n", buf);
        }
        return 0;
    }
    printf("usage: pi [status|json]\n");
    return 1;
}

void register_pi_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "pi",
        .help = "Raspberry Pi (ROS) link: pi [status|json]",
        .hint = NULL,
        .func = &pi_cmd,
    };
    esp_err_t err = esp_console_cmd_register(&cmd);
    if (err != ESP_OK) {
        ESP_LOGW("pi_link", "`pi` command not registered: %s", esp_err_to_name(err));
    }
}

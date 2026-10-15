#!/usr/bin/env python3
"""pi_announce -- show this Pi's IP on the robot's screen from boot.

The robot (mpv2 / mpv2-claw firmware) shows the Pi's IP next to its own while
HOST_INFO frames keep arriving on the J-PI link. Normally only the ROS bridge
sends them. This small service sends one every 2 s from boot -- and NOTHING
else:

  * no HEARTBEAT. An earlier version sent "monitor" heartbeats, and when the
    ROS bridge ran as another user (sudo, a system service, Docker) it could
    not see the bridge holding the port, kept beating, and the robot heard
    "want control" and "don't want control" in turn: the ROS badge came and
    went and the robot dropped out of slave mode;
  * no reading and no input flushing, so it never takes bytes meant for the
    bridge or for an RL policy (mp3link). It does not use pyserial, whose
    open() flushes the input queue the bridge may be sharing: the port is
    opened once, set up with TCSANOW (no flush) and only ever written;
  * never the HOST_PRESENT line.

HOST_INFO only puts an address on the screen, so even when this and another
program write at the same time the worst case is one frame failing its CRC.
It stays quiet while another program it can see has the port open, since
the bridge sends its own HOST_INFO.

Needs the mpv2-claw (or mpv2) firmware that shows the address for HOST_INFO
alone, in any link state. The wire format is mini_pupper_esp_bridge's
protocol.py (pi_protocol.h), version 1.
"""
import os
import socket
import struct
import termios
import time

PORT = os.environ.get("PI_LINK_PORT", "/dev/ttyAMA0")
BAUD = int(os.environ.get("PI_LINK_BAUD", "921600"))

MSG_HOST_INFO = 0x16
PERIOD_S = 2.0


def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def frame(msg_type, seq, payload):
    hdr = bytes((len(payload), msg_type, seq & 0xFF))
    return b"\xA5\x5A" + hdr + payload + struct.pack("<H", crc16(hdr + payload))


def local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.254.254.254", 1))     # picks the interface, sends nothing
        return s.getsockname()[0]
    except OSError:
        return ""
    finally:
        s.close()


def host_info(ip):
    return struct.pack("<16s32s", ip.encode("ascii", "replace")[:15],
                       socket.gethostname().encode("utf-8", "replace")[:31])


def others_have_port(real):
    """True if another process we can see has the UART open."""
    me = str(os.getpid())
    for pid in os.listdir("/proc"):
        if not pid.isdigit() or pid == me:
            continue
        fd_dir = "/proc/%s/fd" % pid
        try:
            for fd in os.listdir(fd_dir):
                try:
                    if os.path.realpath(os.path.join(fd_dir, fd)) == real:
                        return True
                except OSError:
                    pass
        except OSError:
            pass
    return False


def open_port():
    """Open for writing only and set raw 8N1 at BAUD without flushing anything."""
    fd = os.open(PORT, os.O_WRONLY | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0                                            # iflag
    a[1] = 0                                            # oflag: raw
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL # cflag
    a[3] = 0                                            # lflag
    a[4] = a[5] = getattr(termios, "B%d" % BAUD)
    termios.tcsetattr(fd, termios.TCSANOW, a)          # TCSANOW: no flush
    return fd


def main():
    real = os.path.realpath(PORT)
    seq = 0
    fd = None
    yielded = None
    print("pi_announce: HOST_INFO only, every %.0f s on %s @ %d" % (PERIOD_S, PORT, BAUD), flush=True)
    while True:
        busy = others_have_port(real)
        if busy != yielded:
            yielded = busy
            print("pi_announce: " + ("another program has the link, standing by"
                                     if busy else "announcing"), flush=True)
        ip = local_ip()
        if not busy and ip:
            try:
                if fd is None:
                    fd = open_port()
                os.write(fd, frame(MSG_HOST_INFO, seq, host_info(ip)))
                seq += 1
            except BlockingIOError:
                pass                                    # output full: next time
            except OSError as e:
                print("pi_announce: %s: %s" % (PORT, e), flush=True)
                if fd is not None:
                    try:
                        os.close(fd)
                    except OSError:
                        pass
                    fd = None
        time.sleep(PERIOD_S)


if __name__ == "__main__":
    main()

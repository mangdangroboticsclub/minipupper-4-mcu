#!/usr/bin/env python3
"""
Raspberry Pi end of the MP4 ESP32 CORE UART link (components/mpx_pi_link).

Line protocol, newline-terminated ASCII:

    PING <n>   ->  the other side answers  PONG <n>
    HELLO <name>  ->  the other side answers HELLO <its name>
    IP?        ->  the Pi answers  IP <its address>   (also sent every 10 s
                   while running, so the robot can show it under its eyes)
    anything else is printed / logged as-is

Modes:

    mp4_link.py                 interactive: prints everything the ESP sends,
                                answers its pings, and takes commands from
                                the keyboard --  `ping`, `ping 10`, or any
                                other text which is sent as a raw line
    mp4_link.py --ping [N]      send N pings, print round trips, exit 0 if
                                any came back (use this from scripts)
    mp4_link.py --listen        just print what arrives (and answer pings)
    mp4_link.py --daemon        quiet, forever: answer pings, report the IP
                                (what a systemd service runs, see README)
    mp4_link.py --selftest      no hardware: run the protocol against itself
                                over a pty pair, proves this script works

Setup on the Pi (Raspberry Pi OS Bookworm, Pi 5):

    sudo raspi-config  ->  Interface Options  ->  Serial Port
        login shell over serial?   No
        serial port hardware?      Yes
    (this puts `enable_uart=1` in /boot/firmware/config.txt; on the Pi 5 it
    maps to the GPIO14/15 UART, which appears as /dev/ttyAMA0 after a reboot.
    The 3-pin debug connector is a different UART, /dev/ttyAMA10 -- not this.)
    sudo apt install python3-serial
    sudo usermod -aG dialout $USER    (then log out and back in)

Wiring, 3.3 V logic on both sides, no level shifter:

    ESP TX  (GPIO18, J-PI "PI_1")  ->  Pi GPIO15 RXD   header pin 10
    ESP RX  (GPIO8,  J-PI "PI_2")  <-  Pi GPIO14 TXD   header pin 8
    ESP GND                         --  Pi GND          header pin 6
    Do NOT join 5 V or 3V3 between the boards; power the Pi from its own PSU.
"""

import argparse
import os
import select
import sys
import threading
import time

DEFAULT_PORT = "/dev/ttyAMA0"
DEFAULT_BAUD = 115200
OUR_NAME = "raspberrypi"
PING_TIMEOUT_S = 1.0
IP_EVERY_S = 10.0


def my_ip():
    """The address other machines reach this Pi on: the source address of a
    route to the internet (no packet is sent), else the first from hostname -I."""
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        if ip and not ip.startswith("127."):
            return ip
    except OSError:
        pass
    finally:
        s.close()
    try:
        import subprocess
        out = subprocess.run(["hostname", "-I"], capture_output=True, text=True, timeout=2).stdout
        for tok in out.split():
            if tok.count(".") == 3 and not tok.startswith("127."):
                return tok
    except Exception:
        pass
    return None


class Link:
    """One serial port, one reader thread, PING/PONG/HELLO handled inline."""

    def __init__(self, fd_or_serial, name=OUR_NAME, quiet=False):
        self.dev = fd_or_serial          # pyserial Serial, or a raw fd (selftest)
        self.name = name
        self.quiet = quiet
        self.seq = 0
        self.pong_seq = None
        self.pong_t = 0.0
        self.peer = None
        self.rx_lines = 0
        self.rx_bytes = 0
        self.last_rx = 0.0
        self.lock = threading.Lock()
        self._buf = b""
        self._stop = False
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    # -- raw io ------------------------------------------------------------
    def _write(self, data: bytes):
        with self.lock:
            if isinstance(self.dev, int):
                os.write(self.dev, data)
            else:
                self.dev.write(data)
                self.dev.flush()

    def _read(self) -> bytes:
        if isinstance(self.dev, int):
            r, _, _ = select.select([self.dev], [], [], 0.05)
            return os.read(self.dev, 256) if r else b""
        return self.dev.read(self.dev.in_waiting or 1)

    def close(self):
        self._stop = True

    # -- protocol ----------------------------------------------------------
    def send_line(self, line: str):
        self._write((line + "\n").encode("ascii", "replace"))

    def send_ip(self):
        ip = my_ip()
        if ip:
            self.send_line(f"IP {ip}")

    def report_ip_forever(self):
        """Background: the address every IP_EVERY_S, so the robot shows it and
        notices (after 30 s of silence) when the Pi goes away."""
        def loop():
            while not self._stop:
                self.send_ip()
                time.sleep(IP_EVERY_S)
        threading.Thread(target=loop, daemon=True).start()

    def ping(self, timeout=PING_TIMEOUT_S):
        """Round trip in seconds, or None on timeout."""
        self.seq += 1
        seq = self.seq
        t0 = time.monotonic()
        self.send_line(f"PING {seq}")
        while time.monotonic() - t0 < timeout:
            if self.pong_seq == seq:
                return self.pong_t - t0
            time.sleep(0.002)
        return None

    def _handle(self, line: str):
        self.rx_lines += 1
        self.last_rx = time.monotonic()
        if line.startswith("PONG "):
            try:
                self.pong_seq = int(line[5:].strip())
            except ValueError:
                self.pong_seq = None
            self.pong_t = time.monotonic()
            return
        if line == "PING" or line.startswith("PING "):
            self.send_line("PONG " + (line[5:].strip() or "0"))
            if not self.quiet:
                print(f"  esp> {line}   (answered)")
            return
        if line == "IP?":
            self.send_ip()
            return
        if line.startswith("HELLO"):
            self.peer = line[6:].strip() or "?"
            self.send_line(f"HELLO {self.name}")
            self.send_ip()
            if not self.quiet:
                print(f"  esp> {line}   (ESP is up, answered with HELLO {self.name})")
            return
        if not self.quiet:
            print(f"  esp> {line}")

    def _reader(self):
        while not self._stop:
            try:
                data = self._read()
            except Exception as e:  # port vanished, etc.
                print(f"  [read error: {e}]", file=sys.stderr)
                time.sleep(0.5)
                continue
            if not data:
                continue
            self.rx_bytes += len(data)
            self._buf += data
            while b"\n" in self._buf:
                raw, self._buf = self._buf.split(b"\n", 1)
                line = raw.decode("ascii", "replace").rstrip("\r ")
                if line:
                    self._handle(line)


def open_serial(port, baud):
    try:
        import serial  # pyserial
    except ImportError:
        sys.exit("pyserial is missing:  sudo apt install python3-serial")
    try:
        return serial.Serial(port, baud, timeout=0.05)
    except Exception as e:
        sys.exit(f"cannot open {port}: {e}\n"
                 f"  - is the UART enabled? (raspi-config -> Serial Port -> hardware Yes, login No)\n"
                 f"  - on a Pi 5 the GPIO14/15 UART is /dev/ttyAMA0; ls -l /dev/ttyAMA*\n"
                 f"  - permission denied: sudo usermod -aG dialout $USER, then log in again")


def do_ping(link, count):
    ok = 0
    rtts = []
    for i in range(count):
        rtt = link.ping()
        if rtt is None:
            print(f"  {i + 1:2d}  timeout")
        else:
            ok += 1
            rtts.append(rtt * 1000)
            print(f"  {i + 1:2d}  PONG  {rtt * 1000:.1f} ms")
        time.sleep(0.2)
    if ok:
        print(f"  {ok}/{count} replies   min {min(rtts):.1f}  avg {sum(rtts) / ok:.1f}  "
              f"max {max(rtts):.1f} ms\n  >> CONNECTED")
    else:
        print(f"  0/{count} replies\n  >> NOT CONNECTED")
        if link.rx_bytes == 0:
            print("     nothing at all arrived on RX: wires swapped (ESP TX -> Pi RXD),\n"
                  "     GND not shared, the ESP not running a build with mpx_pi_link,\n"
                  "     or the login console still owns this UART (raspi-config).")
        else:
            print(f"     {link.rx_bytes} bytes arrived but no PONG: baud mismatch? "
                  f"(ESP side is CONFIG_MP4_PI_LINK_BAUD, default 115200)")
    return ok > 0


def interactive(link):
    print("  type `ping`, `ping 10`, `status`, `quit`, or any text to send it raw\n")
    link.send_line(f"HELLO {OUR_NAME}")
    while True:
        try:
            cmd = input("pi> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return
        if not cmd:
            continue
        if cmd in ("quit", "exit", "q"):
            return
        if cmd == "status":
            age = (time.monotonic() - link.last_rx) if link.last_rx else None
            print(f"  rx {link.rx_bytes} B in {link.rx_lines} lines, "
                  f"last heard {'never' if age is None else f'{age:.1f} s ago'}, "
                  f"peer {link.peer or 'unknown'}")
            continue
        if cmd.split()[0] == "ping":
            n = int(cmd.split()[1]) if len(cmd.split()) > 1 else 5
            do_ping(link, n)
            continue
        link.send_line(cmd)


def selftest():
    """Two Links talking over a pty pair, no serial hardware involved."""
    # Two ptys; each Link gets a slave end, and two pump threads shuttle bytes
    # between the master ends, so A's writes come out at B and vice versa --
    # a null-modem cable made of file descriptors.
    a_master, a_slave = os.openpty()
    b_master, b_slave = os.openpty()
    import termios
    for fd in (a_slave, b_slave):
        attrs = termios.tcgetattr(fd)
        attrs[0] = attrs[1] = attrs[3] = 0     # raw: no echo, no CR/LF mangling
        termios.tcsetattr(fd, termios.TCSANOW, attrs)

    stop = False

    def pump(src, dst):
        while not stop:
            r, _, _ = select.select([src], [], [], 0.05)
            if r:
                try:
                    os.write(dst, os.read(src, 256))
                except OSError:
                    return

    threading.Thread(target=pump, args=(a_master, b_master), daemon=True).start()
    threading.Thread(target=pump, args=(b_master, a_master), daemon=True).start()

    esp = Link(b_slave, name="fake-esp", quiet=True)
    pi = Link(a_slave, name="pi-selftest", quiet=True)
    time.sleep(0.1)

    pi.send_line("HELLO pi-selftest")
    time.sleep(0.2)
    rtt = pi.ping()
    back = esp.ping()
    stop = True
    ok = rtt is not None and back is not None and esp.peer == "pi-selftest" and pi.peer == "fake-esp"
    print(f"  HELLO exchanged: pi saw '{pi.peer}', esp saw '{esp.peer}'")
    print(f"  pi -> esp ping: {'%.2f ms' % (rtt * 1000) if rtt else 'FAILED'}")
    print(f"  esp -> pi ping: {'%.2f ms' % (back * 1000) if back else 'FAILED'}")
    print("  >> selftest", "OK" if ok else "FAILED")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    ap.add_argument("--ping", nargs="?", const=5, type=int, metavar="N", help="send N pings and exit")
    ap.add_argument("--listen", action="store_true", help="print what arrives; answer pings")
    ap.add_argument("--daemon", action="store_true",
                    help="quiet, forever: answer pings and report the IP (for systemd)")
    ap.add_argument("--selftest", action="store_true", help="protocol self-test over a pty pair, no hardware")
    args = ap.parse_args()

    if args.selftest:
        sys.exit(0 if selftest() else 1)

    ser = open_serial(args.port, args.baud)
    print(f"  {args.port} @ {args.baud} 8N1")
    link = Link(ser, quiet=args.daemon)
    try:
        if args.ping is not None:
            sys.exit(0 if do_ping(link, args.ping) else 1)
        link.report_ip_forever()
        if args.daemon:
            link.send_line(f"HELLO {OUR_NAME}")
            while True:
                time.sleep(60)
        if args.listen:
            link.send_line(f"HELLO {OUR_NAME}")
            print("  listening (Ctrl-C to stop)")
            while True:
                time.sleep(1)
        interactive(link)
    except KeyboardInterrupt:
        print()
    finally:
        link.close()


if __name__ == "__main__":
    main()

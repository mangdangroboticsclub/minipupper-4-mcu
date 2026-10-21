import json
import time

import numpy as np
import pytest

from mp4link import cli
from mp4link import protocol as p
from mp4link.link import Link, LinkError

from fake_esp32 import FakeEsp32


@pytest.fixture
def fake():
    f = FakeEsp32()
    yield f
    f.close()


@pytest.fixture
def link(fake):
    lk = Link(fake.port)
    yield lk
    lk.close()


def test_state_stream_and_echo_latency(link):
    first = link.wait_state(1.0)
    for _ in range(10):
        link.set_all([1550] * 12, enable=0, flags=0)
        time.sleep(0.02)
    assert link.n_states > 20
    assert link.parser.crc_errors == 0
    assert link.latency and max(link.latency) < 0.05
    assert link.latest.tick > first.tick


def test_params_and_refusal_while_armed(link, fake):
    link.wait_state(1.0)
    assert link.set_param(2, "kp_position", 80.0) == pytest.approx(80.0)
    assert link.get_param(2, "kp_position") == pytest.approx(80.0)
    link.set_all([1550] * 12, enable=0x0FFF, flags=p.SA_ARM)
    link.wait_state(0.5)
    with pytest.raises(LinkError, match="refused"):
        link.get_param(2, "kp_position")
    link.disarm()
    deadline = time.time() + 1
    while link.latest.armed and time.time() < deadline:
        time.sleep(0.01)
    assert not link.latest.armed
    with pytest.raises(LinkError, match="refused"):
        link.request(p.CMD_SET_POS, 2, 1550, 300)


def test_watchdog_latches_and_rearm_clears(link):
    link.wait_state(1.0)
    link.set_all([1550] * 12, enable=0x0FFF, flags=p.SA_ARM)
    time.sleep(0.3)                                  # silence > 100 ms
    st = link.wait_state(0.5)
    assert not st.armed and st.flags & p.ST_WATCHDOG
    link.set_all([1550] * 12, enable=0x0FFF, flags=p.SA_ARM)
    time.sleep(0.03)
    st = link.wait_state(0.5)
    assert st.armed and not st.flags & p.ST_WATCHDOG
    link.disarm()


def test_jog_moves_one_servo_and_disarms(fake, tmp_path):
    log = tmp_path / "jog.npz"
    rc = cli.main(["--port", fake.port, "jog", "--id", "5", "--deg", "4", "--seconds", "0.2", "--log", str(log)])
    assert rc == 0
    enables = {e for e, _ in fake.armed_seen}
    assert enables == {1 << 4}
    peak = max(t[4] for _, t in fake.armed_seen)
    assert peak == pytest.approx(1540 + 40, abs=1)        # start 1540 dd + 4 deg
    assert not fake.armed
    data = np.load(log)
    assert data["sent_pos_dd"].shape[1] == 12 and len(data["t_rx"]) > 0


def test_stream_clips_to_the_stand_and_disarms(fake, tmp_path):
    stand = tmp_path / "stand.json"
    stand.write_text(json.dumps({"stand_dd": [1550] * 12}))
    delta = np.zeros((20, 12), dtype=np.int16)
    delta[:, 1] = np.linspace(0, 100, 20)                 # 10 deg on servo 2
    delta[:, 0] = 900                                     # 90 deg on an abduction: clipped to 20
    f = tmp_path / "s.npz"
    np.savez(f, delta_dd=delta, period_s=0.015)
    rc = cli.main(["--port", fake.port, "stream", str(f), "--stand", str(stand)])
    assert rc == 0
    targets = np.array([t for _, t in fake.armed_seen])
    assert targets[:, 0].max() <= 1550 + 200
    assert targets[:, 1].max() == pytest.approx(1650, abs=1)
    assert not fake.armed


def test_config_writes_and_verifies(fake, capsys):
    rc = cli.main(["--port", fake.port, "config", "--ids", "1-3"])
    assert rc == 0
    assert fake.params[3][p.PARAM_ID["kd_position"]] == pytest.approx(400)
    assert fake.params[1][p.PARAM_ID["control_mode"]] == pytest.approx(1)


def test_latched_fault_refused_until_clear(fake, capsys):
    fake.latched = p.ST_FAULT_CUR
    fake.fault_servo = 5
    rc = cli.main(["--port", fake.port, "jog", "--id", "5", "--deg", "3", "--seconds", "0.1"])
    assert rc == 1 and not fake.armed_seen
    assert "fault-current" in capsys.readouterr().err
    rc = cli.main(["--port", fake.port, "jog", "--id", "5", "--deg", "3", "--seconds", "0.1", "--clear"])
    assert rc == 0 and fake.armed_seen and fake.latched == 0


def test_disarm_from_elsewhere_stops_the_command(fake, tmp_path):
    import threading
    stand = tmp_path / "stand.json"
    stand.write_text(json.dumps({"stand_dd": [1550] * 12}))
    f = tmp_path / "s.npz"
    np.savez(f, delta_dd=np.zeros((400, 12), dtype=np.int16), period_s=0.015)

    def idle_later():
        time.sleep(4.0)                      # inside the stream (ramp 2 s + hold 1 s)
        with fake.lock:
            fake.armed = False               # as an IDLE from the USB port would
    threading.Thread(target=idle_later, daemon=True).start()
    t0 = time.time()
    rc = cli.main(["--port", fake.port, "stream", str(f), "--stand", str(stand)])
    assert rc == 1 and time.time() - t0 < 5.5
    assert not fake.armed


def test_implausible_position_blocks_motion(fake):
    fake.pos[0] = 3.0                        # a config echo read as a position
    rc = cli.main(["--port", fake.port, "jog", "--id", "2", "--deg", "3", "--seconds", "0.1"])
    assert rc == 1 and not fake.armed_seen


def test_default_caps(fake, tmp_path):
    stand = tmp_path / "stand.json"
    stand.write_text(json.dumps({"stand_dd": [1550] * 12}))
    assert cli.main(["--port", fake.port, "stand", "--stand", str(stand), "--seconds", "0.1", "--hold", "0.1"]) == 0
    assert fake.caps_armed == {600}
    fake.caps_armed.clear()
    assert cli.main(["--port", fake.port, "jog", "--id", "2", "--deg", "2", "--seconds", "0.1"]) == 0
    assert fake.caps_armed == {300}


# ---- calibration ------------------------------------------------------------
def test_stand_formula_matches_the_firmware():
    # example offsets [deg] and the stand codes and deci-degrees the `main` branch's firmware sends for them
    offs = [-8, 2, 0, 4, 6, 9, -2, 14, 12, -6, 20, 9]
    ticks = cli.offset_ticks(offs)
    assert ticks.tolist() == [485, 517, 511, 524, 530, 540, 505, 557, 550, 492, 577, 540]
    assert cli.ticks_to_dd(ticks).tolist() == [1631, 1534, 1553, 1513, 1495, 1465, 1571, 1413, 1434, 1610, 1353, 1465]


def test_offsets_unset_set_save(link, fake):
    link.wait_state(1.0)
    assert link.get_offset(3) is None
    assert link.set_offset(3, -2.5) == pytest.approx(-2.5)
    assert link.get_offset(3) == pytest.approx(-2.5)
    assert fake.nvs == {}
    assert link.save_offsets() == 1 and fake.nvs == {3: pytest.approx(-2.5)}
    with pytest.raises(LinkError):
        link.set_offset(3, 90.0)
    link.set_all([1550] * 12, enable=0x0FFF, flags=p.SA_ARM)
    time.sleep(0.03)
    with pytest.raises(LinkError):
        link.set_offset(3, 1.0)                    # refused while armed
    link.disarm()


def test_offsets_json_is_a_stand_file(fake, tmp_path):
    fake.offsets = {1: -8.0, 2: 2.0}
    out = tmp_path / "stand.json"
    assert cli.main(["--port", fake.port, "offsets", "--json", str(out)]) == 0
    d = json.loads(out.read_text())
    assert d["stand_dd"][:3] == [1631, 1534, 1553] and d["offsets_stored"][:3] == [True, True, False]


def test_centre_holds_the_centre_on_the_chosen_servos(fake):
    for i in range(1, 13):
        fake.params[i][p.PARAM_ID["kff_current"]] = 0.001
    assert cli.main(["--port", fake.port, "centre", "--ids", "2,3", "--seconds", "0.3"]) == 0
    assert {e for e, _ in fake.armed_seen} == {0b110}
    last = fake.armed_seen[-1][1]
    assert last[1] == 1553 and last[2] == 1553
    assert not fake.armed


def test_calib_refuses_flash_gains_then_holds_and_saves(fake):
    fake.offsets = {5: 1.0}
    assert cli.main(["--port", fake.port, "calib", "--set", "5=3", "--seconds", "0.3", "--save"]) == 1
    assert not fake.armed_seen and fake.nvs == {}      # kff 0: `config` first
    for i in range(1, 13):
        fake.params[i][p.PARAM_ID["kff_current"]] = 0.001
    assert cli.main(["--port", fake.port, "calib", "--set", "5=3", "--seconds", "0.3", "--save"]) == 0
    last = fake.armed_seen[-1][1]
    assert last[4] == cli.ticks_to_dd(cli.offset_ticks([3.0]))[0]
    assert last[0] == 1553                              # unset offsets hold the centre
    assert fake.nvs[5] == pytest.approx(3.0) and len(fake.nvs) == 12


def test_stand_from_nvs(fake):
    fake.offsets = {i: 0.0 for i in range(1, 13)}
    assert cli.main(["--port", fake.port, "stand", "--stand", "nvs", "--seconds", "0.2", "--hold", "0.2"]) == 0
    assert fake.armed_seen[-1][1] == [1553] * 12


def test_calib_zero_starts_remounted_joints_from_zero(fake):
    for i in range(1, 13):
        fake.params[i][p.PARAM_ID["kff_current"]] = 0.001
    fake.offsets = {i: 5.0 for i in range(1, 13)}
    assert cli.main(["--port", fake.port, "calib", "--zero", "1-3", "--seconds", "0.3", "--save"]) == 0
    assert [fake.nvs[i] for i in (1, 2, 3, 4)] == [0.0, 0.0, 0.0, 5.0]


# ---- keepalive and sessions ---------------------------------------------------
def test_keepalive_pings_when_idle_and_requests_still_match(link, fake):
    for i in range(1, 13):
        link.set_param(i, "kp_position", float(10 * i))
    pings = fake.pings
    errors = []

    def worker(ids):
        try:
            for _ in range(3):
                for i in ids:
                    assert link.get_param(i, "kp_position") == pytest.approx(10 * i)
                time.sleep(0.6)                      # idle long enough for a keepalive
        except Exception as exc:                     # pragma: no cover - reported below
            errors.append(exc)
    import threading
    threads = [threading.Thread(target=worker, args=(ids,)) for ids in ((1, 2, 3), (7, 8, 9))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert not errors
    assert fake.pings > pings and link.keepalives > 0



def test_keepalive_stays_silent_while_set_all_streams(link, fake):
    link.wait_state(1.0)
    time.sleep(0.1)
    pings = fake.pings
    for _ in range(75):                              # 1.5 s at 50 Hz
        link.set_all([1550] * 12, enable=0, flags=0)
        time.sleep(0.02)
    assert fake.pings == pings


def test_missing_port_is_a_one_line_error(capsys):
    assert cli.main(["--port", "/dev/no-such-port", "ping", "--seconds", "0.1"]) == 1
    err = capsys.readouterr().err
    assert "cannot open /dev/no-such-port" in err and "Traceback" not in err

@pytest.fixture
def session_fake():
    f = FakeEsp32(session_release_s=1.0, session_wait_s=0.5)
    yield f
    f.close()


def test_session_opens_and_state_arrives(session_fake):
    lk = Link(session_fake.port)
    try:
        assert session_fake.sessions == 1
        st = lk.wait_state(0.5)
        assert st.flags & p.ST_IMU_OK
        assert lk.get_offset(4) == 0.0               # no "unset" status from the `main` branch's firmware
    finally:
        lk.close()


def test_keepalive_holds_the_session_and_its_params(session_fake):
    lk = Link(session_fake.port)
    try:
        assert lk.set_param(2, "kp_position", 80.0) == pytest.approx(80.0)
        time.sleep(2.5)                              # idle, longer than the release time
        assert session_fake.sessions == 1 and session_fake.session
        assert lk.get_param(2, "kp_position") == pytest.approx(80.0)
        lk.wait_state(0.5)
    finally:
        lk.close()


def test_without_keepalive_the_session_ends_and_params_reset(session_fake):
    lk = Link(session_fake.port, keepalive=None)
    try:
        lk.set_param(2, "kp_position", 80.0)
        time.sleep(1.5)
        assert not session_fake.session
        with pytest.raises(LinkError):
            lk.wait_state(0.3)
        assert lk.get_param(2, "kp_position") == 0.0  # reopens (slowly), firmware's own value
        assert session_fake.sessions == 2
    finally:
        lk.close()

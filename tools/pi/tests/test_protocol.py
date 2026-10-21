import struct

import numpy as np
import pytest

from mp4link import protocol as p


def test_crc16_ccitt_false_vector():
    assert p.crc16(b"123456789") == 0x29B1


def test_frame_sizes_and_request_layout():
    assert (p.REQ_LEN, p.RSP_LEN, p.SET_ALL_LEN, p.STATE_LEN) == (10, 14, 36, 106)
    f = p.pack_request(p.CMD_SET_PARAM, 3, 0x1234, 0x5678, pad=11)
    assert f[:8] == bytes([0x42, 10, 3, 11, 0x34, 0x12, 0x78, 0x56])
    assert struct.unpack("<H", f[8:])[0] == p.crc16(f[:8])


def test_set_all_layout():
    pos = list(range(1000, 1012))
    f = p.pack_set_all(7, pos, enable=0x0FFF, flags=p.SA_ARM | p.SA_CLEAR, cur_cap_ma=300)
    assert len(f) == 36
    assert f[0] == 0x43 and f[1] == 1
    assert struct.unpack("<HHHH", f[2:10]) == (7, 5, 0x0FFF, 300)
    assert list(struct.unpack("<12H", f[10:34])) == pos
    with pytest.raises(ValueError):
        p.pack_set_all(1, [0] * 11)


def test_state_roundtrip_and_float_args():
    body = p.STATE.pack(0x63, 1, 9, 123, 456, p.ST_ARMED | p.ST_IMU_OK, 0, 0x0F,
                        *range(12), *range(-6, 6), *range(300, 312), 1, 2, 1000, -5, 6, 7, 789)
    st = p.parse_state(body + struct.pack("<H", p.crc16(body)))
    assert st.seq == 9 and st.tick == 123 and st.t_us == 456 and st.armed
    assert list(st.pos_dd) == list(range(12)) and list(st.cur_ma) == list(range(-6, 6))
    assert list(st.acc_mg) == [1, 2, 1000] and list(st.gyr_ddps) == [-5, 6, 7] and st.imu_t_us == 789
    a1, a2 = p.float_args(0.001)
    assert struct.unpack("<f", struct.pack("<I", a1 | a2 << 16))[0] == np.float32(0.001)


def test_parser_resyncs_after_junk_and_bad_crc():
    body = p.RSP.pack(0x62, 0, 1, 0, 1550, 0, 0.0)
    rsp = body + struct.pack("<H", p.crc16(body))
    bad = bytearray(rsp)
    bad[5] ^= 0xFF
    parser = p.Parser()
    frames = parser.feed(b"# boot text\r\n" + bytes(bad) + rsp[:7])
    frames += parser.feed(rsp[7:] + b"\x63\x00" + rsp)
    # a stray STATE magic holds the stream until 106 bytes can be checked
    frames += parser.feed(bytes(120))
    assert [f for _, f in frames] == [rsp, rsp]
    assert parser.crc_errors >= 1

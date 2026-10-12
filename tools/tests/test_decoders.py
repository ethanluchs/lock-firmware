"""Decoders against hand-built bytes (lock_protocol.h, event_log.h, ble-spec.md §0, §4.4-4.6)."""

import pytest

import lock_client as lc
from lockfakes import (BUSY, EV_BOOT, EV_CMD, EV_CONNECT, EV_DISCONNECT, EV_IDLE, EV_STATE,
                       LOCKED, LOCKING, MALFORMED, NONE, OK, UNLOCKED, UNLOCKING, entry_bytes,
                       status_bytes)


# ---- enum values are the wire values -------------------------------------------

def test_enum_values_match_headers():
    assert [(m.name, int(m)) for m in lc.LockState] == [
        ("LOCKED", 0), ("UNLOCKING", 1), ("UNLOCKED", 2), ("LOCKING", 3)]
    assert [(m.name, int(m)) for m in lc.CmdResult] == [
        ("OK", 0), ("NO_CHANGE", 1), ("BUSY", 2), ("UNKNOWN_COMMAND", 3), ("MALFORMED", 4)]
    assert [(m.name, int(m)) for m in lc.EventType] == [
        ("BOOT", 1), ("STATE_CHANGE", 2), ("COMMAND_RESULT", 3), ("CONNECT", 4),
        ("DISCONNECT", 5), ("IDLE_TIMEOUT", 6)]
    assert (lc.STATUS_LEN, lc.INFO_LEN, lc.LOG_ENTRY_LEN) == (4, 5, 16)
    assert lc.NONE == 0xFF


def test_uuids_match_header():
    suffix = "-5b7e-4d19-9a3f-6c2e81d4b70a"
    assert lc.SERVICE_UUID == "8f2a1c00" + suffix
    assert lc.CMD_CHAR_UUID == "8f2a1c01" + suffix
    assert lc.STATUS_CHAR_UUID == "8f2a1c02" + suffix
    assert lc.INFO_CHAR_UUID == "8f2a1c03" + suffix
    assert lc.LOG_CHAR_UUID == "8f2a1c04" + suffix


# ---- status -------------------------------------------------------------------

# ble-spec.md §4.4 worked examples: bytes -> (state, lastOpcode, lastResult, resultSeq)
SPEC_STATUS_EXAMPLES = [
    ("00 FF FF 00", lc.LockState.LOCKED, 0xFF, None, 0),
    ("01 01 00 01", lc.LockState.UNLOCKING, 0x01, lc.CmdResult.OK, 1),
    ("02 01 00 01", lc.LockState.UNLOCKED, 0x01, lc.CmdResult.OK, 1),
    ("02 01 01 02", lc.LockState.UNLOCKED, 0x01, lc.CmdResult.NO_CHANGE, 2),
    ("02 07 03 03", lc.LockState.UNLOCKED, 0x07, lc.CmdResult.UNKNOWN_COMMAND, 3),
    ("02 01 04 04", lc.LockState.UNLOCKED, 0x01, lc.CmdResult.MALFORMED, 4),
    ("03 00 00 05", lc.LockState.LOCKING, 0x00, lc.CmdResult.OK, 5),
    ("03 01 02 06", lc.LockState.LOCKING, 0x01, lc.CmdResult.BUSY, 6),
    ("00 01 02 06", lc.LockState.LOCKED, 0x01, lc.CmdResult.BUSY, 6),
]


@pytest.mark.parametrize("hexbytes,state,op,res,seq", SPEC_STATUS_EXAMPLES)
def test_status_spec_examples(hexbytes, state, op, res, seq):
    s = lc.decode_status(bytes.fromhex(hexbytes))
    assert s.state is state
    assert s.last_opcode == op
    assert s.last_result is res
    assert s.result_seq == seq
    assert s.settled == (state in (lc.LockState.LOCKED, lc.LockState.UNLOCKED))
    assert s.raw == bytes.fromhex(hexbytes)


def test_status_no_command_since_boot_is_lastresult_ff_not_seq_0():
    # §4.4: test lastResult == FF, not resultSeq == 0 (0 also occurs after wrapping)
    wrapped = lc.decode_status(status_bytes(UNLOCKED, 0x01, OK, 0))
    assert wrapped.last_result is lc.CmdResult.OK
    assert "no command since boot" not in str(wrapped)
    fresh = lc.decode_status(status_bytes(LOCKED))
    assert fresh.last_result is None
    assert "no command since boot" in str(fresh)


def test_status_str():
    assert str(lc.decode_status(bytes.fromhex("01 01 00 01"))) == \
        "UNLOCKING  (last UNLOCK -> OK, resultSeq 1)"
    assert str(lc.decode_status(bytes.fromhex("02 07 03 03"))) == \
        "UNLOCKED  (last 0x07 -> UNKNOWN_COMMAND, resultSeq 3)"


@pytest.mark.parametrize("extra", [b"\x00", b"\xAA\xBB", bytes(100)])
def test_status_longer_payload_accepted_extra_ignored(extra):
    base = status_bytes(LOCKING, 0x00, BUSY, 255)
    s = lc.decode_status(base + extra)
    assert s == lc.decode_status(base)          # equality ignores raw
    assert (s.state, s.last_opcode, s.last_result, s.result_seq) == \
        (lc.LockState.LOCKING, 0x00, lc.CmdResult.BUSY, 255)
    assert s.raw == base + extra


@pytest.mark.parametrize("n", [0, 1, 2, 3])
def test_status_too_short_is_protocol_error(n):
    with pytest.raises(lc.LockProtocolError):
        lc.decode_status(status_bytes(LOCKED, 0, 0, 1)[:n])


def test_status_unknown_state_kept_and_not_settled():
    # §4.4: unknown state -> show "unknown", don't act on it; must not crash
    s = lc.decode_status(bytes([0x09, 0x01, OK, 1]))
    assert s.state == 9 and not isinstance(s.state, lc.LockState)
    assert not s.settled
    assert "?0x09" in str(s)


def test_status_unknown_result_kept_as_int():
    # §4.4/§6.2: unknown lastResult -> treat as failure (CLI: see test_cli); must not crash
    s = lc.decode_status(bytes([UNLOCKED, 0x01, 0x05, 7]))
    assert s.last_result == 5 and not isinstance(s.last_result, lc.CmdResult)
    assert s.last_result is not None
    assert "?0x05" in str(s)


def test_status_unknown_opcode_shown_raw():
    assert "0xAB" in str(lc.decode_status(bytes([LOCKED, 0xAB, 3, 1])))


# ---- info ----------------------------------------------------------------------

def test_info_spec_example():
    i = lc.decode_info(bytes.fromhex("01 01 00 02 03"))   # §4.5: protocol 1.1, fw 0.2.3
    assert (i.protocol_major, i.protocol_minor) == (1, 1)
    assert (i.fw_major, i.fw_minor, i.fw_patch) == (0, 2, 3)
    assert i.protocol == "1.1" and i.firmware == "0.2.3"
    assert str(i) == "protocol 1.1, firmware 0.2.3"


def test_info_longer_payload_accepted():
    # §4.5: Phase 2 is expected to append a lock ID
    i = lc.decode_info(bytes.fromhex("01 02 03 04 05") + bytes(range(16)))
    assert i.protocol == "1.2" and i.firmware == "3.4.5"


@pytest.mark.parametrize("n", [0, 1, 4])
def test_info_too_short_is_protocol_error(n):
    with pytest.raises(lc.LockProtocolError):
        lc.decode_info(bytes([1, 1, 0, 2, 3])[:n])


# ---- log entries ----------------------------------------------------------------

def test_log_entry_field_offsets_little_endian():
    raw = entry_bytes(0x01020304, 0x0506, 0x0708090A, EV_CMD, [0x01, 0x00, 0x01])
    e = lc.decode_log_entry(raw)
    assert e.seq == 0x01020304
    assert e.boot_count == 0x0506
    assert e.ms_since_boot == 0x0708090A
    assert e.type is lc.EventType.COMMAND_RESULT
    assert e.data == bytes([1, 0, 1, 0, 0])


def test_log_spec_worked_example():
    # ble-spec.md §4.6 worked example, verbatim bytes: a first boot, then a phone connecting
    page = bytes.fromhex(
        "04"
        "01 00 00 00 01 00 1A 04 00 00 01 01 00 00 00 00"
        "02 00 00 00 01 00 1B 04 00 00 02 FF 03 00 00 00"
        "03 00 00 00 01 00 03 08 00 00 02 03 00 00 00 00"
        "04 00 00 00 01 00 6E 14 00 00 04 00 00 00 00 00")
    entries = lc.decode_log_page(page)
    assert [(e.seq, e.boot_count, e.ms_since_boot, e.type) for e in entries] == [
        (1, 1, 1050, lc.EventType.BOOT),
        (2, 1, 1051, lc.EventType.STATE_CHANGE),
        (3, 1, 2051, lc.EventType.STATE_CHANGE),
        (4, 1, 5230, lc.EventType.CONNECT)]
    assert [e.describe() for e in entries] == [
        "reset=POWERON, boot target LOCKED",
        "BOOT -> LOCKING",
        "LOCKING -> LOCKED",
        "conn=0"]
    assert entries[0].data == bytes([1, 0, 0, 0, 0])    # reason POWERON, LOCKED, reserved 0
    assert lc.decode_log_page(bytes.fromhex("00")) == []
    assert lc.encode_log_cursor(3) == bytes.fromhex("03 00 00 00")


@pytest.mark.parametrize("etype,data,want", [
    (EV_BOOT, [1, LOCKED, 0], "reset=POWERON, boot target LOCKED"),
    (EV_BOOT, [9, LOCKED, 0], "reset=BROWNOUT, boot target LOCKED"),
    (EV_BOOT, [99, 7, 0], "reset=?99, boot target ?0x07"),
    (EV_STATE, [NONE, LOCKING], "BOOT -> LOCKING"),
    (EV_STATE, [LOCKED, UNLOCKING], "LOCKED -> UNLOCKING"),
    (EV_STATE, [LOCKING, 9], "LOCKING -> ?0x09"),
    (EV_CMD, [0x01, OK, 1], "opcode=UNLOCK result=OK len=1"),
    (EV_CMD, [NONE, MALFORMED, 0], "opcode=NONE result=MALFORMED len=0"),
    (EV_CMD, [0x07, 3, 1], "opcode=0x07 result=UNKNOWN_COMMAND len=1"),
    (EV_CMD, [0x01, 4, 255], "opcode=UNLOCK result=MALFORMED len=255"),
    (EV_CMD, [0x00, 0x09, 1], "opcode=LOCK result=?0x09 len=1"),
    (EV_CONNECT, [0x34, 0x12], "conn=4660"),
    (EV_DISCONNECT, [1, 0, 0x13, 0], "conn=1 reason=0x13 (remote user terminated)"),
    (EV_DISCONNECT, [2, 0, 0x16, 0], "conn=2 reason=0x16 (terminated by lock)"),
    (EV_DISCONNECT, [2, 0, 0x08, 0], "conn=2 reason=0x08 (supervision timeout)"),
    (EV_DISCONNECT, [2, 0, 0x22, 0x01], "conn=2 reason=0x122 (?)"),
    (EV_IDLE, [3, 0], "conn=3"),
])
def test_log_entry_describe(etype, data, want):
    assert lc.decode_log_entry(entry_bytes(5, 1, 100, etype, data)).describe() == want


@pytest.mark.parametrize("etype", [0, 7, 0x80, 0xFF])
def test_log_unknown_event_type_shown_raw(etype):
    # §4.6: unknown type -> show raw or skip; never fail the whole dump
    e = lc.decode_log_entry(entry_bytes(5, 1, 100, etype, [1, 2, 3, 4, 5]))
    assert e.type == etype and not isinstance(e.type, lc.EventType)
    assert e.describe() == "data=01 02 03 04 05"
    assert f"?0x{etype:02X}" in str(e)
    page = bytes([2]) + entry_bytes(5, 1, 1, etype, [9]) + entry_bytes(6, 1, 2, EV_CONNECT, [1])
    assert [x.seq for x in lc.decode_log_page(page)] == [5, 6]


def test_log_entry_seq_zero_rejected():
    with pytest.raises(lc.LockProtocolError):
        lc.decode_log_entry(entry_bytes(0, 1, 1, EV_BOOT))


def test_log_entry_seq_range_1_to_7fffffff():
    assert lc.decode_log_entry(entry_bytes(0x7FFFFFFF, 1, 1, EV_BOOT)).seq == 0x7FFFFFFF
    for seq in (0x80000000, 0xFFFFFFFF):
        with pytest.raises(lc.LockProtocolError, match="valid seqs"):
            lc.decode_log_entry(entry_bytes(seq, 1, 1, EV_BOOT))


@pytest.mark.parametrize("n", [0, 15, 17])
def test_log_entry_wrong_length_rejected(n):
    with pytest.raises(lc.LockProtocolError):
        lc.decode_log_entry((entry_bytes(1, 1, 1, EV_BOOT) + b"\x00")[:n])


def test_log_entry_str():
    e = lc.decode_log_entry(entry_bytes(42, 3, 1500, EV_STATE, [UNLOCKING, UNLOCKED]))
    s = str(e)
    assert "#42" in s and "boot 3" in s and "+" in s and "1500 ms" in s
    assert "STATE_CHANGE" in s and "UNLOCKING -> UNLOCKED" in s


# ---- log pages ------------------------------------------------------------------

def test_log_page_full_129_bytes():
    page = bytes([8]) + b"".join(entry_bytes(i, 1, i, EV_CONNECT, [i]) for i in range(1, 9))
    assert len(page) == 129
    assert [e.seq for e in lc.decode_log_page(page)] == list(range(1, 9))


def test_log_page_trailing_bytes_ignored():
    page = bytes([1]) + entry_bytes(10, 1, 5, EV_CONNECT, [1])
    assert [e.seq for e in lc.decode_log_page(page + b"\x01\x02\x03")] == [10]
    # a count-0 page with appended fields is still "no more entries"
    assert lc.decode_log_page(b"\x00" + bytes(20)) == []


def test_log_page_count_larger_than_payload_rejected():
    with pytest.raises(lc.LockProtocolError):
        lc.decode_log_page(bytes([3]) + entry_bytes(1, 1, 1, EV_BOOT) * 2)
    with pytest.raises(lc.LockProtocolError):
        lc.decode_log_page(bytes([1]) + entry_bytes(1, 1, 1, EV_BOOT)[:15])


def test_log_page_empty_rejected():
    with pytest.raises(lc.LockProtocolError):
        lc.decode_log_page(b"")


@pytest.mark.parametrize("seq,raw", [
    (0, "00 00 00 00"), (3, "03 00 00 00"), (0x01020304, "04 03 02 01"), (0xFFFFFFFF, "FF FF FF FF")])
def test_log_cursor_uint32_le(seq, raw):
    assert lc.encode_log_cursor(seq) == bytes.fromhex(raw)

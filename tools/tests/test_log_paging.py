"""Event log paging (ble-spec.md §4.6): cursor writes, multiple pages, count == 0
termination, gaps in seq, and misbehaving locks."""

import pytest

import lock_client as lc
from lockfakes import (EV_CONNECT, EV_STATE, LOG, LOCK_ADDRESS, STATUS, ScriptedLock, entry_bytes,
                       entry_seq)


def log_lock(seqs, page_size=8, etype=EV_CONNECT):
    """A lock whose log holds entries with these seq numbers and pages like the spec."""
    entries = [entry_bytes(s, 1, (s * 10) & 0xFFFFFFFF, etype, [s & 0xFF]) for s in seqs]

    def reader(cursor):
        sel = [e for e in entries if entry_seq(e) >= cursor][:page_size]
        return bytes([len(sel)]) + b"".join(sel)
    return ScriptedLock(log_reader=reader)


def client():
    return lc.LockClient(LOCK_ADDRESS, op_timeout=0.3)


async def test_multiple_pages_until_count_zero(world):
    p = world.add(log_lock(range(1, 21)))
    async with client() as lock:
        pages = [page async for page in lock.iter_log()]
    assert [[e.seq for e in pg] for pg in pages] == [
        list(range(1, 9)), list(range(9, 17)), list(range(17, 21))]
    # cursor = last seq + 1 each time; the final read returns count 0
    assert p.cursor_writes == [0, 9, 17, 21]
    assert all(len(d) == 4 for u, d in p.writes if u == LOG)


async def test_short_page_is_not_the_end(world):
    # "A page with fewer than 8 entries is not by itself the end; only count == 0 is."
    p = world.add(log_lock(range(1, 11), page_size=3))
    async with client() as lock:
        entries = await lock.read_log()
    assert [e.seq for e in entries] == list(range(1, 11))
    assert p.cursor_writes == [0, 4, 7, 10, 11]


async def test_empty_log(world):
    p = world.add(log_lock([]))
    async with client() as lock:
        assert await lock.read_log() == []
    assert p.cursor_writes == [0]


async def test_gaps_in_seq(world):
    seqs = [1, 2, 5, 9, 10, 11, 30, 31, 32, 33, 100, 0x7FFFFFFF]   # top of the seq range
    p = world.add(log_lock(seqs))
    async with client() as lock:
        entries = await lock.read_log()
    assert [e.seq for e in entries] == seqs
    assert p.cursor_writes == [0, 32, 0x80000000]   # pages end at 31, then 0x7FFFFFFF


async def test_seq_above_range_is_protocol_error(world):
    world.add(log_lock([1, 0x80000000]))
    async with client() as lock:
        with pytest.raises(lc.LockProtocolError, match="valid seqs are 1..0x7FFFFFFF"):
            await lock.read_log()


async def test_start_cursor(world):
    p = world.add(log_lock(range(1, 30)))
    async with client() as lock:
        entries = await lock.read_log(start=25)
    assert [e.seq for e in entries] == [25, 26, 27, 28, 29]
    assert p.cursor_writes[0] == 25


async def test_start_cursor_inside_gap(world):
    world.add(log_lock([1, 2, 50, 51]))
    async with client() as lock:
        assert [e.seq for e in await lock.read_log(start=10)] == [50, 51]


async def test_unknown_event_types_do_not_fail_the_dump(world):
    world.add(log_lock(range(1, 12), etype=0x42))
    async with client() as lock:
        entries = await lock.read_log()
    assert len(entries) == 11 and all(e.type == 0x42 for e in entries)


async def test_lock_ignoring_cursor_is_protocol_error_not_endless(world):
    entries = [entry_bytes(s, 1, s, EV_CONNECT) for s in range(1, 9)]
    world.add(ScriptedLock(log_reader=lambda cursor: bytes([8]) + b"".join(entries)))
    async with client() as lock:
        with pytest.raises(lc.LockProtocolError, match="expected increasing, >= cursor"):
            await lock.read_log()


async def test_non_increasing_page_is_protocol_error(world):
    page = bytes([3]) + entry_bytes(5, 1, 1, EV_STATE) + entry_bytes(7, 1, 1, EV_STATE) \
        + entry_bytes(6, 1, 1, EV_STATE)
    world.add(ScriptedLock(log_reader=lambda cursor: page if cursor == 0 else b"\x00"))
    async with client() as lock:
        with pytest.raises(lc.LockProtocolError, match=r"\[5, 7, 6\]"):
            await lock.read_log()


async def test_truncated_page_is_protocol_error(world):
    world.add(ScriptedLock(log_reader=lambda cursor: bytes([2]) + entry_bytes(1, 1, 1, EV_STATE)))
    async with client() as lock:
        with pytest.raises(lc.LockProtocolError):
            await lock.read_log()


async def test_log_char_missing_on_otherwise_v11_lock(world):
    world.add(ScriptedLock(chars=(lc.CMD_CHAR_UUID, STATUS, lc.INFO_CHAR_UUID)))
    async with client() as lock:
        with pytest.raises(lc.FirmwareTooOldError, match="characteristic .* is missing"):
            await lock.read_log()

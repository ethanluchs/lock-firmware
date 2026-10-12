"""LockClient against scripted peripherals: result matching by resultSeq (ble-spec.md §6.5),
settle waiting, timeouts and the diagnosis read, and GATT error mapping."""

import asyncio
import time

import pytest
from bleak.exc import BleakError, BleakGATTProtocolError

import lock_client as lc
from lockfakes import (BUSY, CMD, LOCK_ADDRESS, LOCKED, LOCKING, MALFORMED, NO_CHANGE, NONE, OK,
                       STATUS, UNKNOWN_COMMAND, UNLOCKED, UNLOCKING, V10_CHARS, FakeLock,
                       ScriptedLock, status_bytes)

OP_T = 0.3      # op/result timeout used in these tests
SETTLE_T = 0.5


def client(**kw):
    kw.setdefault("op_timeout", OP_T)
    kw.setdefault("settle_timeout", SETTLE_T)
    return lc.LockClient(LOCK_ADDRESS, **kw)


def replies(*steps):
    """on_command script: each step is (delay_s, status bytes); the lock's readable
    status follows the notifications."""
    def on_command(lock, data):
        for delay, raw in steps:
            lock.later(delay, raw)
    return on_command


# ---- matching by resultSeq --------------------------------------------------------

async def test_result_is_notification_with_seq_plus_one(world):
    world.add(ScriptedLock(status=status_bytes(LOCKED, 0x00, OK, 5),
                           on_command=replies((0.01, status_bytes(UNLOCKING, 0x01, OK, 6)))))
    async with client() as lock:
        out = await lock.command(lc.CMD_UNLOCK)
    assert out.result is lc.CmdResult.OK
    assert out.result_status.result_seq == 6
    assert out.result_status.state is lc.LockState.UNLOCKING
    assert out.settled is None
    assert out.data == b"\x01"
    assert out.result_ms >= 0 and out.ack_ms >= 0


async def test_late_settle_of_earlier_command_is_not_the_result(world):
    # §6.5: "A status whose resultSeq == S is a settle of an earlier command; it is not your result."
    world.add(ScriptedLock(status=status_bytes(UNLOCKING, 0x01, OK, 5), on_command=replies(
        (0.01, status_bytes(UNLOCKED, 0x01, OK, 5)),            # settle of #5
        (0.03, status_bytes(UNLOCKED, 0x07, UNKNOWN_COMMAND, 6)))))
    async with client() as lock:
        out = await lock.command(0x07)
    assert out.result is lc.CmdResult.UNKNOWN_COMMAND
    assert out.result_status.result_seq == 6
    assert [s.result_seq for s in out.notifications] == [5, 6]


async def test_result_seq_wraps_255_to_0(world):
    world.add(ScriptedLock(status=status_bytes(UNLOCKED, 0x01, NO_CHANGE, 255),
                           on_command=replies((0.01, status_bytes(UNLOCKED, 0x01, NO_CHANGE, 0)))))
    async with client() as lock:
        out = await lock.command(lc.CMD_UNLOCK)
    assert out.result_status.result_seq == 0
    assert out.result is lc.CmdResult.NO_CHANGE


async def test_baseline_seq_skips_status_read(world):
    p = world.add(ScriptedLock(status=status_bytes(LOCKED, 0x00, OK, 9),
                               on_command=replies((0.01, status_bytes(LOCKED, 0x00, NO_CHANGE, 0)))))
    async with client() as lock:
        out = await lock.command(lc.CMD_LOCK, baseline_seq=255)
    assert out.result_status.result_seq == 0
    assert STATUS not in p.reads


async def test_notification_before_write_ack_is_matched(world):
    # §5: "The write response and the result notification may arrive in either order."
    def on_command(lock, data):
        lock.status = status_bytes(UNLOCKING, 0x01, OK, 1)
        lock.notify(lock.status, now=True)       # delivered before write_gatt_char returns
    world.add(ScriptedLock(on_command=on_command))
    async with client() as lock:
        out = await lock.command(lc.CMD_UNLOCK)
    assert out.result is lc.CmdResult.OK and out.result_status.result_seq == 1


async def test_mismatched_opcode_is_protocol_error(world):
    world.add(ScriptedLock(on_command=replies((0.01, status_bytes(LOCKED, 0x00, NO_CHANGE, 1)))))
    async with client() as lock:
        with pytest.raises(lc.LockProtocolError, match="reports opcode LOCK, but UNLOCK was sent"):
            await lock.command(lc.CMD_UNLOCK)


async def test_skipped_seq_is_protocol_error(world):
    world.add(ScriptedLock(status=status_bytes(LOCKED, 0x00, OK, 3),
                           on_command=replies((0.01, status_bytes(UNLOCKING, 0x01, OK, 5)))))
    async with client() as lock:
        with pytest.raises(lc.LockProtocolError, match="jumped from 3 to 5"):
            await lock.command(lc.CMD_UNLOCK)


async def test_skipped_seq_across_wrap_is_protocol_error(world):
    world.add(ScriptedLock(status=status_bytes(LOCKED, 0x00, OK, 255),
                           on_command=replies((0.01, status_bytes(UNLOCKING, 0x01, OK, 1)))))
    async with client() as lock:
        with pytest.raises(lc.LockProtocolError, match="jumped from 255 to 1"):
            await lock.command(lc.CMD_UNLOCK)


@pytest.mark.parametrize("data,op", [(b"", NONE), (b"\x01\x00", 0x01)])
async def test_malformed_writes_match_lastopcode(world, data, op):
    # §4.2: empty write -> lastOpcode FF; >= 2 bytes -> lastOpcode = first byte
    world.add(ScriptedLock(status=status_bytes(UNLOCKED, 0x01, OK, 1),
                           on_command=replies((0.01, status_bytes(UNLOCKED, op, MALFORMED, 2)))))
    async with client() as lock:
        out = await lock.command(data)
    assert out.result is lc.CmdResult.MALFORMED and out.data == data


async def test_bad_notification_during_command_is_protocol_error(world):
    world.add(ScriptedLock(on_command=replies((0.01, b"\x01\x01\x00"))))
    async with client() as lock:
        with pytest.raises(lc.LockProtocolError, match="status payload is 3 bytes"):
            await lock.command(lc.CMD_UNLOCK)


async def test_longer_notification_accepted(world):
    world.add(ScriptedLock(on_command=replies((0.01, status_bytes(UNLOCKING, 0x01, OK, 1) + b"\xEE\xEE"))))
    async with client() as lock:
        out = await lock.command(lc.CMD_UNLOCK)
    assert out.result is lc.CmdResult.OK


async def test_unknown_result_code_returned_not_raised(world):
    world.add(ScriptedLock(on_command=replies((0.01, status_bytes(LOCKED, 0x01, 0x07, 1)))))
    async with client() as lock:
        out = await lock.command(lc.CMD_UNLOCK)
    assert out.result == 7 and not isinstance(out.result, lc.CmdResult)


# ---- missing notification: timeout, then the diagnosis read (§6.5, §7) -----------

async def test_missing_notification_processed_says_firmware_bug(world):
    def on_command(lock, data):
        lock.status = status_bytes(UNLOCKING, 0x01, OK, 1)    # processed, never notified
    world.add(ScriptedLock(on_command=on_command))
    async with client() as lock:
        t0 = time.perf_counter()
        with pytest.raises(lc.LockTimeoutError) as ei:
            await lock.command(lc.CMD_UNLOCK)
        elapsed = time.perf_counter() - t0
    msg = str(ei.value)
    assert "resultSeq 1" in msg and "Outcome unknown" in msg
    assert "processed the command but did not notify" in msg
    assert "UNLOCKING" in msg
    assert OP_T * 0.9 <= elapsed < OP_T + 1.0


async def test_missing_notification_not_processed(world):
    world.add(ScriptedLock(status=status_bytes(LOCKED, 0x00, NO_CHANGE, 4), on_command=lambda l, d: None))
    async with client() as lock:
        with pytest.raises(lc.LockTimeoutError) as ei:
            await lock.command(lc.CMD_UNLOCK)
    msg = str(ei.value)
    assert "Status now reads LOCKED" in msg and "resultSeq 4" in msg
    assert "did not notify" not in msg


async def test_missing_notification_and_status_read_fails(world):
    def on_command(lock, data):
        lock.read_error = BleakError("gone")
    world.add(ScriptedLock(on_command=on_command))
    async with client() as lock:
        with pytest.raises(lc.LockTimeoutError, match="Status read also failed"):
            await lock.command(lc.CMD_UNLOCK)


async def test_only_stale_notifications_then_timeout(world):
    world.add(ScriptedLock(status=status_bytes(UNLOCKING, 0x01, OK, 2),
                           on_command=replies((0.01, status_bytes(UNLOCKED, 0x01, OK, 2)))))
    async with client() as lock:
        with pytest.raises(lc.LockTimeoutError, match="resultSeq 3"):
            await lock.command(lc.CMD_LOCK)


async def test_disconnect_while_waiting_for_result(world):
    def on_command(lock, data):
        asyncio.get_running_loop().call_later(0.02, lock.drop_link)
    world.add(ScriptedLock(on_command=on_command))
    async with client() as lock:
        with pytest.raises(lc.LockConnectionError, match="disconnected while waiting for result"):
            await lock.command(lc.CMD_UNLOCK)


# ---- settle waiting ----------------------------------------------------------------

async def test_command_settle_waits_for_settled_state(world):
    world.add(ScriptedLock(on_command=replies(
        (0.01, status_bytes(UNLOCKING, 0x01, OK, 1)),
        (0.21, status_bytes(UNLOCKED, 0x01, OK, 1)))))
    async with client() as lock:
        out = await lock.command(lc.CMD_UNLOCK, settle=True)
    assert out.result_status.state is lc.LockState.UNLOCKING
    assert out.settled.state is lc.LockState.UNLOCKED and out.settled.result_seq == 1
    assert 0.15 <= out.settled_after_s <= 0.35
    assert [s.state for s in out.notifications] == [lc.LockState.UNLOCKING, lc.LockState.UNLOCKED]


async def test_command_settle_already_settled_result(world):
    world.add(ScriptedLock(status=status_bytes(UNLOCKED, 0x01, OK, 1),
                           on_command=replies((0.01, status_bytes(UNLOCKED, 0x01, NO_CHANGE, 2)))))
    async with client() as lock:
        out = await lock.command(lc.CMD_UNLOCK, settle=True)
    assert out.settled is out.result_status
    assert out.settled_after_s == 0


async def test_command_settle_skips_non_settled_notifications(world):
    # BUSY during UNLOCKING: result is transitional; the next settled status ends the wait
    world.add(ScriptedLock(status=status_bytes(UNLOCKING, 0x01, OK, 1), on_command=replies(
        (0.01, status_bytes(UNLOCKING, 0x00, BUSY, 2)),
        (0.10, status_bytes(UNLOCKED, 0x00, BUSY, 2)))))
    async with client() as lock:
        out = await lock.command(lc.CMD_LOCK, settle=True)
    assert out.result is lc.CmdResult.BUSY
    assert out.settled.state is lc.LockState.UNLOCKED


async def test_command_settle_timeout(world):
    world.add(ScriptedLock(on_command=replies((0.01, status_bytes(UNLOCKING, 0x01, OK, 1)))))
    async with client() as lock:
        t0 = time.perf_counter()
        with pytest.raises(lc.LockTimeoutError, match="settled state after UNLOCKING"):
            await lock.command(lc.CMD_UNLOCK, settle=True, settle_timeout=0.2)
        assert time.perf_counter() - t0 < 1.0


async def test_wait_settled_returns_immediately_when_settled(world):
    world.add(ScriptedLock(status=status_bytes(UNLOCKED, 0x01, OK, 1)))
    async with client() as lock:
        s = await lock.wait_settled(timeout=5)
    assert s.state is lc.LockState.UNLOCKED


async def test_wait_settled_waits_for_notification(world):
    p = world.add(ScriptedLock(status=status_bytes(LOCKING, 0x00, OK, 1)))
    async with client() as lock:
        p.later(0.05, status_bytes(LOCKING, 0x00, OK, 1))   # not settled yet: keep waiting
        p.later(0.10, status_bytes(LOCKED, 0x00, OK, 1))
        s = await lock.wait_settled(timeout=1)
    assert s.state is lc.LockState.LOCKED


async def test_wait_settled_timeout(world):
    world.add(ScriptedLock(status=status_bytes(LOCKING, 0x00, OK, 1)))
    async with client(settle_timeout=0.15) as lock:
        with pytest.raises(lc.LockTimeoutError, match=r"settled state \(now LOCKING\)"):
            await lock.wait_settled()


async def test_wait_settled_unknown_state_does_not_count_as_settled(world):
    world.add(ScriptedLock(status=bytes([9, 0xFF, 0xFF, 0])))
    async with client() as lock:
        with pytest.raises(lc.LockTimeoutError):
            await lock.wait_settled(timeout=0.1)


async def test_unlock_and_lock_against_spec_lock(world):
    world.add(FakeLock(t_act=0.1, t_idle=None))
    async with client(settle_timeout=1.0) as lock:
        out = await lock.unlock()
        assert out.result is lc.CmdResult.OK and out.settled.state is lc.LockState.UNLOCKED
        assert 0.05 <= out.settled_after_s <= 0.3
        out = await lock.unlock()
        assert out.result is lc.CmdResult.NO_CHANGE and out.settled is out.result_status
        out = await lock.lock(settle=False)
        assert out.result is lc.CmdResult.OK and out.settled is None
        # UNLOCK during LOCKING is queued: its result follows the LOCKED settle
        queued = await lock.command(lc.CMD_UNLOCK, baseline_seq=out.result_status.result_seq,
                                    settle=True)
        assert queued.result is lc.CmdResult.OK
        assert [(s.state, s.result_seq) for s in queued.notifications] == [
            (lc.LockState.LOCKED, 3), (lc.LockState.UNLOCKING, 4), (lc.LockState.UNLOCKED, 4)]
        assert queued.settled.state is lc.LockState.UNLOCKED


async def test_watch_status_ends_on_idle_disconnect(world):
    p = world.add(FakeLock(t_act=0.05, t_idle=0.4))
    async with client() as lock:
        await lock.enable_status_notifications()
        asyncio.get_running_loop().call_later(0.05, p._command, b"\x01")
        seen = [s async for s in lock.watch_status()]
        assert not lock.is_connected and lock.disconnected_at is not None
    assert [s.state for s in seen] == [lc.LockState.UNLOCKING, lc.LockState.UNLOCKED]


# ---- GATT errors -> exception classes ---------------------------------------------------

async def test_write_rejected_by_lock(world):
    p = world.add(ScriptedLock())
    async with client() as lock:
        p.write_error = BleakGATTProtocolError(0x03)
        with pytest.raises(lc.LockWriteError, match="rejected"):
            await lock.command(lc.CMD_UNLOCK)


async def test_write_timeout(world):
    p = world.add(ScriptedLock())
    async with client() as lock:
        p.write_hang = True
        with pytest.raises(lc.LockWriteError, match="outcome unknown"):
            await lock.write(CMD, b"\x01")


async def test_write_bleak_error_while_connected(world):
    p = world.add(ScriptedLock())
    async with client() as lock:
        p.write_error = BleakError("busy")
        with pytest.raises(lc.LockWriteError, match="Write failed"):
            await lock.write(CMD, b"\x01")


async def test_write_link_dropped(world):
    p = world.add(ScriptedLock())
    async with client() as lock:
        p.write_error = BleakError("link lost")
        p.write_error_drops_link = True
        with pytest.raises(lc.LockConnectionError, match="Link dropped"):
            await lock.write(CMD, b"\x01")


async def test_read_errors(world):
    p = world.add(ScriptedLock())
    async with client() as lock:
        p.read_error = BleakError("nope")
        with pytest.raises(lc.LockConnectionError, match="failed"):
            await lock.read_status()
        p.read_error, p.read_hang = None, True
        with pytest.raises(lc.LockConnectionError, match="timed out"):
            await lock.read_status()


async def test_not_connected(world):
    world.add(ScriptedLock())
    lock = client()
    with pytest.raises(lc.LockConnectionError, match="Not connected"):
        await lock.write(CMD, b"\x01")


async def test_v10_firmware_raises_too_old(world):
    world.add(ScriptedLock(chars=V10_CHARS))
    async with client() as lock:
        for coro in (lock.read_status, lock.read_info, lock.wait_settled, lock.read_log,
                     lambda: lock.command(1)):
            with pytest.raises(lc.FirmwareTooOldError, match="v1.0 firmware"):
                await coro()
        await lock.send_command(0x01)        # bare v1.0 write still works
    assert world.peripherals[0].cmd_writes() == [b"\x01"]


async def test_missing_command_char_is_incompatible(world):
    p = world.add(ScriptedLock(chars=(STATUS,)))
    lock = client()
    with pytest.raises(lc.IncompatibleFirmwareError) as ei:
        await lock.connect()
    assert not isinstance(ei.value, lc.FirmwareTooOldError)
    assert p.client is None and not lock.is_connected      # disconnected again


async def test_connect_disables_winrt_service_cache(world):
    world.add(ScriptedLock())
    async with client():
        pass
    assert world.clients[0].kwargs.get("winrt") == {"use_cached_services": False}


# ---- queued commands (ble-spec.md §6.2, §6.5) ---------------------------------------------

async def test_result_after_several_settle_notifications(world):
    # a deferred result may follow one or more settle notifications carrying the old resultSeq
    world.add(ScriptedLock(status=status_bytes(UNLOCKING, 0x01, OK, 5), on_command=replies(
        (0.01, status_bytes(UNLOCKING, 0x01, OK, 5)),
        (0.02, status_bytes(UNLOCKED, 0x01, OK, 5)),
        (0.03, status_bytes(LOCKING, 0x00, OK, 6)))))
    async with client() as lock:
        out = await lock.command(lc.CMD_LOCK)
    assert out.result is lc.CmdResult.OK and out.result_status.state is lc.LockState.LOCKING
    assert [s.result_seq for s in out.notifications] == [5, 5, 6]


async def test_queued_result_waits_past_op_timeout_when_lock_was_moving(world):
    # op timeout 0.1 s, but the move ahead takes 0.3 s: the wait adds the settle timeout
    world.add(ScriptedLock(status=status_bytes(UNLOCKING, 0x01, OK, 1), on_command=replies(
        (0.3, status_bytes(UNLOCKED, 0x01, OK, 1)),
        (0.3, status_bytes(LOCKING, 0x00, OK, 2)))))
    async with client(op_timeout=0.1, settle_timeout=0.6) as lock:
        out = await lock.command(lc.CMD_LOCK)
    assert out.result is lc.CmdResult.OK and out.result_ms >= 250


async def test_settled_lock_keeps_the_short_result_timeout(world):
    world.add(ScriptedLock(status=status_bytes(LOCKED, 0x00, OK, 1), on_command=lambda l, d: None))
    async with client(op_timeout=0.1, settle_timeout=5.0) as lock:
        t0 = time.perf_counter()
        with pytest.raises(lc.LockTimeoutError):
            await lock.command(lc.CMD_UNLOCK)
        assert time.perf_counter() - t0 < 1.0


async def test_internal_overflow_ff_busy_is_a_result_not_a_protocol_error(world):
    world.add(ScriptedLock(status=status_bytes(UNLOCKING, 0x01, OK, 1),
                           on_command=replies((0.01, status_bytes(UNLOCKING, NONE, BUSY, 2)))))
    async with client() as lock:
        out = await lock.command(lc.CMD_LOCK)
    assert out.result is lc.CmdResult.BUSY and out.result_status.last_opcode == NONE


async def test_ff_with_other_result_is_still_protocol_error(world):
    world.add(ScriptedLock(on_command=replies((0.01, status_bytes(LOCKED, NONE, MALFORMED, 1)))))
    async with client() as lock:
        with pytest.raises(lc.LockProtocolError, match="reports opcode NONE"):
            await lock.command(lc.CMD_UNLOCK)


def notes(p):
    return [tuple(n) for n in p.notified]


async def test_fake_lock_deferral_ordering(world):
    # UNLOCK, then LOCK at once: no notification for LOCK at write time; on settle the
    # settle notification first, then LOCK's result; the log likewise
    p = world.add(FakeLock(t_act=0.1, t_idle=None))
    async with client(settle_timeout=1.0) as lock:
        await lock.enable_status_notifications()
        n_log = len(p.entries)
        await lock.write(CMD, b"\x01")
        await lock.write(CMD, b"\x00")
        await asyncio.sleep(0.02)
        assert notes(p) == [(UNLOCKING, 0x01, OK, 1)]        # LOCK is queued silently
        assert len(p.entries) == n_log + 2                    # and not logged yet
        await asyncio.sleep(0.3)
    assert notes(p) == [(UNLOCKING, 0x01, OK, 1), (UNLOCKED, 0x01, OK, 1),
                        (LOCKING, 0x00, OK, 2), (LOCKED, 0x00, OK, 2)]
    logged = [lc.decode_log_entry(e) for e in p.entries[n_log:]]
    assert [e.describe() for e in logged if e.type is not lc.EventType.DISCONNECT] == [
        "opcode=UNLOCK result=OK len=1", "LOCKED -> UNLOCKING", "UNLOCKING -> UNLOCKED",
        "opcode=LOCK result=OK len=1", "UNLOCKED -> LOCKING", "LOCKING -> LOCKED"]


async def test_fake_lock_queue_full_busy_and_production_order(world):
    # §6.5 rule 5: the 5th command's immediate BUSY takes S+2, ahead of the 4 queued results
    p = world.add(FakeLock(t_act=0.05, t_idle=None))
    async with client() as lock:
        await lock.enable_status_notifications()
        for b in (b"\x01", b"\x00", b"\x01", b"\x00", b"\x01", b"\x00"):
            await lock.write(CMD, b)
        await asyncio.sleep(0.01)
        assert notes(p) == [(UNLOCKING, 1, OK, 1), (UNLOCKING, 0, BUSY, 2)]
        await asyncio.sleep(0.4)
    assert notes(p)[2:] == [
        (UNLOCKED, 0, BUSY, 2),
        (LOCKING, 0, OK, 3), (LOCKED, 0, OK, 3), (UNLOCKING, 1, OK, 4), (UNLOCKED, 1, OK, 4),
        (LOCKING, 0, OK, 5), (LOCKED, 0, OK, 5), (UNLOCKING, 1, OK, 6), (UNLOCKED, 1, OK, 6)]


async def test_fake_lock_queued_same_direction_and_immediate_unknown(world):
    p = world.add(FakeLock(t_act=0.05, t_idle=None))
    async with client() as lock:
        await lock.enable_status_notifications()
        for _ in range(3):
            await lock.write(CMD, b"\x01")
        await lock.write(CMD, b"\x07")                     # unknown: immediate even mid-move
        await asyncio.sleep(0.15)
    assert notes(p) == [(UNLOCKING, 1, OK, 1), (UNLOCKING, 7, UNKNOWN_COMMAND, 2),
                        (UNLOCKED, 7, UNKNOWN_COMMAND, 2),
                        (UNLOCKED, 1, NO_CHANGE, 3), (UNLOCKED, 1, NO_CHANGE, 4)]


async def test_command_queued_behind_boot_locking(world):
    # every boot starts with LOCKING; an UNLOCK sent then is queued and runs after LOCKED
    p = world.add(FakeLock(t_act=0.15, t_idle=None))
    p.reset(1)
    await asyncio.sleep(0.03)
    async with client(settle_timeout=1.0) as lock:
        assert (await lock.read_status()).state is lc.LockState.LOCKING
        out = await lock.command(lc.CMD_UNLOCK, settle=True)
    assert out.result is lc.CmdResult.OK and out.result_status.result_seq == 1
    assert [s.state for s in out.notifications] == [
        lc.LockState.LOCKED, lc.LockState.UNLOCKING, lc.LockState.UNLOCKED]


# ---- boot, connections, log storage ------------------------------------------------------

async def test_fake_lock_boot_always_locks_with_boot_entries(world):
    p = world.add(FakeLock(t_act=0.05, t_idle=None))
    async with client() as lock:
        await lock.unlock()
    p.reset(1)
    assert p.state == LOCKING
    await asyncio.sleep(0.1)
    assert p.state == LOCKED and p.status() == status_bytes(LOCKED)
    boot2 = [e for e in map(lc.decode_log_entry, p.entries) if e.boot_count == 2]
    assert [(e.type, e.data) for e in boot2] == [
        (lc.EventType.BOOT, bytes([1, LOCKED, 0, 0, 0])),
        (lc.EventType.STATE_CHANGE, bytes([NONE, LOCKING, 0, 0, 0])),
        (lc.EventType.STATE_CHANGE, bytes([LOCKING, LOCKED, 0, 0, 0]))]


async def test_second_connection_refused_first_undisturbed(world):
    world.add(FakeLock(t_idle=None))
    async with client() as first:
        with pytest.raises(lc.LockConnectionError):
            await client().connect()
        assert first.is_connected
        assert (await first.unlock()).result is lc.CmdResult.OK


async def test_log_entries_readable_only_once_saved(world):
    p = world.add(FakeLock(t_idle=None, flush_delay=0.1))
    await asyncio.sleep(0.12)                              # boot entries saved
    async with client() as lock:
        assert [e.type for e in await lock.read_log()] == [
            lc.EventType.BOOT, lc.EventType.STATE_CHANGE, lc.EventType.STATE_CHANGE]
        await asyncio.sleep(0.12)
        assert (await lock.read_log())[-1].type is lc.EventType.CONNECT
    assert len(p.entries) == 5


async def test_ring_log_keeps_newest_256(world):
    p = world.add(FakeLock(t_idle=None))
    async with client() as lock:
        for _ in range(300):
            await lock.write(CMD, b"\x07")
        entries = await lock.read_log()
    assert len(entries) == 256 == len(p.entries)
    assert entries[-1].seq == 304 and entries[0].seq == 49  # 4 boot/connect + 300 results


async def test_disconnect_bleak_error_wrapped(world, monkeypatch):
    world.add(ScriptedLock())
    lock = client()
    await lock.connect()

    async def bad_disconnect():
        raise BleakError("stack hiccup")
    monkeypatch.setattr(lock._client, "disconnect", bad_disconnect)
    with pytest.raises(lc.LockConnectionError, match="Disconnect failed: stack hiccup"):
        await lock.disconnect()


async def test_disconnect_error_does_not_mask_block_error(world, monkeypatch):
    world.add(ScriptedLock())
    with pytest.raises(lc.LockProtocolError, match="first"):
        async with client() as lock:
            async def bad_disconnect():
                raise BleakError("stack hiccup")
            monkeypatch.setattr(lock._client, "disconnect", bad_disconnect)
            raise lc.LockProtocolError("first")

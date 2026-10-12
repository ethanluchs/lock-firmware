"""CLI: argument parsing for every subcommand, the exception -> exit code map, and
end-to-end runs of each subcommand through main() against fake locks."""

import asyncio

import pytest
from bleak.exc import (BleakBluetoothNotAvailableError, BleakBluetoothNotAvailableReason,
                       BleakError, BleakGATTProtocolError)

import lock_client as lc
from lockfakes import (BUSY, LOCK_ADDRESS, LOCKED, OK, UNLOCKED, UNLOCKING,
                       FakeLock, ScriptedLock, status_bytes)


# ---- argument parsing (main() with _run stubbed) ---------------------------------

@pytest.fixture
def parsed(monkeypatch):
    """parsed(argv) -> the Namespace main() hands to _run."""
    got = {}

    async def fake_run(args):
        got["args"] = args
        return 0

    monkeypatch.setattr(lc, "_run", fake_run)

    def parse(argv):
        assert lc.main(argv) == 0
        return got["args"]
    return parse


def test_global_defaults(parsed):
    a = parsed(["status"])
    assert a.command == "status"
    assert a.name == lc.DEFAULT_NAME and a.address is None
    assert a.scan_timeout == lc.DEFAULT_SCAN_TIMEOUT
    assert a.timeout == lc.DEFAULT_OP_TIMEOUT
    assert a.settle_timeout == lc.DEFAULT_SETTLE_TIMEOUT
    assert a.verbose == 0


def test_global_options(parsed):
    a = parsed(["-vv", "--name", "VT-LOCK-007", "--address", "11:22:33:44:55:66",
                "--scan-timeout", "3.5", "--timeout", "2", "--settle-timeout", "7", "info"])
    assert (a.verbose, a.name, a.address) == (2, "VT-LOCK-007", "11:22:33:44:55:66")
    assert (a.scan_timeout, a.timeout, a.settle_timeout) == (3.5, 2.0, 7.0)
    assert a.command == "info"


def test_any_clears_name(parsed):
    assert parsed(["--any", "scan"]).name is None


@pytest.mark.parametrize("cmd", ["scan", "info", "status", "watch"])
def test_plain_subcommands(parsed, cmd):
    assert parsed([cmd]).command == cmd


@pytest.mark.parametrize("cmd", ["unlock", "lock"])
def test_unlock_lock_no_settle(parsed, cmd):
    assert parsed([cmd]).no_settle is False
    a = parsed([cmd, "--no-settle"])
    assert a.command == cmd and a.no_settle is True


@pytest.mark.parametrize("text,data", [
    ("07", b"\x07"), ("01 00", b"\x01\x00"), ("0x01", b"\x01"), ("01,ff", b"\x01\xff"),
    ("", b""), ("DEADbeef", b"\xde\xad\xbe\xef")])
def test_send_hex_parsing(parsed, text, data):
    a = parsed(["send", text])
    assert a.command == "send" and a.data == data


@pytest.mark.parametrize("argv", [["send", "zz"], ["send", "1"], ["send"]])
def test_send_bad_hex_is_usage_error(argv, capsys):
    with pytest.raises(SystemExit) as ei:
        lc.main(argv)
    assert ei.value.code == 2          # argparse usage error


def test_log_from(parsed):
    assert parsed(["log"]).start == 0
    assert parsed(["log", "--from", "120"]).start == 120


def test_acceptance_options(parsed):
    a = parsed(["acceptance"])
    assert (a.interactive, a.t_act, a.idle_timeout) == (False, lc.T_ACT_LED, lc.T_IDLE)
    a = parsed(["acceptance", "--interactive", "--t-act", "2.5", "--idle-timeout", "45"])
    assert (a.interactive, a.t_act, a.idle_timeout) == (True, 2.5, 45.0)


@pytest.mark.parametrize("argv", [[], ["bogus"], ["status", "--no-settle"], ["log", "--from", "x"]])
def test_usage_errors(argv):
    with pytest.raises(SystemExit) as ei:
        lc.main(argv)
    assert ei.value.code == 2


# ---- error class -> exit code (EXIT_CODES and main's lookup) ----------------------------

class _OtherLockError(lc.LockError):
    pass


class _TooOldSubclass(lc.FirmwareTooOldError):
    pass


@pytest.mark.parametrize("exc,code", [
    (lc.BluetoothUnavailableError("x"), 12),
    (lc.LockNotFoundError("x"), 3),
    (lc.LockConnectionError("x"), 4),
    (lc.IncompatibleFirmwareError("x"), 5),
    (lc.LockWriteError("x"), 6),
    (lc.FirmwareTooOldError("x"), 7),
    (_TooOldSubclass("x"), 7),
    (lc.LockProtocolError("x"), 8),
    (lc.LockTimeoutError("x"), 9),
    (lc.LockError("x"), 1),
    (_OtherLockError("x"), 1),
])
def test_error_exit_codes(monkeypatch, capsys, exc, code):
    async def boom(args):
        raise exc
    monkeypatch.setattr(lc, "_run", boom)
    assert lc.main(["status"]) == code
    assert capsys.readouterr().err.startswith("error: x")


def test_every_lock_error_class_has_a_distinct_code():
    classes = [c for c in vars(lc).values()
               if isinstance(c, type) and issubclass(c, lc.LockError)]
    assert set(classes) == set(lc.EXIT_CODES)
    codes = list(lc.EXIT_CODES.values())
    assert len(codes) == len(set(codes))
    assert not {lc.EXIT_NOT_ACCEPTED, lc.EXIT_ACCEPTANCE_FAILED, 0, 130} & set(codes)


def test_keyboard_interrupt_exit_130(monkeypatch, capsys):
    async def interrupted(args):
        raise KeyboardInterrupt
    monkeypatch.setattr(lc, "_run", interrupted)
    assert lc.main(["watch"]) == 130


# ---- end to end through main() ---------------------------------------------------------

def run(*argv, timeout="0.5"):
    return lc.main(["--address", LOCK_ADDRESS, "--timeout", timeout, "--settle-timeout", "1", *argv])


def test_status(world, capsys):
    world.add(FakeLock(info=(1, 1, 0, 9, 4)))
    assert run("status") == 0
    out = capsys.readouterr().out
    assert "versions: protocol 1.1, firmware 0.9.4" in out
    assert "status:   LOCKED  (no command since boot, resultSeq 0)" in out


def test_unlock_then_lock(world, capsys):
    p = world.add(FakeLock(t_act=0.1, t_idle=None))
    assert run("unlock") == 0
    out = capsys.readouterr().out
    assert "result:  OK  (state UNLOCKING, resultSeq 1" in out
    assert "settled: UNLOCKED" in out
    assert p.state == UNLOCKED
    assert run("unlock") == 0               # NO_CHANGE counts as success
    assert "result:  NO_CHANGE" in capsys.readouterr().out
    assert run("lock", "--no-settle") == 0
    out = capsys.readouterr().out
    assert "result:  OK  (state LOCKING" in out and "settled:" not in out
    assert p.cmd_writes() == [b"\x01", b"\x01", b"\x00"]


def test_unlock_busy_exits_10(world, capsys):
    world.add(ScriptedLock(status=status_bytes(UNLOCKING, 0x01, OK, 1),
                           on_command=lambda l, d: l.later(0.01, status_bytes(UNLOCKING, 0x01, BUSY, 2))))
    assert run("unlock", "--no-settle") == lc.EXIT_NOT_ACCEPTED == 10
    assert "result:  BUSY" in capsys.readouterr().out


def test_unlock_unknown_result_code_exits_10(world, capsys):
    # §6.2: an unknown result code is treated as failure
    world.add(ScriptedLock(on_command=lambda l, d: l.later(0.01, status_bytes(LOCKED, 0x01, 0x09, 1))))
    assert run("unlock") == 10
    assert "?0x09" in capsys.readouterr().out


@pytest.mark.parametrize("hexdata,want", [("07", "UNKNOWN_COMMAND"), ("01 00", "MALFORMED"),
                                          ("", "MALFORMED")])
def test_send(world, capsys, hexdata, want):
    p = world.add(FakeLock(t_idle=None))
    assert run("send", hexdata) == 0
    assert f"result:  {want}" in capsys.readouterr().out
    assert p.cmd_writes() == [bytes.fromhex(hexdata)]
    assert p.state == LOCKED                 # not executed


def test_watch_until_idle_disconnect(world, capsys):
    world.add(FakeLock(t_idle=0.3))
    assert run("watch") == 0
    out = capsys.readouterr().out
    assert out.startswith("now     LOCKED")
    assert "lock disconnected after" in out


def test_log(world, capsys):
    p = world.add(FakeLock(seq_gap=2))
    assert run("log") == 0
    out = capsys.readouterr().out
    lines = out.splitlines()[:-1]
    # BOOT, BOOT -> LOCKING, LOCKING -> LOCKED, CONNECT (seq gaps of 2); the DISCONNECT
    # is logged after the dump
    assert [l.split()[0] for l in lines] == ["#1", "#4", "#7", "#10"]
    assert "reset=POWERON, boot target LOCKED" in lines[0]
    assert "BOOT -> LOCKING" in lines[1] and "LOCKING -> LOCKED" in lines[2]
    assert "CONNECT" in lines[3]
    assert out.rstrip().endswith("4 entries")
    assert len(p.entries) == 5
    assert run("log", "--from", "1000") == 0
    assert "log is empty" in capsys.readouterr().out


def test_info(world, capsys):
    world.add(FakeLock())
    assert run("info") == 0
    out = capsys.readouterr().out
    assert "versions: protocol 1.1" in out and lc.STATUS_CHAR_UUID in out


def test_scan(world, capsys):
    world.add(FakeLock(rssi=-70))
    world.add(FakeLock(address="AA:BB:CC:DD:EE:02", name=None, rssi=-40,
                       service_uuids=(lc.SERVICE_UUID.upper(),)))
    world.add(ScriptedLock(address="AA:BB:CC:DD:EE:03", name="headphones", service_uuids=()))
    assert lc.main(["--scan-timeout", "0.05", "scan"]) == 0
    lines = [l.strip() for l in capsys.readouterr().out.splitlines() if l.strip()]
    assert lines == ["<no name yet> [AA:BB:CC:DD:EE:02] RSSI -40 dBm",
                     "VT-LOCK-001 [AA:BB:CC:DD:EE:01] RSSI -70 dBm"]


def test_scan_finds_nothing_exits_3(world):
    world.add(ScriptedLock(name="headphones", service_uuids=()))
    assert lc.main(["--scan-timeout", "0.05", "scan"]) == 3


def test_connect_by_name_scan(world, capsys):
    world.add(FakeLock(address="AA:BB:CC:DD:EE:09", name="VT-LOCK-002"))
    world.add(FakeLock())
    assert lc.main(["--scan-timeout", "0.5", "status"]) == 0
    assert world.clients[-1].address == LOCK_ADDRESS


def test_any_connects_to_other_name(world):
    world.add(FakeLock(address="AA:BB:CC:DD:EE:09", name="VT-LOCK-002"))
    assert lc.main(["--any", "--scan-timeout", "0.5", "status"]) == 0
    assert world.clients[-1].address == "AA:BB:CC:DD:EE:09"


# ---- failure exit codes with faked scanner/client ---------------------------------------

def test_not_found_exits_3_and_names_other_locks(world, capsys):
    world.add(FakeLock(address="AA:BB:CC:DD:EE:09", name="VT-LOCK-002"))
    assert lc.main(["--scan-timeout", "0.1", "status"]) == 3
    err = capsys.readouterr().err
    assert "No lock named 'VT-LOCK-001'" in err and "VT-LOCK-002 [AA:BB:CC:DD:EE:09]" in err


def test_not_found_when_lock_busy_with_other_client(world, capsys):
    p = world.add(FakeLock())
    p.client = object()                     # connected elsewhere: not advertising (§2.3)
    assert lc.main(["--scan-timeout", "0.1", "unlock"]) == 3


@pytest.mark.parametrize("argv", [["--scan-timeout", "0.1", "status"], ["--scan-timeout", "0.1", "scan"]])
def test_bluetooth_unavailable_exits_12(world, argv):
    world.bt_error = BleakBluetoothNotAvailableError("radio off", BleakBluetoothNotAvailableReason.POWERED_OFF)
    assert lc.main(argv) == lc.EXIT_BLUETOOTH_UNAVAILABLE == 12


def test_bluetooth_unavailable_on_connect_exits_12(world):
    p = world.add(FakeLock())
    p.connect_error = BleakBluetoothNotAvailableError("radio off", BleakBluetoothNotAvailableReason.POWERED_OFF)
    assert run("status") == 12


def test_usage_error_and_bluetooth_unavailable_codes_differ():
    assert lc.EXIT_BLUETOOTH_UNAVAILABLE != 2       # 2 is argparse's usage-error code
    assert 2 not in lc.EXIT_CODES.values()
    assert "12 Bluetooth unavailable" in lc.__doc__


@pytest.mark.parametrize("argv", [["--scan-timeout", "0.1", "status"], ["--scan-timeout", "0.1", "scan"]])
def test_other_scan_bleak_error_is_lock_error(world, argv, capsys):
    world.bt_error = BleakError("adapter busy")
    assert lc.main(argv) == 4
    assert "Bluetooth scan failed: adapter busy" in capsys.readouterr().err


@pytest.mark.parametrize("err", [BleakError("Could not get GATT services: Unreachable"),
                                 asyncio.TimeoutError()])
def test_connection_failure_exits_4(world, capsys, err):
    p = world.add(FakeLock())
    p.connect_error = err
    assert run("unlock") == 4
    assert "error:" in capsys.readouterr().err


def test_address_not_present_exits_4(world):
    assert run("status") == 4


def test_incompatible_firmware_exits_5(world):
    p = world.add(ScriptedLock(chars=(lc.STATUS_CHAR_UUID,)))
    assert run("status") == 5
    assert p.client is None


@pytest.mark.parametrize("cmd", [["status"], ["unlock"], ["lock"], ["log"], ["watch"]])
def test_v10_firmware_exits_7(world, capsys, cmd):
    world.add(FakeLock(v10=True))
    assert run(*cmd) == 7
    assert "v1.0 firmware" in capsys.readouterr().err


def test_v10_firmware_send_falls_back_to_bare_write(world, capsys):
    p = world.add(FakeLock(v10=True, t_idle=None))
    assert run("send", "01") == 0
    assert "v1.0 firmware: no result available" in capsys.readouterr().out
    assert p.cmd_writes() == [b"\x01"]


def test_v10_firmware_info_works(world, capsys):
    world.add(FakeLock(v10=True))
    assert run("info") == 0
    assert "no info characteristic (v1.0 firmware" in capsys.readouterr().out


def test_write_rejected_exits_6(world):
    p = world.add(ScriptedLock())
    p.write_error = BleakGATTProtocolError(0x0E)
    assert run("unlock") == 6


def test_write_timeout_exits_6(world):
    p = world.add(ScriptedLock())
    p.write_hang = True
    assert run("unlock", timeout="0.2") == 6


def test_protocol_error_exits_8(world):
    world.add(ScriptedLock(status=b"\x00\xff"))
    assert run("status") == 8


def test_result_timeout_exits_9(world, capsys):
    world.add(ScriptedLock(on_command=lambda l, d: None))
    assert run("unlock", timeout="0.2") == 9
    assert "Outcome unknown" in capsys.readouterr().err


def test_settle_timeout_exits_9(world):
    world.add(ScriptedLock(on_command=lambda l, d: l.later(0.01, status_bytes(UNLOCKING, 0x01, OK, 1))))
    assert lc.main(["--address", LOCK_ADDRESS, "--settle-timeout", "0.2", "unlock"]) == 9


def test_cli_always_disconnects_after_error(world):
    p = world.add(ScriptedLock(on_command=lambda l, d: None))
    run("unlock", timeout="0.1")
    assert p.client is None and not world.clients[-1].is_connected

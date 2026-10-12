"""`lock_client.py acceptance` (docs/v1-spec.md §10, TOOL-002) end to end through main(),
against FakeLock (docs/ble-spec.md v1.1) with timing scaled down: T_act 0.1 s,
T_idle 0.8 s (the CLI's --t-act / --idle-timeout), quiet window 0.1 s, log flush 0.03 s."""

import asyncio
import builtins

import pytest

import lock_client as lc
from conftest import T_ACT, T_IDLE, acceptance_argv
from lockfakes import (BUSY, EV_CMD, EV_CONNECT, EV_IDLE, EV_STATE, LOCKED, LOCKING, NONE, UNLOCKED,
                       FakeLock)

AUTO_STEPS = ["2", "3", "4", "5", "6", "9", "10"]


@pytest.fixture
def runs(monkeypatch, fast_acceptance):
    """Records each Acceptance instance (and its loop) so tests can read per-step verdicts."""
    seen = []
    orig = lc.Acceptance.run

    async def run(self):
        seen.append((self, asyncio.get_running_loop()))
        return await orig(self)
    monkeypatch.setattr(lc.Acceptance, "run", run)
    monkeypatch.setattr(lc.Acceptance, "LOG_FLUSH_S", 0.06)
    return seen


def verdicts(runs):
    acc, _ = runs[-1]
    return {num: (verdict, detail) for num, verdict, detail in acc.results}


def failed(runs):
    return {n for n, (v, _) in verdicts(runs).items() if v == "FAIL"}


def lock(**kw):
    kw.setdefault("t_act", T_ACT)
    kw.setdefault("t_idle", T_IDLE)
    kw.setdefault("flush_delay", 0.03)   # durable log: entries readable shortly after the event
    return FakeLock(**kw)


def interactive(monkeypatch, runs, p, *, on_en=None, on_unplug=None):
    """Answer the step 7/8 prompts by acting on the fake lock (in the lock's loop)."""
    prompts = []

    def default_en():
        p.reset(1)                       # ESP32 reports EN as POWERON

    def default_unplug():
        p.power_off()
        p.reset(1)

    def fake_input(text):                # runs in a worker thread (asyncio.to_thread)
        prompts.append(text)
        _, loop = runs[-1]
        if "press EN" in text:
            loop.call_soon_threadsafe(on_en or default_en)
        elif "Unplug" in text:
            loop.call_soon_threadsafe(on_unplug or default_unplug)
        return ""
    monkeypatch.setattr(builtins, "input", fake_input)
    return prompts


# ---- a correct lock passes ------------------------------------------------------------

LOCK_ADDRESS_IF = {"address": "AA:BB:CC:DD:EE:01", "scan": None}


@pytest.mark.parametrize("by", ["address", "scan"])
def test_correct_lock_passes_and_ends_locked(world, runs, capsys, by):
    p = world.add(lock())
    rc = lc.main(acceptance_argv(address=LOCK_ADDRESS_IF[by]))
    out = capsys.readouterr().out
    v = verdicts(runs)
    assert rc == 0, out
    assert {n: v[n][0] for n in AUTO_STEPS} == {n: "PASS" for n in AUTO_STEPS}, out
    assert v["7"][0] == v["8"][0] == "SKIP" and v["1"][0] == "MANUAL"
    assert "ALL AUTOMATIC STEPS PASSED" in out
    assert "cleanup: lock left LOCKED" in out
    assert p.state == LOCKED and p.client is None
    assert p.idle_drops == 1
    # 3 UNLOCK, 4 UNLOCK, 5 07; 6a setup LOCK, UNLOCK, LOCK (queued);
    # 6b UNLOCK, then LOCK UNLOCK LOCK UNLOCK (queued) and LOCK (BUSY); it ends UNLOCKED,
    # so cleanup sends one more LOCK
    assert p.cmd_writes() == [b"\x01", b"\x01", b"\x07", b"\x00", b"\x01", b"\x00",
                              b"\x01", b"\x00", b"\x01", b"\x00", b"\x01", b"\x00", b"\x00"]


def test_passes_with_seq_gaps_and_short_pages(world, runs, capsys):
    # §4.6: gaps are possible; a page shorter than 8 is not the end
    world.add(lock(seq_gap=3, page_size=3))
    assert lc.main(acceptance_argv()) == 0, capsys.readouterr().out
    assert "missing seq numbers" in verdicts(runs)["10"][1]


def test_passes_with_slow_flash(world, runs, capsys):
    # entries become readable up to ~50 ms after the event; the runner waits before reading
    world.add(lock(flush_delay=0.05))
    assert lc.main(acceptance_argv()) == 0, capsys.readouterr().out


def test_step2_waits_out_the_boot_locking(world, runs, capsys, monkeypatch):
    # the lock reboots just before the run: step 2 reads LOCKING, waits, passes with LOCKED
    # t_idle must outlast step 6b's 5 back-to-back moves (queued commands don't
    # reset the idle timer) yet stay under conftest's 1.5 s link-wait cap so
    # step 9 can see the drop: 5 x 0.2 s = 1.0 s < 1.4 s < 1.5 s.
    p = world.add(lock(t_act=0.2, t_idle=1.4))
    orig = lc.Acceptance.step2

    async def step2(self):
        p.reset(1)
        await asyncio.sleep(0.03)
        return await orig(self)
    monkeypatch.setattr(lc.Acceptance, "step2", step2)
    rc = lc.main(acceptance_argv(t_act=0.2, t_idle=1.4))
    out = capsys.readouterr().out
    assert "status:   LOCKING" in out and "settled:  LOCKED" in out
    assert rc == 0, out


def test_interactive_steps_7_and_8(world, runs, capsys, monkeypatch):
    p = world.add(lock())
    prompts = interactive(monkeypatch, runs, p)
    rc = lc.main(acceptance_argv("--interactive"))
    out = capsys.readouterr().out
    assert rc == 0, out
    assert len(prompts) == 2
    v = verdicts(runs)
    assert v["7"][0] == "PASS" and v["8"][0] == "PASS", out
    assert "locked itself" in v["7"][1] and "locked itself" in v["8"][1]
    assert p.state == LOCKED and p.boot_count == 3
    # both reboots happened from UNLOCKED: 6b ends UNLOCKED, step 8 unlocks first
    assert p.cmd_writes()[-1] == b"\x01"
    # step 10 found both boot sequences
    assert v["10"][0] == "PASS", out


def test_interactive_without_stdin_fails_steps_7_8(world, runs, capsys, monkeypatch):
    world.add(lock())

    def no_stdin(text):
        raise EOFError
    monkeypatch.setattr(builtins, "input", no_stdin)
    assert lc.main(acceptance_argv("--interactive")) == 11
    assert {"7", "8"} <= failed(runs)
    assert "no interactive input" in verdicts(runs)["7"][1]


# ---- protocol version ---------------------------------------------------------------------

def test_protocol_major_2_exits_incompatible_without_commands(world, runs, capsys):
    p = world.add(lock(info=(2, 0, 1, 0, 0)))
    assert lc.main(acceptance_argv()) == 5
    assert "protocol 2.0" in capsys.readouterr().err
    assert p.cmd_writes() == [] and p.client is None


def test_protocol_1_0_info_fails_step2_only(world, runs, capsys):
    world.add(lock(info=(1, 0, 1, 0, 0)))
    assert lc.main(acceptance_argv()) == 11
    assert failed(runs) == {"2"}
    assert "expected 1.x with x >= 1" in verdicts(runs)["2"][1]


# ---- injected faults make the right step fail ---------------------------------------------

def test_fault_lock_comes_up_unlocked_fails_step2(world, runs, capsys):
    # old firmware: boot restores the saved target, which was UNLOCKED
    world.add(lock(restore_saved=True, saved=UNLOCKED))
    assert lc.main(acceptance_argv()) == 11
    assert failed(runs) == {"2"}
    assert "expected LOCKED (every boot locks" in verdicts(runs)["2"][1]


def test_fault_no_queue_busy_during_move_fails_step6(world, runs, capsys):
    # old firmware: a command during a move gets BUSY instead of being queued
    world.add(lock(reject_during_move=BUSY))
    assert lc.main(acceptance_argv()) == 11
    assert failed(runs) == {"6"}
    assert "6a: LOCK sent during UNLOCKING gave BUSY, expected OK" in verdicts(runs)["6"][1]


def test_fault_queued_result_before_settle_fails_step6(world, runs, capsys):
    world.add(lock(result_before_settle=True))
    assert lc.main(acceptance_argv()) == 11
    assert "6" in failed(runs)
    assert "6a: notifications" in verdicts(runs)["6"][1]
    assert {"2", "3", "4", "5", "9"} & failed(runs) == set()


@pytest.mark.parametrize("cap,why", [(3, "the 5th command gave"), (5, "the 5th command gave")])
def test_fault_wrong_queue_depth_fails_step6b(world, runs, capsys, cap, why):
    world.add(lock(queue_cap=cap))
    assert lc.main(acceptance_argv()) == 11
    assert failed(runs) == {"6"}
    detail = verdicts(runs)["6"][1]
    assert detail.startswith("6b: ") and why in detail and "BUSY" in detail


def test_fault_queued_result_logged_before_settle_fails_step10(world, runs, capsys):
    # notifications are right, but the log has a queued result ahead of the settle
    world.add(lock(log_queued_early=True))
    assert lc.main(acceptance_argv()) == 11
    assert failed(runs) == {"10"}
    assert "COMMAND_RESULT" in verdicts(runs)["10"][1] or \
        "opcode=" in verdicts(runs)["10"][1]


def test_fault_boot_restores_unlocked_fails_7_8(world, runs, capsys, monkeypatch):
    p = world.add(lock(restore_saved=True))        # starts LOCKED (nothing saved yet)
    interactive(monkeypatch, runs, p)
    assert lc.main(acceptance_argv("--interactive")) == 11
    assert failed(runs) == {"7", "8", "10"}
    assert "expected LOCKING or LOCKED (every boot locks)" in verdicts(runs)["7"][1]
    assert "every boot locks" in verdicts(runs)["8"][1]
    assert "BOOT target=LOCKED" in verdicts(runs)["10"][1]


def test_fault_power_on_restores_unlocked_fails_8(world, runs, capsys, monkeypatch):
    p = world.add(lock())

    def unplug():
        p.power_off()
        p.restore_saved = True                     # only the power-on path is wrong
        p.reset(1)
    interactive(monkeypatch, runs, p, on_unplug=unplug)
    assert lc.main(acceptance_argv("--interactive")) == 11
    assert failed(runs) == {"8", "10"}
    assert verdicts(runs)["7"][0] == "PASS"


def test_fault_en_does_not_reboot_fails_7(world, runs, capsys, monkeypatch):
    p = world.add(lock())
    interactive(monkeypatch, runs, p, on_en=lambda: None)
    assert lc.main(acceptance_argv("--interactive")) == 11
    assert "7" in failed(runs)
    assert "did not reboot" in verdicts(runs)["7"][1]
    assert verdicts(runs)["8"][0] == "PASS"


def test_fault_missing_boot_locking_entry_fails_10(world, runs, capsys, monkeypatch):
    p = world.add(lock(drop_log=lambda t, d: t == EV_STATE and d[0] == NONE))
    interactive(monkeypatch, runs, p)
    assert lc.main(acceptance_argv("--interactive")) == 11
    assert failed(runs) == {"10"}
    assert "BOOT -> LOCKING" in verdicts(runs)["10"][1]


# The timing faults use a longer idle timeout so the slow transitions can't race it.
SLOW_IDLE = 1.0   # < the 1.5 s cap on link waits in conftest.fast_acceptance


def test_fault_settle_too_slow(world, runs, capsys):
    world.add(lock(settle_delay=T_ACT + 0.5, t_idle=SLOW_IDLE))
    assert lc.main(acceptance_argv(t_idle=SLOW_IDLE)) == 11
    assert "3" in failed(runs)
    assert {"2", "4", "5", "9"} & failed(runs) == set()
    assert "UNLOCKING lasted" in verdicts(runs)["3"][1]


def test_fault_settle_too_fast(world, runs, capsys):
    # The runner expects T_act = 0.45 s; the lock settles after 0.05 s
    world.add(lock(t_act=0.05, t_idle=SLOW_IDLE))
    assert lc.main(acceptance_argv(t_act=0.45, t_idle=SLOW_IDLE)) == 11
    assert failed(runs) == {"3"}
    assert "expected T_act = 0.45" in verdicts(runs)["3"][1]


def test_fault_no_idle_disconnect(world, runs, capsys):
    p = world.add(lock(idle_disconnect=False))
    assert lc.main(acceptance_argv()) == 11
    assert failed(runs) == {"9"}
    assert "no idle timeout" in verdicts(runs)["9"][1]
    assert p.client is None and p.state == LOCKED


def test_fault_slow_readvertising(world, runs, capsys):
    # §2.3: advertising resumes within 1 s of a disconnect; this lock takes 1.7 s (runner allows 1.5)
    world.add(lock(readv_delay=1.7))
    assert lc.main(acceptance_argv()) == 11
    assert "9" in failed(runs)
    assert "re-advertising" in verdicts(runs)["9"][1]


@pytest.mark.parametrize("what,drop", [
    ("UNKNOWN_COMMAND result", lambda t, d: t == EV_CMD and d[0] == 0x07),
    ("BUSY result", lambda t, d: t == EV_CMD and d[1] == BUSY),
    ("queued LOCK result", lambda t, d: t == EV_CMD and d[0] == 0x00 and d[1] == 0),
    ("settle before a queued result", lambda t, d: t == EV_STATE and d[0] == LOCKING),
    ("IDLE_TIMEOUT", lambda t, d: t == EV_IDLE),
    ("CONNECT", lambda t, d: t == EV_CONNECT),
])
def test_fault_missing_log_entries(world, runs, capsys, what, drop):
    world.add(lock(drop_log=drop))
    assert lc.main(acceptance_argv()) == 11
    assert failed(runs) == {"10"}, what
    assert "not found" in verdicts(runs)["10"][1]

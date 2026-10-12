"""Shared fixtures. Every test runs with bleak's BleakClient/BleakScanner
replaced inside lock_client: by a guard that fails the test (default), or by
the fake radio of the `world` fixture. No test can reach a real adapter."""

from __future__ import annotations

import os
import sys

import pytest

TOOLS = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

import lock_client as lc  # noqa: E402
import lockfakes  # noqa: E402


class _RealBleForbidden:
    def __init__(self, *a, **kw):
        raise AssertionError("test tried to construct a real bleak object (no real BLE allowed)")

    @classmethod
    async def discover(cls, *a, **kw):
        raise AssertionError("test tried to scan with real bleak")

    @classmethod
    async def find_device_by_filter(cls, *a, **kw):
        raise AssertionError("test tried to scan with real bleak")


@pytest.fixture(autouse=True)
def no_real_ble(monkeypatch):
    import bleak
    monkeypatch.setattr(lc, "BleakClient", _RealBleForbidden)
    monkeypatch.setattr(lc, "BleakScanner", _RealBleForbidden)
    monkeypatch.setattr(bleak, "BleakClient", _RealBleForbidden)
    monkeypatch.setattr(bleak, "BleakScanner", _RealBleForbidden)


@pytest.fixture
def world(no_real_ble, monkeypatch) -> lockfakes.FakeWorld:
    w = lockfakes.FakeWorld()
    monkeypatch.setattr(lc, "BleakClient", w.client_class())
    monkeypatch.setattr(lc, "BleakScanner", w.scanner_class())
    return w


@pytest.fixture
def fast_acceptance(monkeypatch):
    """Shrink the acceptance runner's fixed waits: the 0.5 s quiet window, and the
    `T_idle + 15 s` / 30 s link waits (capped at 1.5 s, which is still well past
    the fakes' 0.8 s idle timeout)."""
    monkeypatch.setattr(lc.Acceptance, "QUIET_S", 0.1)
    orig_wait = lc.LockClient.wait_disconnected
    orig_next = lc.LockClient.next_status

    async def wait_disconnected(self, timeout=None):
        return await orig_wait(self, None if timeout is None else min(timeout, 1.5))

    async def next_status(self, queue, timeout):
        return await orig_next(self, queue, None if timeout is None else min(timeout, 1.5))

    monkeypatch.setattr(lc.LockClient, "wait_disconnected", wait_disconnected)
    monkeypatch.setattr(lc.LockClient, "next_status", next_status)


# Scaled-down timing used by the acceptance tests (the fake lock uses the same).
T_ACT = 0.1
T_IDLE = 0.8


def acceptance_argv(*extra, address: str | None = lockfakes.LOCK_ADDRESS, t_act: float = T_ACT,
                    t_idle: float = T_IDLE) -> list[str]:
    argv = ["--timeout", "1", "--settle-timeout", "2", "--scan-timeout", "2"]
    if address:
        argv += ["--address", address]
    return argv + ["acceptance", "--t-act", str(t_act), "--idle-timeout", str(t_idle), *extra]

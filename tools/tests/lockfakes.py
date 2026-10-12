"""Fake bleak layer and simulated locks for the lock_client tests. No radio.

- FakeWorld: the "air". Holds peripherals and hands out FakeBleakClient /
  FakeBleakScanner classes bound to it (the conftest `world` fixture patches
  them into lock_client in place of bleak's BleakClient / BleakScanner).
- FakePeripheral: base for anything the fake client can connect to.
- FakeLock: follows docs/ble-spec.md v1.1 (status/info/log characteristics,
  resultSeq, settling after T_act, always-lock boot, the 4-deep command queue
  during moves, idle timeout, one connection at a time, a 256-entry ring log
  with cursor paging), with switches to inject specific firmware faults.
- ScriptedLock: a peripheral whose replies a test scripts by hand, for the
  command/result matching edge cases.

Byte layouts are built by hand from lib/lock_protocol/src/lock_protocol.h and
lib/event_log/src/event_log.h, not with lock_client's own encoders.
"""

from __future__ import annotations

import asyncio
import time
from typing import Callable

from bleak.exc import BleakError

import lock_client as lc

CMD = lc.CMD_CHAR_UUID
STATUS = lc.STATUS_CHAR_UUID
INFO = lc.INFO_CHAR_UUID
LOG = lc.LOG_CHAR_UUID
ALL_CHARS = (CMD, STATUS, INFO, LOG)
V10_CHARS = (CMD,)

LOCK_ADDRESS = "AA:BB:CC:DD:EE:01"

# Wire values (lib/lock_core/src/lock_types.h), spelled out so the tests don't
# depend on lock_client's enums being right.
LOCKED, UNLOCKING, UNLOCKED, LOCKING = 0, 1, 2, 3
OK, NO_CHANGE, BUSY, UNKNOWN_COMMAND, MALFORMED = 0, 1, 2, 3, 4
NONE = 0xFF
EV_BOOT, EV_STATE, EV_CMD, EV_CONNECT, EV_DISCONNECT, EV_IDLE = 1, 2, 3, 4, 5, 6


def status_bytes(state: int, op: int = NONE, res: int = NONE, seq: int = 0) -> bytes:
    """lock_protocol.h Status: [0] state [1] lastOpcode [2] lastResult [3] resultSeq."""
    return bytes([state, op, res, seq])


def entry_bytes(seq: int, boot: int, ms: int, etype: int, data=b"") -> bytes:
    """event_log.h entry, 16 bytes LE: [0..3] seq [4..5] bootCount [6..9] msSinceBoot
    [10] type [11..15] data (zero-padded)."""
    data = bytes(data)[:5]
    out = bytearray(16)
    out[0:4] = seq.to_bytes(4, "little")
    out[4:6] = boot.to_bytes(2, "little")
    out[6:10] = ms.to_bytes(4, "little")
    out[10] = etype
    out[11:11 + len(data)] = data
    return bytes(out)


def entry_seq(raw: bytes) -> int:
    return int.from_bytes(raw[0:4], "little")


# ---- bleak stand-ins --------------------------------------------------------

class FakeDevice:
    """Stands in for bleak.backends.device.BLEDevice."""

    def __init__(self, address: str, name: str | None) -> None:
        self.address = address
        self.name = name

    def __str__(self) -> str:
        return f"{self.address}: {self.name}"


class FakeAdv:
    """Stands in for bleak.backends.scanner.AdvertisementData."""

    def __init__(self, local_name, service_uuids, rssi) -> None:
        self.local_name = local_name
        self.service_uuids = list(service_uuids)
        self.rssi = rssi


class FakeChar:
    def __init__(self, uuid: str) -> None:
        self.uuid = uuid
        self.properties = ["read", "write", "notify"]
        self.description = "fake"


class FakeService:
    def __init__(self, uuid: str, chars) -> None:
        self.uuid = uuid
        self.description = "fake service"
        self.characteristics = [FakeChar(u) for u in chars]


class FakeServices:
    def __init__(self, chars) -> None:
        self._chars = [c.lower() for c in chars]

    def get_characteristic(self, uuid: str):
        return FakeChar(uuid) if uuid.lower() in self._chars else None

    def __iter__(self):
        return iter([FakeService(lc.SERVICE_UUID, self._chars)])


class FakeWorld:
    """Everything the fake radio can see, plus the fake bleak classes bound to it."""

    def __init__(self) -> None:
        self.peripherals: list[FakePeripheral] = []
        self.scanners: list = []
        self.bt_error: Exception | None = None   # raised by every scan
        self.clients: list[FakeBleakClient] = []

    def add(self, p: "FakePeripheral") -> "FakePeripheral":
        p.world = self
        self.peripherals.append(p)
        return p

    def by_address(self, address: str):
        return next((p for p in self.peripherals if p.address.upper() == address.upper()), None)

    def advertised(self, p: "FakePeripheral") -> None:
        """Report one advertisement of `p` to every running detection-callback scanner."""
        if not p.advertising:
            return
        for s in list(self.scanners):
            if s.cb is not None:
                s.cb(p.device(), p.adv())

    def client_class(self):
        world = self

        class BoundClient(FakeBleakClient):
            def __init__(self, address_or_device, disconnected_callback=None, timeout=10.0, **kw):
                super().__init__(world, address_or_device, disconnected_callback, timeout, **kw)
        return BoundClient

    def scanner_class(self):
        world = self

        class BoundScanner(FakeBleakScanner):
            pass
        BoundScanner.world = world
        return BoundScanner


class FakeBleakClient:
    def __init__(self, world: FakeWorld, address_or_device, disconnected_callback, timeout, **kw):
        self.world = world
        self.address = address_or_device if isinstance(address_or_device, str) \
            else address_or_device.address
        self.disc_cb = disconnected_callback
        self.timeout = timeout
        self.kwargs = kw
        self._connected = False
        self.notify_cbs: dict[str, Callable] = {}
        self.conn_id = 0
        self.p: FakePeripheral | None = None
        world.clients.append(self)

    @property
    def is_connected(self) -> bool:
        return self._connected

    @property
    def services(self) -> FakeServices:
        return FakeServices(self.p.chars if self.p else ())

    async def connect(self, **_kw) -> bool:
        await asyncio.sleep(0)
        p = self.world.by_address(self.address)
        if p is None:
            raise BleakError(f"Device with address {self.address} was not found.")
        if p.connect_error is not None:
            raise p.connect_error
        if p.connect_hang:
            await asyncio.sleep(3600)
        if not p.powered or p.client is not None:
            raise BleakError(f"Device with address {self.address} was not found.")
        self.p = p
        self._connected = True
        self.notify_cbs = {}
        p.client = self
        p.on_connect(self)
        return True

    async def disconnect(self) -> bool:
        if self._connected:
            p = self.p
            p.on_disconnect(self, 0x13)
            self._link_down()
        return True

    def _link_down(self) -> None:
        """The link is gone, whoever ended it."""
        if not self._connected:
            return
        self._connected = False
        if self.p is not None and self.p.client is self:
            self.p.client = None
        self.notify_cbs = {}
        if self.disc_cb is not None:
            asyncio.get_running_loop().call_soon(self.disc_cb, self)

    def _check(self, uuid: str) -> None:
        if not self._connected:
            raise BleakError("Not connected")
        if uuid.lower() not in [c.lower() for c in self.p.chars]:
            raise BleakError(f"Characteristic {uuid} was not found!")

    async def write_gatt_char(self, uuid, data, response=None) -> None:
        self._check(uuid)
        assert response is True, "lock characteristics are write-with-response only (spec 4.1)"
        self.p.writes.append((uuid, bytes(data)))
        await self.p.on_write(uuid, bytes(data))

    async def read_gatt_char(self, uuid, **_kw) -> bytearray:
        self._check(uuid)
        self.p.reads.append(uuid)
        return bytearray(await self.p.on_read(uuid))

    async def start_notify(self, uuid, callback, **_kw) -> None:
        self._check(uuid)
        self.p.subscribes.append(uuid)
        self.notify_cbs[uuid] = callback

    async def stop_notify(self, uuid) -> None:
        self.notify_cbs.pop(uuid, None)


class FakeBleakScanner:
    world: FakeWorld = None  # set on the bound subclass

    def __init__(self, detection_callback=None, **_kw) -> None:
        self.cb = detection_callback

    async def __aenter__(self):
        if self.world.bt_error is not None:
            raise self.world.bt_error
        self.world.scanners.append(self)
        for p in self.world.peripherals:
            if p.advertising and self.cb is not None:
                asyncio.get_running_loop().call_soon(self.cb, p.device(), p.adv())
        return self

    async def __aexit__(self, *exc) -> None:
        self.world.scanners.remove(self)

    @classmethod
    async def discover(cls, timeout: float = 5.0, return_adv: bool = False, **_kw):
        if cls.world.bt_error is not None:
            raise cls.world.bt_error
        await asyncio.sleep(min(timeout, 0.01))
        found = {p.address: (p.device(), p.adv()) for p in cls.world.peripherals if p.advertising}
        return found if return_adv else [d for d, _ in found.values()]

    @classmethod
    async def find_device_by_filter(cls, filterfunc, timeout: float = 10.0, **_kw):
        if cls.world.bt_error is not None:
            raise cls.world.bt_error
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while True:
            for p in cls.world.peripherals:
                if p.advertising:
                    dev = p.device()
                    if filterfunc(dev, p.adv()):
                        return dev
            if loop.time() >= deadline:
                return None
            await asyncio.sleep(0.01)


# ---- peripherals --------------------------------------------------------------

class FakePeripheral:
    def __init__(self, *, address: str = LOCK_ADDRESS, name: str | None = lc.DEFAULT_NAME,
                 rssi: int = -50, chars=ALL_CHARS, service_uuids=(lc.SERVICE_UUID,)) -> None:
        self.address = address
        self.name = name
        self.rssi = rssi
        self.chars = tuple(chars)
        self.service_uuids = tuple(service_uuids)
        self.world: FakeWorld | None = None
        self.client: FakeBleakClient | None = None
        self.powered = True
        self.connect_error: Exception | None = None
        self.connect_hang = False
        self.writes: list[tuple[str, bytes]] = []
        self.reads: list[str] = []
        self.subscribes: list[str] = []
        self.notified: list[bytes] = []

    @property
    def advertising(self) -> bool:
        return self.powered and self.client is None

    def device(self) -> FakeDevice:
        return FakeDevice(self.address, self.name)

    def adv(self) -> FakeAdv:
        return FakeAdv(self.name, self.service_uuids, self.rssi)

    def cmd_writes(self) -> list[bytes]:
        return [d for u, d in self.writes if u == CMD]

    # hooks
    def on_connect(self, client: FakeBleakClient) -> None:
        pass

    def on_disconnect(self, client: FakeBleakClient, reason: int) -> None:
        pass

    async def on_write(self, uuid: str, data: bytes) -> None:
        await asyncio.sleep(0)

    async def on_read(self, uuid: str) -> bytes:
        raise BleakError(f"read of {uuid} not supported")

    # peripheral -> central
    def notify(self, data: bytes, *, now: bool = False) -> None:
        """Send a status notification (delivered on the next loop turn, as bleak does)."""
        c = self.client
        if c is None or STATUS not in c.notify_cbs:
            return
        self.notified.append(bytes(data))
        cb = c.notify_cbs[STATUS]
        if now:
            cb(FakeChar(STATUS), bytearray(data))
        else:
            asyncio.get_running_loop().call_soon(cb, FakeChar(STATUS), bytearray(data))

    def drop_link(self) -> None:
        """The peripheral ends the connection."""
        if self.client is not None:
            self.client._link_down()


class ScriptedLock(FakePeripheral):
    """Replies exactly as the test scripts. `on_command(lock, data)` runs on
    every command write (it may notify, change `status`, raise, or sleep)."""

    def __init__(self, *, status: bytes = status_bytes(LOCKED), info: bytes = bytes([1, 1, 0, 9, 0]),
                 on_command=None, log_reader=None, **kw) -> None:
        super().__init__(**kw)
        self.status = status
        self.info = info
        self.on_command = on_command
        self.log_reader = log_reader     # fn(cursor) -> bytes
        self.cursor = 0
        self.cursor_writes: list[int] = []
        self.write_error: Exception | None = None
        self.write_hang = False
        self.read_error: Exception | None = None
        self.read_hang = False

    def later(self, delay: float, data: bytes, *, new_status: bool = True) -> None:
        def fire():
            if new_status:
                self.status = data
            self.notify(data, now=True)
        asyncio.get_running_loop().call_later(delay, fire)

    async def on_write(self, uuid: str, data: bytes) -> None:
        if self.write_hang:
            await asyncio.sleep(3600)
        if self.write_error is not None:
            if getattr(self, "write_error_drops_link", False):
                self.drop_link()
            raise self.write_error
        if uuid == CMD and self.on_command is not None:
            r = self.on_command(self, data)
            if asyncio.iscoroutine(r):
                await r
        elif uuid == LOG and len(data) == 4:
            self.cursor = int.from_bytes(data, "little")
            self.cursor_writes.append(self.cursor)
        await asyncio.sleep(0)

    async def on_read(self, uuid: str) -> bytes:
        if self.read_hang:
            await asyncio.sleep(3600)
        if self.read_error is not None:
            raise self.read_error
        if uuid == STATUS:
            return self.status
        if uuid == INFO:
            return self.info
        if uuid == LOG:
            return self.log_reader(self.cursor)
        raise BleakError("unknown characteristic")


class FakeLock(FakePeripheral):
    """A lock that follows docs/ble-spec.md v1.1, with fault switches.

    Behavior: every boot logs BOOT [reason, LOCKED, 0] and STATE_CHANGE FF -> LOCKING,
    then settles to LOCKED after t_act. A LOCK/UNLOCK written during a move is
    queued (up to queue_cap, silently); on settle: the settle notification and its
    log entry, then each queued command's result (notification, COMMAND_RESULT,
    and a STATE_CHANGE if it starts a move); an OK move makes the rest wait again.
    A further valid command with the queue full gets BUSY at once. Malformed and
    unknown commands always get immediate results. The log is a ring of
    log_capacity entries, each readable flush_delay seconds after it is written.

    Faults (all off by default):
      reject_during_move   old firmware: no queue, answer a move-time command with this code
      queue_cap            queue depth (spec: 4)
      restore_saved        old firmware: boot drives to the last commanded target
      saved                last commanded target at construction (with restore_saved)
      result_before_settle queued results are notified and logged before the settle
      log_queued_early     queued results are logged (not notified) before the settle
      settle_delay         actual actuation time (default t_act)
      idle_disconnect      False: never drop an idle client
      readv_delay          seconds between a disconnect and advertising again
      drop_log(type, data) True: that event is never written to the log
      seq_gap              seq numbers skipped after every entry (spec allows gaps)
      page_size            max entries per log page (spec: 8)
    """

    def __init__(self, *, t_act: float = 0.15, t_idle: float | None = 0.8,
                 info=(1, 1, 0, 9, 0), v10: bool = False, saved: int | None = None,
                 reject_during_move: int | None = None, queue_cap: int = 4,
                 restore_saved: bool = False, result_before_settle: bool = False,
                 log_queued_early: bool = False,
                 settle_delay: float | None = None, idle_disconnect: bool = True,
                 readv_delay: float = 0.05, drop_log=None, seq_gap: int = 0,
                 page_size: int = 8, log_capacity: int = 256, flush_delay: float = 0.0,
                 first_seq: int = 1, **kw) -> None:
        super().__init__(chars=V10_CHARS if v10 else ALL_CHARS, **kw)
        self.t_act = t_act
        self.t_idle = t_idle
        self.info = bytes(info)
        self.saved = saved
        self.reject_during_move = reject_during_move
        self.queue_cap = queue_cap
        self.restore_saved = restore_saved
        self.result_before_settle = result_before_settle
        self.log_queued_early = log_queued_early
        self.settle_delay = settle_delay
        self.idle_disconnect = idle_disconnect
        self.readv_delay = readv_delay
        self.drop_log = drop_log or (lambda t, d: False)
        self.seq_gap = seq_gap
        self.page_size = page_size
        self.log_capacity = log_capacity
        self.flush_delay = flush_delay
        self.log_ring: list[tuple[float, bytes]] = []   # (readable from perf_counter, entry)
        self.next_seq = first_seq
        self.boot_count = 0
        self.conn_id = 0
        self.cursor = 0
        self.pending: list[int] = []
        self.timers: list[asyncio.TimerHandle] = []
        self.idle_handle: asyncio.TimerHandle | None = None
        self.readv_at = 0.0
        self.idle_drops = 0
        self._boot(reason=1, settle_now=True)

    # -- internals --

    @property
    def entries(self) -> list[bytes]:
        """Every entry in the ring, saved to flash or not."""
        return [e for _, e in self.log_ring]

    def ms(self) -> int:
        return int((time.perf_counter() - self.t_boot) * 1000)

    def log(self, etype: int, data) -> None:
        if self.drop_log(etype, bytes(data)):
            return
        raw = entry_bytes(self.next_seq, self.boot_count, self.ms(), etype, data)
        self.log_ring.append((time.perf_counter() + self.flush_delay, raw))
        del self.log_ring[:-self.log_capacity]
        self.next_seq += 1 + self.seq_gap

    def status(self) -> bytes:
        return status_bytes(self.state, self.last_op, self.last_res, self.seq)

    @property
    def moving(self) -> bool:
        return self.state in (UNLOCKING, LOCKING)

    @property
    def advertising(self) -> bool:
        return super().advertising and time.perf_counter() >= self.readv_at

    def _boot(self, reason: int, settle_now: bool) -> None:
        for h in self.timers:
            h.cancel()
        self.timers = []
        self.pending = []
        self.boot_count += 1
        self.t_boot = time.perf_counter()
        self.powered = True
        target = self.saved if self.restore_saved and self.saved is not None else LOCKED
        self.log(EV_BOOT, [reason, target, 0])
        self.last_op = self.last_res = NONE
        self.seq = 0
        self.state = UNLOCKING if target == UNLOCKED else LOCKING
        self.log(EV_STATE, [NONE, self.state])
        if settle_now:   # constructed outside an event loop: the boot move is already over
            self._settle()
        else:
            self._schedule_settle()

    def _schedule_settle(self) -> None:
        delay = self.t_act if self.settle_delay is None else self.settle_delay
        self.timers.append(asyncio.get_running_loop().call_later(delay, self._settle))

    def _execute(self, op: int, res: int, length: int, notes: list, logs: list) -> None:
        """Decide one command's result; collect its notification and log entries."""
        before = self.state
        if res == OK:
            self.saved = UNLOCKED if op == 0x01 else LOCKED
            self.state = UNLOCKING if op == 0x01 else LOCKING
            self._schedule_settle()
        self.last_op, self.last_res, self.seq = op, res, (self.seq + 1) & 0xFF
        notes.append(self.status())
        logs.append((EV_CMD, [op, res, min(length, 255)]))   # firmware order: result, then move
        if res == OK:
            logs.append((EV_STATE, [before, self.state]))

    @staticmethod
    def _evaluate(op: int, state: int) -> int:
        return NO_CHANGE if (op == 0x01) == (state == UNLOCKED) else OK

    def _emit(self, notes: list, logs: list) -> None:
        for n in notes:
            self.notify(n)
        for etype, data in logs:
            self.log(etype, data)

    def _settle(self) -> None:
        if not self.powered or not self.moving:
            return
        frm = self.state
        self.state = UNLOCKED if frm == UNLOCKING else LOCKED
        settle_note, settle_log = [self.status()], [(EV_STATE, [frm, self.state])]
        q_notes: list = []
        q_logs: list = []
        while self.pending:
            op = self.pending.pop(0)
            self._execute(op, self._evaluate(op, self.state), 1, q_notes, q_logs)
            if self.moving:
                break                     # the rest wait for this move
        if self.result_before_settle:
            self._emit(q_notes + settle_note, q_logs + settle_log)
        elif self.log_queued_early:
            self._emit(settle_note + q_notes, q_logs + settle_log)
        else:
            self._emit(settle_note + q_notes, settle_log + q_logs)

    def _command(self, data: bytes) -> None:
        op = data[0] if data else NONE
        valid = len(data) == 1 and op in (0x00, 0x01)
        if valid and self.moving:
            if self.reject_during_move is not None:
                res = self.reject_during_move
            elif len(self.pending) < self.queue_cap:
                self.pending.append(op)       # no notification, no log entry yet
                return
            else:
                res = BUSY
        elif len(data) != 1:
            res = MALFORMED
        elif not valid:
            res = UNKNOWN_COMMAND
        else:
            res = self._evaluate(op, self.state)
        notes: list = []
        logs: list = []
        self._execute(op, res, len(data), notes, logs)
        self._emit(notes, logs)

    def _reset_idle(self) -> None:
        if self.idle_handle is not None:
            self.idle_handle.cancel()
            self.idle_handle = None
        if self.client is not None and self.t_idle is not None and self.idle_disconnect:
            self.idle_handle = asyncio.get_running_loop().call_later(self.t_idle, self._idle)

    def _idle(self) -> None:
        c = self.client
        if c is None:
            return
        self.idle_drops += 1
        self.log(EV_IDLE, [c.conn_id & 0xFF, c.conn_id >> 8])
        self._went_away(c, 0x16)
        self.drop_link()

    def _went_away(self, c: FakeBleakClient, reason: int) -> None:
        if self.idle_handle is not None:
            self.idle_handle.cancel()
            self.idle_handle = None
        self.log(EV_DISCONNECT, [c.conn_id & 0xFF, c.conn_id >> 8, reason, 0])
        self.readv_at = time.perf_counter() + self.readv_delay
        loop = asyncio.get_running_loop()
        loop.call_later(self.readv_delay + 0.001, lambda: self.world and self.world.advertised(self))

    # -- hooks --

    def on_connect(self, client: FakeBleakClient) -> None:
        self.conn_id += 1
        client.conn_id = self.conn_id
        self.cursor = 0                     # cursor resets on every connection
        self.log(EV_CONNECT, [self.conn_id & 0xFF, self.conn_id >> 8])
        self._reset_idle()

    def on_disconnect(self, client: FakeBleakClient, reason: int) -> None:
        self._went_away(client, reason)

    async def on_write(self, uuid: str, data: bytes) -> None:
        await asyncio.sleep(0)
        if uuid == CMD:
            self._command(data)
            self._reset_idle()              # only command writes count as activity
        elif uuid == LOG and len(data) == 4:
            self.cursor = int.from_bytes(data, "little")

    async def on_read(self, uuid: str) -> bytes:
        await asyncio.sleep(0)
        if uuid == STATUS:
            return self.status()
        if uuid == INFO:
            return self.info
        if uuid == LOG:
            now = time.perf_counter()
            saved = [e for t, e in self.log_ring if t <= now]
            sel = [e for e in saved if entry_seq(e) >= self.cursor][:self.page_size]
            return bytes([len(sel)]) + b"".join(sel)
        raise BleakError("unknown characteristic")

    # -- for interactive steps --

    def power_off(self) -> None:
        self.powered = False
        for h in self.timers:
            h.cancel()
        self.timers = []
        self.pending = []
        if self.idle_handle is not None:
            self.idle_handle.cancel()
            self.idle_handle = None
        self.drop_link()   # power loss: nothing gets logged

    def reset(self, reason: int = 1) -> None:
        """EN pressed / power back: reboot, drive to LOCKED (ESP32 reports EN as POWERON)."""
        if self.client is not None:
            self.drop_link()
        self._boot(reason, settle_now=False)
        asyncio.get_running_loop().call_later(0.02, lambda: self.world and self.world.advertised(self))

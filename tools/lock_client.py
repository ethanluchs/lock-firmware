"""Scriptable BLE test client for the VT bike lock (see docs/ble-spec.md v1.1).

Command line:

    python tools/lock_client.py scan             # list nearby locks
    python tools/lock_client.py status           # read state, last result and versions
    python tools/lock_client.py unlock           # send 0x01, print the result, wait for the settled state
    python tools/lock_client.py lock             # send 0x00, likewise
    python tools/lock_client.py lock --no-settle # print the result only
    python tools/lock_client.py send 07          # write raw hex bytes, print the result (test bad input)
    python tools/lock_client.py send "01 00"     #   -> MALFORMED (length != 1)
    python tools/lock_client.py watch            # stream status notifications (Ctrl+C or idle disconnect ends it)
    python tools/lock_client.py log              # dump the whole event log, decoded
    python tools/lock_client.py log --from 120   # entries with seq >= 120
    python tools/lock_client.py info             # versions and the lock's GATT table (works on v1.0 too)
    python tools/lock_client.py acceptance       # docs/v1-spec.md §10, automatic steps (~1 min)
    python tools/lock_client.py acceptance --interactive   # also steps 7-8 (press EN, unplug USB)
    python tools/lock_client.py -v unlock        # also show debug logging

    Options (before the subcommand): --name NAME, --any, --address ADDR (skip
    scanning), --scan-timeout S, --timeout S (per GATT operation and per command
    result; plus the settle timeout if the command is queued behind a move),
    --settle-timeout S (wait for UNLOCKING/LOCKING to finish).

    Exit codes: 0 ok, 1 other error, 2 usage error (argparse), 3 lock not found,
    4 connection, 5 incompatible firmware (protocol major != 1), 6 write failed,
    7 firmware too old (v1.0), 8 protocol violation, 9 timeout, 10 lock/unlock
    not accepted, 11 acceptance failed, 12 Bluetooth unavailable, 130 Ctrl+C.

As a library:

    import asyncio, logging
    from lock_client import connect_lock, CmdResult, LockState

    async def main():
        logging.basicConfig(level=logging.INFO, format="%(message)s")
        async with connect_lock() as lock:          # scan + connect; disconnects on exit
            info = await lock.read_info()           # protocol 1.1, firmware x.y.z
            print(info, await lock.read_status())
            out = await lock.unlock()               # waits for the result and the settled state
            if out.result is CmdResult.OK:
                print("unlocked after", out.settled_after_s, "s")
            out = await lock.command(0x07)          # any bytes; result matched by resultSeq
            assert out.result is CmdResult.UNKNOWN_COMMAND
            async for status in lock.watch_status():  # until the lock disconnects us
                print(status)
            for entry in await lock.read_log():
                print(entry)

    asyncio.run(main())

v1.1 notes: every command produces exactly one status notification carrying a
new resultSeq; settling produces one more with the same resultSeq. A LOCK or
UNLOCK written while the lock is moving (including the LOCKING every boot
starts with) is queued, up to 4 deep: its result notification comes after the
move's settle notification, so command() waits longer for a result when the
lock was moving. BUSY means the queue was full. The lock drops a client that
sends no command for 30 s, so keep scripts moving or reconnect. It accepts one
connection at a time. Against v1.0 firmware (no status characteristic)
everything that needs v1.1 raises FirmwareTooOldError; `send` falls back to a
bare write. Commands that rely on 1.x semantics refuse a lock whose protocol
major version is not 1 (IncompatibleFirmwareError); `info` and `send` work
with any version.

Every failure raises a subclass of LockError, so a script can catch exactly the
cases it cares about. The CLI maps each one to a distinct exit code (EXIT_CODES).
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import struct
import sys
import time
from contextlib import asynccontextmanager
from dataclasses import dataclass, field
from enum import IntEnum
from typing import AsyncIterator, Callable

from bleak import BleakClient, BleakScanner
from bleak.backends.characteristic import BleakGATTCharacteristic
from bleak.backends.device import BLEDevice
from bleak.backends.scanner import AdvertisementData
from bleak.exc import BleakBluetoothNotAvailableError, BleakError, BleakGATTProtocolError

# ---- protocol constants (docs/ble-spec.md §1; lib/lock_protocol/src/lock_protocol.h) ----

SERVICE_UUID = "8f2a1c00-5b7e-4d19-9a3f-6c2e81d4b70a"
CMD_CHAR_UUID = "8f2a1c01-5b7e-4d19-9a3f-6c2e81d4b70a"
STATUS_CHAR_UUID = "8f2a1c02-5b7e-4d19-9a3f-6c2e81d4b70a"   # v1.1: READ + NOTIFY
INFO_CHAR_UUID = "8f2a1c03-5b7e-4d19-9a3f-6c2e81d4b70a"     # v1.1: READ
LOG_CHAR_UUID = "8f2a1c04-5b7e-4d19-9a3f-6c2e81d4b70a"      # v1.1: WRITE cursor + READ pages
DEFAULT_NAME = "VT-LOCK-001"

# Phase 1 opcodes. PROVISIONAL: Phase 2 removes unauthenticated lock/unlock.
CMD_LOCK = 0x00
CMD_UNLOCK = 0x01
CMD_NAMES = {CMD_LOCK: "LOCK", CMD_UNLOCK: "UNLOCK"}
NONE = 0xFF  # "no value" in lastOpcode / lastResult / log opcode fields

STATUS_LEN = 4
INFO_LEN = 5
LOG_ENTRY_LEN = 16

DEFAULT_SCAN_TIMEOUT = 10.0
DEFAULT_CONNECT_TIMEOUT = 15.0  # Windows connects are often 2-5 s; leave headroom
DEFAULT_OP_TIMEOUT = 5.0        # per GATT operation, and write -> result notification
DEFAULT_SETTLE_TIMEOUT = 10.0   # UNLOCKING/LOCKING -> settled (T_act is 1 s for the LED)
T_IDLE = 30.0                   # lock drops a client that sends no command for this long
T_ACT_LED = 1.0
QUEUE_DEPTH = 4                 # LOCK/UNLOCK commands the lock queues behind a move
PROTOCOL_MAJOR = 1              # the protocol major version this client speaks
LOG_SEQ_MAX = 0x7FFFFFFF        # log seqs are 1..LOG_SEQ_MAX
BOOT_FROM = 0xFF                # STATE_CHANGE "from" value of the boot move (no prior state)

log = logging.getLogger("lock_client")


# ---- decoded types ---------------------------------------------------------
# Values mirror lib/lock_core/src/lock_types.h and lib/event_log/src/event_log.h.
# Decoders keep unknown values as plain ints (a newer firmware may add some).

class LockState(IntEnum):
    LOCKED = 0
    UNLOCKING = 1
    UNLOCKED = 2
    LOCKING = 3


class CmdResult(IntEnum):
    OK = 0
    NO_CHANGE = 1
    BUSY = 2
    UNKNOWN_COMMAND = 3
    MALFORMED = 4


class EventType(IntEnum):
    BOOT = 1
    STATE_CHANGE = 2
    COMMAND_RESULT = 3
    CONNECT = 4
    DISCONNECT = 5
    IDLE_TIMEOUT = 6


SETTLED_STATES = (LockState.LOCKED, LockState.UNLOCKED)

# esp_reset_reason_t (ESP-IDF), for decoding BOOT entries.
RESET_REASONS = {
    0: "UNKNOWN", 1: "POWERON", 2: "EXT", 3: "SW", 4: "PANIC", 5: "INT_WDT",
    6: "TASK_WDT", 7: "WDT", 8: "DEEPSLEEP", 9: "BROWNOUT", 10: "SDIO", 11: "USB",
    12: "JTAG", 13: "EFUSE", 14: "PWR_GLITCH", 15: "CPU_LOCKUP",
}
# Common HCI disconnect reasons, for decoding DISCONNECT entries.
HCI_REASONS = {
    0x08: "supervision timeout", 0x13: "remote user terminated",
    0x16: "terminated by lock", 0x3E: "failed to establish",
}


def _coerce(enum_cls, value: int):
    try:
        return enum_cls(value)
    except ValueError:
        return value


def _name(value) -> str:
    return value.name if isinstance(value, IntEnum) else f"?0x{value:02X}"


def _from_name(value: int) -> str:
    """STATE_CHANGE "from": FF is the boot move (no prior state)."""
    return "BOOT" if value == BOOT_FROM else _name(_coerce(LockState, value))


def opcode_name(op: int) -> str:
    if op == NONE:
        return "NONE"
    return f"{CMD_NAMES[op]}" if op in CMD_NAMES else f"0x{op:02X}"


def result_name(result: CmdResult | int | None) -> str:
    return "NONE" if result is None else _name(result)


@dataclass(frozen=True)
class Status:
    """Status characteristic (4 bytes; longer payloads are accepted, extra bytes ignored)."""
    state: LockState | int
    last_opcode: int                    # NONE (0xFF): no command since boot, or an empty write
    last_result: CmdResult | int | None  # None: no command since boot
    result_seq: int                     # +1 (mod 256) per command result since boot; 0 before the first
    raw: bytes = field(default=b"", compare=False)
    received: float = field(default=0.0, compare=False, repr=False)  # time.perf_counter()

    @property
    def settled(self) -> bool:
        return self.state in SETTLED_STATES

    def __str__(self) -> str:
        if self.last_result is None:
            last = "no command since boot"
        else:
            last = f"last {opcode_name(self.last_opcode)} -> {result_name(self.last_result)}"
        return f"{_name(self.state)}  ({last}, resultSeq {self.result_seq})"


@dataclass(frozen=True)
class Info:
    """Info characteristic (5 bytes; longer payloads are accepted, extra bytes ignored)."""
    protocol_major: int
    protocol_minor: int
    fw_major: int
    fw_minor: int
    fw_patch: int
    raw: bytes = field(default=b"", compare=False)

    @property
    def protocol(self) -> str:
        return f"{self.protocol_major}.{self.protocol_minor}"

    @property
    def firmware(self) -> str:
        return f"{self.fw_major}.{self.fw_minor}.{self.fw_patch}"

    def __str__(self) -> str:
        return f"protocol {self.protocol}, firmware {self.firmware}"


@dataclass(frozen=True)
class LogEntry:
    """One 16-byte event log entry (layout in lib/event_log/src/event_log.h)."""
    seq: int
    boot_count: int
    ms_since_boot: int
    type: EventType | int
    data: bytes  # 5 bytes, meaning per type; see describe()

    def describe(self) -> str:
        d, t = self.data, self.type
        if t is EventType.BOOT:
            reason = RESET_REASONS.get(d[0], f"?{d[0]}")
            return f"reset={reason}, boot target {_name(_coerce(LockState, d[1]))}"
        if t is EventType.STATE_CHANGE:
            return f"{_from_name(d[0])} -> {_name(_coerce(LockState, d[1]))}"
        if t is EventType.COMMAND_RESULT:
            res = None if d[1] == NONE else _coerce(CmdResult, d[1])
            return f"opcode={opcode_name(d[0])} result={result_name(res)} len={d[2]}"
        conn = d[0] | d[1] << 8
        if t is EventType.DISCONNECT:
            reason = d[2] | d[3] << 8
            return f"conn={conn} reason=0x{reason:02X} ({HCI_REASONS.get(reason, '?')})"
        if t in (EventType.CONNECT, EventType.IDLE_TIMEOUT):
            return f"conn={conn}"
        return f"data={d.hex(' ')}"

    def __str__(self) -> str:
        return (f"#{self.seq:<6} boot {self.boot_count:<4} +{self.ms_since_boot:>9} ms  "
                f"{_name(self.type):<14} {self.describe()}")


def decode_status(data: bytes, received: float = 0.0) -> Status:
    if len(data) < STATUS_LEN:
        raise LockProtocolError(f"status payload is {len(data)} bytes, expected >= {STATUS_LEN}: "
                                f"{data.hex(' ') or '<empty>'}")
    state, op, res, seq = data[:STATUS_LEN]
    return Status(_coerce(LockState, state), op,
                  None if res == NONE else _coerce(CmdResult, res), seq,
                  bytes(data), received)


def decode_info(data: bytes) -> Info:
    if len(data) < INFO_LEN:
        raise LockProtocolError(f"info payload is {len(data)} bytes, expected >= {INFO_LEN}: "
                                f"{data.hex(' ') or '<empty>'}")
    return Info(*data[:INFO_LEN], raw=bytes(data))


def decode_log_entry(data: bytes) -> LogEntry:
    if len(data) != LOG_ENTRY_LEN:
        raise LockProtocolError(f"log entry is {len(data)} bytes, expected {LOG_ENTRY_LEN}")
    seq, boot, ms, etype = struct.unpack_from("<IHIB", data)
    if seq == 0 or seq > LOG_SEQ_MAX:
        raise LockProtocolError(f"log entry has seq {seq}: valid seqs are 1..0x{LOG_SEQ_MAX:X}")
    return LogEntry(seq, boot, ms, _coerce(EventType, etype), bytes(data[11:16]))


def decode_log_page(data: bytes) -> list[LogEntry]:
    """Page = [count][count x 16-byte entries]; bytes after the last entry are ignored."""
    if not data:
        raise LockProtocolError("log page is empty (expected at least the count byte)")
    count = data[0]
    need = 1 + count * LOG_ENTRY_LEN
    if len(data) < need:
        raise LockProtocolError(f"log page says {count} entries ({need} bytes) but is "
                                f"{len(data)} bytes")
    return [decode_log_entry(data[1 + i * LOG_ENTRY_LEN: 1 + (i + 1) * LOG_ENTRY_LEN])
            for i in range(count)]


def encode_log_cursor(seq: int) -> bytes:
    return struct.pack("<I", seq)


# ---- errors -----------------------------------------------------------------

class LockError(Exception):
    """Base class for everything this module raises."""


class BluetoothUnavailableError(LockError):
    """The laptop's Bluetooth is off, missing, or blocked."""


class LockNotFoundError(LockError):
    """Scan finished without seeing a matching lock."""


class LockConnectionError(LockError):
    """Could not connect, or the link dropped mid-operation."""


class IncompatibleFirmwareError(LockError):
    """Connected, but the expected service/characteristic is not there."""


class FirmwareTooOldError(IncompatibleFirmwareError):
    """The lock runs an older protocol (e.g. v1.0: no status characteristic)."""


class LockWriteError(LockError):
    """A write was rejected, timed out, or its outcome is unknown."""


class LockProtocolError(LockError):
    """The lock sent something that contradicts docs/ble-spec.md."""


class LockTimeoutError(LockError):
    """An expected notification (command result, settled state) never came.
    For a command result, the outcome is unknown."""


# ---- discovery --------------------------------------------------------------

@dataclass(frozen=True)
class FoundLock:
    device: BLEDevice
    name: str | None
    rssi: int

    @property
    def address(self) -> str:
        return self.device.address

    def __str__(self) -> str:
        return f"{self.name or '<no name yet>'} [{self.address}] RSSI {self.rssi} dBm"


def _advertises_lock_service(adv: AdvertisementData) -> bool:
    return SERVICE_UUID in (u.lower() for u in adv.service_uuids)


async def scan(timeout: float = 5.0) -> list[FoundLock]:
    """Return every advertiser exposing the lock service, strongest signal first.

    Filters on the service UUID rather than the name: the UUID is in the primary
    advertising packet, while the name only arrives in the scan response.
    """
    try:
        found = await BleakScanner.discover(timeout=timeout, return_adv=True)
    except BleakBluetoothNotAvailableError as e:
        raise BluetoothUnavailableError(f"Bluetooth is not available: {e}") from e
    except BleakError as e:
        raise LockConnectionError(f"Bluetooth scan failed: {e}") from e
    locks = [
        FoundLock(dev, adv.local_name or dev.name, adv.rssi)
        for dev, adv in found.values()
        if _advertises_lock_service(adv)
    ]
    return sorted(locks, key=lambda f: f.rssi, reverse=True)


async def find_lock(name: str | None = DEFAULT_NAME,
                    timeout: float = DEFAULT_SCAN_TIMEOUT) -> FoundLock:
    """Scan until a lock matching `name` is seen (name=None: any lock).

    Returns as soon as it matches instead of waiting out the full timeout.
    """
    others: dict[str, str] = {}  # locks seen with a different name, for the error message
    last_adv: dict[str, AdvertisementData] = {}  # find_device_by_filter drops the adv; keep RSSI

    def matches(dev: BLEDevice, adv: AdvertisementData) -> bool:
        last_adv[dev.address] = adv
        if not _advertises_lock_service(adv):
            return False
        seen_name = adv.local_name or dev.name
        if name is None or seen_name == name:
            return True
        if seen_name:
            others[dev.address] = seen_name
        return False

    log.info("[scan]  looking for %s (up to %.0f s)...",
             repr(name) if name else "any lock", timeout)
    try:
        dev = await BleakScanner.find_device_by_filter(matches, timeout=timeout)
    except BleakBluetoothNotAvailableError as e:
        raise BluetoothUnavailableError(f"Bluetooth is not available: {e}") from e
    except BleakError as e:
        raise LockConnectionError(f"Bluetooth scan failed: {e}") from e

    if dev is None:
        hint = (
            "Check: board powered and running the lock firmware; within range; "
            "no phone currently connected to it (the lock accepts one connection at a "
            "time and stops advertising while connected); Windows Bluetooth is on."
        )
        if others:
            seen = ", ".join(f"{n} [{a}]" for a, n in others.items())
            hint = f"Saw lock(s) with other names: {seen}. Use --name. " + hint
        raise LockNotFoundError(f"No lock named {name!r} found in {timeout:.0f} s. {hint}")

    adv = last_adv[dev.address]
    found = FoundLock(dev, adv.local_name or dev.name, adv.rssi)
    log.info("[scan]  found %s", found)
    return found


# ---- connection -------------------------------------------------------------

@dataclass(frozen=True)
class WriteResult:
    data: bytes
    ack_ms: float  # time from issuing the write to the lock's ATT write response


@dataclass(frozen=True)
class CommandOutcome:
    """What one command produced (docs/ble-spec.md §6)."""
    data: bytes                  # bytes written to the command characteristic
    ack_ms: float                # write issued -> ATT write response
    result_status: Status        # the notification carrying this command's resultSeq
    result_ms: float             # write issued -> result notification
    settled: Status | None       # first settled status after the result, if awaited
    notifications: tuple[Status, ...]  # every status notification seen, in order

    @property
    def result(self) -> CmdResult | int | None:
        return self.result_status.last_result

    @property
    def settled_after_s(self) -> float | None:
        """Result notification -> settled notification (approximately T_act after OK)."""
        if self.settled is None:
            return None
        return self.settled.received - self.result_status.received


class LockClient:
    """One BLE connection to one lock.

    Wraps BleakClient with lock-specific checks, timeouts and errors. Prefer
    `async with LockClient(...)` or `connect_lock()` so the link is always
    released. A v1.1 lock drops a client after 30 s without a command, but
    while connected it hides from every other client.
    """

    def __init__(self, target: FoundLock | BLEDevice | str, *,
                 connect_timeout: float = DEFAULT_CONNECT_TIMEOUT,
                 op_timeout: float = DEFAULT_OP_TIMEOUT,
                 settle_timeout: float = DEFAULT_SETTLE_TIMEOUT) -> None:
        device = target.device if isinstance(target, FoundLock) else target
        self._label = str(target) if isinstance(target, FoundLock) else str(device)
        self.address = device if isinstance(device, str) else device.address
        self._op_timeout = op_timeout
        self._settle_timeout = settle_timeout
        self._expect_disconnect = False
        self._disconnected = asyncio.Event()
        self._status_listeners: list[asyncio.Queue[Status | LockError]] = []
        self._status_notifying = False
        self.connected_at: float | None = None     # perf_counter when connect() returned
        self.disconnected_at: float | None = None  # perf_counter when the link went down
        self.last_status: Status | None = None
        self._client = BleakClient(
            device,
            disconnected_callback=self._on_disconnect,
            timeout=connect_timeout,
            # Windows caches each device's GATT table. v1.1 added
            # characteristics, and a stale cache would make them "not found".
            winrt={"use_cached_services": False},
        )

    # -- lifecycle --

    async def connect(self) -> None:
        log.info("[conn]  connecting to %s...", self._label)
        self._disconnected.clear()
        self._expect_disconnect = False
        self._status_notifying = False
        self.disconnected_at = None
        t0 = time.perf_counter()
        try:
            await self._client.connect()
        except asyncio.TimeoutError as e:
            raise LockConnectionError(
                f"Timed out connecting to {self._label}. The lock may have gone out "
                "of range or been connected to by another device.") from e
        except BleakBluetoothNotAvailableError as e:
            raise BluetoothUnavailableError(f"Bluetooth is not available: {e}") from e
        except BleakError as e:
            raise LockConnectionError(f"Could not connect to {self._label}: {e}") from e

        if not self.has_char(CMD_CHAR_UUID):
            await self.disconnect()
            raise IncompatibleFirmwareError(
                f"Connected, but command characteristic {CMD_CHAR_UUID} is missing. "
                "Is this board running the lock firmware?")
        self.connected_at = time.perf_counter()
        log.info("[conn]  connected in %.2f s%s", self.connected_at - t0,
                 "" if self.has_char(STATUS_CHAR_UUID)
                 else " (no status characteristic: v1.0 firmware)")

    async def disconnect(self) -> None:
        if self._client.is_connected:
            self._expect_disconnect = True
            try:
                await self._client.disconnect()
            except BleakError as e:
                if self._client.is_connected:
                    raise LockConnectionError(f"Disconnect failed: {e}") from e
                log.debug("[conn]  disconnect raised after the link was gone: %s", e)
            log.info("[conn]  disconnected")

    async def __aenter__(self) -> LockClient:
        await self.connect()
        return self

    async def __aexit__(self, exc_type, *_exc) -> None:
        try:
            await self.disconnect()
        except LockError:
            if exc_type is None:
                raise
            # don't mask the error that ended the block

    @property
    def is_connected(self) -> bool:
        return self._client.is_connected

    def _on_disconnect(self, _client: BleakClient) -> None:
        self.disconnected_at = time.perf_counter()
        self._disconnected.set()
        if not self._expect_disconnect:
            log.warning("[conn]  the lock closed the connection (or the link was lost)")

    async def wait_disconnected(self, timeout: float | None = None) -> float:
        """Wait until the link drops; return perf_counter time it dropped."""
        try:
            await asyncio.wait_for(self._disconnected.wait(), timeout)
        except asyncio.TimeoutError as e:
            raise LockTimeoutError(f"Still connected after {timeout:.0f} s.") from e
        assert self.disconnected_at is not None
        return self.disconnected_at

    def _require_connected(self) -> None:
        if not self._client.is_connected:
            raise LockConnectionError("Not connected to the lock.")

    def has_char(self, char_uuid: str) -> bool:
        return self._client.services.get_characteristic(char_uuid) is not None

    def _require_char(self, char_uuid: str, what: str) -> None:
        self._require_connected()
        if self.has_char(char_uuid):
            return
        if not self.has_char(STATUS_CHAR_UUID):
            raise FirmwareTooOldError(
                f"{what} needs BLE protocol 1.1, but this lock has no status characteristic "
                f"({STATUS_CHAR_UUID}): it is running v1.0 firmware. Only bare writes work "
                "(`send 01`, `send 00`); flash v1.1 firmware for results, status and the log.")
        raise FirmwareTooOldError(
            f"{what}: characteristic {char_uuid} is missing on this lock "
            "(firmware built without it?).")

    # -- generic GATT operations --

    async def write(self, char_uuid: str, data: bytes) -> WriteResult:
        """Write with response and wait for the lock's acknowledgement.

        An ack means the lock *received* the bytes, not that it acted on them
        (docs/ble-spec.md §6.1). Use command() to get the result.
        """
        self._require_connected()
        t0 = time.perf_counter()
        try:
            await asyncio.wait_for(
                self._client.write_gatt_char(char_uuid, data, response=True),
                self._op_timeout)
        except asyncio.TimeoutError as e:
            raise LockWriteError(
                f"No write response within {self._op_timeout:.0f} s; outcome unknown.") from e
        except BleakGATTProtocolError as e:
            raise LockWriteError(f"Lock rejected the write: {e}") from e
        except BleakError as e:
            if not self._client.is_connected:
                raise LockConnectionError(
                    f"Link dropped during write; outcome unknown: {e}") from e
            raise LockWriteError(f"Write failed: {e}") from e
        return WriteResult(bytes(data), (time.perf_counter() - t0) * 1000)

    async def read(self, char_uuid: str) -> bytes:
        self._require_connected()
        try:
            return bytes(await asyncio.wait_for(
                self._client.read_gatt_char(char_uuid), self._op_timeout))
        except asyncio.TimeoutError as e:
            raise LockConnectionError(f"Read of {char_uuid} timed out.") from e
        except BleakError as e:
            raise LockConnectionError(f"Read of {char_uuid} failed: {e}") from e

    async def subscribe(self, char_uuid: str) -> asyncio.Queue[bytes]:
        """Enable notifications; each notification is put on the returned queue.

        Raw building block. For the status characteristic use
        status_listener() / watch_status() instead: a characteristic can only
        have one subscription per connection.
        """
        self._require_connected()
        queue: asyncio.Queue[bytes] = asyncio.Queue()

        def on_notify(_char: BleakGATTCharacteristic, value: bytearray) -> None:
            queue.put_nowait(bytes(value))

        try:
            await self._client.start_notify(char_uuid, on_notify)
        except BleakError as e:
            raise LockConnectionError(f"Could not subscribe to {char_uuid}: {e}") from e
        return queue

    def describe_gatt(self) -> str:
        lines = []
        for service in self._client.services:
            lines.append(f"service {service.uuid}  ({service.description})")
            for char in service.characteristics:
                props = ",".join(char.properties)
                lines.append(f"  char  {char.uuid}  [{props}]  ({char.description})")
        return "\n".join(lines)

    # -- v1.1: status, info --

    async def read_status(self) -> Status:
        self._require_char(STATUS_CHAR_UUID, "Reading the status")
        status = decode_status(await self.read(STATUS_CHAR_UUID), time.perf_counter())
        self.last_status = status
        return status

    async def read_info(self) -> Info:
        self._require_char(INFO_CHAR_UUID, "Reading the versions")
        return decode_info(await self.read(INFO_CHAR_UUID))

    async def check_protocol(self) -> Info | None:
        """Raise IncompatibleFirmwareError unless the lock speaks protocol 1.x.
        Returns the versions, or None for v1.0 firmware (no info characteristic;
        protocol 1.0, so the 1.1 features raise FirmwareTooOldError when used)."""
        if not self.has_char(INFO_CHAR_UUID):
            return None
        info = await self.read_info()
        if info.protocol_major != PROTOCOL_MAJOR:
            raise IncompatibleFirmwareError(
                f"The lock speaks BLE protocol {info.protocol} (firmware {info.firmware}); this "
                f"client only understands {PROTOCOL_MAJOR}.x. Use `info` or `send`, or update "
                "lock_client.py.")
        return info

    def _on_status_notify(self, _char: BleakGATTCharacteristic, value: bytearray) -> None:
        try:
            item: Status | LockError = decode_status(bytes(value), time.perf_counter())
            self.last_status = item
            log.debug("[notify] %s", item)
        except LockProtocolError as e:
            item = e
            log.warning("[notify] bad status notification: %s", e)
        for q in list(self._status_listeners):
            q.put_nowait(item)

    async def enable_status_notifications(self) -> None:
        """Subscribe to the status characteristic (idempotent per connection).
        The lock sends nothing on subscribe; read_status() for the current value."""
        self._require_char(STATUS_CHAR_UUID, "Status notifications")
        if self._status_notifying:
            return
        try:
            await asyncio.wait_for(
                self._client.start_notify(STATUS_CHAR_UUID, self._on_status_notify),
                self._op_timeout)
        except (BleakError, asyncio.TimeoutError) as e:
            raise LockConnectionError(f"Could not subscribe to status notifications: {e}") from e
        self._status_notifying = True

    @asynccontextmanager
    async def status_listener(self) -> AsyncIterator[asyncio.Queue[Status | LockError]]:
        """Queue receiving every status notification while the block runs.
        Subscribes first if needed. Items are Status (or LockProtocolError for a
        malformed notification)."""
        await self.enable_status_notifications()
        queue: asyncio.Queue[Status | LockError] = asyncio.Queue()
        self._status_listeners.append(queue)
        try:
            yield queue
        finally:
            self._status_listeners.remove(queue)

    async def next_status(self, queue: asyncio.Queue[Status | LockError],
                          timeout: float | None) -> Status | None:
        """Next notification from a status_listener() queue; None if the link drops
        first. Raises LockTimeoutError after `timeout` seconds (None: no limit)."""
        get = asyncio.ensure_future(queue.get())
        disc = asyncio.ensure_future(self._disconnected.wait())
        try:
            done, _ = await asyncio.wait({get, disc}, timeout=timeout,
                                         return_when=asyncio.FIRST_COMPLETED)
        finally:
            for t in (get, disc):
                if not t.done():
                    t.cancel()
        if get in done:
            item = get.result()
            if isinstance(item, LockError):
                raise item
            return item
        if disc in done:
            return None
        raise LockTimeoutError(f"No status notification within {timeout:.1f} s.")

    async def _wait_status(self, queue: asyncio.Queue[Status | LockError],
                           pred: Callable[[Status], bool], timeout: float,
                           what: str, seen: list[Status]) -> Status:
        deadline = time.perf_counter() + timeout
        while True:
            remaining = deadline - time.perf_counter()
            if remaining <= 0:
                raise LockTimeoutError(f"No {what} within {timeout:.1f} s.")
            try:
                status = await self.next_status(queue, remaining)
            except LockTimeoutError:
                raise LockTimeoutError(f"No {what} within {timeout:.1f} s.") from None
            if status is None:
                raise LockConnectionError(f"Lock disconnected while waiting for {what}.")
            seen.append(status)
            if pred(status):
                return status

    async def watch_status(self) -> AsyncIterator[Status]:
        """Yield every status notification until the lock disconnects."""
        async with self.status_listener() as q:
            while True:
                status = await self.next_status(q, None)
                if status is None:
                    return
                yield status

    async def wait_settled(self, timeout: float | None = None) -> Status:
        """Return the status once the lock is LOCKED or UNLOCKED (now, or after
        the next settling notification)."""
        timeout = self._settle_timeout if timeout is None else timeout
        async with self.status_listener() as q:
            status = await self.read_status()
            if status.settled:
                return status
            return await self._wait_status(q, lambda s: s.settled, timeout,
                                           f"settled state (now {_name(status.state)})", [])

    # -- v1.1: commands with results --

    async def command(self, data: bytes | int, *, settle: bool = False,
                      timeout: float | None = None, settle_timeout: float | None = None,
                      baseline_seq: int | None = None) -> CommandOutcome:
        """Write a command and wait for *its* result notification.

        The result is the first notification whose resultSeq is one more
        (mod 256) than before the write; settle notifications of an earlier
        move (same resultSeq as before) are skipped. `baseline_seq` skips the
        status read that establishes "before" (e.g. when the previous result is
        known). With settle=True, also wait until the state is LOCKED or UNLOCKED.
        `data` may be any bytes (an int is one opcode byte), to test bad input.

        If the lock was moving at the write (last known state UNLOCKING or
        LOCKING), a LOCK/UNLOCK is queued and its result only comes after the
        settle, so the result wait is `timeout` + `settle_timeout`.
        """
        data = bytes([data]) if isinstance(data, int) else bytes(data)
        timeout = self._op_timeout if timeout is None else timeout
        settle_timeout = self._settle_timeout if settle_timeout is None else settle_timeout
        seen: list[Status] = []
        async with self.status_listener() as q:
            if baseline_seq is None:
                baseline_seq = (await self.read_status()).result_seq
            expected = (baseline_seq + 1) & 0xFF
            moving = self.last_status is not None and not self.last_status.settled
            result_timeout = timeout + (settle_timeout if moving else 0.0)

            def is_result(s: Status) -> bool:
                if s.result_seq == expected:
                    return True
                if s.result_seq != baseline_seq:
                    raise LockProtocolError(
                        f"resultSeq jumped from {baseline_seq} to {s.result_seq} "
                        f"(expected {expected}): a command result was missed or another "
                        "command was processed.")
                return False  # e.g. a late settling notification of an earlier command

            t0 = time.perf_counter()
            ack = await self.write(CMD_CHAR_UUID, data)
            try:
                result = await self._wait_status(
                    q, is_result, result_timeout,
                    f"result notification (resultSeq {expected}) for {data.hex(' ') or '<empty>'}",
                    seen)
            except LockTimeoutError as e:
                raise LockTimeoutError(f"{e} Outcome unknown. {await self._status_hint(expected)}") from None
            result_ms = (result.received - t0) * 1000
            want_op = data[0] if data else NONE
            # (FF, BUSY): the firmware's internal command queue overflowed and it no
            # longer knows the opcode. A legitimate result for this command.
            overflow = result.last_opcode == NONE and result.last_result is CmdResult.BUSY
            if result.last_opcode != want_op and not overflow:
                raise LockProtocolError(
                    f"result notification (resultSeq {expected}) reports opcode "
                    f"{opcode_name(result.last_opcode)}, but {opcode_name(want_op)} was sent.")
            log.info("[cmd]   %s -> %s, state %s (%.0f ms)", data.hex(" ") or "<empty>",
                     result_name(result.last_result), _name(result.state), result_ms)

            settled = None
            if settle:
                settled = result if result.settled else await self._wait_status(
                    q, lambda s: s.settled, settle_timeout,
                    f"settled state after {_name(result.state)}", seen)
        return CommandOutcome(data, ack.ack_ms, result, result_ms, settled, tuple(seen))

    async def _status_hint(self, expected_seq: int) -> str:
        try:
            now = await self.read_status()
        except LockError as e:
            return f"(Status read also failed: {e})"
        if now.result_seq == expected_seq:
            return (f"Status now reads {now}: the lock processed the command but did not "
                    "notify it (firmware bug: every command must be notified).")
        return f"Status now reads {now}."

    async def unlock(self, *, settle: bool = True) -> CommandOutcome:
        return await self.command(CMD_UNLOCK, settle=settle)

    async def lock(self, *, settle: bool = True) -> CommandOutcome:
        return await self.command(CMD_LOCK, settle=settle)

    async def send_command(self, opcode: int) -> WriteResult:
        """v1.0-style bare write: no result. Works against v1.0 firmware."""
        if not 0 <= opcode <= 0xFF:
            raise ValueError(f"opcode must be one byte, got {opcode}")
        result = await self.write(CMD_CHAR_UUID, bytes([opcode]))
        log.info("[write] 0x%02X (%s) acknowledged by lock in %.0f ms",
                 opcode, CMD_NAMES.get(opcode, "not a lock command"), result.ack_ms)
        return result

    # -- v1.1: event log --

    async def iter_log(self, start: int = 0) -> AsyncIterator[list[LogEntry]]:
        """Yield the log page by page (oldest first), from seq >= start.
        Reads don't count as activity: a long dump must finish within the idle
        timeout (256 entries is ~32 round trips, a few seconds)."""
        self._require_char(LOG_CHAR_UUID, "Reading the event log")
        cursor = start
        for _ in range(1 << 16):  # cannot loop forever even if the lock misbehaves
            await self.write(LOG_CHAR_UUID, encode_log_cursor(cursor))
            page = decode_log_page(await self.read(LOG_CHAR_UUID))
            if not page:
                return
            seqs = [e.seq for e in page]
            if seqs[0] < cursor or any(b <= a for a, b in zip(seqs, seqs[1:])):
                raise LockProtocolError(
                    f"log page for cursor {cursor} has seqs {seqs}: expected increasing, >= cursor")
            yield page
            cursor = seqs[-1] + 1
        raise LockProtocolError("event log paging did not terminate")

    async def read_log(self, start: int = 0) -> list[LogEntry]:
        entries: list[LogEntry] = []
        async for page in self.iter_log(start):
            entries.extend(page)
        return entries


@asynccontextmanager
async def connect_lock(name: str | None = DEFAULT_NAME, *, address: str | None = None,
                       scan_timeout: float = DEFAULT_SCAN_TIMEOUT,
                       **client_kwargs) -> AsyncIterator[LockClient]:
    """Find a lock (by name, or directly by address), connect, and always disconnect."""
    target: FoundLock | str = address if address else await find_lock(name, scan_timeout)
    async with LockClient(target, **client_kwargs) as client:
        yield client


# ---- acceptance sequence (docs/v1-spec.md §10, TOOL-002) ---------------------

class StepFailed(Exception):
    pass


# Event types the runner can predict exactly: every one in the run's log window must be expected.
_CORE_EVENTS = (EventType.BOOT, EventType.STATE_CHANGE, EventType.COMMAND_RESULT)


@dataclass(frozen=True)
class ExpectedEvent:
    """A log entry the acceptance run expects, derived from what it observed over BLE.
    `data` is a prefix of the entry's data bytes; None matches any byte."""
    type: EventType
    data: tuple[int | None, ...] = ()

    def matches(self, e: LogEntry) -> bool:
        return e.type is self.type and all(
            want is None or want == got for want, got in zip(self.data, e.data))

    def __str__(self) -> str:
        if self.type is EventType.STATE_CHANGE:
            return f"STATE_CHANGE {_from_name(self.data[0])} -> {_name(_coerce(LockState, self.data[1]))}"
        if self.type is EventType.COMMAND_RESULT:
            res = _coerce(CmdResult, self.data[1]) if self.data[1] != NONE else None
            return f"COMMAND_RESULT {opcode_name(self.data[0])} -> {result_name(res)}"
        if self.type is EventType.BOOT:
            return f"BOOT target={_name(_coerce(LockState, self.data[1]))}"
        return self.type.name


def _state_event(frm: int, to: int) -> ExpectedEvent:
    return ExpectedEvent(EventType.STATE_CHANGE, (int(frm), int(to)))


def _transition(result_status: Status) -> tuple[LockState, LockState]:
    """(from, to) of the state change an OK command started."""
    if result_status.state == LockState.UNLOCKING:
        return LockState.LOCKED, LockState.UNLOCKING
    return LockState.UNLOCKED, LockState.LOCKING


def _settle_target(s: LockState) -> LockState:
    return {LockState.UNLOCKING: LockState.UNLOCKED, LockState.LOCKING: LockState.LOCKED}.get(s, s)


def _st(state: LockState, op: int, res: CmdResult, seq: int) -> Status:
    """A Status to compare notifications against (equality ignores raw bytes and time)."""
    return Status(state, op, res, seq & 0xFF)


class Acceptance:
    """Runs docs/v1-spec.md §10. Each step prints PASS/FAIL/SKIP; run() returns
    True if nothing failed. Expected log events are derived from what the lock
    reported over BLE, so step 10 cross-checks the log against the notifications."""

    QUIET_S = 0.5      # after the last expected notification, nothing more may arrive
    LOG_FLUSH_S = 0.3  # log entries are readable once saved to flash (~100 ms)

    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.t_act: float = args.t_act
        self.t_idle: float = args.idle_timeout
        self.lock: LockClient | None = None
        self.target: FoundLock | str | None = args.address
        self.results: list[tuple[str, str, str]] = []
        self.expected: list[list[ExpectedEvent]] = []  # ordered groups; any order within a group
        self.log_start: int | None = None
        self.incompatible: IncompatibleFirmwareError | None = None

    # -- helpers --

    def expect(self, *group: ExpectedEvent) -> None:
        self.expected.append(list(group))

    async def connect(self, *, expect_log: bool = True, scan_timeout: float | None = None) -> LockClient:
        if self.lock is not None and self.lock.is_connected:
            return self.lock
        self.lock = None  # dropped link (if any): the lock logs it, but it is not an expected event
        if not isinstance(self.target, str):  # rescan each time: a BLEDevice handle can go stale
            self.target = await find_lock(self.args.name, scan_timeout or self.args.scan_timeout)
        lock = LockClient(self.target, op_timeout=self.args.timeout,
                          settle_timeout=self.args.settle_timeout)
        await lock.connect()
        if expect_log:
            self.expect(ExpectedEvent(EventType.CONNECT))
        self.lock = lock
        return lock

    async def disconnect(self, *, expect_log: bool = True) -> None:
        if self.lock is not None and self.lock.is_connected:
            await self.lock.disconnect()
            if expect_log:
                self.expect(ExpectedEvent(EventType.DISCONNECT))
        self.lock = None

    def record(self, data: bytes, out: CommandOutcome) -> None:
        """Expected log events for a command, from its observed outcome."""
        op = data[0] if data else NONE
        res = out.result_status.last_result
        cmd = ExpectedEvent(EventType.COMMAND_RESULT, (op, NONE if res is None else int(res)))
        if out.result is CmdResult.OK:
            self.expect(cmd, _state_event(*_transition(out.result_status)))
        else:
            self.expect(cmd)
        if out.settled is not None and out.settled is not out.result_status:
            self.record_settle(out.result_status.state)

    def record_settle(self, transitional: LockState | int) -> None:
        self.expect(_state_event(transitional, _settle_target(transitional)))

    def record_trace(self, before: Status, seen: list[Status]) -> None:
        """Expected log events for a run of notifications, in the order they arrived:
        a new resultSeq is a command result (plus its move if OK); a settled state
        with the same resultSeq is a settle."""
        prev = before
        for s in seen:
            if s.result_seq != prev.result_seq:
                res = NONE if s.last_result is None else int(s.last_result)
                cmd = ExpectedEvent(EventType.COMMAND_RESULT, (s.last_opcode, res))
                if s.last_result is CmdResult.OK:
                    self.expect(cmd, _state_event(*_transition(s)))
                else:
                    self.expect(cmd)
            elif s.state != prev.state and s.settled:
                self.record_settle(prev.state)
            prev = s

    async def cmd(self, data: bytes, *, settle: bool, quiet: float) -> tuple[CommandOutcome, list[Status]]:
        """Run one command; return its outcome and every notification seen until
        `quiet` seconds after the outcome (to check nothing extra arrives)."""
        lock = await self.connect()
        async with lock.status_listener() as q:
            out = await lock.command(data, settle=settle)
            self.record(data, out)
            await asyncio.sleep(quiet)
            seen = self.drain(q)
        return out, seen

    @staticmethod
    def drain(q: asyncio.Queue[Status | LockError]) -> list[Status]:
        seen = [q.get_nowait() for _ in range(q.qsize())]
        for s in seen:
            if isinstance(s, LockError):
                raise s
        return seen

    async def collect(self, lock: LockClient, q: asyncio.Queue[Status | LockError], n: int,
                      per_item_timeout: float, *, keep_on_disconnect: bool = False) -> list[Status]:
        """Up to `n` notifications (stops early on a timeout), then whatever else
        arrives within the quiet window. With keep_on_disconnect, a dropped link
        ends collection and returns what arrived, so the caller can still check
        it and record it for step 10."""
        seen: list[Status] = []
        while len(seen) < n:
            try:
                s = await lock.next_status(q, per_item_timeout)
            except LockTimeoutError:
                break
            if s is None:
                if keep_on_disconnect:
                    return seen
                raise LockConnectionError("Lock disconnected while waiting for notifications.")
            seen.append(s)
        await asyncio.sleep(self.QUIET_S)
        return seen + self.drain(q)

    async def ensure_state(self, want: LockState) -> None:
        """Setup between steps: bring the lock to `want` (not itself a test)."""
        lock = await self.connect()
        status = await lock.wait_settled()
        if status.state == want:
            return
        out, _ = await self.cmd(bytes([CMD_UNLOCK if want == LockState.UNLOCKED else CMD_LOCK]),
                                settle=True, quiet=0)
        if out.settled is None or out.settled.state != want:
            raise StepFailed(f"setup: could not bring the lock to {want.name} "
                             f"(result {result_name(out.result)})")
        print(f"      setup: lock is now {want.name}")

    @staticmethod
    def check(cond: bool, msg: str) -> None:
        if not cond:
            raise StepFailed(msg)

    def check_exact(self, seen: list[Status], want: list[Status], what: str) -> None:
        self.check(seen == want, f"{what}: expected exactly {len(want)} notification(s) "
                   f"{[str(s) for s in want]}, got {[str(s) for s in seen]}")

    async def prompt(self, text: str) -> None:
        try:
            await asyncio.to_thread(input, f"\n  >>> {text}\n  >>> Press Enter to continue: ")
        except EOFError:
            raise StepFailed("no interactive input available (stdin closed)") from None

    # -- steps --

    async def step2(self) -> str:
        lock = await self.connect(expect_log=False)
        info = await lock.read_info()
        if info.protocol_major != PROTOCOL_MAJOR:
            self.incompatible = IncompatibleFirmwareError(
                f"The lock speaks BLE protocol {info.protocol}; this acceptance run needs "
                f"{PROTOCOL_MAJOR}.x. Stopping without sending any command.")
            raise self.incompatible
        status = await lock.read_status()
        print(f"      versions: {info}\n      status:   {status}")
        if not status.settled:  # e.g. still in the boot LOCKING
            status = await lock.wait_settled()
            print(f"      settled:  {status}")
        await asyncio.sleep(self.LOG_FLUSH_S)  # let recent entries (e.g. the settle) reach flash
        try:
            entries = await lock.read_log()
            self.log_start = entries[-1].seq + 1 if entries else 0
            print(f"      log: {len(entries)} existing entries; this run's events start at seq "
                  f">= {self.log_start}")
        except LockError as e:
            print(f"      log: unavailable ({e}); step 10 will fail")
        self.check(info.protocol_minor >= 1, f"protocol {info.protocol}, expected 1.x with x >= 1")
        if status.state != LockState.LOCKED:
            await self.ensure_state(LockState.LOCKED)
            raise StepFailed(f"state was {_name(status.state)}, expected LOCKED (every boot locks; "
                             "reboot the lock before the run if it was used since). "
                             "Locked it so the remaining steps can run.")
        return f"LOCKED; {info}"

    async def step3(self) -> str:
        out, seen = await self.cmd(bytes([CMD_UNLOCK]), settle=True, quiet=self.QUIET_S)
        r, s = out.result_status, out.settled
        self.check(out.result is CmdResult.OK, f"result {result_name(out.result)}, expected OK")
        self.check(r.state == LockState.UNLOCKING,
                   f"result notification state {_name(r.state)}, expected UNLOCKING")
        self.check(s is not None and s.state == LockState.UNLOCKED and s.result_seq == r.result_seq,
                   f"settled notification {s}, expected UNLOCKED with resultSeq {r.result_seq}")
        self.check_exact(seen, [r, s], "notifications")
        dt = out.settled_after_s
        self.check(abs(dt - self.t_act) <= 0.3,
                   f"UNLOCKING lasted {dt:.2f} s, expected T_act = {self.t_act:.2f} s +/- 0.3")
        return f"OK -> UNLOCKING -> UNLOCKED after {dt:.2f} s (check: LED blinked, now on)"

    async def step4(self) -> str:
        out, seen = await self.cmd(bytes([CMD_UNLOCK]), settle=False, quiet=self.t_act + self.QUIET_S)
        r = out.result_status
        self.check(out.result is CmdResult.NO_CHANGE, f"result {result_name(out.result)}, expected NO_CHANGE")
        self.check(r.state == LockState.UNLOCKED, f"state {_name(r.state)}, expected UNLOCKED")
        self.check_exact(seen, [r], "notifications (nothing may move)")
        return "NO_CHANGE, still UNLOCKED, no further notifications"

    async def step5(self) -> str:
        out, seen = await self.cmd(bytes([0x07]), settle=False, quiet=self.t_act + self.QUIET_S)
        r = out.result_status
        self.check(out.result is CmdResult.UNKNOWN_COMMAND,
                   f"result {result_name(out.result)}, expected UNKNOWN_COMMAND")
        self.check(r.state == LockState.UNLOCKED, f"state {_name(r.state)}, expected UNLOCKED (unchanged)")
        self.check_exact(seen, [r], "notifications")
        return "UNKNOWN_COMMAND (lastOpcode 0x07), state unchanged"

    async def step6(self) -> str:
        a = await self.step6a()
        b = await self.step6b()
        return f"{a}; {b}"

    async def step6a(self) -> str:
        """Lock, wait until LOCKED; Unlock, then Lock immediately: LOCK is queued and
        runs after the UNLOCKED settle."""
        await self.ensure_state(LockState.LOCKED)
        lock = await self.connect()
        async with lock.status_listener() as q:
            before = await lock.read_status()
            s0 = before.result_seq
            a = await lock.command(bytes([CMD_UNLOCK]))
            # Immediately, without waiting for the settle; baseline from the UNLOCK result.
            b = await lock.command(bytes([CMD_LOCK]), baseline_seq=a.result_status.result_seq,
                                   settle=True)
            await asyncio.sleep(self.QUIET_S)
            seen = self.drain(q)
        self.record_trace(before, seen)
        self.check(a.result is CmdResult.OK and a.result_status.state == LockState.UNLOCKING,
                   f"6a: UNLOCK gave {a.result_status}, expected OK / UNLOCKING")
        self.check(b.result is CmdResult.OK,
                   f"6a: LOCK sent during UNLOCKING gave {result_name(b.result)}, expected OK "
                   "after the UNLOCKED settle (queued)")
        t_write = b.result_status.received - b.result_ms / 1000
        unlocked = next((s for s in seen if s.state == LockState.UNLOCKED), None)
        self.check(unlocked is not None, "6a: no UNLOCKED settle notification")
        self.check(unlocked.received > t_write,
                   "6a: LOCK was written after UNLOCKING had already settled, so nothing was "
                   "queued; the laptop is too slow for this T_act")
        self.check_exact(seen, [
            _st(LockState.UNLOCKING, CMD_UNLOCK, CmdResult.OK, s0 + 1),
            _st(LockState.UNLOCKED, CMD_UNLOCK, CmdResult.OK, s0 + 1),
            _st(LockState.LOCKING, CMD_LOCK, CmdResult.OK, s0 + 2),
            _st(LockState.LOCKED, CMD_LOCK, CmdResult.OK, s0 + 2),
        ], "6a: notifications (UNLOCK OK, UNLOCKED settle, then the queued LOCK OK, LOCKED settle)")
        return (f"6a queued LOCK -> OK {b.result_ms:.0f} ms after its write, after the "
                "UNLOCKED settle")

    async def step6b(self) -> str:
        """From LOCKED: Unlock, then while UNLOCKING 4 more valid commands (queued) and a
        5th (BUSY at once). The 4 then run in order, one move each."""
        queued = [CMD_LOCK, CMD_UNLOCK, CMD_LOCK, CMD_UNLOCK]
        fifth = CMD_LOCK
        await self.ensure_state(LockState.LOCKED)
        lock = await self.connect()
        async with lock.status_listener() as q:
            before = await lock.read_status()
            s0 = before.result_seq
            await lock.command(bytes([CMD_UNLOCK]))
            acks = []
            for op in [*queued, fifth]:
                await lock.write(CMD_CHAR_UUID, bytes([op]))
                acks.append(time.perf_counter())
            # UNLOCK OK, BUSY, settle, 4 x (result + settle); each arrives within one move
            # A lock with the wrong queue depth sends fewer notifications than
            # expected and may idle-disconnect before the wait ends; keep what came.
            seen = await self.collect(lock, q, 3 + 2 * len(queued),
                                      self.args.settle_timeout + self.args.timeout,
                                      keep_on_disconnect=True)
        self.record_trace(before, seen)
        # resultSeq counts results as they are produced (ble-spec §6.5 rule 5): the 5th
        # command's immediate BUSY overtakes the 4 queued results.
        want = [_st(LockState.UNLOCKING, CMD_UNLOCK, CmdResult.OK, s0 + 1),
                _st(LockState.UNLOCKING, fifth, CmdResult.BUSY, s0 + 2),
                _st(LockState.UNLOCKED, fifth, CmdResult.BUSY, s0 + 2)]
        state = LockState.UNLOCKED
        for k, op in enumerate(queued):
            moving = LockState.UNLOCKING if op == CMD_UNLOCK else LockState.LOCKING
            state = _settle_target(moving)
            want += [_st(moving, op, CmdResult.OK, s0 + 3 + k), _st(state, op, CmdResult.OK, s0 + 3 + k)]
        settles = [s for s in seen[1:] if s.settled]
        self.check(not settles or acks[-1] < settles[0].received,
                   "6b: the 5 writes were not all acknowledged before UNLOCKING settled, so the "
                   "queue limit was not tested; the laptop is too slow for this T_act")
        # Match the opcode too: a lock with a shallower queue answers an earlier
        # command BUSY, which would otherwise pass for the 5th command's BUSY.
        self.check(len(seen) > 1 and seen[1].last_result is CmdResult.BUSY
                   and seen[1].last_opcode == fifth
                   and seen[1].result_seq == want[1].result_seq,
                   f"6b: the 5th command gave {seen[1] if len(seen) > 1 else 'nothing'}, expected "
                   f"an immediate BUSY with resultSeq {want[1].result_seq} (the queue holds "
                   f"{QUEUE_DEPTH})")
        self.check_exact(seen, want, "6b: notifications")
        return f"6b 5th command BUSY at once, the {len(queued)} queued ran in order, ended {state.name}"

    async def check_rebooted(self) -> Status:
        """After a human-caused reboot: reconnect, prove it rebooted, check it locked itself."""
        # Every boot: BOOT (target LOCKED), FF -> LOCKING, then the settle, which may be
        # logged before or after our CONNECT.
        self.expect(ExpectedEvent(EventType.BOOT, (None, LockState.LOCKED, 0)))
        self.expect(_state_event(BOOT_FROM, LockState.LOCKING))
        self.expect(_state_event(LockState.LOCKING, LockState.LOCKED),
                    ExpectedEvent(EventType.CONNECT))
        lock = await self.connect(expect_log=False, scan_timeout=max(self.args.scan_timeout, 20.0))
        status = await lock.read_status()
        print(f"      after reboot: {status}")
        self.check(status.result_seq == 0 and status.last_result is None,
                   f"status {status} shows commands since boot: the lock did not reboot")
        self.check(status.state in (LockState.LOCKING, LockState.LOCKED),
                   f"after boot the state is {_name(status.state)}, expected LOCKING or LOCKED "
                   "(every boot locks)")
        if not status.settled:
            status = await lock.wait_settled()
            print(f"      settled: {status}")
        self.check(status.state == LockState.LOCKED,
                   f"came up {_name(status.state)}, expected LOCKED (every boot locks)")
        return status

    async def step7(self) -> str:
        await self.ensure_state(LockState.UNLOCKED)
        await self.disconnect()
        await self.prompt("Step 7: press EN (reset) on the DevKit now. Wait for the boot banner "
                          "on the serial monitor (reset reason POWERON).")
        await self.check_rebooted()
        return "rebooted from UNLOCKED and locked itself, resultSeq 0 (check the banner yourself)"

    async def step8(self) -> str:
        await self.ensure_state(LockState.UNLOCKED)
        await self.disconnect()  # a slow human must not trip the idle timeout
        await self.prompt("Step 8: the lock is UNLOCKED. Unplug the DevKit's USB cable, wait a "
                          "few seconds, plug it back in, and wait for the boot banner.")
        await self.check_rebooted()
        return "powered up from UNLOCKED and locked itself"

    async def step9(self) -> str:
        await self.disconnect()
        target = self.target if isinstance(self.target, str) else await find_lock(
            self.args.name, self.args.scan_timeout)
        lock = LockClient(target, op_timeout=self.args.timeout)
        address = lock.address.upper()
        adv_times: list[float] = []

        def on_adv(dev: BLEDevice, adv: AdvertisementData) -> None:
            if dev.address.upper() == address and _advertises_lock_service(adv):
                adv_times.append(time.perf_counter())

        t_start = time.perf_counter()
        await lock.connect()
        self.expect(ExpectedEvent(EventType.CONNECT))
        lock._expect_disconnect = True  # the lock is supposed to drop us
        print(f"      connected; doing nothing for up to {self.t_idle + 15:.0f} s...")
        try:
            async with BleakScanner(detection_callback=on_adv):
                t_disc = await lock.wait_disconnected(self.t_idle + 15)
                self.expect(ExpectedEvent(EventType.IDLE_TIMEOUT), ExpectedEvent(EventType.DISCONNECT))
                for _ in range(50):  # up to 5 s
                    if adv_times:
                        break
                    await asyncio.sleep(0.1)
        except LockTimeoutError:
            await lock.disconnect()
            raise StepFailed(f"still connected after {self.t_idle + 15:.0f} s: no idle timeout") from None
        except BleakError as e:
            raise StepFailed(f"could not scan for re-advertising: {e}") from None
        # The link came up between t_start and connected_at, so idle time is within these bounds.
        idle_max, idle_min = t_disc - t_start, t_disc - lock.connected_at
        self.check(idle_max >= self.t_idle - 1.0 and idle_min <= self.t_idle + 2.0,
                   f"disconnected after {idle_min:.1f}-{idle_max:.1f} s, expected T_idle = {self.t_idle:.0f} s")
        self.check(bool(adv_times), "lock not seen advertising within 5 s of the disconnect")
        readv = max(0.0, adv_times[0] - t_disc)
        # 0.5 s allowance for Windows' disconnect-callback and scan-report latency.
        self.check(readv <= 1.5, f"re-advertising seen {readv:.2f} s after the disconnect, expected <= 1 s")
        return f"dropped after {idle_min:.1f}-{idle_max:.1f} s idle; advertising again {readv:.2f} s later"

    async def step10(self) -> str:
        if self.log_start is None:
            raise StepFailed("event log was unavailable at the start of the run")
        lock = await self.connect(expect_log=False)
        await asyncio.sleep(self.LOG_FLUSH_S)  # entries are readable once saved to flash
        entries = await lock.read_log(self.log_start)
        print(f"      {len(entries)} entries since seq {self.log_start}:")
        for e in entries:
            print(f"        {e}")
        seqs = [e.seq for e in entries]
        self.check(all(b > a for a, b in zip(seqs, seqs[1:])), "sequence numbers not increasing")
        gaps = sum(b - a - 1 for a, b in zip(seqs, seqs[1:]))
        for a, b in zip(entries, entries[1:]):
            self.check(b.boot_count >= a.boot_count and
                       (b.boot_count != a.boot_count or b.ms_since_boot >= a.ms_since_boot),
                       f"#{b.seq}: boot count / ms since boot went backwards after #{a.seq}")
        i = 0
        for n, group in enumerate(self.expected):
            remaining = list(group)
            while remaining:
                if i >= len(entries):
                    missing = ", ".join(str(x) for x in remaining)
                    raise StepFailed(f"expected event {n + 1}/{len(self.expected)} not found "
                                     f"(in order): {missing}")
                hit = next((x for x in remaining if x.matches(entries[i])), None)
                if hit is not None:
                    remaining.remove(hit)
                i += 1
        # Boot, state and command entries are all predictable (one client: this run), so
        # they must match the expected ones exactly, in order: this catches e.g. a queued
        # command's result logged before the settle of the move ahead of it.
        core = [e for e in entries if e.type in _CORE_EVENTS]
        j = 0
        for group in self.expected:
            remaining = [x for x in group if x.type in _CORE_EVENTS]
            while remaining:
                if j >= len(core):
                    raise StepFailed("expected event not found: " + ", ".join(map(str, remaining)))
                e = core[j]
                hit = next((x for x in remaining if x.matches(e)), None)
                if hit is None:
                    raise StepFailed(f"#{e.seq} is {_name(e.type)} {e.describe()}, but the next "
                                     f"expected event is {' or '.join(map(str, remaining))}")
                remaining.remove(hit)
                j += 1
        if j < len(core):
            e = core[j]
            raise StepFailed(f"unexpected entry #{e.seq} {_name(e.type)} {e.describe()} after the "
                             "last expected event")
        total = sum(len(g) for g in self.expected)
        return (f"all {total} expected events found in order; {len(entries)} entries, "
                f"seq {seqs[0] if seqs else '-'}..{seqs[-1] if seqs else '-'}"
                + (f", {gaps} missing seq numbers" if gaps else ""))

    # -- driver --

    async def step(self, num: str, title: str, fn) -> None:
        print(f"\n[{num}] {title}")
        try:
            verdict, detail = "PASS", await fn()
        except StepFailed as e:
            verdict, detail = "FAIL", str(e)
        except LockError as e:
            verdict, detail = "FAIL", f"{type(e).__name__}: {e}"
            if self.lock is not None and not self.lock.is_connected:
                self.lock = None
        except Exception as e:  # keep going so later steps still run and cleanup locks
            verdict, detail = "FAIL", f"unexpected {type(e).__name__}: {e}"
        print(f"    {verdict}  {detail}")
        self.results.append((num, verdict, detail))

    def skip(self, num: str, title: str, why: str) -> None:
        print(f"\n[{num}] {title}\n    SKIP  {why}")
        self.results.append((num, "SKIP", why))

    async def run(self) -> bool:
        print("Acceptance sequence, docs/v1-spec.md section 10. Keep the serial monitor open.")
        self.results.append(("1", "MANUAL", "check the boot banner on the serial monitor"))
        try:
            await self.step("2", "Read status and versions", self.step2)
            if self.incompatible is not None:
                raise self.incompatible  # exit code of IncompatibleFirmwareError
            await self.step("3", "Unlock", self.step3)
            await self.step("4", "Unlock again", self.step4)
            await self.step("5", "Send 07", self.step5)
            await self.step("6", "Unlock then lock at once (queued); queue limit", self.step6)
            if self.args.interactive:
                await self.step("7", "Press EN while UNLOCKED", self.step7)
                await self.step("8", "Unplug USB while UNLOCKED", self.step8)
            else:
                self.skip("7", "Press EN while UNLOCKED", "needs --interactive")
                self.skip("8", "Unplug USB while UNLOCKED", "needs --interactive")
            await self.step("9", f"Connect and do nothing (~{self.t_idle:.0f} s)", self.step9)
            await self.step("10", "Dump the event log", self.step10)
        finally:
            if self.incompatible is None:
                await self.cleanup()
            else:  # unknown protocol: send nothing
                try:
                    await self.disconnect(expect_log=False)
                except LockError:
                    pass
        print("\nSummary")
        for num, verdict, detail in self.results:
            print(f"  {num:>2}  {verdict:<6} {detail if verdict != 'PASS' else ''}".rstrip())
        failed = [num for num, verdict, _ in self.results if verdict == "FAIL"]
        print(f"\n{'FAILED: steps ' + ', '.join(failed) if failed else 'ALL AUTOMATIC STEPS PASSED'}")
        return not failed

    async def cleanup(self) -> None:
        """Leave the lock LOCKED where possible, and disconnected."""
        try:
            lock = await self.connect(expect_log=False)
            status = await lock.wait_settled()
            if status.state != LockState.LOCKED:
                out = await lock.command(CMD_LOCK, settle=True)
                status = out.settled or out.result_status
            print(f"\ncleanup: lock left {_name(status.state)}")
        except (LockError, StepFailed) as e:
            print(f"\ncleanup: could not leave the lock LOCKED: {e}", file=sys.stderr)
        finally:
            try:
                await self.disconnect(expect_log=False)
            except LockError:
                pass

# ---- CLI --------------------------------------------------------------------

EXIT_ACCEPTANCE_FAILED = 11
EXIT_NOT_ACCEPTED = 10  # unlock/lock: the lock answered, but did not reach the requested state
EXIT_BLUETOOTH_UNAVAILABLE = 12  # was 2 before, which argparse also uses for usage errors

# Commands that rely on protocol 1.x semantics; `info` and `send` work with any version.
NEEDS_PROTOCOL_1 = ("status", "unlock", "lock", "watch", "log")

EXIT_CODES = {
    BluetoothUnavailableError: EXIT_BLUETOOTH_UNAVAILABLE,
    LockNotFoundError: 3,
    LockConnectionError: 4,
    FirmwareTooOldError: 7,       # keep before its base class
    IncompatibleFirmwareError: 5,
    LockWriteError: 6,
    LockProtocolError: 8,
    LockTimeoutError: 9,
    LockError: 1,  # keep last: base class catches anything not listed above
}


def _parse_hex(text: str) -> bytes:
    try:
        return bytes.fromhex(text.replace("0x", "").replace(",", " "))
    except ValueError:
        raise argparse.ArgumentTypeError(f"not hex bytes: {text!r} (example: 01 or '01 ff')")


def _print_outcome(out: CommandOutcome) -> None:
    r = out.result_status
    print(f"result:  {result_name(r.last_result)}  (state {_name(r.state)}, resultSeq "
          f"{r.result_seq}, {out.result_ms:.0f} ms after write)")
    if out.settled is not None and out.settled is not r:
        print(f"settled: {_name(out.settled.state)}  ({out.settled_after_s:.2f} s later)")


async def _run(args: argparse.Namespace) -> int:
    if args.command == "scan":
        log.info("[scan]  scanning %.0f s for advertisers of the lock service...", args.scan_timeout)
        locks = await scan(args.scan_timeout)
        if not locks:
            raise LockNotFoundError("No locks found.")
        for found in locks:
            print(f"  {found}")
        return 0

    if args.command == "acceptance":
        ok = await Acceptance(args).run()
        return 0 if ok else EXIT_ACCEPTANCE_FAILED

    async with connect_lock(args.name, address=args.address, scan_timeout=args.scan_timeout,
                            op_timeout=args.timeout, settle_timeout=args.settle_timeout) as lock:
        info = await lock.check_protocol() if args.command in NEEDS_PROTOCOL_1 else None
        if args.command == "info":
            if lock.has_char(INFO_CHAR_UUID):
                print(f"versions: {await lock.read_info()}\n")
            else:
                print("versions: no info characteristic (v1.0 firmware, protocol 1.0)\n")
            print(lock.describe_gatt())
        elif args.command == "status":
            if info is not None:
                print(f"versions: {info}")
            print(f"status:   {await lock.read_status()}")
        elif args.command in ("unlock", "lock"):
            out = await lock.command(CMD_UNLOCK if args.command == "unlock" else CMD_LOCK,
                                     settle=not args.no_settle)
            _print_outcome(out)
            if out.result not in (CmdResult.OK, CmdResult.NO_CHANGE):
                return EXIT_NOT_ACCEPTED
        elif args.command == "send":
            if lock.has_char(STATUS_CHAR_UUID):
                _print_outcome(await lock.command(args.data))
            else:
                result = await lock.write(CMD_CHAR_UUID, args.data)
                print(f"acknowledged in {result.ack_ms:.0f} ms (v1.0 firmware: no result available)")
        elif args.command == "watch":
            t0 = time.perf_counter()
            print(f"now     {await lock.read_status()}")
            prev_seq = lock.last_status.result_seq
            async for s in lock.watch_status():
                kind = "result" if s.result_seq != prev_seq else "state "
                prev_seq = s.result_seq
                print(f"+{s.received - t0:7.3f}s {kind}  {s}", flush=True)
            print(f"lock disconnected after {time.perf_counter() - t0:.1f} s "
                  f"(it drops clients that send no command for {T_IDLE:.0f} s)")
        elif args.command == "log":
            count = 0
            async for page in lock.iter_log(args.start):
                for e in page:
                    print(e)
                count += len(page)
            print(f"{count} entries" if count else "log is empty")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="BLE test client for the VT bike lock (docs/ble-spec.md v1.1).",
        epilog="exit codes: 0 ok, 1 other error, 2 usage error, 3 lock not found, "
               "4 connection, 5 incompatible firmware (incl. protocol major version != 1), "
               "6 write failed, 7 firmware too old (v1.0), 8 protocol violation, "
               "9 result/settle timeout, 10 lock/unlock not accepted (e.g. BUSY: queue full), "
               "11 acceptance failed, 12 Bluetooth unavailable, 130 interrupted")
    parser.add_argument("-v", "--verbose", action="count", default=0,
                        help="-v: debug output from this tool, -vv: also from bleak")
    parser.add_argument("--name", default=DEFAULT_NAME,
                        help=f"advertised name to match (default {DEFAULT_NAME})")
    parser.add_argument("--any", dest="name", action="store_const", const=None,
                        help="connect to the first lock found, whatever its name")
    parser.add_argument("--address", help="connect to this address directly, skipping the name scan")
    parser.add_argument("--scan-timeout", type=float, default=DEFAULT_SCAN_TIMEOUT, metavar="S")
    parser.add_argument("--timeout", type=float, default=DEFAULT_OP_TIMEOUT, metavar="S",
                        help=f"per GATT operation and per command result (default {DEFAULT_OP_TIMEOUT:.0f})")
    parser.add_argument("--settle-timeout", type=float, default=DEFAULT_SETTLE_TIMEOUT, metavar="S",
                        help=f"wait for UNLOCKING/LOCKING to finish (default {DEFAULT_SETTLE_TIMEOUT:.0f})")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("scan", help="list nearby locks")
    sub.add_parser("info", help="print versions and the GATT table (works on v1.0 firmware)")
    sub.add_parser("status", help="read the state, last command result and versions")
    for name, op in (("unlock", "0x01"), ("lock", "0x00")):
        p = sub.add_parser(name, help=f"send {op}, print the result and the settled state")
        p.add_argument("--no-settle", action="store_true",
                       help="print the result only; don't wait for the transition to finish")
    send = sub.add_parser("send", help="write raw bytes to the command characteristic, print the result")
    send.add_argument("data", type=_parse_hex, help="hex bytes, e.g. 07 or '01 ff' (\"\" for empty)")
    sub.add_parser("watch", help="print status notifications until Ctrl+C or the lock's idle disconnect")
    logp = sub.add_parser("log", help="dump the event log, decoded")
    logp.add_argument("--from", dest="start", type=int, default=0, metavar="SEQ",
                      help="first sequence number to fetch (default: everything)")
    acc = sub.add_parser("acceptance", help="run the docs/v1-spec.md section 10 acceptance sequence",
                         description="Runs steps 2-6, 9, 10 automatically (step 9 waits ~T_idle); "
                                     "steps 7-8 (press EN, unplug USB; the lock must come up "
                                     "LOCKED) need --interactive. Prints PASS/FAIL per step, "
                                     f"leaves the lock LOCKED, exits {EXIT_ACCEPTANCE_FAILED} on any failure.")
    acc.add_argument("--interactive", action="store_true",
                     help="also run steps 7 (press EN) and 8 (unplug and replug USB), prompting you")
    acc.add_argument("--t-act", type=float, default=T_ACT_LED, metavar="S",
                     help=f"expected actuation time (default {T_ACT_LED:.0f}, the LED)")
    acc.add_argument("--idle-timeout", type=float, default=T_IDLE, metavar="S",
                     help=f"expected idle timeout (default {T_IDLE:.0f})")
    args = parser.parse_args(argv)

    logging.basicConfig(format="%(message)s",
                        level=logging.DEBUG if args.verbose >= 2 else logging.INFO)
    log.setLevel(logging.DEBUG if args.verbose else logging.INFO)

    try:
        return asyncio.run(_run(args))
    except LockError as e:
        print(f"error: {e}", file=sys.stderr)
        return next(code for cls, code in EXIT_CODES.items() if isinstance(e, cls))
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())

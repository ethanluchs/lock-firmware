"""Scriptable BLE test client for the VT bike lock (see docs/ble-spec.md).

Command line:

    python tools/lock_client.py scan             # list nearby locks
    python tools/lock_client.py unlock           # find VT-LOCK-001, write 0x01
    python tools/lock_client.py lock             # find VT-LOCK-001, write 0x00
    python tools/lock_client.py send 02          # write raw hex bytes (test reserved opcodes)
    python tools/lock_client.py info             # dump the lock's GATT table
    python tools/lock_client.py -v unlock        # also show debug logging

    Options: --name NAME, --address ADDR (skip scanning), --scan-timeout SECONDS

As a library (multi-step sequences, Phase 2 handshakes):

    import asyncio, logging
    from lock_client import connect_lock

    async def main():
        logging.basicConfig(level=logging.INFO, format="%(message)s")
        async with connect_lock() as lock:          # scan + connect; disconnects on exit
            await lock.unlock()
            await asyncio.sleep(2)
            await lock.lock()

    asyncio.run(main())

Every failure raises a subclass of LockError, so a script can catch exactly the
cases it cares about. The CLI maps each one to a distinct exit code.
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import sys
import time
from contextlib import asynccontextmanager
from dataclasses import dataclass
from typing import AsyncIterator

from bleak import BleakClient, BleakScanner
from bleak.backends.characteristic import BleakGATTCharacteristic
from bleak.backends.device import BLEDevice
from bleak.backends.scanner import AdvertisementData
from bleak.exc import BleakBluetoothNotAvailableError, BleakError, BleakGATTProtocolError

# ---- protocol constants (docs/ble-spec.md §1) ------------------------------

SERVICE_UUID = "8f2a1c00-5b7e-4d19-9a3f-6c2e81d4b70a"
CMD_CHAR_UUID = "8f2a1c01-5b7e-4d19-9a3f-6c2e81d4b70a"
DEFAULT_NAME = "VT-LOCK-001"

# Phase 1 opcodes. PROVISIONAL: Phase 2 removes unauthenticated lock/unlock.
CMD_LOCK = 0x00
CMD_UNLOCK = 0x01
CMD_NAMES = {CMD_LOCK: "LOCK", CMD_UNLOCK: "UNLOCK"}

DEFAULT_SCAN_TIMEOUT = 10.0
DEFAULT_CONNECT_TIMEOUT = 15.0  # Windows connects are often 2-5 s; leave headroom
DEFAULT_OP_TIMEOUT = 5.0

log = logging.getLogger("lock_client")


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


class LockWriteError(LockError):
    """A write was rejected, timed out, or its outcome is unknown."""


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

    if dev is None:
        hint = (
            "Check: board powered and running the lock firmware; within range; "
            "no phone currently connected to it (the lock stops advertising while "
            "connected); Windows Bluetooth is on."
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


class LockClient:
    """One BLE connection to one lock.

    Wraps BleakClient with lock-specific checks, timeouts and errors. Prefer
    `async with LockClient(...)` or `connect_lock()` so the link is always
    released: the Phase 1 lock has no idle timeout, so a leaked connection
    hides it from every other client until the link drops.
    """

    def __init__(self, target: FoundLock | BLEDevice | str, *,
                 connect_timeout: float = DEFAULT_CONNECT_TIMEOUT,
                 op_timeout: float = DEFAULT_OP_TIMEOUT) -> None:
        device = target.device if isinstance(target, FoundLock) else target
        self._label = str(target) if isinstance(target, FoundLock) else str(device)
        self._op_timeout = op_timeout
        self._expect_disconnect = False
        self._client = BleakClient(
            device,
            disconnected_callback=self._on_disconnect,
            timeout=connect_timeout,
            # Windows caches each device's GATT table. Phase 2 adds
            # characteristics, and a stale cache would make them "not found".
            winrt={"use_cached_services": False},
        )

    # -- lifecycle --

    async def connect(self) -> None:
        log.info("[conn]  connecting to %s...", self._label)
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

        if self._client.services.get_characteristic(CMD_CHAR_UUID) is None:
            await self.disconnect()
            raise IncompatibleFirmwareError(
                f"Connected, but command characteristic {CMD_CHAR_UUID} is missing. "
                "Is this board running the lock firmware?")
        log.info("[conn]  connected in %.2f s", time.perf_counter() - t0)

    async def disconnect(self) -> None:
        if self._client.is_connected:
            self._expect_disconnect = True
            await self._client.disconnect()
            log.info("[conn]  disconnected")

    async def __aenter__(self) -> LockClient:
        await self.connect()
        return self

    async def __aexit__(self, *exc_info) -> None:
        await self.disconnect()

    @property
    def is_connected(self) -> bool:
        return self._client.is_connected

    def _on_disconnect(self, _client: BleakClient) -> None:
        if not self._expect_disconnect:
            log.warning("[conn]  lock disconnected unexpectedly")

    def _require_connected(self) -> None:
        if not self._client.is_connected:
            raise LockConnectionError("Not connected to the lock.")

    # -- generic GATT operations (the building blocks for Phase 2) --

    async def write(self, char_uuid: str, data: bytes) -> WriteResult:
        """Write with response and wait for the lock's acknowledgement.

        An ack means the lock *received* the bytes, not that it acted on them
        (docs/ble-spec.md §6.1).
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

        Round trips then read naturally:
            q = await lock.subscribe(STATUS_UUID)
            await lock.write(CMD_CHAR_UUID, request)
            reply = await asyncio.wait_for(q.get(), timeout=5)
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

    # -- Phase 1 commands --

    async def send_command(self, opcode: int) -> WriteResult:
        if not 0 <= opcode <= 0xFF:
            raise ValueError(f"opcode must be one byte, got {opcode}")
        result = await self.write(CMD_CHAR_UUID, bytes([opcode]))
        label = CMD_NAMES.get(opcode, "reserved, lock will ignore it")
        log.info("[write] 0x%02X (%s) acknowledged by lock in %.0f ms",
                 opcode, label, result.ack_ms)
        return result

    async def unlock(self) -> WriteResult:
        return await self.send_command(CMD_UNLOCK)

    async def lock(self) -> WriteResult:
        return await self.send_command(CMD_LOCK)


@asynccontextmanager
async def connect_lock(name: str | None = DEFAULT_NAME, *, address: str | None = None,
                       scan_timeout: float = DEFAULT_SCAN_TIMEOUT,
                       **client_kwargs) -> AsyncIterator[LockClient]:
    """Find a lock (by name, or directly by address), connect, and always disconnect."""
    target: FoundLock | str = address if address else await find_lock(name, scan_timeout)
    async with LockClient(target, **client_kwargs) as client:
        yield client


# ---- CLI --------------------------------------------------------------------

EXIT_CODES = {
    BluetoothUnavailableError: 2,
    LockNotFoundError: 3,
    LockConnectionError: 4,
    IncompatibleFirmwareError: 5,
    LockWriteError: 6,
    LockError: 1,  # keep last: base class catches anything not listed above
}


def _parse_hex(text: str) -> bytes:
    try:
        return bytes.fromhex(text.replace("0x", "").replace(",", " "))
    except ValueError:
        raise argparse.ArgumentTypeError(f"not hex bytes: {text!r} (example: 01 or '01 ff')")


async def _run(args: argparse.Namespace) -> None:
    if args.command == "scan":
        log.info("[scan]  scanning %.0f s for advertisers of the lock service...", args.scan_timeout)
        locks = await scan(args.scan_timeout)
        if not locks:
            raise LockNotFoundError("No locks found.")
        for found in locks:
            print(f"  {found}")
        return

    async with connect_lock(args.name, address=args.address,
                            scan_timeout=args.scan_timeout) as lock:
        if args.command == "info":
            print(lock.describe_gatt())
        elif args.command in ("unlock", "lock"):
            await (lock.unlock() if args.command == "unlock" else lock.lock())
            log.info("        note: Phase 1 firmware does not report whether the actuator "
                     "moved; check the LED or serial monitor.")
        elif args.command == "send":
            result = await lock.write(CMD_CHAR_UUID, args.data)
            log.info("[write] %s acknowledged by lock in %.0f ms",
                     args.data.hex(" ") or "<empty>", result.ack_ms)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="BLE test client for the VT bike lock.")
    parser.add_argument("-v", "--verbose", action="count", default=0,
                        help="-v: debug output from this tool, -vv: also from bleak")
    parser.add_argument("--name", default=DEFAULT_NAME,
                        help=f"advertised name to match (default {DEFAULT_NAME})")
    parser.add_argument("--any", dest="name", action="store_const", const=None,
                        help="connect to the first lock found, whatever its name")
    parser.add_argument("--address", help="connect to this address directly, skipping the name scan")
    parser.add_argument("--scan-timeout", type=float, default=DEFAULT_SCAN_TIMEOUT, metavar="S")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("scan", help="list nearby locks")
    sub.add_parser("info", help="connect and print the GATT table")
    sub.add_parser("unlock", help="write 0x01")
    sub.add_parser("lock", help="write 0x00")
    send = sub.add_parser("send", help="write raw bytes to the command characteristic")
    send.add_argument("data", type=_parse_hex, help="hex bytes, e.g. 02 or '01 ff'")
    args = parser.parse_args(argv)

    logging.basicConfig(format="%(message)s",
                        level=logging.DEBUG if args.verbose >= 2 else logging.INFO)
    log.setLevel(logging.DEBUG if args.verbose else logging.INFO)

    try:
        asyncio.run(_run(args))
    except LockError as e:
        print(f"error: {e}", file=sys.stderr)
        return next(code for cls, code in EXIT_CODES.items() if isinstance(e, cls))
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())

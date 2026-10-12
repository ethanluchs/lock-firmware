# VT-LOCK BLE Interface Specification

| | |
|---|---|
| **Spec version** | **1.1.0** (Phase 1, V1 firmware) |
| **Status** | Phase 1: stable, implement against it. Phase 2 (§9): preview only, not implementable yet. 1.1.0 is backward compatible with 1.0.0 (§0, "Compatibility"). |
| **Firmware** | `src/main.cpp` (ESP32, Arduino core 3.3.x, Bluedroid BLE stack). Byte layouts: `lib/lock_protocol/src/lock_protocol.h`, `lib/event_log/src/event_log.h`, `lib/lock_core/src/lock_types.h` |
| **Last updated** | 2026-10-12 |
| **Firmware owner** | ethanluchs |

This document is the contract between the lock firmware and the iOS app. If the
firmware and this document disagree, that is a bug. Report it rather than
working around it. For byte layouts, the firmware headers listed above are the
source of truth this document is written from; a mismatch between them and this
document is a documentation bug.

---

## 0. How to read this document

**Requirement keywords.** MUST / MUST NOT / SHOULD / MAY are used as in RFC 2119.
"The app MUST" is a hard requirement; "SHOULD" is a strong recommendation.

**Stability labels.** Every behavior is tagged with one of these:

| Label | Meaning | What the app should do |
|---|---|---|
| **[STABLE]** | Will remain true in Phase 2. | Safe to hard-code. |
| **[PROVISIONAL]** | True for Phase 1 firmware, but **will change or be removed** in Phase 2. | Use it, but keep it in one isolated place in the app (one file or type) so Phase 2 is a contained change. Do not let other code depend on it. |

**Versioning.** `MAJOR.MINOR.PATCH`. A MAJOR bump means existing apps will break
(Phase 2 will be 2.0.0, because unauthenticated unlock is removed). MINOR means
additive and backward compatible. PATCH means documentation fixes only. The
lock reports the MAJOR.MINOR it implements in the info characteristic (§4.5).

**Forward compatibility. [STABLE]** A later MINOR version may append fields to
the end of any value the lock sends (status, info, log page) and may add
characteristics. Therefore:

- The app MUST accept a value that is **longer** than documented and MUST
  ignore the extra bytes. Never check `value.count == N`; check `>= N`.
- The app MUST treat a value **shorter** than documented as a protocol error.
- The app MUST NOT crash on an enum value it does not know (state, result code,
  event type). §4.4–§4.6 say what to do with each.

**Compatibility with 1.0.0 (what changed for an existing app).** A correct 1.0
app keeps working against 1.1 firmware unchanged:

| 1.1 change | Effect on a correct 1.0 app |
|---|---|
| New characteristics `…1C02`–`…1C04` | None. 1.0 required lookup by UUID (§3), so shifted handles don't matter. |
| Unknown opcodes now produce result `UNKNOWN_COMMAND` | None. The write is still acknowledged and the state still doesn't change. The result is only visible to an app that subscribes. |
| Writes of length ≠ 1 now produce `MALFORMED` and are **not executed** | None for a correct app, which sends exactly one byte (1.0 §4.2 said not to rely on extra bytes being ignored). An app that sent `01 00` and relied on it unlocking breaks. |
| Idle timeout (30 s without a command write) | None. 1.0 already required disconnecting as soon as finished (§5 rule 2). |
| A valid LOCK/UNLOCK sent while the lock is moving is **queued** (up to 4) and executed in order after the move settles | None, and it is what makes back-to-back commands work (below). |
| A second phone's connection attempt is refused | None. 1.0 already said only one phone can be connected (§2.3). |
| Power-up / reset | Unchanged: the lock always drives to LOCKED on power-up or reset, as 1.0 stated (§6.3). |

**Why back-to-back commands from a 1.0 app work.** A correct 1.0 app writes a
command, waits for the write response (§5 rule 1), and may then write the next
one, for example UNLOCK then LOCK a moment later. It never reads a result. If
the lock answered the LOCK with `BUSY` because the unlock was still moving, the
LOCK would be silently dropped and the lock left UNLOCKED, with nothing to tell
the 1.0 app. Because 1.1 queues it instead, the LOCK runs as soon as UNLOCKED
settles and the lock ends LOCKED, as the app intended. `BUSY` happens only when
more than 4 commands are already waiting (§6.2), which a 1.0 app that waits for
each write response does not do in normal use.

**Notation.** Bytes are written in hex (`0x01`); a byte string is written as
space-separated hex (`02 01 00 05`). Multi-byte integers are little-endian.
UUIDs are case-insensitive; CoreBluetooth's `CBUUID` compares them correctly
regardless of case.

---

## 1. Quick reference

| Item | Value | Stability |
|---|---|---|
| Lock service UUID | `8F2A1C00-5B7E-4D19-9A3F-6C2E81D4B70A` | [STABLE] |
| Command characteristic | `8F2A1C01-…`, Write (with response) only | [STABLE] |
| Status characteristic (1.1) | `8F2A1C02-…`, Read + Notify, 4 bytes (§4.4) | [STABLE] |
| Info characteristic (1.1) | `8F2A1C03-…`, Read, 5 bytes (§4.5) | [STABLE] |
| Log characteristic (1.1) | `8F2A1C04-…`, Write (with response) + Read (§4.6) | [PROVISIONAL] |
| Reserved for future characteristics | `8F2A1C05-…` through `8F2A1CFF-…` | [STABLE] |
| UUID suffix of every characteristic above | `-5B7E-4D19-9A3F-6C2E81D4B70A` | [STABLE] |
| Advertised name | `VT-LOCK-001` | [PROVISIONAL] |
| Command `0x01` | Unlock | [PROVISIONAL] |
| Command `0x00` | Lock | [PROVISIONAL] |
| Payload length | Exactly 1 byte; anything else → result `MALFORMED` | [PROVISIONAL] |
| States | `0` LOCKED, `1` UNLOCKING, `2` UNLOCKED, `3` LOCKING | [STABLE] |
| Result codes | `0` OK, `1` NO_CHANGE, `2` BUSY, `3` UNKNOWN_COMMAND, `4` MALFORMED; `0xFF` = none yet | [STABLE] (values) |
| Result delivery | One status notification per command, matched by `resultSeq` (§6.5); a command sent during a move is queued and its result arrives after the move settles | [STABLE] |
| Power-up / reset | Always drives to LOCKED (fail-secure) (§6.3) | [STABLE] |
| Connections | One phone at a time; a second connection is refused (§2.3) | [STABLE] |
| Idle timeout | Lock disconnects after 30 s with no command write (§6.6) | [STABLE] that it exists; [PROVISIONAL] 30 s |
| Actuation time `T_act` | 1 s (LED stand-in); servo value TBD | [PROVISIONAL] |
| Pairing / encryption | None | [STABLE]: Phase 2 authenticates at the application layer, not via BLE pairing |

```swift
let lockServiceUUID = CBUUID(string: "8F2A1C00-5B7E-4D19-9A3F-6C2E81D4B70A")
let commandCharUUID = CBUUID(string: "8F2A1C01-5B7E-4D19-9A3F-6C2E81D4B70A")
let statusCharUUID  = CBUUID(string: "8F2A1C02-5B7E-4D19-9A3F-6C2E81D4B70A")
let infoCharUUID    = CBUUID(string: "8F2A1C03-5B7E-4D19-9A3F-6C2E81D4B70A")
let logCharUUID     = CBUUID(string: "8F2A1C04-5B7E-4D19-9A3F-6C2E81D4B70A")
```

---

## 2. Advertising and discovery

### 2.1 What the lock broadcasts

The lock advertises as a connectable peripheral (`ADV_IND`) every 20–40 ms
whenever no phone is connected.

| Packet | Contents |
|---|---|
| Advertising packet | Flags (LE General Discoverable, BR/EDR not supported); 128-bit service UUID list containing the lock service UUID |
| Scan response | TX Power Level; Complete Local Name `VT-LOCK-001` |

The name is in the **scan response**, not the main advertising packet. This is
because a 128-bit UUID plus the name do not fit in the 31-byte advertising
packet. iOS performs active scanning in the foreground, so
`advertisementData[CBAdvertisementDataLocalNameKey]` will normally be populated,
but a given discovery callback can arrive before the scan response does.

### 2.2 How the app MUST discover the lock

1. **[STABLE]** Scan with a service filter:
   `central.scanForPeripherals(withServices: [lockServiceUUID], options: nil)`.
   The service UUID is in the main advertising packet, so this filter is
   reliable, including for background scanning.
2. **[PROVISIONAL]** The app MAY use the advertised name `VT-LOCK-001` for display
   or to pick a specific dev board. It MUST NOT use the name as the lock's
   identity. Every Phase 1 board advertises the same hard-coded name, and
   Phase 2 will change the naming scheme.
3. The app MUST NOT use `CBPeripheral.identifier` as a lock ID outside the phone.
   iOS generates that UUID per phone; it is not the lock's address and differs
   on every phone. A real, stable lock ID will be provided by the firmware in
   Phase 2 (§9).

### 2.3 Visibility rules

- **[STABLE]** **One phone at a time.** The lock stops advertising while a phone
  is connected and resumes within 1 s after any disconnect (including its own
  idle-timeout disconnect, §6.6). While one phone is connected, other phones
  cannot discover it. A second phone that tries to connect anyway (e.g. to a
  previously discovered `CBPeripheral`) is refused without disturbing the
  connected phone: it sees `didFailToConnect` or an immediate
  `didDisconnectPeripheral`. Treat that as "lock in use by another phone".
- **[STABLE]** After power-on or reset, the lock begins advertising, typically
  about 2 s later (1 s boot delay plus BLE start-up). The first boot after
  flashing new firmware can take noticeably longer, because the lock formats
  its log filesystem. Do not hard-code the 2 s.

---

## 3. GATT layout

```
Lock Service  8F2A1C00-5B7E-4D19-9A3F-6C2E81D4B70A   (primary)
├── Command   8F2A1C01-5B7E-4D19-9A3F-6C2E81D4B70A   properties: Write
├── Status    8F2A1C02-5B7E-4D19-9A3F-6C2E81D4B70A   properties: Read, Notify   (+ CCCD)   1.1
├── Info      8F2A1C03-5B7E-4D19-9A3F-6C2E81D4B70A   properties: Read                      1.1
└── Log       8F2A1C04-5B7E-4D19-9A3F-6C2E81D4B70A   properties: Write, Read               1.1
```

The order shown is not guaranteed (see below). The lock also exposes the
standard Generic Access (`0x1800`) and Generic Attribute (`0x1801`) services,
which iOS handles itself. The app SHOULD ignore them.

The app MUST:

- **[STABLE]** Look up characteristics **by UUID**, never by position or
  attribute handle. Versions add characteristics to this service, which shifts
  handles and ordering.
- **[STABLE]** Run `discoverServices` / `discoverCharacteristics` on **every**
  connection rather than caching `CBCharacteristic` objects across connections.
- **[STABLE]** Treat "lock service found but command characteristic missing" as
  *incompatible firmware* (§7), not as a transient error.
- **[STABLE]** Decide what the lock supports from the info characteristic
  (§4.5): protocol MAJOR must be `1`; MINOR ≥ 1 means everything in this
  document is present. If the **status characteristic is missing**, the lock
  runs 1.0 firmware: either fall back to 1.0 behavior (write only, no state,
  display "Unlock sent") or report "lock firmware too old". Do not retry.

---

## 4. Characteristics: byte formats

### 4.1 Command: write type

**[STABLE]** The app MUST write with `type: .withResponse`. The characteristic
does not declare the Write Without Response property, and CoreBluetooth will
refuse to send a `.withoutResponse` write to it (nothing reaches the lock).

### 4.2 Command: framing

**[STABLE]** Byte 0 of every write to the command characteristic is an
**opcode**. In Phase 2, some opcodes will carry a payload in bytes 1..N. Keep
this "first byte = opcode" structure in the app's encoder.

**[PROVISIONAL]** Phase 1 opcodes. Every write is acknowledged (ATT success);
the outcome is reported in the status characteristic (§4.4, §6.2):

| Write (hex) | Length | Meaning | Result | State change |
|---|---|---|---|---|
| `01` | 1 | UNLOCK | `OK`, `NO_CHANGE` or `BUSY` (§6.2); during a move, queued and answered after it settles | LOCKED → UNLOCKING → UNLOCKED, if `OK` |
| `00` | 1 | LOCK | `OK`, `NO_CHANGE` or `BUSY`; during a move, queued as for UNLOCK | UNLOCKED → LOCKING → LOCKED, if `OK` |
| `02`–`FF` | 1 | Reserved | `UNKNOWN_COMMAND` | None |
| *(empty)* | 0 | — | `MALFORMED` (lastOpcode `FF`) | None |
| anything | ≥ 2 | — | `MALFORMED` (lastOpcode = first byte) | None. **Not executed**, even if byte 0 is `01`. |

**[PROVISIONAL]** The app MUST send **exactly one byte**. 1.0 firmware read the
first byte and ignored the rest; 1.1 rejects such writes with `MALFORMED`. In
Phase 2, longer writes become the normal format.

### 4.3 Command: example

```swift
// Unlock (Phase 1, unauthenticated)
peripheral.writeValue(Data([0x01]), for: commandChar, type: .withResponse)
// The write acknowledgement arrives in peripheral(_:didWriteValueFor:error:).
// The *result* arrives as a status notification (§4.4, §6.5).
```

### 4.4 Status (1.1)

`8F2A1C02-…`, **Read + Notify**. **[STABLE]** layout; later versions may append
bytes (§0).

| Byte | Field | Values |
|---|---|---|
| 0 | `state` | `00` LOCKED, `01` UNLOCKING, `02` UNLOCKED, `03` LOCKING |
| 1 | `lastOpcode` | Opcode of the most recent command result (byte 0 of the write). `FF` in four cases, told apart by `lastResult` (below). |
| 2 | `lastResult` | Result code of the most recent command result (§6.2). `FF` if there has been no command since boot. |
| 3 | `resultSeq` | Incremented by 1 for every command result since boot, wrapping `FF` → `00`. `00` before the first command. |

- `state` is the **commanded** state, not a verified position: V1 has no
  position sensing (§6.3).
- Unknown `state` value: show "unknown", don't offer lock/unlock based on it.
  Unknown `lastResult` value: treat the command as failed.
- `lastOpcode`/`lastResult` describe the **most recent command**, not the
  event that caused this particular notification. Only a **change of
  `resultSeq`** means "a new result" (§6.5).
- To tell "no command since boot" apart, test `lastResult == FF`, not
  `resultSeq == 0` (it is also `0` after wrapping).
- `lastOpcode == FF` has four meanings; `lastResult` tells them apart:

  | `lastResult` | Meaning of `lastOpcode = FF` | Log COMMAND_RESULT (§4.6) |
  |---|---|---|
  | `FF` | No command since boot | none |
  | `04` MALFORMED | The write was empty | opcode `FF`, length `0` |
  | `03` UNKNOWN_COMMAND | The write was the single byte `FF` | opcode `FF`, length `1` |
  | `02` BUSY | Rare: the firmware's internal event queue overflowed and the command's bytes were lost (§6.2) | opcode `FF`, result BUSY |

  (An empty write and a 2-byte write starting with `FF` are both MALFORMED;
  the log's length byte tells them apart.)

**Worked examples** (one session, starting just after boot, lock LOCKED):

| Moment | Status bytes | Decoded |
|---|---|---|
| Read after connecting | `00 FF FF 00` | LOCKED, no command since boot |
| Notification after writing `01` | `01 01 00 01` | UNLOCKING; UNLOCK → OK; resultSeq 1 |
| Notification ~`T_act` later (settled) | `02 01 00 01` | UNLOCKED; same resultSeq 1, so not a new result |
| Notification after writing `01` again | `02 01 01 02` | UNLOCKED; UNLOCK → NO_CHANGE; resultSeq 2 |
| Notification after writing `07` | `02 07 03 03` | UNLOCKED; `07` → UNKNOWN_COMMAND |
| Notification after writing `01 00` | `02 01 04 04` | UNLOCKED; MALFORMED (not executed) |
| Notification after writing `00` | `03 00 00 05` | LOCKING; LOCK → OK |
| Writing `01` during LOCKING | *(no notification yet)* | UNLOCK is queued behind the move |
| Notification when LOCKING settles | `00 00 00 05` | LOCKED; resultSeq unchanged, so not a new result |
| Notification right after, as the queued UNLOCK runs | `01 01 00 06` | UNLOCKING; UNLOCK → OK; resultSeq 6 |
| Notification ~`T_act` later (settled) | `02 01 00 06` | UNLOCKED; resultSeq unchanged |

**CoreBluetooth usage.**

```swift
// After discovery, on every connection: subscribe FIRST, then read, so no change is missed.
peripheral.setNotifyValue(true, for: statusChar)
// -> peripheral(_:didUpdateNotificationStateFor:error:)  then:
peripheral.readValue(for: statusChar)

// Read responses AND notifications both arrive here; they are indistinguishable,
// so treat every callback as "the latest status".
func peripheral(_ p: CBPeripheral, didUpdateValueFor c: CBCharacteristic, error: Error?) {
    guard c.uuid == statusCharUUID, error == nil, let v = c.value else { return }
    guard let s = LockStatus(v) else { /* shorter than 4 bytes: protocol error */ return }
    handleStatus(s)
}

struct LockStatus {
    let state, lastOpcode, lastResult, resultSeq: UInt8
    init?(_ data: Data) {
        let b = [UInt8](data)
        guard b.count >= 4 else { return nil }   // longer is fine: ignore the extra bytes
        (state, lastOpcode, lastResult, resultSeq) = (b[0], b[1], b[2], b[3])
    }
}
```

The lock sends **nothing** when the app connects or subscribes; the initial read
is required.

### 4.5 Info (1.1)

`8F2A1C03-…`, **Read**. **[STABLE]** layout; later versions may append bytes
(Phase 2 is expected to append a lock ID).

| Byte | Field |
|---|---|
| 0 | Protocol (this document) MAJOR |
| 1 | Protocol MINOR |
| 2 | Firmware MAJOR |
| 3 | Firmware MINOR |
| 4 | Firmware PATCH |

Example: `01 01 00 02 03` = protocol 1.1, firmware 0.2.3. The protocol PATCH
number is not reported (it never changes behavior).

The app SHOULD read this once per connection, before anything else, and:

- protocol MAJOR ≠ `1` → incompatible firmware (§7), disconnect;
- protocol `1.1` or higher MINOR → use this document.

The firmware version is for display and bug reports only; the app MUST NOT
derive behavior from it.

### 4.6 Event log (1.1)

`8F2A1C04-…`, **Write (with response) + Read**. **[PROVISIONAL]**: V1 is
read-only; Phase 2 adds acknowledging/clearing and backend upload, which may
change this characteristic. The 16-byte entry layout is fixed for protocol
1.x.

The lock keeps at least 256 entries in non-volatile storage. When full, the
oldest is overwritten.

**Durability. [STABLE]** An entry becomes readable over BLE only once it has
been saved to flash, normally within about 100 ms of the event. So a read made
immediately after an event (for example your own CONNECT) may not include it
yet; read again later. In return, **a `seq` the app has seen is never reused**,
even after power loss. An event lost to power loss before it was saved was
never visible to any client, so it is simply a gap.

Retrieval is paged with a **cursor**:

1. Write the cursor: a sequence number, 4 bytes, uint32 little-endian. Writes
   of any other length are ignored and leave the cursor unchanged. The cursor
   is reset to `0` on every new connection.
2. Read. The lock returns the **oldest entries with `seq` ≥ cursor**, up to 8:

   | Byte | Field |
   |---|---|
   | 0 | `count`, 0–8. `0` means there are no more entries. |
   | 1 … 16·count | `count` entries, 16 bytes each, oldest first |

   A full page is 129 bytes. Bytes after the last entry MUST be ignored (§0).
3. A read does **not** advance the cursor. Write `last entry's seq + 1`, read
   again, and repeat until `count == 0`. A page with fewer than 8 entries is
   not by itself the end; only `count == 0` is.

**Entry, 16 bytes:**

| Bytes | Field | Type | Notes |
|---|---|---|---|
| 0–3 | `seq` | uint32 LE | 1, 2, 3, … across reboots. Valid range `1`..`0x7FFFFFFF` (never 0). Strictly increasing, but **gaps are possible** (overwritten or unreadable slots, events lost to power loss before saving). Never reused once a client has seen it. |
| 4–5 | `bootCount` | uint16 LE | Boot counter when the event happened |
| 6–9 | `msSinceBoot` | uint32 LE | Milliseconds since that boot. **No wall-clock time**: the lock has no trusted clock. |
| 10 | `type` | uint8 | Event type, below |
| 11–15 | `data` | 5 bytes | Meaning per type, zero-padded |

| `type` | Event | `data` |
|---|---|---|
| `01` | BOOT | [0] reset reason (ESP-IDF `esp_reset_reason_t`: `01` POWERON, `03` software, `04` panic, `05`–`07` watchdog, `09` brownout, …). On the ESP32, pressing the EN (reset) button reports `01` POWERON, not EXT. [1] boot target state, always `00` LOCKED; [2] reserved, `00` |
| `02` | STATE_CHANGE | [0] from state, or `FF` for the boot transition into LOCKING (no prior state); [1] to state |
| `03` | COMMAND_RESULT | [0] opcode (`FF` if the write was empty); [1] result code; [2] write length, capped at 255 |
| `04` | CONNECT | [0–1] connection id, uint16 LE |
| `05` | DISCONNECT | [0–1] connection id, uint16 LE; [2–3] HCI reason, uint16 LE (e.g. `0x13` remote user terminated, `0x16` terminated by the lock, `0x08` supervision timeout) |
| `06` | IDLE_TIMEOUT | [0–1] connection id, uint16 LE (followed by the DISCONNECT) |

Unknown `type` values: show them raw or skip them; never fail the whole dump.

**Worked example.** A real log: the first boot after flashing (boot count 1),
then a phone connects (connection id 0). The phone writes cursor 0 and reads
one page of 65 bytes (count + 4 × 16):

```
write  00 00 00 00                     cursor = 0 (also the default after connecting)
read   04
       01 00 00 00 01 00 1A 04 00 00 01 01 00 00 00 00
       02 00 00 00 01 00 1B 04 00 00 02 FF 03 00 00 00
       03 00 00 00 01 00 03 08 00 00 02 03 00 00 00 00
       04 00 00 00 01 00 6E 14 00 00 04 00 00 00 00 00
write  05 00 00 00                     cursor = last seq + 1 = 5
read   00                              count = 0: done
```

| `seq` | `bootCount` | `msSinceBoot` | `type` | `data` | Decoded |
|---|---|---|---|---|---|
| 1 | 1 | 1050 (`1A 04 00 00`) | `01` BOOT | `01 00 00 00 00` | Reset reason `01` POWERON; boot target LOCKED; reserved |
| 2 | 1 | 1051 (`1B 04 00 00`) | `02` STATE_CHANGE | `FF 03 00 00 00` | From `FF` (boot, no prior state) to LOCKING |
| 3 | 1 | 2051 (`03 08 00 00`) | `02` STATE_CHANGE | `03 00 00 00 00` | LOCKING → LOCKED (after `T_act` = 1 s) |
| 4 | 1 | 5230 (`6E 14 00 00`) | `04` CONNECT | `00 00 00 00 00` | Connection id 0 |

**CoreBluetooth usage.** Write the cursor with
`writeValue(withUnsafeBytes(of: cursor.littleEndian) { Data($0) }, for: logChar, type: .withResponse)`,
wait for `didWriteValueFor`, then `readValue(for: logChar)`. A page is larger
than the default ATT MTU; CoreBluetooth performs the long read (ATT Read Blob)
automatically and delivers the whole value in one `didUpdateValueFor`.
Reading the log does not count as activity for the idle timeout (§6.6); a full
256-entry dump is about 32 round trips and fits easily within it.

---

## 5. Session sequence

A **session** is one connect → command(s) → disconnect cycle.

```
App (central)                                   Lock (peripheral)
   |                                               | advertising (20–40 ms interval)
   |-- scanForPeripherals([lockService]) ------->  |
   |<----------- didDiscover (adv + scan rsp) ---  |
   |-- stopScan                                    |
   |-- connect(peripheral) ---------------------->  | stops advertising; idle timer starts
   |<----------- didConnect --------------------   |
   |-- discoverServices([lockService]) ---------->  |
   |-- discoverCharacteristics([cmd, status, info, log]) ->
   |-- readValue(info) --------------------------->  |   (check protocol version)
   |-- setNotifyValue(true, status) ------------->  |   (nothing is notified now)
   |-- readValue(status) ------------------------->  |   current state; remember resultSeq = S
   |-- writeValue([0x01], .withResponse) -------->  | command received; idle timer restarts
   |<----------- didWriteValueFor (error == nil) - | ATT Write Response
   |<----------- didUpdateValueFor(status) ------  | notify: UNLOCKING, OK, resultSeq S+1
   |                                               | ... T_act ...
   |<----------- didUpdateValueFor(status) ------  | notify: UNLOCKED, resultSeq S+1 (settled)
   |-- cancelPeripheralConnection --------------->  |
   |<----------- didDisconnectPeripheral --------  | resumes advertising within 1 s
```

The write response and the result notification may arrive in either order.

Rules:

1. **[STABLE]** The app MUST wait for `didWriteValueFor` before sending the next
   write.
2. **[STABLE]** The app MUST disconnect (`cancelPeripheralConnection`) as soon
   as it has finished, ideally within 1 s of the last expected notification.
   While connected, the lock is invisible to every other phone. The lock
   enforces an idle timeout (§6.6) as a backstop only; do not rely on it.
3. **[STABLE]** Multiple commands in one session are allowed (e.g. unlock, then
   lock). Waiting for each write response (rule 1) guarantees each command is
   processed and gets its own result.
4. **[STABLE]** For a command that starts a move, the app SHOULD wait for the
   settled notification before reporting completion or sending the next
   command. A valid command sent during a move is not rejected: it is queued
   (up to 4) and executed after the move settles, and its result arrives then
   (§6.2, §6.5).

---

## 6. Behavior and guarantees

### 6.1 What a successful write means

`didWriteValueFor` with `error == nil` means **the lock received the bytes**.
It does **not** mean the command was accepted or the actuator moved. The
outcome arrives as a status notification (§6.5). The write is acknowledged
with success even when the result is `BUSY`, `UNKNOWN_COMMAND` or `MALFORMED`;
the lock does not use ATT errors for command results.

### 6.2 Command results

**[STABLE]** values and meanings; Phase 2 will add codes (e.g. authentication
failures). Every command produces exactly one result.

| Code | Name | Meaning | App action |
|---|---|---|---|
| `00` | OK | Executed; a transition has started (state is now UNLOCKING or LOCKING) | Wait for the settled notification (§6.5) |
| `01` | NO_CHANGE | Executed, but already in the requested state; nothing moved | Treat as success |
| `02` | BUSY | The queue of commands waiting for a move already holds 4; not executed. Rarely, the firmware's internal event queue overflowed; then `lastOpcode` may be `FF` (§4.4). | Wait for the settled state, then retry if still needed |
| `03` | UNKNOWN_COMMAND | Opcode not recognized; not executed | App bug or version mismatch; don't retry |
| `04` | MALFORMED | Write was not exactly 1 byte; not executed | App bug; don't retry |
| `FF` | (none) | Only in status `lastResult`: no command since boot | — |
| other | — | Unknown (newer firmware) | Treat as failure |

**Response matrix** (command × state when the lock receives it):

| Command | LOCKED | UNLOCKING | UNLOCKED | LOCKING |
|---|---|---|---|---|
| UNLOCK `01` | OK → UNLOCKING | queued (BUSY if 4 already queued) | NO_CHANGE | queued (BUSY if 4 already queued) |
| LOCK `00` | NO_CHANGE | queued (BUSY if 4 already queued) | OK → LOCKING | queued (BUSY if 4 already queued) |
| other opcode | UNKNOWN_COMMAND | UNKNOWN_COMMAND | UNKNOWN_COMMAND | UNKNOWN_COMMAND |
| length ≠ 1 | MALFORMED | MALFORMED | MALFORMED | MALFORMED |

**Queued commands.** A valid LOCK/UNLOCK that arrives while the lock is moving
(including the LOCKING right after boot, §6.3) gets no immediate result. It
waits, and queued commands are executed in the order received once the move
settles, each by the LOCKED/UNLOCKED columns above, so each gets `OK` or
`NO_CHANGE`. One that gets `OK` starts a new move, and any commands still
queued wait for that move too. A move in progress is never reversed or cut
short. Malformed and unknown commands are never queued; they are answered
immediately, even during a move.

**Timing.** A queued command's result arrives after the move(s) ahead of it
settle: up to (queue position + 1) × `T_act` after the write, where queue
position is the number of commands queued ahead of it (0 for the first).

### 6.3 Lock state and boot

- **[STABLE]** The state is readable and notified (§4.4).
- **[STABLE until position sensing exists]** The state means **commanded**, not
  physically verified. V1 has no sensor on the pin; a jammed or unpowered
  actuator still reports the commanded state. The app MAY say "Unlocked" based
  on the status, but MUST NOT present it as a physical guarantee in
  safety-relevant UI.
- **[STABLE]** A transition completes even if the app disconnects. The next
  connection reads the result.
- **[PROVISIONAL]** The state persists after disconnect: if the app unlocks and
  disconnects, the lock stays unlocked. Do not build features that depend on
  this. Phase 2 is expected to add automatic re-locking.
- **[STABLE]** **On power-up or reset the lock is locked (fail-secure).**
  Whatever state it was in before, including UNLOCKED or mid-move, it drives
  to LOCKED. The lock does not save its state across reboots.
- **[STABLE]** **Right after boot** the lock is in LOCKING for `T_act` while it
  drives to LOCKED, then settles. Usually this finishes before advertising
  starts (§2.3), but with a slower actuator a phone may connect during it. A
  valid command sent then is queued like any other (§6.2). If a client is
  subscribed, the settle is notified with `resultSeq` `00`.
- **No reboot indicator in status.** 1.1 has no reliable way to detect a
  reboot from the status characteristic. In particular, `resultSeq` going
  down does not mean a reboot, because it wraps `FF` → `00`. Reboots are
  visible in the event log: each BOOT entry, and the `bootCount` of every
  entry (§4.6).

### 6.4 Idempotence

**[PROVISIONAL]** Phase 1 LOCK and UNLOCK are idempotent: sending UNLOCK to an
unlocked lock returns `NO_CHANGE` and does nothing. After an unknown outcome
(§7), the app SHOULD read the status before retrying; retrying blindly is
still safe in Phase 1. **This will not be true in Phase 2** (§9). Do not build
generic retry logic that assumes it.

### 6.5 Status notifications and matching results

**[STABLE]** rules. They describe what is sent to a subscribed client:

1. **Every command write produces exactly one status notification**, whatever
   its opcode or length, sent when the command's result is decided. It
   carries `resultSeq` incremented by 1 (mod 256), the command's `lastOpcode`
   and `lastResult`, and the state **after** the command. For an executed
   UNLOCK with `OK` that state is already UNLOCKING (LOCKING for LOCK).
   - Malformed, unknown and `BUSY` results are notified immediately.
   - A command executed straight away is notified immediately.
   - A **queued** command is notified when it is executed, after the move
     ahead of it settles (§6.2): first the settle notification (rule 2), then
     the queued command's result.
2. **Settling produces exactly one more notification**, with `resultSeq`
   **unchanged** and the settled state. `lastOpcode`/`lastResult` still describe
   the most recent command result, which may belong to a later command (for
   example a MALFORMED write during the move).
3. Nothing else is notified. In particular, nothing is notified on connect or
   on subscribe. Reads never cause notifications.
4. `resultSeq` restarts at `00` on every boot.
5. `resultSeq` numbers results in the order they are **produced**, not the
   order the writes were sent. Queued commands are answered in the order they
   were written, but an immediate result (MALFORMED, UNKNOWN_COMMAND, BUSY)
   can overtake the result of an earlier, still-queued command.

**Matching a result to the command that caused it.** Only one app can be
connected, so only that app's commands advance `resultSeq`:

```swift
// Before writing: S = resultSeq from the latest status (initial read or last notification).
let expectedSeq = lastStatus.resultSeq &+ 1          // UInt8, wraps FF -> 00
pending = Pending(seq: expectedSeq, opcode: 0x01, deadline: now + 5)
peripheral.writeValue(Data([0x01]), for: commandChar, type: .withResponse)

// In handleStatus(s):
if let p = pending, s.resultSeq == p.seq {
    pending = nil
    handleResult(s.lastResult, state: s.state)    // OK -> keep waiting for settle
} else if waitingForSettle, s.state == 0x00 || s.state == 0x02 {
    // settled (resultSeq == the result's seq)
}
// A status whose resultSeq == S is a settle of an earlier command; it is not your result.
```

- Wait for the matching result with a timeout: 5 s recommended, plus
  (k + 1) × `T_act` if the lock was moving when you wrote, where k is the
  number of your earlier commands still waiting for a result (§6.2, Timing).
  If it doesn't
  arrive, read the status: if `resultSeq == expectedSeq`, its `lastResult` is
  your result (the notification was lost, which is a firmware bug worth
  reporting); if it is still `S`, the command was not processed.
- Wait for the settled notification with a timeout of `T_act` plus margin
  (10 s recommended while the servo's `T_act` is unknown).
- If you send a second valid command before the first one's result arrives
  (not recommended), its result has `resultSeq` S+2. That holds as long as
  you send only valid 1-byte commands and keep no more than 4 queued (rule 5).

### 6.6 Idle timeout

- **[STABLE]** The lock disconnects a client that has written **no command**
  for `T_idle`. Only writes to the command characteristic restart the timer
  (with any content, including invalid ones). Reads, log-cursor writes and
  subscribing do not count.
- **[PROVISIONAL]** `T_idle` = 30 s, measured from the connection or the last
  command write.
- The app sees `didDisconnectPeripheral` with a non-nil error (typically
  `CBError.peripheralDisconnected`). If the app had nothing pending, this is a
  normal end of session, not a failure. The lock logs an IDLE_TIMEOUT event and
  resumes advertising within 1 s.
- A transition in progress at that moment completes normally (§6.3).

---

## 7. Errors and timeouts

CoreBluetooth does not time out most operations by itself; for example, a
pending `connect` waits indefinitely. **The app MUST implement its own
timeouts.** Recommended values:

| Step | Timeout | On timeout or failure |
|---|---|---|
| Scan | 10 s | Report "Lock not found". Likely causes: out of range, lock unpowered, or **another phone is connected** (§2.3). |
| Connect | 10 s | `cancelPeripheralConnection`, report "Could not connect". Retrying once is reasonable. |
| Service + characteristic discovery | 5 s | Disconnect, report "Could not connect". |
| Service present, command characteristic missing | — | Disconnect, report **incompatible firmware**. Do not retry. If this happens during development right after a firmware change, see the GATT caching note below. |
| Status characteristic missing | — | 1.0 firmware: fall back to 1.0 behavior or report "lock firmware too old" (§3). Do not retry. |
| Info: protocol MAJOR ≠ 1 | — | Disconnect, report **incompatible firmware**. |
| Read / subscribe | 5 s | Disconnect, report "Could not connect". |
| Status value shorter than 4 bytes (info: 5) | — | Protocol error: disconnect, report a firmware problem. |
| Write response (`didWriteValueFor`) | 5 s | Disconnect. **The outcome is unknown.** The lock may or may not have acted. Reconnect and read the status. |
| `didWriteValueFor` with non-nil `error` | — | The lock did not receive the command. Report the failure; do not assume the state changed. |
| Result notification (§6.5) | 5 s after the write, plus (k + 1) × `T_act` if the command may have been queued (§6.5) | Read the status: `resultSeq` tells whether the command was processed and what its result was. If it wasn't processed, the outcome is "not executed". |
| Result `BUSY` | — | Wait for the settled notification, then decide whether to retry. |
| Result `UNKNOWN_COMMAND` / `MALFORMED` | — | App bug or version mismatch. Don't retry. |
| Settled notification after `OK` | `T_act` + margin (10 s) | Read the status. If still transitional, report "Lock did not finish moving"; disconnect. |
| `didDisconnectPeripheral` after ≥ 30 s without a command | — | Idle timeout (§6.6). Normal if nothing was pending. |
| Unexpected `didDisconnectPeripheral` (error ≠ nil) | — | If a result was pending, the outcome is unknown: reconnect and read the status. A transition in progress completes anyway (§6.3). |

**Signal loss.** If the phone walks out of range, the link drops after the
connection supervision timeout (a few seconds; iOS chooses it). The lock then
resumes advertising automatically.

**GATT caching (development note).** iOS may cache a peripheral's GATT table.
After a firmware update changes the table (e.g. 1.0 → 1.1), the app may see
stale services, for example a missing status characteristic, which looks like
1.0 firmware, until Bluetooth is toggled off and on in iOS Settings.
Rediscovering on every connection (§3) limits the impact but does not fully
prevent it.

---

## 8. Security

**[PROVISIONAL] Phase 1 has no security at all.**

- No pairing, bonding, or link encryption. All traffic is plaintext.
- **Anyone in BLE range can unlock the lock** using a generic app such as nRF
  Connect or LightBlue by writing `0x01`, and can read its state and event log.
- Phase 1 firmware is for development and demos only and MUST NOT be deployed
  on a real bike.

**[STABLE]** The app MUST NOT initiate BLE pairing. Phase 2 security is
implemented in the application protocol (credentials verified by the lock), not
through iOS pairing.

---

## 9. Phase 2 preview (non-normative)

> Nothing in this section is final. **Do not implement any byte formats or UUID
> assignments from this section.** It exists so the Phase 1 app is shaped in a
> way that survives Phase 2.

### 9.1 What Phase 2 adds

The lock stays **permanently offline**: no Wi-Fi, no cellular, no network time.
Unlocking will require a **credential** that the app obtains from the backend
*ahead of time* while the phone has internet:

- Each credential is bound to one lock ID and an expiry time, and is signed
  (HMAC) with a secret known only to the backend and that specific lock.
- At the lock, a **challenge–response** round prevents replay. The lock sends a
  fresh random nonce, and the app proves possession of the credential for that
  nonce.

A likely session shape (illustrative only):

```
connect → discover → read lock info (lock ID, protocol version)
        → subscribe to status notifications
        → request challenge → lock returns nonce
        → write credential + proof → lock notifies result code
        → disconnect
```

Version 1.1 already provides the info read, the status subscription and the
result notification in this shape.

### 9.2 What will change

| Phase 1 (1.1) behavior | Phase 2 |
|---|---|
| `0x01` / `0x00` unlock/lock with no credential | **Removed.** Unauthenticated commands will be rejected. |
| 1-byte writes; any other length → `MALFORMED` | Writes of roughly 60–150 bytes (opcode + credential + proof). |
| Command, status, info and log characteristics (`…1C01`–`…1C04`) | Kept; further characteristics in the reserved range `8F2A1C05-…`–`8F2A1CFF-…`. |
| Result codes `00`–`04` in the status notification | Same mechanism; new codes added (e.g. authentication failures). |
| Status: state + last result | Same layout; fields may be appended (§0). More states possible. |
| Info: protocol + firmware version | Same layout; lock ID appended. |
| Event log readable by anyone, never cleared | Log acknowledgement/clearing and upload to the backend; access likely restricted. |
| Commands idempotent, retry always safe | Each nonce is single-use. A retry means starting the handshake again from "request challenge". |
| Hard-coded name `VT-LOCK-001` | Per-lock identifier; naming scheme TBD. |
| Idle timeout 30 s | Kept; value may change. |
| Always LOCKED after power-up or reset | Kept (fail-secure). |
### 9.3 Build the Phase 1 app so this is a small change

1. **Separate transport from protocol.** Make the BLE layer generic: connect,
   discover by UUID, read, write-with-response of arbitrary length, subscribe to
   notifications, with per-step timeouts. Put all byte encoding (opcodes,
   payloads, status decoding) in one separate module. Phase 2 should mostly
   replace that module.
2. **Model a session as several request/response steps,** not a single
   "write and done" call. Match each response to its request (`resultSeq`,
   §6.5).
3. **Decode tolerantly:** accept longer values, unknown enum values (§0).
4. **Do not assume a 20-byte write limit.** For `.withResponse` writes,
   CoreBluetooth splits long values across several packets automatically
   (`maximumWriteValueLength(for: .withResponse)`), and the lock reassembles
   them.
5. **Plan for offline credential storage.** The app will need to download and
   store credentials before the rider reaches the bike, because the lock can
   never check with a server.
6. **Avoid everything marked [PROVISIONAL]** outside the one module that
   isolates it.

---

## 10. Testing without the app

- **nRF Connect** or **LightBlue** (iOS/Android): connect to `VT-LOCK-001`.
  Enable notifications on `8F2A1C02-…` (status), then write `01` or `00` to
  `8F2A1C01-…` using a write *with* response and watch the status notifications.
  Read `8F2A1C03-…` for the versions. For the log, write `00000000` to
  `8F2A1C04-…` and read it.
- **`tools/lock_client.py`** (laptop, Python + bleak): `status`, `unlock`,
  `lock`, `send <hex>` (prints the result code), `watch` (streams
  notifications), `log` (decoded event log), `info` (versions and GATT table),
  and `acceptance` (the V1 acceptance sequence). See the module docstring.
- **Serial monitor** at 115200 baud shows the firmware's view: boot banner,
  `[ble]`, `[cmd]`, `[state]`, `[act]`, `[log]` lines.

---

## 11. Version history

| Version | Date | Change |
|---|---|---|
| 1.0.0 | 2026-10-06 | Initial Phase 1 spec. |
| 1.1.0 | 2026-10-12 | V1 firmware, backward compatible with a correct 1.0 app. Added status (`…1C02`), info (`…1C03`) and event log (`…1C04`) characteristics; result codes delivered by status notification, matched by `resultSeq`; forward-compatibility rule (§0). Unknown opcodes now report `UNKNOWN_COMMAND`; writes of length ≠ 1 now report `MALFORMED` and are not executed. Valid commands sent during a move are queued (up to 4) and executed in order after it settles; `BUSY` only when the queue is full. Added 30 s idle timeout (reset by any command write). One phone at a time is enforced; a second connection is refused. Event log entries are readable only once saved to flash, so a seen `seq` is never reused. Power-up/reset still always drives to LOCKED ([STABLE], as in 1.0). Updated quick reference, error table and Phase 2 table. |

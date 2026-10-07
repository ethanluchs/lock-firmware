# VT-LOCK BLE Interface Specification

| | |
|---|---|
| **Spec version** | **1.0.0** (Phase 1) |
| **Status** | Phase 1: stable, implement against it. Phase 2 (§9): preview only, not implementable yet. |
| **Firmware** | `src/main.cpp` (ESP32, Arduino core 3.3.x, Bluedroid BLE stack) |
| **Last updated** | 2026-10-06 |
| **Firmware owner** | ethanluchs |

This document is the contract between the lock firmware and the iOS app. If the
firmware and this document disagree, that is a bug. Report it rather than
working around it.

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
additive and backward compatible. PATCH means documentation fixes only.

**Notation.** Bytes are written in hex (`0x01`). UUIDs are case-insensitive;
CoreBluetooth's `CBUUID` compares them correctly regardless of case.

---

## 1. Quick reference

| Item | Value | Stability |
|---|---|---|
| Lock service UUID | `8F2A1C00-5B7E-4D19-9A3F-6C2E81D4B70A` | [STABLE] |
| Command characteristic UUID | `8F2A1C01-5B7E-4D19-9A3F-6C2E81D4B70A` | [STABLE] |
| Command characteristic properties | Write (with response) only | [STABLE] |
| Reserved for future characteristics | `8F2A1C02-…` through `8F2A1CFF-…` (same suffix `-5B7E-4D19-9A3F-6C2E81D4B70A`) | [STABLE] |
| Advertised name | `VT-LOCK-001` | [PROVISIONAL] |
| Command `0x01` | Unlock | [PROVISIONAL] |
| Command `0x00` | Lock | [PROVISIONAL] |
| Payload length | Exactly 1 byte | [PROVISIONAL] |
| Pairing / encryption | None | [STABLE]: Phase 2 authenticates at the application layer, not via BLE pairing |

```swift
let lockServiceUUID = CBUUID(string: "8F2A1C00-5B7E-4D19-9A3F-6C2E81D4B70A")
let commandCharUUID = CBUUID(string: "8F2A1C01-5B7E-4D19-9A3F-6C2E81D4B70A")
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
  is connected and resumes immediately after disconnect. While one phone is
  connected, other phones cannot discover it.
- **[STABLE]** After power-on, the lock begins advertising within about 2 s
  (1 s boot delay plus BLE start-up).

---

## 3. GATT layout

```
Lock Service  8F2A1C00-5B7E-4D19-9A3F-6C2E81D4B70A   (primary)
└── Command   8F2A1C01-5B7E-4D19-9A3F-6C2E81D4B70A   properties: Write
                                                     no descriptors, no CCCD
```

The lock also exposes the standard Generic Access (`0x1800`) and Generic
Attribute (`0x1801`) services, which iOS handles itself. The app SHOULD ignore
them.

The app MUST:

- **[STABLE]** Look up characteristics **by UUID**, never by position or
  attribute handle. Phase 2 adds characteristics to this service, which shifts
  handles and ordering.
- **[STABLE]** Run `discoverServices` / `discoverCharacteristics` on **every**
  connection rather than caching `CBCharacteristic` objects across connections.
- **[STABLE]** Treat "lock service found but command characteristic missing" as
  *incompatible firmware* (§7), not as a transient error.

Phase 1 has **no readable or notifying characteristics**. The lock state cannot
be read (see §6.3).

---

## 4. Command characteristic: byte format

### 4.1 Write type

**[STABLE]** The app MUST write with `type: .withResponse`. The characteristic
does not declare the Write Without Response property, and CoreBluetooth will
refuse to send a `.withoutResponse` write to it (nothing reaches the lock).

### 4.2 Framing

**[STABLE]** Byte 0 of every write to the command characteristic is an
**opcode**. In Phase 2, some opcodes will carry a payload in bytes 1..N. Keep
this "first byte = opcode" structure in the app's encoder.

**[PROVISIONAL]** Phase 1 opcodes:

| Write (hex) | Length | Meaning | Lock behavior |
|---|---|---|---|
| `01` | 1 | UNLOCK | Actuator energized (unlocked). |
| `00` | 1 | LOCK | Actuator de-energized (locked). |
| `02`–`FF` | 1 | Reserved | Ignored, no state change. Write still succeeds (§6.2). |
| *(empty)* | 0 | — | Ignored, no state change. Write still succeeds. |

**[PROVISIONAL]** The app MUST send **exactly one byte**. Phase 1 firmware
happens to read only the first byte and silently ignore any extra bytes. **Do
not rely on that.** In Phase 2, trailing bytes carry meaning, so a stray extra
byte will become an error.

### 4.3 Example

```swift
// Unlock (Phase 1, unauthenticated)
peripheral.writeValue(Data([0x01]), for: commandChar, type: .withResponse)
// Result arrives in peripheral(_:didWriteValueFor:error:)
```

---

## 5. Session sequence

A **session** is one connect → command(s) → disconnect cycle.

```
App (central)                                   Lock (peripheral)
   |                                               | advertising (20–40 ms interval)
   |-- scanForPeripherals([lockService]) ------->  |
   |<----------- didDiscover (adv + scan rsp) ---  |
   |-- stopScan                                    |
   |-- connect(peripheral) ---------------------->  | stops advertising
   |<----------- didConnect --------------------   |
   |-- discoverServices([lockService]) ---------->  |
   |-- discoverCharacteristics([command]) ------->  |
   |-- writeValue([0x01], .withResponse) -------->  | onWrite: command queued
   |<----------- didWriteValueFor (error == nil) - | ATT Write Response
   |                                               | actuator changes ≤ ~20 ms later
   |-- cancelPeripheralConnection --------------->  |
   |<----------- didDisconnectPeripheral --------  | resumes advertising immediately
```

Rules:

1. **[STABLE]** The app MUST wait for `didWriteValueFor` before sending the next
   write.
2. **[STABLE]** The app MUST disconnect (`cancelPeripheralConnection`) as soon
   as it has finished, ideally within 1 s of the last write response. **The
   Phase 1 lock has no idle timeout**: a phone that stays connected makes the
   lock invisible to every other phone until the link drops.
3. **[STABLE]** Multiple commands in one session are allowed (e.g. unlock, then
   lock). Commands sent less than about 10 ms apart may be coalesced, in which
   case only the last one is guaranteed to execute. Waiting for each write
   response (rule 1) avoids this.

---

## 6. Behavior and guarantees

### 6.1 What a successful write means

`didWriteValueFor` with `error == nil` means **the lock received the bytes**.
It does **not** mean the actuator moved. The firmware acknowledges the write
immediately and actuates afterward (typically within 20 ms).

### 6.2 Unknown and malformed commands

**[PROVISIONAL]** Reserved opcodes (`0x02`–`0xFF`) and empty writes are
**acknowledged successfully and then ignored**. The app gets no error, and the
lock state does not change. The firmware logs them on its serial console only.
Phase 2 will reject them with an explicit error code instead (§9).

### 6.3 Lock state

- **[PROVISIONAL]** Phase 1 offers **no way to read the lock state** and no
  confirmation of actuation. The app MUST NOT display "Unlocked" as a fact. It
  MAY say something like "Unlock sent".
- **[PROVISIONAL]** The state persists after disconnect: if the app unlocks and
  disconnects, the lock stays unlocked. Do not build features that depend on
  this. Phase 2 is expected to add automatic re-locking.
- **[STABLE]** On power-up or reset the lock is **locked** (fail-secure).

### 6.4 Idempotence

**[PROVISIONAL]** Phase 1 commands are idempotent: sending UNLOCK to an unlocked
lock does nothing. Retrying a command is therefore always safe in Phase 1.
**This will not be true in Phase 2** (§9). Do not build generic retry logic
that assumes it.

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
| Write response (`didWriteValueFor`) | 5 s | Disconnect. **The outcome is unknown.** The lock may or may not have acted. |
| `didWriteValueFor` with non-nil `error` | — | The lock did not accept the write. Report the failure; do not assume the state changed. |
| Unexpected `didDisconnectPeripheral` (error ≠ nil) | — | If no write response was received, the outcome is unknown. In Phase 1 it is safe to reconnect and resend (§6.4). |

**Signal loss.** If the phone walks out of range, the link drops after the
connection supervision timeout (a few seconds; iOS chooses it). The lock then
resumes advertising automatically.

**GATT caching (development note).** iOS may cache a peripheral's GATT table.
After a firmware update changes the table, the app may see stale services until
Bluetooth is toggled off and on in iOS Settings. Rediscovering on every
connection (§3) limits the impact but does not fully prevent it.

---

## 8. Security

**[PROVISIONAL] Phase 1 has no security at all.**

- No pairing, bonding, or link encryption. All traffic is plaintext.
- **Anyone in BLE range can unlock the lock** using a generic app such as nRF
  Connect or LightBlue by writing `0x01`.
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

### 9.2 What will change

| Phase 1 behavior | Phase 2 |
|---|---|
| `0x01` / `0x00` unlock/lock with no credential | **Removed.** Unauthenticated commands will be rejected. |
| 1-byte writes | Writes of roughly 60–150 bytes (opcode + credential + proof). |
| Only the command characteristic | New characteristics in the reserved UUID range `8F2A1C02-…`–`8F2A1CFF-…` (read and notify). |
| Unknown commands silently ignored | Explicit result/error codes, delivered by notification. |
| No lock state or feedback | Status reported by notification. |
| Commands idempotent, retry always safe | Each nonce is single-use. A retry means starting the handshake again from "request challenge". |
| Hard-coded name `VT-LOCK-001` | Per-lock identifier; naming scheme TBD. |
| No idle timeout | The lock will drop idle connections. |

### 9.3 Build the Phase 1 app so this is a small change

1. **Separate transport from protocol.** Make the BLE layer generic: connect,
   discover by UUID, read, write-with-response of arbitrary length, subscribe to
   notifications, with per-step timeouts. Put all byte encoding (opcodes,
   payloads) in one separate module. Phase 2 should mostly replace that module.
2. **Model a session as several request/response steps,** not a single
   "write and done" call.
3. **Support notifications in the transport layer now,** even though Phase 1
   does not use them.
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

- **nRF Connect** or **LightBlue** (iOS/Android): connect to `VT-LOCK-001`, open
  the characteristic `8F2A1C01-…`, and write the byte `01` or `00` using a
  write *with* response.
- **`tools/lock_client.py`** (laptop, Python + bleak): runs the same session
  from the command line, e.g. `python tools/lock_client.py unlock`. See the
  module docstring.
- **Serial monitor** at 115200 baud shows the firmware's view: `[ble] client
  connected`, `[ble] write: 0x01 (1 bytes)`, `[act] UNLOCKED`, and so on.

---

## 11. Version history

| Version | Date | Change |
|---|---|---|
| 1.0.0 | 2026-10-06 | Initial Phase 1 spec. |

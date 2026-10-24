# iOS Integration Guide (V1)

| | |
|---|---|
| **For** | The iOS developer building the V1 app against the lock |
| **Protocol** | `ble-spec.md` **1.1.0** (firmware 0.2.x reports protocol 1.1) |
| **Date** | 2026-10-22 |
| **Firmware contact** | ethanluchs |

This is the "start here" guide. It tells you what to build, in what order, and
what will trip you up. It does **not** repeat the byte formats: those live in
[`ble-spec.md`](ble-spec.md), which is the contract between the app and the
lock. If this guide and `ble-spec.md` disagree, `ble-spec.md` wins. If the lock
and `ble-spec.md` disagree, that's a firmware bug, so tell the firmware team
and don't work around it.

---

## 1. The lock in one minute

- An ESP32 board running our firmware. It is a **BLE peripheral**, and the
  phone is the **central**. There's no Wi-Fi or internet on the lock, ever.
- It has four states: `LOCKED`, `UNLOCKING`, `UNLOCKED`, `LOCKING`. A
  move takes `T_act` (1 s with the LED stand-in; the servo value is not
  decided yet).
- You send a 1-byte command (`0x01` unlock, `0x00` lock). The lock answers
  through a **status notification**, not through the write itself.
- **One phone at a time.** While a phone is connected, the lock stops
  advertising and other phones can't see it. It disconnects a phone that sends
  no command for 30 s.
- On power-up or reset it always drives to `LOCKED`.
- **V1 has no security.** Anyone with nRF Connect can unlock it. That's
  intentional for the demo. Real security (signed credentials from the backend)
  is Phase 2, and the app should be shaped so that's a contained change (§4).

---

## 2. What to build for V1

### 2.1 Scope

| Feature | In V1? | Notes |
|---|---|---|
| Find the lock, connect, lock/unlock | Yes (demo) | The core flow (§3). |
| Show lock state and the result of each command | Yes (demo) | See §2.2: "live" only while connected. |
| Version / compatibility check | Yes, silently | Read the info characteristic every connection; refuse protocol major ≠ 1. Display optional. |
| Accounts and backend | App-side only | The lock knows nothing about users. See §7. |
| Event log viewer | No | The lock supports it (`ble-spec.md` §4.6), but it's not needed for the demo. |
| Auth, credentials, multiple locks, background unlock | No | Phase 2 / out of scope. |

### 2.2 "Live state" means "live while connected"

The obvious design (connect when the screen opens, stay connected, show the
state live) **does not work** with this lock:

- the lock drops you after 30 s with no command (`ble-spec.md` §6.6), and
- while you're connected nobody else can find the lock (§2.3 of the spec).

Instead, make every interaction a **short session**:

| User action | Session |
|---|---|
| Opens the lock screen / pulls to refresh | connect → read status → disconnect |
| Taps Unlock or Lock | connect → read status → send command → wait for result and settle → disconnect |

During a session, show state changes live from notifications (e.g. "Unlocking…"
→ "Unlocked"). Between sessions, show the **last known state and when you read
it** ("Unlocked · checked 2 min ago"), not something that looks live.

Never present the state as a physical guarantee. V1 has no sensor on the pin,
so `UNLOCKED` means "the lock commanded the actuator to unlock".

---

## 3. How a session works

### 3.1 BLE basics (skip if you've used CoreBluetooth)

- **CBCentralManager** scans and connects. **CBPeripheral** is the lock once
  you've found it. Everything is asynchronous: you call a method and the
  answer arrives later in a delegate callback.
- A **service** groups **characteristics**. Each characteristic is a small
  value you can **read**, **write**, or **subscribe to** (notify). The lock has
  one service with four characteristics (§3.2).
- After connecting you must **discover** services and then characteristics
  before you can use them, and you must do it again on every connection.
- You must keep a **strong reference** to the `CBPeripheral` from
  `didDiscover`, or iOS cancels the connection.
- Wait for `centralManagerDidUpdateState` to report `.poweredOn` before you
  scan.
- Add `NSBluetoothAlwaysUsageDescription` to `Info.plist`, or the app crashes
  when it creates the central manager.
- **The iOS Simulator has no Bluetooth.** All BLE testing needs a real iPhone.
- CoreBluetooth has almost no built-in timeouts (a `connect` waits forever).
  You must add your own timeouts (`ble-spec.md` §7 has the values).

### 3.2 The four characteristics

All UUIDs share the suffix `-5B7E-4D19-9A3F-6C2E81D4B70A`.

| Characteristic | UUID prefix | You... | V1 app uses it? |
|---|---|---|---|
| Service | `8F2A1C00` | scan for it | Yes |
| Command | `8F2A1C01` | write 1 byte, **with response** | Yes |
| Status | `8F2A1C02` | read + subscribe; 4 bytes: state, lastOpcode, lastResult, resultSeq | Yes |
| Info | `8F2A1C03` | read; 5 bytes: protocol major/minor, firmware major/minor/patch | Yes (compat check) |
| Log | `8F2A1C04` | write a cursor, then read pages | No (V1 optional) |

Byte layouts: `ble-spec.md` §4.

### 3.3 The order of calls

This is `ble-spec.md` §5 mapped to CoreBluetooth. Each step has a timeout;
on any failure, disconnect and tell the user.

| # | You call | You wait for | Timeout |
|---|---|---|---|
| 1 | `scanForPeripherals(withServices: [service])` | `didDiscover` → `stopScan()` | 10 s ("Lock not found", maybe in use by another phone) |
| 2 | `connect(peripheral)` | `didConnect` (or `didFailToConnect`) | 10 s |
| 3 | `discoverServices([service])`, then `discoverCharacteristics(...)` | `didDiscoverServices`, `didDiscoverCharacteristicsFor` | 5 s |
| 4 | `readValue(for: info)` | `didUpdateValueFor` | 5 s. Protocol major ≠ 1 → incompatible firmware. |
| 5 | `setNotifyValue(true, for: status)` | `didUpdateNotificationStateFor` | 5 s |
| 6 | `readValue(for: status)` | `didUpdateValueFor` | 5 s. Remember `resultSeq` as `S`. |
| 7 | `writeValue(Data([0x01]), for: command, type: .withResponse)` | `didWriteValueFor` | 5 s |
| 8 | (nothing) | status with `resultSeq == S+1` = **your result** | 5 s, plus `T_act` per move ahead of you |
| 9 | (nothing, if result was OK) | status with a settled state (`LOCKED`/`UNLOCKED`) = **done** | 10 s while servo `T_act` is unknown |
| 10 | `cancelPeripheralConnection` | `didDisconnectPeripheral` | Disconnect right away; don't linger. |

For a refresh-only session, stop after step 6.

### 3.4 Reading the result: the part most likely to go wrong

- `didWriteValueFor` with no error only means **the lock received the bytes**.
  It doesn't mean the lock accepted the command.
- The result arrives as a **status notification**. Read responses and
  notifications both come through `didUpdateValueFor` and look identical;
  treat each one as "the latest status".
- Match your result by **`resultSeq`**, not by `lastResult`. Before writing,
  `expectedSeq = lastStatus.resultSeq &+ 1` (wrapping `UInt8` add). The first
  status with that `resultSeq` is your result.
- An `OK` result arrives with the state already `UNLOCKING`/`LOCKING`. One more
  notification follows when the move settles, with the **same** `resultSeq`.
  That's "done", not a second result.
- Result → UI:

  | Result | Meaning | Show |
  |---|---|---|
  | `OK` (0) | Moving | "Unlocking…", then "Unlocked" on settle |
  | `NO_CHANGE` (1) | Already in that state | Success ("Already unlocked") |
  | `BUSY` (2) | Too many queued commands | Wait for settle, then retry if still needed |
  | `UNKNOWN_COMMAND` (3), `MALFORMED` (4) | App bug | Error; don't retry; tell the firmware team |
  | anything else | Newer firmware | Treat as failure |

- If the result never arrives, read the status. If `resultSeq == expectedSeq`,
  its `lastResult` is your result. If it's still `S`, your command wasn't
  processed.
- If the write times out or the link drops before the result, **the outcome is
  unknown**: the lock may have moved. Reconnect and read the status before you
  tell the user anything.

The full rules, with byte-level worked examples, are in `ble-spec.md` §4.4 and
§6.5.

---

## 4. Recommended app structure

Phase 2 replaces the 1-byte commands with a challenge–response handshake
(`ble-spec.md` §9). Split the code so that's a contained change:

```
LockProtocol   pure Swift, no CoreBluetooth. UUIDs, command bytes, status/info
               decoding. Everything [PROVISIONAL] lives here. Unit-tested.
LockSession    the session state machine (§3.3): steps, timeouts, resultSeq
               matching. Talks to a transport protocol, so it can run against
               a fake lock in tests.
BLETransport   thin CoreBluetooth wrapper: scan, connect, discover by UUID,
               read, write-with-response (any length), subscribe.
UI             shows LockSession's published state.
```

Rules from `ble-spec.md` §0 and §3 that belong in this structure:

- Decode tolerantly. A value **longer** than documented is fine (ignore the
  extra bytes), and a value **shorter** is a protocol error. Never check
  `count == 4`; check `count >= 4`.
- Unknown state or result values must not crash. Map them to `.unknown(byte)`.
- Look up characteristics **by UUID**, never by position, and don't cache
  `CBCharacteristic` objects across connections.
- Don't assume a 20-byte write limit (Phase 2 writes are ~60–150 bytes; iOS
  splits them for you on `.withResponse`).

### 4.1 `LockProtocol` (starting point)

A sketch to start from; it hasn't been compiled. It covers everything the V1
app needs to decode.

```swift
import CoreBluetooth

enum LockUUID {
    static let service = CBUUID(string: "8F2A1C00-5B7E-4D19-9A3F-6C2E81D4B70A")
    static let command = CBUUID(string: "8F2A1C01-5B7E-4D19-9A3F-6C2E81D4B70A")
    static let status  = CBUUID(string: "8F2A1C02-5B7E-4D19-9A3F-6C2E81D4B70A")
    static let info    = CBUUID(string: "8F2A1C03-5B7E-4D19-9A3F-6C2E81D4B70A")
}

enum LockState: Equatable {
    case locked, unlocking, unlocked, locking, unknown(UInt8)

    init(_ b: UInt8) {
        switch b {
        case 0: self = .locked
        case 1: self = .unlocking
        case 2: self = .unlocked
        case 3: self = .locking
        default: self = .unknown(b)
        }
    }
    var isSettled: Bool { self == .locked || self == .unlocked }
}

enum CommandResult: Equatable {
    case ok, noChange, busy, unknownCommand, malformed
    case noneSinceBoot          // 0xFF in lastResult
    case unknown(UInt8)

    init(_ b: UInt8) {
        switch b {
        case 0: self = .ok
        case 1: self = .noChange
        case 2: self = .busy
        case 3: self = .unknownCommand
        case 4: self = .malformed
        case 0xFF: self = .noneSinceBoot
        default: self = .unknown(b)
        }
    }
}

struct LockStatus: Equatable {
    let state: LockState
    let lastOpcode: UInt8
    let lastResult: CommandResult
    let resultSeq: UInt8

    init?(_ data: Data) {
        let b = [UInt8](data)
        guard b.count >= 4 else { return nil }   // longer is fine
        state = LockState(b[0]); lastOpcode = b[1]
        lastResult = CommandResult(b[2]); resultSeq = b[3]
    }
}

struct LockInfo: Equatable {
    let protocolMajor, protocolMinor, fwMajor, fwMinor, fwPatch: UInt8

    init?(_ data: Data) {
        let b = [UInt8](data)
        guard b.count >= 5 else { return nil }
        (protocolMajor, protocolMinor, fwMajor, fwMinor, fwPatch) = (b[0], b[1], b[2], b[3], b[4])
    }
    var isCompatible: Bool { protocolMajor == 1 && protocolMinor >= 1 }
    var firmwareString: String { "\(fwMajor).\(fwMinor).\(fwPatch)" }
}

// [PROVISIONAL] Phase 1 only. Phase 2 replaces this with credential + proof.
enum LockCommand {
    case lock, unlock
    var payload: Data { Data([self == .unlock ? 0x01 : 0x00]) }
}
```

### 4.2 `LockSession` / `BLETransport`

The skeleton below shows where each step from §3.3 goes in the delegate
callbacks. It's a sketch: the timeouts, error reporting and
transport/session split are left for you to fill in.

```swift
final class LockClient: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private var central: CBCentralManager!
    private var lock: CBPeripheral?                 // strong reference required
    private var chars: [CBUUID: CBCharacteristic] = [:]
    private var lastStatus: LockStatus?
    private var pendingSeq: UInt8?                  // resultSeq we're waiting for
    private var waitingForSettle = false
    private var queuedCommand: LockCommand?

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    func run(_ command: LockCommand?) {              // nil = refresh only
        queuedCommand = command
        central.scanForPeripherals(withServices: [LockUUID.service])   // + 10 s timeout
    }

    // MARK: Central
    func centralManagerDidUpdateState(_ c: CBCentralManager) { /* require .poweredOn */ }

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral,
                        advertisementData: [String: Any], rssi: NSNumber) {
        c.stopScan()
        lock = p
        c.connect(p)                                // + 10 s timeout
    }

    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
        p.delegate = self
        p.discoverServices([LockUUID.service])
    }

    func centralManager(_ c: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        // Could not connect. Also how "in use by another phone" can look.
    }

    func centralManager(_ c: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
        // If pendingSeq != nil or waitingForSettle: outcome unknown -> reconnect + read status.
        lock = nil; chars = [:]
    }

    // MARK: Peripheral
    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        guard let svc = p.services?.first(where: { $0.uuid == LockUUID.service }) else { return fail() }
        p.discoverCharacteristics([LockUUID.command, LockUUID.status, LockUUID.info], for: svc)
    }

    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor s: CBService, error: Error?) {
        for c in s.characteristics ?? [] { chars[c.uuid] = c }
        guard chars[LockUUID.command] != nil else { return fail() }  // incompatible firmware
        guard chars[LockUUID.status] != nil, let info = chars[LockUUID.info] else {
            return fail()                                            // v1.0 firmware: too old
        }
        p.readValue(for: info)
    }

    func peripheral(_ p: CBPeripheral, didUpdateValueFor c: CBCharacteristic, error: Error?) {
        guard error == nil, let v = c.value else { return fail() }
        switch c.uuid {
        case LockUUID.info:
            guard let info = LockInfo(v), info.isCompatible else { return fail() }
            p.setNotifyValue(true, for: chars[LockUUID.status]!)     // subscribe BEFORE reading
        case LockUUID.status:
            guard let s = LockStatus(v) else { return fail() }       // protocol error
            handleStatus(s, p)
        default: break
        }
    }

    func peripheral(_ p: CBPeripheral, didUpdateNotificationStateFor c: CBCharacteristic, error: Error?) {
        guard error == nil else { return fail() }
        p.readValue(for: c)        // the lock sends nothing on subscribe; this read is required
    }

    private func handleStatus(_ s: LockStatus, _ p: CBPeripheral) {
        let isFirstRead = (lastStatus == nil)
        lastStatus = s
        // update UI with s.state here

        if isFirstRead {
            guard let cmd = queuedCommand else { return finish(p) }  // refresh only
            queuedCommand = nil
            pendingSeq = s.resultSeq &+ 1
            p.writeValue(cmd.payload, for: chars[LockUUID.command]!, type: .withResponse)
            return                                                   // + write/result timeouts
        }
        if let seq = pendingSeq, s.resultSeq == seq {                // this is OUR result
            pendingSeq = nil
            switch s.lastResult {
            case .ok: waitingForSettle = true                        // + 10 s settle timeout
            case .noChange: return finish(p)
            default: return fail()                                   // busy / bug / unknown
            }
        }
        if waitingForSettle && pendingSeq == nil && s.state.isSettled {
            waitingForSettle = false
            finish(p)
        }
    }

    func peripheral(_ p: CBPeripheral, didWriteValueFor c: CBCharacteristic, error: Error?) {
        // error != nil: the lock did NOT get the command. nil: received, result still pending.
        // The write response and the result notification can arrive in either order.
    }

    private func finish(_ p: CBPeripheral) { central.cancelPeripheralConnection(p) }
    private func fail() { if let p = lock { central.cancelPeripheralConnection(p) } /* + report */ }
}
```

`tools/lock_client.py` (Python) implements the same flow end to end, including
timeouts and every error case. If you're unsure how a corner case should
behave, look at how `LockClient.command()` handles it.

---

## 5. Gotchas checklist

Every one of these has cost someone time on a BLE project. Each is specified
in `ble-spec.md`; the section is in brackets.

- [ ] Write with `.withResponse`. `.withoutResponse` is silently dropped by iOS
      for this characteristic. [§4.1]
- [ ] Send **exactly one byte**. `01 00` is rejected as `MALFORMED`, not
      executed. [§4.2]
- [ ] Subscribe **before** the initial status read, and always do that read.
      Nothing is sent on connect or subscribe. [§4.4]
- [ ] Match results by `resultSeq`, not `lastResult`. [§6.5]
- [ ] Wait for `didWriteValueFor` before the next write. [§5 rule 1]
- [ ] Disconnect as soon as you're done. The lock is invisible to everyone
      else while you're connected. [§5 rule 2]
- [ ] A disconnect after 30 s idle is normal if nothing was pending. [§6.6]
- [ ] Don't use `CBPeripheral.identifier` or the name `VT-LOCK-001` as a lock
      ID. Both are per-phone or shared by every dev board. [§2.2]
- [ ] Don't initiate pairing. [§8]
- [ ] **GATT caching:** after the firmware team reflashes the board, iOS may
      show stale characteristics (e.g. "status missing → firmware too old").
      Toggle Bluetooth off/on in iOS Settings. [§7]
- [ ] Don't hard-code timing. `T_act` will change when the servo goes in, and
      boot-to-advertising takes ~2 s but longer after a fresh flash. [§2.3,
      §6.3]

---

## 6. Testing (shared board)

You'll only have the lock at meetups, so most of the work should be testable
without it.

**Without the board**

- **Unit-test `LockProtocol`** with the byte fixtures from `ble-spec.md` §4.4,
  for example:

  | Bytes | Expected |
  |---|---|
  | `00 FF FF 00` | LOCKED, no command since boot |
  | `01 01 00 01` | UNLOCKING, UNLOCK → OK, seq 1 |
  | `02 01 00 01` | UNLOCKED, same seq (settle, not a new result) |
  | `02 07 03 03` | UNLOCKED, `07` → UNKNOWN_COMMAND |
  | `02 01 04 04 AA` | Valid. The extra byte is ignored. |
  | `02 01 04` | `nil` (too short) |
  | `09 01 00 01` | state `.unknown(9)`, no crash |
  | `01 01 00 02 03` (info) | protocol 1.1, firmware 0.2.3, compatible |

- **Unit-test `LockSession` against a fake transport** that replays
  notification sequences from `ble-spec.md` §4.4 and §6.5. Cover these:
  result before write-ack, settle of an earlier move arriving before your
  result, `NO_CHANGE`, `BUSY`, no result (timeout → read status), and a
  disconnect mid-command.
- Build the UI against the fake, including the "last checked" and "outcome
  unknown" states.

**At a meetup with the board**

1. First, use **nRF Connect** or **LightBlue** on your phone to see the
   service, subscribe to status, write `01`, and watch the notifications
   (`ble-spec.md` §10). This confirms the board works before you debug your
   app.
2. Run your app with the firmware team watching the serial monitor (115200
   baud). Every command and state change shows up there, so you can see
   which side is wrong.
3. Walk through the acceptance sequence in `v1-spec.md` §10 from the app:
   unlock, unlock again (`NO_CHANGE`), lock, unlock-then-lock quickly
   (queued), connect and idle (disconnected at 30 s), and press EN while
   unlocked (comes back `LOCKED`).
4. Also test: a second phone trying to connect while yours is connected
   ("in use"), walking out of range mid-command, and killing the app
   mid-unlock.

---

## 7. Accounts and backend in V1

The lock has **no idea who is using it**. In V1:

- Any phone can lock and unlock. Accounts, bike listings, bookings and
  permissions are entirely app/backend logic. The lock can't enforce any of
  them.
- If the backend wants a ride history ("user X unlocked bike Y at T"), the app
  records it from the phone's side: what it sent, the result it got, and the
  phone's clock. The lock doesn't record users or wall-clock time.
- There's only one lock and every board advertises the same name, so V1 can
  treat "the lock" as a single hard-coded bike. Don't build a bike ↔ lock
  pairing scheme on the name or `CBPeripheral.identifier`. Phase 2 adds a real
  lock ID in the info characteristic.

**Phase 2 will need the backend.** Plan for these now, but don't build them:

- the backend issues signed, time-limited **credentials** per lock;
- the app downloads them **ahead of time**, because the rider may have no
  signal at the bike, and stores them securely on the phone;
- unlocking becomes request challenge → send credential + proof → result;
- the lock's event log is carried back to the backend by the phone.

If your account/booking model has a "current rider of bike Y", it's the
natural place for Phase 2 credentials to attach.

---

## 8. Open items that affect the app

| Item | Effect on you | Until it's decided |
|---|---|---|
| Spec review (BLE-007) | `ble-spec.md` 1.1 is implemented in firmware but hasn't been walked through with you yet | Read it, bring questions, and flag anything that's awkward on iOS |
| Servo `T_act` (OPEN-1) | Move duration, so the settle timeout and any progress UI | Use a 10 s settle timeout; don't animate a fixed duration |
| LED or servo at the demo (OPEN-5) | None for the app | — |
| Demo date (OPEN-4) | Schedule | — |
| Lock identity, naming | How the app picks "this bike's" lock | One lock, use the service UUID filter |

Questions or mismatches with the lock: contact the firmware owner (ethanluchs).
Include the iOS version, the status bytes you saw, and what the serial monitor
showed, if you have it.

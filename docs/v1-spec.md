# Bike Lock V1 Specification

| | |
|---|---|
| **Document version** | 0.2 (draft, team decisions applied) |
| **Date** | 2026-10-12 |
| **Scope** | Lock firmware and bench hardware for the V1 MVP demo |
| **Related** | [`docs/ble-spec.md`](ble-spec.md) (BLE byte-level contract), [`docs/ios-integration.md`](ios-integration.md) (iOS start-here guide), `tools/lock_client.py` (test client) |
| **Status** | Draft. Unresolved items are tagged **OPEN-n** (§12). The choices listed in §13 have been reviewed and decided or accepted by the team. |

---

## 1. Summary

V1 proves the complete lock/unlock flow on bench hardware:

1. A command arrives over BLE.
2. The lock moves through a defined state machine and drives the actuator.
3. It returns to `LOCKED` after any reboot.
4. It reports its state back to the client.
5. It records what happened.

On the bench, three things stand in for the real system:

| Real system | V1 bench stand-in |
|---|---|
| iOS app | `tools/lock_client.py` on a laptop |
| Servo | LED (servo added when chosen, OPEN-1) |
| Battery | Laptop USB power |

V1 deliberately has **no authentication, no battery, and no custom PCB**. §11
lists everything deferred, so nobody builds on assumptions about it.

---

## 2. Context

### 2.1 Bench setup

```
 Laptop                           ESP32 DevKit V1                 Actuator
 lock_client.py   ── BLE ──►   lock firmware    ── GPIO ──►  LED (V1 stand-in)
 (stands in for                                              servo (when chosen)
  the iOS app)        ◄── USB: power + serial log ──
```

### 2.2 Ownership and seams

| Area | Owner |
|---|---|
| Lock firmware, BLE interface, test client | Firmware |
| Lock mechanism (servo-driven pin, chain), servo selection, enclosure, power hardware | Hardware / mechanical |
| iOS app, backend | iOS / backend team |

The seams are where one team's work depends on another's:

- **Firmware ↔ iOS:** the BLE interface, defined byte-for-byte in `ble-spec.md`.
  Changes are agreed before they're implemented (BLE-007).
- **Firmware ↔ hardware:** the actuator interface (ACT-001) and the servo
  parameters (OPEN-1).

### 2.3 Stages

| Stage | Contents | In this document? |
|---|---|---|
| 1. Bench | DevKit on laptop USB power, LED stand-in, full state machine and BLE flow | Yes |
| 2. Servo | Real servo replaces the LED via the actuator interface; same firmware otherwise | Yes (ACT-003, PWR-002), blocked on OPEN-1 |
| 3. Independent power | Battery, charging, sleep, power gating, current measurement, later a custom PCB | No (§11) |

---

## 3. Conventions

- **IDs** are `AREA-NNN`. They are never renumbered. A removed requirement is
  marked *withdrawn* and keeps its ID, so references stay valid.
- **"shall"** marks a requirement. Text without "shall" is explanation.
- **Priority:**
  - **D**: required for the demo.
  - **V1**: part of V1, but may land after the demo.
- **Verification:**
  - **T**: test, scripted with `lock_client.py` where possible.
  - **I**: inspection (code or design review).
  - **Demo**: shown live.

| Section | Area | Prefix |
|---|---|---|
| §4 | Lock state machine | `STATE` |
| §5.1 | Command handling | `CMD` |
| §5.2 | Actuator | `ACT` |
| §6 | BLE interface | `BLE` |
| §7 | Event log | `LOG` |
| §8 | Reliability and failure behavior | `FAIL` |
| §9.1 | Diagnostics | `DIAG` |
| §9.2 | Power (V1 stage) | `PWR` |
| §9.3 | Test client | `TOOL` |

---

## 4. Lock state machine (STATE)

### 4.1 States

| State | Meaning |
|---|---|
| `LOCKED` | Actuator has been driven to the locked position. |
| `UNLOCKING` | Actuator is moving toward unlocked. |
| `UNLOCKED` | Actuator has been driven to the unlocked position. |
| `LOCKING` | Actuator is moving toward locked. |

```
            UNLOCK                     actuation complete
  LOCKED ───────────► UNLOCKING ───────────────────────► UNLOCKED
    ▲                                                        │
    │   actuation complete                       LOCK        │
    └─────────────────────── LOCKING ◄───────────────────────┘
                                ▲
                                │  power-up / reset
                                │  (from any state, STATE-005)
                              BOOT
```

**Limitation.** V1 has no position sensing (§11), so a state means "the
firmware commanded this", not "the pin is verified to be here".

### 4.2 Requirements

| ID | Requirement | Pri | Verify |
|---|---|---|---|
| STATE-001 | The firmware shall always be in exactly one of the states in §4.1. | D | I |
| STATE-002 | The firmware shall only make the transitions shown in §4.1. | D | I, T |
| STATE-003 | A transitional state (`UNLOCKING`, `LOCKING`) shall last for the actuation time `T_act` and then settle into its target state. `T_act` shall be a single compile-time constant. For the LED it is a placeholder long enough to see (1 s, §13 #7); for the servo it comes from OPEN-1. | D | T |
| STATE-004 | *Withdrawn in 0.2.* (Was: save the target state to non-volatile storage before starting an actuation.) The team decided the lock always locks on boot (STATE-005, §13 #1), so there is no saved state to restore. | — | — |
| STATE-005 | On every power-up or reset, whatever the state before, the firmware shall enter `LOCKING`, drive the actuator to locked for `T_act`, and settle in `LOCKED` (fail-secure). The lock state shall not be saved across reboots. | D | T |
| STATE-006 | A transition in progress shall complete even if the BLE client disconnects. | D | T |
| STATE-007 | Every state change shall be reported to a connected client (BLE-002) and recorded in the event log (LOG-002). | D (report), V1 (log) | T |

---

## 5. Commands and actuation

### 5.1 Command handling (CMD)

**Result codes.** Every command produces exactly one of these. Their byte
values are defined in `ble-spec.md` v1.1.

| Result | Meaning |
|---|---|
| `OK` | Command executed; a transition has started. |
| `NO_CHANGE` | Command executed, but the lock is already in the requested state; nothing moved. |
| `BUSY` | The pending-command queue already holds 4 commands; command not executed. Retry after the lock settles. Rarely, the firmware's internal event queue overflowed (the result's opcode may then be `FF`). |
| `UNKNOWN_COMMAND` | Opcode not recognized. |
| `MALFORMED` | Write was not exactly 1 byte. |

**Response matrix** (command × state when the command arrives):

| Command | `LOCKED` | `UNLOCKING` | `UNLOCKED` | `LOCKING` |
|---|---|---|---|---|
| UNLOCK (`0x01`) | start unlocking → `OK` | queue (`BUSY` if 4 queued) | `NO_CHANGE` | queue (`BUSY` if 4 queued) |
| LOCK (`0x00`) | `NO_CHANGE` | queue (`BUSY` if 4 queued) | start locking → `OK` | queue (`BUSY` if 4 queued) |
| Any other opcode | `UNKNOWN_COMMAND` | `UNKNOWN_COMMAND` | `UNKNOWN_COMMAND` | `UNKNOWN_COMMAND` |
| Length ≠ 1 byte | `MALFORMED` | `MALFORMED` | `MALFORMED` | `MALFORMED` |

A queued command (including one that arrives during the boot `LOCKING`,
STATE-005) gets no immediate result. Queued commands are executed in arrival
order once the move settles, each by the `LOCKED`/`UNLOCKED` columns, so each
gets `OK` or `NO_CHANGE`; one that starts a move makes the rest wait for that
move too. Malformed and unknown commands are answered immediately, even during
a move. A queued command's result therefore arrives up to (queue position + 1)
× `T_act` after it was sent, where queue position is the number of commands
queued ahead of it (0 for the first).

| ID | Requirement | Pri | Verify |
|---|---|---|---|
| CMD-001 | The firmware shall respond to each command exactly as in the response matrix above. | D | T |
| CMD-002 | Commands shall pass from the BLE callback to the control logic through a queue. No command shall be silently dropped or overwritten. | D | I |
| CMD-003 | The BLE callback shall only validate and enqueue. It shall not actuate, write to flash, or block. | D | I |
| CMD-004 | Every command shall produce exactly one result code, reported to the client (BLE-002) when the command is executed (for a queued command, after the move ahead of it settles) and logged (LOG-002). | D (report), V1 (log) | T |
| CMD-005 | If 4 valid commands are already queued behind a move, or the internal event queue overflows, the command shall be answered with `BUSY`, not dropped. | V1 | T |
| CMD-006 | A valid LOCK/UNLOCK arriving during a transition shall be queued (up to 4) and executed in arrival order after the transition settles. A transition in progress shall never be reversed or cut short. | D | T |

### 5.2 Actuator (ACT)

| ID | Requirement | Pri | Verify |
|---|---|---|---|
| ACT-001 | All hardware actuation shall go through one actuator interface: initialize, start a move to locked or unlocked, and report when the move is complete. Replacing the LED with the servo shall only require changing the code behind this interface. | D | I |
| ACT-002 | **LED driver:** LED steadily on = `UNLOCKED`, off = `LOCKED`, blinking at about 4 Hz = transitioning. | D | Demo |
| ACT-003 | **Servo driver:** drive a hobby servo by PWM between two configured positions (locked angle, unlocked angle). The angles, PWM range and travel time come from OPEN-1. | V1 (D if parts arrive in time) | T |
| ACT-004 | The servo signal pin shall not be an ESP32 strapping pin (0, 2, 5, 12, 15) or a flash pin (6–11). Candidates: 25, 26, 27, 32, 33. The LED stand-in may stay on GPIO 2 (the onboard LED), for the bench only. | V1 | I |

---

## 6. BLE interface (BLE)

This section states **what** the BLE interface must provide. **How**, meaning
UUIDs and byte formats, belongs in `ble-spec.md`. V1 requires `ble-spec.md`
v1.1.0. It is backward compatible with a correct 1.0 app (BLE-008) but not
purely additive: writes that aren't exactly 1 byte are now rejected
(`MALFORMED`) instead of executed, idle clients are disconnected (BLE-005), a
second connection is refused (BLE-006), and commands sent during a move are
queued (CMD-006). See `ble-spec.md` §0, "Compatibility".

| ID | Requirement | Pri | Verify |
|---|---|---|---|
| BLE-001 | Lock and unlock commands shall remain as defined in `ble-spec.md` 1.0: opcodes `0x00` and `0x01` on the command characteristic, written with response. | D | T |
| BLE-002 | The lock shall expose its current state and the result of the most recent command through a characteristic that can be both read and subscribed to (notifications). A notification shall be sent on every state change and every command result. Each command result shall advance a result counter (`resultSeq`, mod 256, reset at boot) so the client can match a result to its command. | D | T |
| BLE-003 | The lock shall expose a readable firmware version and BLE protocol version, so a client can tell which version of the interface it is talking to. | D | T |
| BLE-004 | The lock shall let a client retrieve the event log (§7). | V1 | T |
| BLE-005 | The lock shall disconnect any client that makes no write to the command characteristic (valid or not) for `T_idle` (30 s), so one forgotten connection cannot block everyone else. Any write to the command characteristic resets the timer; reads, log-cursor writes and subscribing do not. | D | T |
| BLE-006 | After any disconnect, the lock shall resume advertising within 1 s. Only one phone shall be connected at a time: the lock shall not advertise while a phone is connected, and shall refuse a second connection attempt without disturbing the connected phone. | D | T |
| BLE-007 | `ble-spec.md` shall be updated to v1.1.0 and reviewed with the iOS team **before** the firmware implementing it is merged. | D | I |
| BLE-008 | An app written against `ble-spec.md` 1.0 shall keep working against V1 firmware. | D | T |

---

## 7. Event log (LOG)

The lock records its own history, because the backend can only learn what
happened at an offline lock from data a phone carries back. V1 records and
exposes the log. Clearing it and uploading it to the backend come later (§11).

| ID | Requirement | Pri | Verify |
|---|---|---|---|
| LOG-001 | Events shall be stored in non-volatile storage and survive reboot and power loss. An entry shall become readable over BLE only once it is saved to flash (normally within about 100 ms), so a sequence number a client has seen is never reused, even after power loss; an entry lost to power loss before saving was never visible. | V1 | T |
| LOG-002 | At minimum the firmware shall log: boot (with reset reason), state change (from, to), command result (opcode, result), client connect, client disconnect, idle-timeout disconnect. | V1 | T |
| LOG-003 | Each entry shall contain a sequence number that increases across reboots, a boot counter, milliseconds since boot, the event type, and event data. Entries shall not claim wall-clock time; the lock has no trusted clock (§11). Sequence numbers are strictly increasing with possible gaps, in the range 1..0x7FFFFFFF, and a number a client has seen is never reused (LOG-001). | V1 | I |
| LOG-004 | The log shall hold at least 256 entries. When full, the oldest entry is overwritten. | V1 | T |
| LOG-005 | Actuation shall never wait on a log write. | V1 | I |
| LOG-006 | In V1, clients can read the log but not clear it. | V1 | I |

---

## 8. Reliability and failure behavior (FAIL)

| ID | Requirement | Pri | Verify |
|---|---|---|---|
| FAIL-001 | After a reboot at any moment (power loss, brownout, crash, watchdog), the lock shall recover per STATE-005 with no user action. | D | T |
| FAIL-002 | The firmware shall enable a watchdog so that a hung control loop reboots the lock. | V1 | T |
| FAIL-003 | The firmware shall read the reset reason at boot and report it on serial (DIAG-002) and in the log (LOG-002). Note: on the ESP32, pressing EN reports `POWERON` (1), not an external reset. | D (serial), V1 (log) | T |
| FAIL-004 | If the client disconnects mid-transition, the transition shall complete (STATE-006), and the next client shall be able to read the resulting state. | D | T |
| FAIL-005 | No input shall crash the firmware or change state except as specified in §5.1. This includes unknown opcodes, wrong lengths, commands during transitions, and rapid repeated writes. | D | T |
| FAIL-006 | If the BLE stack fails to start, the firmware shall log it and reboot after a delay, rather than running with no radio as it does today. | V1 | T |

---

## 9. Supporting requirements

### 9.1 Diagnostics (DIAG)

| ID | Requirement | Pri | Verify |
|---|---|---|---|
| DIAG-001 | Serial output at 115200 baud, every line tagged by subsystem: `[boot]`, `[ble]`, `[cmd]`, `[state]`, `[act]`, `[log]`. | D | I |
| DIAG-002 | At boot, print a banner with firmware version, reset reason, boot count, and the boot state (always `LOCKED`, STATE-005). | D | Demo |
| DIAG-003 | The firmware version shall be defined in exactly one place and used for both serial output and BLE-003. | D | I |

### 9.2 Power, V1 stage (PWR)

| ID | Requirement | Pri | Verify |
|---|---|---|---|
| PWR-001 | V1 shall be powered from the laptop through the DevKit's USB port. | D | Demo |
| PWR-002 | When a servo is fitted, it shall be powered from its own 5 V supply (e.g. USB power bank or bench supply), sharing ground with the DevKit. It shall not be powered from the DevKit's 5V or 3V3 pins, because servo current spikes can reset the ESP32. | V1 | I |
| PWR-003 | The control logic shall be event-driven: it waits on the command queue and timers rather than polling in a busy loop. This lets sleep be added in Stage 3 without restructuring. | D | I |

### 9.3 Test client (TOOL)

| ID | Requirement | Pri | Verify |
|---|---|---|---|
| TOOL-001 | `lock_client.py` shall support everything V1 adds: read the state, subscribe to state and result notifications, read the versions, and (V1) dump the event log. | D (state, results, versions), V1 (log) | T |
| TOOL-002 | `lock_client.py` shall be able to run the acceptance sequence in §10 as a script and exit non-zero on any failure. | V1 | T |

---

## 10. Demo acceptance sequence

Run from the laptop with the serial monitor open. The same sequence applies with
the servo in place of the LED.

| # | Action | Expected |
|---|---|---|
| 1 | Power on the DevKit | Serial shows the boot banner (version, reset reason, boot count, boot state `LOCKED`). LED blinks for `T_act` (boot `LOCKING`), then off. |
| 2 | Read status | State `LOCKED` (after any boot), versions shown. |
| 3 | Unlock | Notifications `UNLOCKING`, then `UNLOCKED`; result `OK`. LED blinks, then stays on. |
| 4 | Unlock again | Result `NO_CHANGE`; LED unchanged. |
| 5 | Send `07` | Result `UNKNOWN_COMMAND`; state unchanged. |
| 6a | Lock, wait until `LOCKED`. Then Unlock, then immediately Lock | UNLOCK → `OK` (`UNLOCKING`). No immediate result for LOCK; after `UNLOCKED` settles, LOCK → `OK`, then `LOCKING` → `LOCKED`. |
| 6b | From `LOCKED`: Unlock, then while `UNLOCKING` send 4 more valid commands (Lock, Unlock, Lock, Unlock), then a 5th (Lock) | The 4 get no immediate result (queued); the 5th gets `BUSY` immediately. Then the 4 queued commands each get `OK` in order as the move ahead of them settles; the lock ends `UNLOCKED` after 5 moves. |
| 7 | Press EN (reset) while `UNLOCKED` | Reboots and drives to `LOCKED` (via `LOCKING`); banner shows reset reason `POWERON` (ESP32 reports EN as power-on). |
| 8 | Unlock, then unplug USB while `UNLOCKED`; plug it back in | Powers up and drives to `LOCKED`. No timing pressure. |
| 9 | Connect and do nothing | Disconnected after `T_idle`; lock is discoverable again within 1 s. |
| 10 | (V1) Dump the event log | Every event above, in order, from the log, with strictly increasing sequence numbers. |

---

## 11. Deferred: not in V1

Nothing in this table is implemented in V1. Do not build dependencies on any of
it.

| Item | Why deferred | Notes |
|---|---|---|
| **Authentication** (HMAC credentials, nonce challenge) | Phase 2 | V1 is unauthenticated: anyone with a BLE app can unlock it. Bench and demo use only. |
| **Time-limited access** (credential valid within a time window) | Needs a trusted clock, which an offline lock doesn't have | Options when revisited: battery-backed clock module, use-limited credentials, session-based credentials. Phone-supplied time is not trustworthy. |
| **Revocation** (cancelling access before it expires) | Needs authentication first | An offline lock only learns about revocations from a phone; short credential lifetimes are the main protection. |
| **Clearing the log on acknowledgement; uploading it to the backend** | Needs an app/backend protocol | V1 log is read-only (LOG-006). |
| **Lock identity and provisioning** (per-lock ID, per-lock secret, naming) | Only one lock in V1 | Needed before Phase 2. |
| **Battery, charging, power-path** | Stage 3; sizing waits on OPEN-1 | See OPEN-2, OPEN-3. |
| **Battery life target (2 weeks); sleep, slower advertising, servo power gating** | Stage 3 | Needs current measurements. Target requires an average draw of about 9 mA or less on one 18650 cell. |
| **Low-battery policy** (keep enough charge to lock; warnings) | Stage 3 | Important: V1 has no manual override (see below). |
| **Custom PCB** | After Stage 3 is proven on modules | |
| **Actuation confirmation / position sensing** | Hardware decision | Until then, state means "commanded" (§4.1). |
| **Fault state, jam handling** | Depends on position sensing | |
| **More states** (e.g. "pending unlock", "riding") | Need app-side meaning; likely Phase 2 | The state machine (§4) is built to be extended. |
| **Manual override** (key) | Not in V1 | A dead battery while locked leaves the lock stuck. |
| **Enclosure, weatherproofing, physical tamper resistance** | Hardware / mechanical | |
| **Flash encryption, secure boot** | Phase 2, with secrets | |
| **Performance targets** (unlock latency, range) | V1 target is "it works" | |
| **Multiple locks, Android** | Out of scope | iOS only. |

---

## 12. Open items

| ID | Question | Owner | Blocks |
|---|---|---|---|
| OPEN-1 | Servo: model, locked/unlocked angles, travel time, current draw (normal and stalled), operating voltage | Hardware / mechanical | ACT-003, `T_act` for the servo, power sizing |
| OPEN-2 | Use a power-path charger (device runs while charging)? | Hardware + firmware | Stage 3 |
| OPEN-3 | Battery capacity | Hardware | Depends on OPEN-1 and measured current |
| OPEN-4 | Exact demo date | Team | Schedule |
| OPEN-5 | Demo with the servo or the LED | Team | Depends on parts arriving |

---

## 13. Choices made in this draft: reviewed

The 0.1 draft proposed these positions. The team has reviewed them; the
outcome of each is recorded below.

| # | Position | Where | Status |
|---|---|---|---|
| 1 | **Always drive to `LOCKED` on boot** (via `LOCKING`), whatever the state before; the state is not saved. (0.1 proposed restoring the last saved target.) | STATE-004 (withdrawn), STATE-005 | **Decided** |
| 2 | **Valid commands during a transition are queued** (up to 4) and executed in order after it settles; `BUSY` only when the queue is full. (0.1 proposed `BUSY` for every command during a transition.) | §5.1, CMD-005, CMD-006 | **Decided** |
| 3 | Repeating the current state gets `NO_CHANGE` without moving the actuator | §5.1 | Accepted |
| 4 | Writes that aren't exactly 1 byte get `MALFORMED`. 1.0 firmware used the first byte and ignored the rest; that behavior was marked provisional, so this is allowed. | §5.1 | Accepted |
| 5 | Idle timeout `T_idle` = 30 s | BLE-005 | Accepted |
| 6 | LED: on = unlocked, off = locked, blinking = moving | ACT-002 | Accepted |
| 7 | LED actuation time `T_act` = 1 s | STATE-003 | Accepted |
| 8 | Event log holds at least 256 entries, read-only in V1 | LOG-004, LOG-006 | Accepted |
| 9 | The D / V1 split: state machine, queue, status notifications and idle timeout are demo-critical; event log, watchdog and servo driver may follow | All tables | Accepted |
| 10 | Status, version and log become new characteristics in `ble-spec.md` v1.1.0 | §6 | Accepted (still to be reviewed with the iOS team per BLE-007) |

---

## Revision history

| Version | Date | Change |
|---|---|---|
| 0.1 | 2026-10-12 | Initial draft for team review. |
| 0.2 | 2026-10-12 | Team decisions applied. Boot always drives to `LOCKED` (STATE-005 rewritten, STATE-004 withdrawn, §4.1 diagram); commands during a transition are queued (§5.1 results and matrix, CMD-004, CMD-005, new CMD-006). Added `resultSeq` (BLE-002), one phone at a time (BLE-006), idle timer reset by any command write (BLE-005), durable log and sequence range (LOG-001, LOG-003), EN reports `POWERON` (FAIL-003). Corrected the "purely additive" claim in §6. Summary, DIAG-002, §10 acceptance sequence and §13 updated. |

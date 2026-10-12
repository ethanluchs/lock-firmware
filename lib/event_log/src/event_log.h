#pragma once
// Persistent event log (docs/v1-spec.md §7). Pure C++; storage is injected.
//
// Entry layout, 16 bytes, little-endian. This is BOTH the storage format and
// the BLE wire format (docs/ble-spec.md v1.1), so never change it in place:
//   [0..3]   seq          uint32  1, 2, 3, ... across reboots; valid range
//                                 1..kMaxSeq (0 and higher values are invalid)
//   [4..5]   bootCount    uint16  boot counter at the time of the event
//   [6..9]   msSinceBoot  uint32  milliseconds since that boot
//   [10]     type         EventType
//   [11..15] data         5 bytes, meaning per type (below), zero-padded
//
// Ring placement: the entry with sequence number s lives in slot
// (s - 1) % capacity. Recovery after reboot scans all slots, ignores slots
// that are empty, unreadable, or whose seq does not match their slot, and
// continues from the highest valid seq.
//
// Durability: storage may buffer writes (the ESP32 backend keeps them in RAM
// until flushed). readFrom() only returns entries up to the durable seq, i.e.
// entries known to be on flash. An entry lost to power loss before its flush
// was never visible to a client, so reusing its seq after reboot is harmless.

#include <stddef.h>
#include <stdint.h>

namespace lock {
namespace log {

constexpr size_t kEntrySize = 16;
constexpr size_t kDataSize = 5;
// Highest valid seq. A real lock never gets near it; anything above it in
// storage is corruption and is rejected, so a garbage entry can never push
// numbering into wraparound.
constexpr uint32_t kMaxSeq = 0x7FFFFFFF;

enum class EventType : uint8_t {
  // data[0] = reset reason (esp_reset_reason_t value)
  // data[1] = boot target, always LockState::Locked (the lock always locks on
  //           power-up; kept so the layout is unchanged)
  // data[2] = 0 (reserved; was "target came from storage" in early drafts)
  Boot = 1,
  // data[0] = from (LockState), or kNone (0xFF) for the boot transition into
  //           Locking, which has no prior state
  // data[1] = to (LockState)
  StateChange = 2,
  // data[0] = opcode (kNone if empty write), data[1] = CmdResult,
  // data[2] = write length, capped at 255
  CommandResult = 3,
  // data[0..1] = connection id, uint16 LE
  Connect = 4,
  // data[0..1] = connection id, uint16 LE; data[2..3] = HCI reason, uint16 LE
  Disconnect = 5,
  // data[0..1] = connection id, uint16 LE
  IdleTimeout = 6,
};

struct LogEntry {
  uint32_t seq;
  uint16_t bootCount;
  uint32_t msSinceBoot;
  EventType type;
  uint8_t data[kDataSize];
};

void pack(const LogEntry &e, uint8_t out[kEntrySize]);
// Returns false if the bytes cannot be a valid entry (seq == 0 or > kMaxSeq,
// or type is not a known EventType).
bool unpack(const uint8_t in[kEntrySize], LogEntry *out);

// Fixed number of 16-byte slots in non-volatile storage.
class LogStorage {
 public:
  virtual ~LogStorage() = default;
  virtual size_t capacity() const = 0;
  // False if the slot has never been written or cannot be read.
  virtual bool read(size_t slot, uint8_t out[kEntrySize]) = 0;
  virtual bool write(size_t slot, const uint8_t in[kEntrySize]) = 0;
};

class EventLog {
 public:
  explicit EventLog(LogStorage &storage) : storage_(storage) {}

  // Scan storage and recover the next sequence number. `bootCount` is stamped
  // on every entry appended during this boot. Must be called before append().
  void begin(uint16_t bootCount);

  // Append one entry. `dataLen` > kDataSize is truncated; shorter data is
  // zero-padded; data may be null when dataLen == 0. Returns the storage
  // write result, which for buffering storage only means "accepted", not
  // "on flash". The sequence number is consumed even if the write fails, so
  // numbers never repeat within a boot. Returns false without writing if
  // the sequence space (kMaxSeq) is exhausted.
  bool append(EventType type, uint32_t msSinceBoot, const uint8_t *data, size_t dataLen);

  // Copy up to maxEntries entries with cursor <= seq <= durableSeq() into
  // `out`, oldest first. Entries already overwritten are skipped. Returns the
  // number copied. Paging: next cursor = out[n - 1].seq + 1.
  size_t readFrom(uint32_t cursor, LogEntry *out, size_t maxEntries);

  // The caller reports that every entry up to and including `seq` is on
  // flash (after a successful storage flush). Values below the current
  // durable seq or at/after nextSeq() are clamped; it never moves backwards.
  void markDurable(uint32_t seq);
  // Highest seq readFrom() may return. After begin(): the highest recovered
  // seq (everything recovered came from flash).
  uint32_t durableSeq() const { return durableSeq_; }

  uint32_t nextSeq() const { return nextSeq_; }
  // Upper bound on the number of retrievable entries (<= capacity). Slots
  // lost to failed writes or corruption still count.
  size_t size() const;

 private:
  LogStorage &storage_;
  uint16_t bootCount_ = 0;
  uint32_t nextSeq_ = 1;
  uint32_t durableSeq_ = 0;
};

}  // namespace log
}  // namespace lock

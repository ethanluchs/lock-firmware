#pragma once
// In-memory LogStorage with fault injection, for host tests of event_log.
//
// Faults:
//   - unreadable[slot]   : read() of that slot returns false (bad sector)
//   - failWrite[slot]    : write() to that slot returns false, nothing stored
//   - failAllWrites      : every write() returns false, nothing stored
//   - powerCutAfter      : after N more successful writes, the next write is
//                          "torn" (only the first tornBytes bytes land) and
//                          returns false; from then on the flash is dead
//                          (all writes fail) until restorePower().
//   - erasedReadsOk      : never-written slots read back as 0xFF bytes with
//                          success (raw NOR flash), instead of returning false.
//   - poke()             : write arbitrary bytes into a slot (garbage).
//   - buffered           : like the ESP32 backend, successful writes land in a
//                          RAM buffer (read() sees it) until flush() copies
//                          them to the cells; loseBuffer() models power loss
//                          before the flush.

#include <stdint.h>
#include <string.h>

#include <map>
#include <vector>

#include "event_log.h"

struct FakeStorage : lock::log::LogStorage {
  using Cell = std::vector<uint8_t>;

  explicit FakeStorage(size_t cap) { resize(cap); }

  void resize(size_t cap) {
    cap_ = cap;
    cells.resize(cap, Cell(lock::log::kEntrySize, 0xFF));
    written.resize(cap, false);
    unreadable.resize(cap, false);
    failWrite.resize(cap, false);
  }

  size_t capacity() const override { return cap_; }

  bool read(size_t slot, uint8_t out[lock::log::kEntrySize]) override {
    reads++;
    if (slot >= cap_) {
      outOfRangeAccess++;
      return false;
    }
    if (unreadable[slot]) return false;
    auto it = buffer.find(slot);
    if (it != buffer.end()) {
      memcpy(out, it->second.data(), lock::log::kEntrySize);
      return true;
    }
    if (!written[slot] && !erasedReadsOk) return false;
    memcpy(out, cells[slot].data(), lock::log::kEntrySize);
    return true;
  }

  bool write(size_t slot, const uint8_t in[lock::log::kEntrySize]) override {
    writes++;
    lastWriteSlot = slot;
    if (slot >= cap_) {
      outOfRangeAccess++;
      return false;
    }
    if (powerDead || failAllWrites || failWrite[slot]) return false;
    if (powerCutAfter == 0) {
      // Torn write: only a prefix lands, then the power is gone.
      memcpy(cells[slot].data(), in, tornBytes);
      if (tornBytes > 0) written[slot] = true;
      powerDead = true;
      powerCutAfter = -1;
      return false;
    }
    if (powerCutAfter > 0) powerCutAfter--;
    if (buffered) {
      buffer[slot] = Cell(in, in + lock::log::kEntrySize);
      return true;
    }
    memcpy(cells[slot].data(), in, lock::log::kEntrySize);
    written[slot] = true;
    return true;
  }

  void poke(size_t slot, const uint8_t bytes[lock::log::kEntrySize]) {
    memcpy(cells[slot].data(), bytes, lock::log::kEntrySize);
    written[slot] = true;
  }

  void restorePower() { powerDead = false; }

  void flush() {
    for (auto &kv : buffer) {
      cells[kv.first] = kv.second;
      written[kv.first] = true;
    }
    buffer.clear();
  }
  void loseBuffer() { buffer.clear(); }

  size_t cap_ = 0;
  std::vector<Cell> cells;
  std::vector<bool> written, unreadable, failWrite;
  bool failAllWrites = false;
  bool erasedReadsOk = false;
  bool buffered = false;
  std::map<size_t, Cell> buffer;
  long powerCutAfter = -1;  // -1 = never
  size_t tornBytes = 0;
  bool powerDead = false;
  size_t reads = 0, writes = 0, outOfRangeAccess = 0, lastWriteSlot = 0;
};

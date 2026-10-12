#pragma once
// BLE wire formats for protocol v1.1 (docs/ble-spec.md is the human-readable
// contract; this header is its source of truth for byte layouts).
// Pure C++: encoders only, no BLE stack calls. All multi-byte fields are
// little-endian.

#include <stddef.h>
#include <stdint.h>

#include "event_log.h"
#include "lock_types.h"

namespace lock {
namespace proto {

constexpr uint8_t kProtocolMajor = 1;
constexpr uint8_t kProtocolMinor = 1;

constexpr const char *kServiceUuid = "8f2a1c00-5b7e-4d19-9a3f-6c2e81d4b70a";
// WRITE (with response). 1-byte opcode. Unchanged from 1.0.
constexpr const char *kCommandCharUuid = "8f2a1c01-5b7e-4d19-9a3f-6c2e81d4b70a";
// READ + NOTIFY. Status payload below. Notified on every state change and
// every command result.
constexpr const char *kStatusCharUuid = "8f2a1c02-5b7e-4d19-9a3f-6c2e81d4b70a";
// READ. Info payload below.
constexpr const char *kInfoCharUuid = "8f2a1c03-5b7e-4d19-9a3f-6c2e81d4b70a";
// WRITE (with response) + READ. Write a 4-byte cursor, then read pages.
constexpr const char *kLogCharUuid = "8f2a1c04-5b7e-4d19-9a3f-6c2e81d4b70a";

// ---- Status: 4 bytes ------------------------------------------------------
//   [0] state        LockState
//   [1] lastOpcode   opcode of the most recent command, kNone if none yet
//   [2] lastResult   CmdResult of the most recent command, kNone if none yet
//   [3] resultSeq    +1 (wrapping) for every command result since boot; 0
//                    before the first. A client tells "my command's result
//                    arrived" from "a state change arrived" by watching this.
constexpr size_t kStatusLen = 4;
struct Status {
  LockState state;
  uint8_t lastOpcode;
  uint8_t lastResult;
  uint8_t resultSeq;
};
void encodeStatus(const Status &s, uint8_t out[kStatusLen]);

// ---- Info: 5 bytes --------------------------------------------------------
//   [0] protocol major  [1] protocol minor
//   [2] firmware major  [3] firmware minor  [4] firmware patch
constexpr size_t kInfoLen = 5;
void encodeInfo(uint8_t fwMajor, uint8_t fwMinor, uint8_t fwPatch, uint8_t out[kInfoLen]);

// ---- Log retrieval ----------------------------------------------------------
// Client writes a 4-byte cursor (uint32 LE). Writes of any other length are
// ignored and leave the cursor unchanged. The cursor resets to 0 on every new
// connection.
// Each read returns one page of entries with seq >= cursor, oldest first:
//   [0]      count (0..kLogPageMaxEntries); 0 means no more entries
//   [1..]    count x 16-byte entries (layout in event_log.h)
// A read does not advance the cursor. The client writes last seq + 1 to page.
constexpr size_t kLogCursorLen = 4;
constexpr size_t kLogPageMaxEntries = 8;
constexpr size_t kLogPageMaxLen = 1 + kLogPageMaxEntries * log::kEntrySize;  // 129

// Returns false (cursor untouched) unless len == kLogCursorLen.
bool decodeLogCursor(const uint8_t *data, size_t len, uint32_t *cursor);

// Encodes min(count, kLogPageMaxEntries) entries. Returns bytes written, or 0
// if outCap is too small for the page.
size_t encodeLogPage(const log::LogEntry *entries, size_t count, uint8_t *out, size_t outCap);

}  // namespace proto
}  // namespace lock

#include "lock_protocol.h"

namespace lock {
namespace proto {

void encodeStatus(const Status &s, uint8_t out[kStatusLen]) {
  out[0] = static_cast<uint8_t>(s.state);
  out[1] = s.lastOpcode;
  out[2] = s.lastResult;
  out[3] = s.resultSeq;
}

void encodeInfo(uint8_t fwMajor, uint8_t fwMinor, uint8_t fwPatch, uint8_t out[kInfoLen]) {
  out[0] = kProtocolMajor;
  out[1] = kProtocolMinor;
  out[2] = fwMajor;
  out[3] = fwMinor;
  out[4] = fwPatch;
}

bool decodeLogCursor(const uint8_t *data, size_t len, uint32_t *cursor) {
  if (len != kLogCursorLen || data == nullptr || cursor == nullptr) return false;
  // Assemble explicitly so the result is independent of host endianness.
  *cursor = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
            (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
  return true;
}

size_t encodeLogPage(const log::LogEntry *entries, size_t count, uint8_t *out, size_t outCap) {
  if (out == nullptr) return 0;
  // A null entry list can only describe an empty page.
  size_t n = (entries == nullptr) ? 0 : count;
  if (n > kLogPageMaxEntries) n = kLogPageMaxEntries;
  const size_t needed = 1 + n * log::kEntrySize;
  if (outCap < needed) return 0;

  out[0] = static_cast<uint8_t>(n);
  for (size_t i = 0; i < n; ++i) {
    log::pack(entries[i], out + 1 + i * log::kEntrySize);
  }
  return needed;
}

}  // namespace proto
}  // namespace lock

#include "event_log.h"

// seq never wraps: append() stops at kMaxSeq. At one event per second that
// is ~68 years, far beyond the product life, so stopping is the safe failure.

namespace lock {
namespace log {

namespace {

void putU16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void putU32(uint8_t *p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

uint16_t getU16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t getU32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool isKnownType(uint8_t t) {
  switch (static_cast<EventType>(t)) {
    case EventType::Boot:
    case EventType::StateChange:
    case EventType::CommandResult:
    case EventType::Connect:
    case EventType::Disconnect:
    case EventType::IdleTimeout:
      return true;
  }
  return false;
}

// Callers guarantee seq != 0 and cap != 0.
size_t slotFor(uint32_t seq, size_t cap) { return static_cast<size_t>((seq - 1) % cap); }

}  // namespace

void pack(const LogEntry &e, uint8_t out[kEntrySize]) {
  putU32(out + 0, e.seq);
  putU16(out + 4, e.bootCount);
  putU32(out + 6, e.msSinceBoot);
  out[10] = static_cast<uint8_t>(e.type);
  for (size_t i = 0; i < kDataSize; i++) out[11 + i] = e.data[i];
}

bool unpack(const uint8_t in[kEntrySize], LogEntry *out) {
  uint32_t seq = getU32(in + 0);
  // Erased flash (all 0xFF) and zeroed slots both fail here; so does garbage
  // with a huge seq, which would otherwise push numbering into wraparound.
  if (seq == 0 || seq > kMaxSeq || !isKnownType(in[10])) return false;
  out->seq = seq;
  out->bootCount = getU16(in + 4);
  out->msSinceBoot = getU32(in + 6);
  out->type = static_cast<EventType>(in[10]);
  for (size_t i = 0; i < kDataSize; i++) out->data[i] = in[11 + i];
  return true;
}

void EventLog::begin(uint16_t bootCount) {
  bootCount_ = bootCount;
  nextSeq_ = 1;
  size_t cap = storage_.capacity();
  uint32_t highest = 0;
  // One slot at a time: the control task stack is small.
  uint8_t buf[kEntrySize];
  LogEntry e;
  for (size_t slot = 0; slot < cap; slot++) {
    if (!storage_.read(slot, buf)) continue;
    if (!unpack(buf, &e)) continue;
    // A misplaced seq is corruption (or a different capacity); trusting it
    // could jump nextSeq_ far ahead or back.
    if (slotFor(e.seq, cap) != slot) continue;
    if (e.seq > highest) highest = e.seq;
  }
  nextSeq_ = highest + 1;
  // Everything recovered was read back from flash, so it is durable.
  durableSeq_ = highest;
}

bool EventLog::append(EventType type, uint32_t msSinceBoot, const uint8_t *data,
                      size_t dataLen) {
  // Refuse rather than wrap: a reused or zero seq would corrupt client cursors.
  if (nextSeq_ > kMaxSeq) return false;
  LogEntry e;
  // Consume the number before writing so a failed write never lets the same
  // seq be reused for a different event in this boot.
  e.seq = nextSeq_++;
  e.bootCount = bootCount_;
  e.msSinceBoot = msSinceBoot;
  e.type = type;
  if (dataLen > kDataSize) dataLen = kDataSize;
  for (size_t i = 0; i < kDataSize; i++) e.data[i] = (data && i < dataLen) ? data[i] : 0;

  size_t cap = storage_.capacity();
  if (cap == 0) return false;
  uint8_t buf[kEntrySize];
  pack(e, buf);
  return storage_.write(slotFor(e.seq, cap), buf);
}

size_t EventLog::readFrom(uint32_t cursor, LogEntry *out, size_t maxEntries) {
  size_t cap = storage_.capacity();
  if (cap == 0 || durableSeq_ == 0 || maxEntries == 0 || out == nullptr) return 0;

  // The retained window is set by what was appended (non-durable appends
  // still overwrite ring slots); visibility stops at the durable seq so a
  // client never sees an entry that power loss could take back.
  uint32_t appended = nextSeq_ - 1;
  uint32_t oldest = (appended > cap) ? static_cast<uint32_t>(appended - cap + 1) : 1;
  uint32_t newest = durableSeq_;
  uint32_t start = (cursor < oldest) ? oldest : cursor;  // also covers cursor 0
  if (start > newest) return 0;

  size_t n = 0;
  uint8_t buf[kEntrySize];
  LogEntry e;
  // Count iterations rather than compare s <= newest, so newest == UINT32_MAX
  // cannot loop forever.
  uint32_t remaining = newest - start + 1;
  for (uint32_t s = start; remaining > 0 && n < maxEntries; s++, remaining--) {
    if (!storage_.read(slotFor(s, cap), buf)) continue;
    // The slot must hold exactly seq s. Anything else is an older entry left
    // behind by a failed write, or corruption; never return it as seq s.
    if (!unpack(buf, &e) || e.seq != s) continue;
    out[n++] = e;
  }
  return n;
}

void EventLog::markDurable(uint32_t seq) {
  // Only seqs already handed out can be on flash.
  uint32_t highestIssued = nextSeq_ - 1;
  if (seq > highestIssued) seq = highestIssued;
  if (seq > durableSeq_) durableSeq_ = seq;
}

size_t EventLog::size() const {
  // size() is const and cannot read storage, so this is the retained window,
  // an upper bound: slots lost to failed writes or corruption still count.
  uint32_t written = nextSeq_ - 1;
  size_t cap = storage_.capacity();
  return (written < cap) ? static_cast<size_t>(written) : cap;
}

}  // namespace log
}  // namespace lock

// Host unit tests for lib/event_log (docs/v1-spec.md §7, docs/ble-spec.md §4.6).
//
// Requirement mapping:
//   LOG-001  survive reboot / power loss       -> reboot_*, power_*
//   LOG-002  event types and data layout       -> pack_*, append_*
//   LOG-003  entry fields; seq increases across reboots, never 0 -> pack_*,
//            reboot_*, fault_*, limitation_*
//   LOG-004  >= 256 entries, oldest overwritten -> wrap_*, read_*
//   durability gating (header)                  -> durable_*
//   seq ceiling kMaxSeq (header)                -> unpack_*, maxseq_*
//   LOG-005  actuation never waits on a log write: not host-testable (I)
//   LOG-006  read-only in V1: API has no clear(); nothing to test (I)
//
// Tests named limitation_* assert the CURRENT behavior of a known, accepted
// design limitation so reviewers can see it. They are not bugs.

#include <unity.h>

#include <map>
#include <set>
#include <vector>

#include "event_log.h"
#include "fake_storage.h"

using namespace lock::log;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- helpers

// Deterministic per-tag payload so we can check that a returned entry is
// exactly the one appended with that seq.
static uint32_t msFor(uint32_t tag) { return tag * 1000u + 7u; }
static void dataFor(uint32_t tag, uint8_t d[kDataSize]) {
  for (size_t i = 0; i < kDataSize; i++) d[i] = static_cast<uint8_t>(tag * 31u + i);
}
static EventType typeFor(uint32_t tag) { return static_cast<EventType>(1 + tag % 6); }

// Appends without reporting a flush: the entry is not readable yet.
static bool appendBuffered(EventLog &log, uint32_t tag) {
  uint8_t d[kDataSize];
  dataFor(tag, d);
  return log.append(typeFor(tag), msFor(tag), d, kDataSize);
}

// Models write-through storage: every append is followed by a successful
// flush, so it becomes durable at once. Most tests are about ring and recovery
// behavior, not durability.
static bool appendTagged(EventLog &log, uint32_t tag) {
  bool ok = appendBuffered(log, tag);
  log.markDurable(log.nextSeq() - 1);
  return ok;
}

// Same for direct append() calls.
static bool appendDurable(EventLog &log, EventType type, uint32_t ms, const uint8_t *d,
                          size_t len) {
  bool ok = log.append(type, ms, d, len);
  log.markDurable(log.nextSeq() - 1);
  return ok;
}

static void assertTagged(const LogEntry &e, uint32_t tag, uint16_t boot) {
  TEST_ASSERT_EQUAL_UINT16(boot, e.bootCount);
  TEST_ASSERT_EQUAL_UINT32(msFor(tag), e.msSinceBoot);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(typeFor(tag)), static_cast<uint8_t>(e.type));
  uint8_t d[kDataSize];
  dataFor(tag, d);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(d, e.data, kDataSize);
}

// Dump the whole log the way the BLE layer does: pages of 8, cursor starts at
// 0, next cursor = last seq + 1, stop on an empty page. Asserts strictly
// increasing seqs across pages (no duplicates) and bounded page count.
static std::vector<LogEntry> dumpPaged(EventLog &log, uint32_t cursor = 0, size_t page = 8) {
  std::vector<LogEntry> all;
  LogEntry buf[8];
  for (int guard = 0; guard < 100000; guard++) {
    size_t n = log.readFrom(cursor, buf, page);
    TEST_ASSERT_TRUE(n <= page);
    if (n == 0) return all;
    for (size_t i = 0; i < n; i++) {
      if (!all.empty()) TEST_ASSERT_TRUE_MESSAGE(buf[i].seq > all.back().seq, "seq not increasing");
      TEST_ASSERT_TRUE_MESSAGE(buf[i].seq >= cursor, "entry older than cursor");
      all.push_back(buf[i]);
    }
    cursor = buf[n - 1].seq + 1;
  }
  TEST_FAIL_MESSAGE("paging did not terminate");
  return all;
}

static std::vector<uint32_t> seqsOf(const std::vector<LogEntry> &v) {
  std::vector<uint32_t> s;
  for (auto &e : v) s.push_back(e.seq);
  return s;
}

static std::vector<uint32_t> range(uint32_t first, uint32_t last) {
  std::vector<uint32_t> s;
  for (uint32_t i = first; i <= last; i++) s.push_back(i);
  return s;
}

static void assertSeqs(const std::vector<uint32_t> &want, const std::vector<uint32_t> &got) {
  TEST_ASSERT_EQUAL_size_t_MESSAGE(want.size(), got.size(), "entry count");
  for (size_t i = 0; i < want.size(); i++) TEST_ASSERT_EQUAL_UINT32(want[i], got[i]);
}

static void makeRaw(uint8_t out[kEntrySize], uint32_t seq, uint8_t type) {
  LogEntry e{seq, 9, 99, static_cast<EventType>(type), {1, 2, 3, 4, 5}};
  pack(e, out);
}

// ---------------------------------------------------------------- pack/unpack (LOG-002, LOG-003)

void test_pack_exact_layout_little_endian() {
  LogEntry e{0x04030201u, 0x0605u, 0x0A090807u, EventType::Disconnect, {0x0C, 0x0D, 0x0E, 0x0F, 0x10}};
  uint8_t b[kEntrySize];
  memset(b, 0xEE, sizeof b);
  pack(e, b);
  const uint8_t want[kEntrySize] = {0x01, 0x02, 0x03, 0x04,  // seq LE
                                    0x05, 0x06,              // bootCount LE
                                    0x07, 0x08, 0x09, 0x0A,  // msSinceBoot LE
                                    0x05,                    // type DISCONNECT
                                    0x0C, 0x0D, 0x0E, 0x0F, 0x10};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(want, b, kEntrySize);
}

void test_pack_matches_ble_spec_worked_example() {
  // docs/ble-spec.md §4.6 worked example, both entries byte for byte.
  LogEntry a{1, 1, 1234, EventType::Boot, {1, 0, 0, 0, 0}};
  LogEntry b{2, 1, 2236, EventType::StateChange, {3, 0, 0, 0, 0}};
  const uint8_t wantA[] = {0x01, 0, 0, 0, 0x01, 0, 0xD2, 0x04, 0, 0, 0x01, 0x01, 0, 0, 0, 0};
  const uint8_t wantB[] = {0x02, 0, 0, 0, 0x01, 0, 0xBC, 0x08, 0, 0, 0x02, 0x03, 0, 0, 0, 0};
  uint8_t out[kEntrySize];
  pack(a, out);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(wantA, out, kEntrySize);
  pack(b, out);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(wantB, out, kEntrySize);
}

void test_pack_unpack_roundtrip() {
  const uint32_t seqs[] = {1, 2, 255, 256, 0x01000000u, kMaxSeq - 1, kMaxSeq};
  const uint16_t boots[] = {0, 1, 0x00FF, 0xFFFF};
  const uint32_t mss[] = {0, 1, 0x80000000u, 0xFFFFFFFFu};
  for (uint32_t s : seqs)
    for (uint16_t bc : boots)
      for (uint32_t ms : mss)
        for (uint8_t t = 1; t <= 6; t++) {
          LogEntry in{s, bc, ms, static_cast<EventType>(t), {0xFF, 0x00, 0x80, 0x7F, t}};
          uint8_t b[kEntrySize];
          pack(in, b);
          LogEntry out;
          memset(&out, 0xA5, sizeof out);
          TEST_ASSERT_TRUE(unpack(b, &out));
          TEST_ASSERT_EQUAL_UINT32(s, out.seq);
          TEST_ASSERT_EQUAL_UINT16(bc, out.bootCount);
          TEST_ASSERT_EQUAL_UINT32(ms, out.msSinceBoot);
          TEST_ASSERT_EQUAL_UINT8(t, static_cast<uint8_t>(out.type));
          TEST_ASSERT_EQUAL_UINT8_ARRAY(in.data, out.data, kDataSize);
        }
}

void test_unpack_rejects_seq_zero() {
  uint8_t b[kEntrySize];
  makeRaw(b, 0, 1);
  LogEntry out;
  TEST_ASSERT_FALSE(unpack(b, &out));
}

void test_unpack_rejects_seq_above_kMaxSeq() {
  TEST_ASSERT_EQUAL_HEX32(0x7FFFFFFFu, kMaxSeq);
  const uint32_t bad[] = {kMaxSeq + 1, 0x80000001u, 0xC0000000u, 0xFFFFFFFEu, 0xFFFFFFFFu};
  for (uint32_t seq : bad) {
    uint8_t b[kEntrySize];
    makeRaw(b, seq, 1);
    LogEntry out;
    TEST_ASSERT_FALSE(unpack(b, &out));
  }
  uint8_t b[kEntrySize];
  makeRaw(b, kMaxSeq, 1);
  LogEntry out;
  TEST_ASSERT_TRUE(unpack(b, &out));
  TEST_ASSERT_EQUAL_UINT32(kMaxSeq, out.seq);
}

void test_unpack_accepts_exactly_the_six_known_types() {
  int accepted = 0;
  for (int t = 0; t < 256; t++) {
    uint8_t b[kEntrySize];
    makeRaw(b, 5, static_cast<uint8_t>(t));
    LogEntry out;
    bool ok = unpack(b, &out);
    TEST_ASSERT_EQUAL_MESSAGE(t >= 1 && t <= 6, ok, "type acceptance");
    if (ok) accepted++;
  }
  TEST_ASSERT_EQUAL_INT(6, accepted);
}

void test_unpack_rejects_erased_and_zeroed() {
  uint8_t ff[kEntrySize], zz[kEntrySize];
  memset(ff, 0xFF, sizeof ff);
  memset(zz, 0x00, sizeof zz);
  LogEntry out;
  TEST_ASSERT_FALSE(unpack(ff, &out));
  TEST_ASSERT_FALSE(unpack(zz, &out));
}

// ---------------------------------------------------------------- append (LOG-002, LOG-003)

void test_append_stamps_fields_and_placement() {
  FakeStorage st(256);
  EventLog log(st);
  log.begin(42);
  const uint8_t d[] = {9, 8, 7, 6, 5};
  TEST_ASSERT_TRUE(log.append(EventType::CommandResult, 123456, d, 5));
  TEST_ASSERT_EQUAL_size_t(0, st.lastWriteSlot);  // seq 1 -> slot 0
  LogEntry e;
  TEST_ASSERT_TRUE(unpack(st.cells[0].data(), &e));
  TEST_ASSERT_EQUAL_UINT32(1, e.seq);
  TEST_ASSERT_EQUAL_UINT16(42, e.bootCount);
  TEST_ASSERT_EQUAL_UINT32(123456, e.msSinceBoot);
  TEST_ASSERT_EQUAL_UINT8(3, static_cast<uint8_t>(e.type));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(d, e.data, 5);
}

void test_append_truncates_long_data() {
  FakeStorage st(8);
  EventLog log(st);
  log.begin(1);
  const uint8_t d[] = {1, 2, 3, 4, 5, 6, 7, 8};
  TEST_ASSERT_TRUE(appendDurable(log, EventType::Boot, 0, d, sizeof d));
  LogEntry e;
  TEST_ASSERT_EQUAL_size_t(1, log.readFrom(0, &e, 1));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(d, e.data, 5);
  // Bytes beyond the 16-byte entry were not written anywhere.
  TEST_ASSERT_EQUAL_UINT8(0xFF, st.cells[1][0]);
}

void test_append_zero_pads_short_data() {
  FakeStorage st(8);
  EventLog log(st);
  log.begin(1);
  const uint8_t d[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
  TEST_ASSERT_TRUE(appendDurable(log, EventType::Connect, 0, d, 2));
  LogEntry e;
  TEST_ASSERT_EQUAL_size_t(1, log.readFrom(0, &e, 1));
  const uint8_t want[] = {0xAA, 0xBB, 0, 0, 0};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(want, e.data, 5);
  // Raw storage bytes 13..15 are zero too (not leftover 0xFF).
  TEST_ASSERT_EQUAL_UINT8(0, st.cells[0][13]);
  TEST_ASSERT_EQUAL_UINT8(0, st.cells[0][15]);
}

void test_append_null_data() {
  FakeStorage st(8);
  EventLog log(st);
  log.begin(1);
  TEST_ASSERT_TRUE(appendDurable(log, EventType::IdleTimeout, 5, nullptr, 0));
  // Not covered by the header contract (null is only promised for len 0), but
  // the implementation treats null as all-zero data rather than crashing.
  TEST_ASSERT_TRUE(appendDurable(log, EventType::IdleTimeout, 6, nullptr, 5));
  LogEntry e[2];
  TEST_ASSERT_EQUAL_size_t(2, log.readFrom(0, e, 2));
  const uint8_t zero[kDataSize] = {};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(zero, e[0].data, kDataSize);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(zero, e[1].data, kDataSize);
}

// ---------------------------------------------------------------- wrap (LOG-004)

static void checkWrap(size_t cap, uint32_t total) {
  FakeStorage st(cap);
  EventLog log(st);
  log.begin(1);
  for (uint32_t t = 1; t <= total; t++) {
    TEST_ASSERT_TRUE(appendTagged(log, t));
    TEST_ASSERT_EQUAL_size_t((t - 1) % cap, st.lastWriteSlot);  // ring placement
  }
  TEST_ASSERT_EQUAL_UINT32(total + 1, log.nextSeq());
  size_t expect = total < cap ? total : cap;
  TEST_ASSERT_EQUAL_size_t(expect, log.size());
  auto all = dumpPaged(log);
  uint32_t first = total - static_cast<uint32_t>(expect) + 1;
  assertSeqs(range(first, total), seqsOf(all));
  for (auto &e : all) assertTagged(e, e.seq, 1);
  TEST_ASSERT_EQUAL_size_t(0, st.outOfRangeAccess);
}

void test_wrap_capacity_1() {
  for (uint32_t n : {1u, 2u, 3u, 17u}) checkWrap(1, n);
}
void test_wrap_capacity_2() {
  for (uint32_t n : {1u, 2u, 3u, 4u, 5u, 101u}) checkWrap(2, n);
}
void test_wrap_capacity_8() {
  for (uint32_t n = 1; n <= 8 * 6 + 3; n++) checkWrap(8, n);
}
void test_wrap_capacity_256_many_times() {
  for (uint32_t n : {1u, 255u, 256u, 257u, 512u, 513u, 256u * 10 + 77}) checkWrap(256, n);
}

// ---------------------------------------------------------------- readFrom (LOG-004, BLE §4.6)

void test_read_empty_log() {
  FakeStorage st(8);
  EventLog log(st);
  log.begin(1);
  LogEntry out[8];
  TEST_ASSERT_EQUAL_size_t(0, log.readFrom(0, out, 8));
  TEST_ASSERT_EQUAL_size_t(0, log.readFrom(1, out, 8));
  TEST_ASSERT_EQUAL_size_t(0, log.size());
}

void test_read_cursors() {
  FakeStorage st(256);
  EventLog log(st);
  log.begin(1);
  for (uint32_t t = 1; t <= 600; t++) appendTagged(log, t);  // retained: 345..600
  LogEntry out[300];

  // cursor 0 -> oldest retained
  TEST_ASSERT_EQUAL_size_t(256, log.readFrom(0, out, 300));
  TEST_ASSERT_EQUAL_UINT32(345, out[0].seq);
  TEST_ASSERT_EQUAL_UINT32(600, out[255].seq);
  // cursor older than oldest retained -> oldest retained
  TEST_ASSERT_EQUAL_size_t(8, log.readFrom(1, out, 8));
  TEST_ASSERT_EQUAL_UINT32(345, out[0].seq);
  TEST_ASSERT_EQUAL_size_t(8, log.readFrom(344, out, 8));
  TEST_ASSERT_EQUAL_UINT32(345, out[0].seq);
  // exact cursor
  TEST_ASSERT_EQUAL_size_t(8, log.readFrom(345, out, 8));
  TEST_ASSERT_EQUAL_UINT32(345, out[0].seq);
  TEST_ASSERT_EQUAL_size_t(8, log.readFrom(500, out, 8));
  for (size_t i = 0; i < 8; i++) TEST_ASSERT_EQUAL_UINT32(500 + i, out[i].seq);
  assertTagged(out[0], 500, 1);
  // tail shorter than a page
  TEST_ASSERT_EQUAL_size_t(3, log.readFrom(598, out, 8));
  TEST_ASSERT_EQUAL_UINT32(600, out[2].seq);
  // newest exactly
  TEST_ASSERT_EQUAL_size_t(1, log.readFrom(600, out, 8));
  // past the newest
  TEST_ASSERT_EQUAL_size_t(0, log.readFrom(601, out, 8));
  TEST_ASSERT_EQUAL_size_t(0, log.readFrom(0xFFFFFFFFu, out, 8));
  // degenerate args
  TEST_ASSERT_EQUAL_size_t(0, log.readFrom(0, out, 0));
  TEST_ASSERT_EQUAL_size_t(0, log.readFrom(0, nullptr, 8));
}

void test_read_paging_by_8_full_256() {
  FakeStorage st(256);
  EventLog log(st);
  log.begin(3);
  for (uint32_t t = 1; t <= 256 * 3 + 5; t++) appendTagged(log, t);
  uint32_t newest = 256 * 3 + 5;
  auto all = dumpPaged(log);
  assertSeqs(range(newest - 255, newest), seqsOf(all));
  for (auto &e : all) assertTagged(e, e.seq, 3);
}

void test_read_paging_by_8_with_gaps() {
  // Failed writes and unreadable slots leave gaps; paging still collects
  // everything else exactly once (BLE §4.6: "gaps are possible").
  FakeStorage st(64);
  EventLog log(st);
  log.begin(1);
  std::set<uint32_t> missing;
  for (uint32_t t = 1; t <= 200; t++) {
    bool fail = (t % 7 == 0) || (t >= 160 && t <= 175);  // a run of 16 > page size
    st.failAllWrites = fail;
    TEST_ASSERT_EQUAL(!fail, appendTagged(log, t));
    if (fail) missing.insert(t);
  }
  st.failAllWrites = false;
  st.unreadable[(150 - 1) % 64] = true;
  missing.insert(150);
  std::vector<uint32_t> want;
  for (uint32_t s = 137; s <= 200; s++)
    if (!missing.count(s)) want.push_back(s);
  auto all = dumpPaged(log);
  assertSeqs(want, seqsOf(all));
  for (auto &e : all) assertTagged(e, e.seq, 1);
}

void test_read_paging_with_appends_between_pages() {
  // Events keep arriving while a client pages; overwritten entries are
  // skipped and the client never sees a duplicate or goes backwards.
  FakeStorage st(16);
  EventLog log(st);
  log.begin(1);
  uint32_t tag = 0;
  for (int i = 0; i < 20; i++) appendTagged(log, ++tag);
  LogEntry buf[8];
  uint32_t cursor = 0, last = 0;
  std::vector<uint32_t> got;
  for (int pages = 0; pages < 100; pages++) {
    size_t n = log.readFrom(cursor, buf, 8);
    if (n == 0) break;
    for (size_t i = 0; i < n; i++) {
      TEST_ASSERT_TRUE(buf[i].seq > last);
      last = buf[i].seq;
      assertTagged(buf[i], buf[i].seq, 1);
      got.push_back(buf[i].seq);
    }
    cursor = last + 1;
    for (int k = 0; k < 10 && tag < 60; k++) appendTagged(log, ++tag);  // 10 new per page
  }
  TEST_ASSERT_EQUAL_UINT32(60, last);  // eventually caught up with the newest
}

// ---------------------------------------------------------------- reboot recovery (LOG-001, LOG-003)

void test_reboot_empty_storage() {
  FakeStorage st(256);
  EventLog log(st);
  log.begin(1);
  TEST_ASSERT_EQUAL_UINT32(1, log.nextSeq());
  TEST_ASSERT_EQUAL_size_t(0, log.size());
  st.erasedReadsOk = true;  // raw flash: unwritten slots read back as 0xFF
  EventLog log2(st);
  log2.begin(2);
  TEST_ASSERT_EQUAL_UINT32(1, log2.nextSeq());
  LogEntry out[8];
  TEST_ASSERT_EQUAL_size_t(0, log2.readFrom(0, out, 8));
}

void test_reboot_partial_and_new_bootcount() {
  FakeStorage st(256);
  {
    EventLog log(st);
    log.begin(1);
    for (uint32_t t = 1; t <= 10; t++) appendTagged(log, t);
  }
  EventLog log(st);
  log.begin(2);
  TEST_ASSERT_EQUAL_UINT32(11, log.nextSeq());
  TEST_ASSERT_EQUAL_size_t(10, log.size());
  appendTagged(log, 11);
  auto all = dumpPaged(log);
  assertSeqs(range(1, 11), seqsOf(all));
  for (uint32_t i = 0; i < 10; i++) assertTagged(all[i], i + 1, 1);
  assertTagged(all[10], 11, 2);  // new bootCount stamped on later entries
}

void test_reboot_wrapped_many_times() {
  FakeStorage st(256);
  uint32_t tag = 0;
  for (uint16_t boot = 1; boot <= 6; boot++) {
    EventLog log(st);
    log.begin(boot);
    TEST_ASSERT_EQUAL_UINT32(tag + 1, log.nextSeq());  // seq continues across reboots
    for (int i = 0; i < 300; i++) appendTagged(log, ++tag);
  }
  EventLog log(st);
  log.begin(7);
  TEST_ASSERT_EQUAL_UINT32(1801, log.nextSeq());
  TEST_ASSERT_EQUAL_size_t(256, log.size());
  auto all = dumpPaged(log);
  assertSeqs(range(1545, 1800), seqsOf(all));
  for (auto &e : all) assertTagged(e, e.seq, static_cast<uint16_t>((e.seq - 1) / 300 + 1));
}

void test_reboot_wrapped_exactly_at_boundary() {
  for (uint32_t total : {255u, 256u, 257u}) {
    FakeStorage st(256);
    {
      EventLog log(st);
      log.begin(1);
      for (uint32_t t = 1; t <= total; t++) appendTagged(log, t);
    }
    EventLog log(st);
    log.begin(2);
    TEST_ASSERT_EQUAL_UINT32(total + 1, log.nextSeq());
  }
}

void test_reboot_corrupted_storage() {
  FakeStorage st(8);
  {
    EventLog log(st);
    log.begin(1);
    for (uint32_t t = 1; t <= 20; t++) appendTagged(log, t);  // slots hold 13..20
  }
  // slot 0 = seq 17, slot 3 = 20 (newest), slot 4 = 13, slot 5 = 14, slot 6 = 15
  st.unreadable[4] = true;                         // seq 13 unreadable
  uint8_t g[kEntrySize];
  memset(g, 0xAB, sizeof g);                       // garbage, invalid type
  st.poke(5, g);                                   // seq 14 destroyed
  makeRaw(g, 1000001, 1);                          // valid-looking, wrong slot
  st.poke(6, g);                                   // (1000001-1)%8 = 0 != 6
  EventLog log(st);
  log.begin(2);
  TEST_ASSERT_EQUAL_UINT32(21, log.nextSeq());     // misplaced 1000001 ignored
  auto all = dumpPaged(log);
  assertSeqs({16, 17, 18, 19, 20}, seqsOf(all));
  for (auto &e : all) assertTagged(e, e.seq, 1);
  TEST_ASSERT_EQUAL_size_t(0, st.outOfRangeAccess);
}

void test_reboot_all_slots_unreadable() {
  FakeStorage st(8);
  {
    EventLog log(st);
    log.begin(1);
    for (uint32_t t = 1; t <= 12; t++) appendTagged(log, t);
  }
  for (size_t i = 0; i < 8; i++) st.unreadable[i] = true;
  EventLog log(st);
  log.begin(2);
  TEST_ASSERT_EQUAL_UINT32(1, log.nextSeq());  // nothing recoverable
  LogEntry out[8];
  TEST_ASSERT_EQUAL_size_t(0, log.readFrom(0, out, 8));
}

void test_reboot_after_capacity_change_ignores_misplaced() {
  // Entries written with capacity 8 then read with capacity 4: entries whose
  // slot no longer matches are ignored instead of trusted.
  FakeStorage st(8);
  {
    EventLog log(st);
    log.begin(1);
    for (uint32_t t = 1; t <= 8; t++) appendTagged(log, t);  // slot i holds seq i+1
  }
  st.cap_ = 4;  // only slots 0..3 visible: seqs 1,2,3,4 -> (s-1)%4 == slot
  EventLog log(st);
  log.begin(2);
  TEST_ASSERT_EQUAL_UINT32(5, log.nextSeq());
  TEST_ASSERT_EQUAL_size_t(0, st.outOfRangeAccess);
}

// ---------------------------------------------------------------- fault handling (LOG-003)

void test_fault_failed_write_consumes_seq_and_hides_stale() {
  FakeStorage st(8);
  EventLog log(st);
  log.begin(1);
  for (uint32_t t = 1; t <= 10; t++) appendTagged(log, t);  // slot 2 holds seq 3
  st.failAllWrites = true;
  TEST_ASSERT_FALSE(appendTagged(log, 11));  // seq 11 -> slot 2, fails
  st.failAllWrites = false;
  TEST_ASSERT_EQUAL_UINT32(12, log.nextSeq());  // consumed
  TEST_ASSERT_TRUE(appendTagged(log, 12));
  auto all = dumpPaged(log);
  // Window is 5..12; seq 11 missing, and the stale seq 3 in slot 2 is never
  // returned (as 3 or as 11).
  assertSeqs({5, 6, 7, 8, 9, 10, 12}, seqsOf(all));
  for (auto &e : all) assertTagged(e, e.seq, 1);
  LogEntry one;
  TEST_ASSERT_EQUAL_size_t(1, log.readFrom(11, &one, 1));
  TEST_ASSERT_EQUAL_UINT32(12, one.seq);
}

void test_fault_failed_write_unwrapped_slot() {
  FakeStorage st(8);
  EventLog log(st);
  log.begin(1);
  appendTagged(log, 1);
  st.failWrite[1] = true;
  TEST_ASSERT_FALSE(appendTagged(log, 2));
  appendTagged(log, 3);
  assertSeqs({1, 3}, seqsOf(dumpPaged(log)));
}

void test_fault_unreadable_slot_skipped_on_read() {
  FakeStorage st(8);
  EventLog log(st);
  log.begin(1);
  for (uint32_t t = 1; t <= 5; t++) appendTagged(log, t);
  st.unreadable[2] = true;
  assertSeqs({1, 2, 4, 5}, seqsOf(dumpPaged(log)));
}

void test_fault_capacity_zero() {
  FakeStorage st(0);
  EventLog log(st);
  log.begin(1);
  TEST_ASSERT_EQUAL_UINT32(1, log.nextSeq());
  TEST_ASSERT_FALSE(log.append(EventType::Boot, 0, nullptr, 0));
  TEST_ASSERT_FALSE(appendTagged(log, 2));
  TEST_ASSERT_EQUAL_UINT32(3, log.nextSeq());  // seqs consumed even with no storage
  TEST_ASSERT_EQUAL_size_t(0, log.size());
  LogEntry out[8];
  TEST_ASSERT_EQUAL_size_t(0, log.readFrom(0, out, 8));
  TEST_ASSERT_EQUAL_size_t(0, st.writes);
  TEST_ASSERT_EQUAL_size_t(0, st.reads);
  EventLog log2(st);
  log2.begin(2);
  TEST_ASSERT_EQUAL_UINT32(1, log2.nextSeq());
}

// ---------------------------------------------------------------- simulated power loss (LOG-001)

// Cut the power after `okWrites` successful appends; the next write is torn
// with `torn` bytes landing. Then recover on the same storage and check:
// every surviving entry is intact and the recovered nextSeq is sane.
static void powerCutCase(size_t cap, uint32_t before, uint32_t okWrites, size_t torn) {
  FakeStorage st(cap);
  uint32_t tag = 0;
  {
    EventLog log(st);
    log.begin(1);
    for (uint32_t i = 0; i < before; i++) appendTagged(log, ++tag);
  }
  uint32_t lastGood;
  {
    EventLog log(st);
    log.begin(2);
    st.powerCutAfter = okWrites;
    st.tornBytes = torn;
    for (uint32_t i = 0; i < okWrites + 5; i++) appendTagged(log, ++tag);  // later ones fail
    lastGood = before + okWrites;
  }
  st.restorePower();
  EventLog log(st);
  log.begin(3);
  uint32_t ns = log.nextSeq();
  // torn == 16 means the whole entry landed (storage reported failure late).
  uint32_t expectNext = (torn == kEntrySize) ? lastGood + 2 : lastGood + 1;
  if (torn == 0 || torn == kEntrySize) TEST_ASSERT_EQUAL_UINT32(expectNext, ns);
  auto all = dumpPaged(log);
  uint32_t newestIntact = (torn == kEntrySize) ? lastGood + 1 : lastGood;
  for (auto &e : all) {
    if (e.seq <= newestIntact) {
      assertTagged(e, e.seq, e.seq <= before ? 1 : 2);
    }
  }
  // Everything intact inside the window is returned.
  uint32_t lo = newestIntact > cap ? newestIntact - static_cast<uint32_t>(cap) + 1 : 1;
  std::set<uint32_t> got;
  for (auto &e : all) got.insert(e.seq);
  uint32_t tornSlotSeq = lastGood + 1;
  for (uint32_t s = lo; s <= newestIntact; s++) {
    bool overwrittenByTorn = torn > 0 && torn < kEntrySize && ((s - 1) % cap == (tornSlotSeq - 1) % cap);
    if (!overwrittenByTorn) TEST_ASSERT_TRUE_MESSAGE(got.count(s), "intact entry lost after power cut");
  }
  // And logging works again afterwards.
  TEST_ASSERT_TRUE(appendTagged(log, 999));
  LogEntry last;
  TEST_ASSERT_EQUAL_size_t(1, log.readFrom(ns, &last, 1));
  TEST_ASSERT_EQUAL_UINT32(ns, last.seq);
  TEST_ASSERT_EQUAL_UINT16(3, last.bootCount);
}

void test_power_cut_clean() {
  powerCutCase(8, 0, 3, 0);
  powerCutCase(8, 5, 10, 0);
  powerCutCase(256, 300, 100, 0);
}

void test_power_cut_whole_entry_landed() {
  powerCutCase(8, 5, 10, kEntrySize);
  powerCutCase(256, 1000, 7, kEntrySize);
}

void test_power_cut_torn_writes() {
  for (size_t torn = 1; torn < kEntrySize; torn++) {
    powerCutCase(8, 5, 10, torn);
    powerCutCase(8, 0, 2, torn);  // torn into a never-written (0xFF) slot
    powerCutCase(256, 600, 3, torn);
  }
}

// ---------------------------------------------------------------- documented limitations

void limitation_size_is_upper_bound() {
  // LIMITATION: size() is an upper bound when slots are lost.
  FakeStorage st(8);
  EventLog log(st);
  log.begin(1);
  for (uint32_t t = 1; t <= 8; t++) {
    st.failAllWrites = (t % 2 == 0);
    appendTagged(log, t);
  }
  st.failAllWrites = false;
  st.unreadable[0] = true;
  TEST_ASSERT_EQUAL_size_t(8, log.size());               // claims 8
  TEST_ASSERT_EQUAL_size_t(3, dumpPaged(log).size());    // only 3, 5, 7 retrievable
}

void limitation_seq_reused_after_failed_newest_write() {
  // LIMITATION: after a reboot, seqs whose writes failed can be reused.
  FakeStorage st(8);
  {
    EventLog log(st);
    log.begin(1);
    for (uint32_t t = 1; t <= 5; t++) appendTagged(log, t);
    st.failAllWrites = true;
    appendTagged(log, 6);
    appendTagged(log, 7);
    TEST_ASSERT_EQUAL_UINT32(8, log.nextSeq());
    st.failAllWrites = false;
  }
  EventLog log(st);
  log.begin(2);
  TEST_ASSERT_EQUAL_UINT32(6, log.nextSeq());  // 6 and 7 will be issued again
}

void limitation_seq_reused_after_newest_corrupted() {
  // LIMITATION: if the newest entry gets corrupted, its seq is reused after
  // reboot. A BLE client whose cursor was already past it misses the new one.
  FakeStorage st(8);
  {
    EventLog log(st);
    log.begin(1);
    for (uint32_t t = 1; t <= 12; t++) appendTagged(log, t);
  }
  st.unreadable[(12 - 1) % 8] = true;
  EventLog log(st);
  log.begin(2);
  TEST_ASSERT_EQUAL_UINT32(12, log.nextSeq());
  st.unreadable[(12 - 1) % 8] = false;
  appendTagged(log, 500);
  LogEntry out[8];
  TEST_ASSERT_EQUAL_size_t(0, log.readFrom(13, out, 8));  // client at cursor 13 sees nothing
  TEST_ASSERT_EQUAL_size_t(1, log.readFrom(12, out, 8));
  TEST_ASSERT_EQUAL_UINT16(2, out[0].bootCount);         // a different event under seq 12
}

void limitation_garbage_that_looks_valid_is_accepted() {
  // LIMITATION: no checksum. A slot holding garbage that has a seq in
  // 1..kMaxSeq matching its slot and a known type is returned as a real
  // entry, and a high fake seq makes recovery jump ahead (but never past
  // kMaxSeq; see test_reboot_garbage_above_kMaxSeq_is_ignored).
  FakeStorage st(8);
  {
    EventLog log(st);
    log.begin(1);
    for (uint32_t t = 1; t <= 4; t++) appendTagged(log, t);
  }
  uint8_t g[kEntrySize];
  makeRaw(g, 4, 2);  // overwrite seq 4's slot with plausible junk
  st.poke(3, g);
  makeRaw(g, 1000006, 6);  // (1000006-1)%8 == 5 -> fits slot 5
  st.poke(5, g);
  EventLog log(st);
  log.begin(2);
  TEST_ASSERT_EQUAL_UINT32(1000007, log.nextSeq());  // jumped
  auto all = dumpPaged(log);
  // Window is now 999999..1000006, so the genuine 1..4 vanish from reads.
  assertSeqs({1000006}, seqsOf(all));
  TEST_ASSERT_EQUAL_UINT16(9, all[0].bootCount);  // junk values returned as real
}

void test_reboot_garbage_above_kMaxSeq_is_ignored() {
  // Same shape of garbage as above, but with seqs beyond kMaxSeq that still
  // match their slot: recovery must ignore them instead of jumping toward
  // wraparound.
  FakeStorage st(8);
  {
    EventLog log(st);
    log.begin(1);
    for (uint32_t t = 1; t <= 4; t++) appendTagged(log, t);
  }
  uint8_t g[kEntrySize];
  makeRaw(g, 0x80000006u, 6);  // (0x80000006-1)%8 == 5
  st.poke(5, g);
  makeRaw(g, 0xFFFFFFFFu, 2);  // (0xFFFFFFFF-1)%8 == 6
  st.poke(6, g);
  EventLog log(st);
  log.begin(2);
  TEST_ASSERT_EQUAL_UINT32(5, log.nextSeq());
  TEST_ASSERT_EQUAL_UINT32(4, log.durableSeq());
  auto all = dumpPaged(log);
  assertSeqs({1, 2, 3, 4}, seqsOf(all));
  for (auto &e : all) assertTagged(e, e.seq, 1);
}

void limitation_torn_write_mixes_new_seq_with_old_payload() {
  // LIMITATION (no checksum): a write torn after the 4 seq bytes leaves the
  // new seq in front of the previous occupant's payload; it is accepted as
  // the new seq with the old event's contents.
  FakeStorage st(8);
  uint32_t tag = 0;
  {
    EventLog log(st);
    log.begin(1);
    for (int i = 0; i < 8; i++) appendTagged(log, ++tag);  // slot 0 holds seq 1
    st.powerCutAfter = 0;
    st.tornBytes = 4;
    appendTagged(log, ++tag);  // seq 9 -> slot 0, torn
  }
  st.restorePower();
  EventLog log(st);
  log.begin(2);
  TEST_ASSERT_EQUAL_UINT32(10, log.nextSeq());
  LogEntry e;
  TEST_ASSERT_EQUAL_size_t(1, log.readFrom(9, &e, 1));
  TEST_ASSERT_EQUAL_UINT32(9, e.seq);
  assertTagged(e, 1, 1);  // payload of seq 1, not seq 9
}

// ---------------------------------------------------------------- header contract at the seq ceiling

void test_seq_never_zero_even_after_max_seq_in_storage() {
  // Header: valid seqs are 1..kMaxSeq; LOG-003: seq increases across
  // reboots and is never 0. A slot holding seq 0xFFFFFFFF in its correct slot
  // used to make begin() set nextSeq to 0; it is now rejected as corrupt.
  FakeStorage st(256);
  uint8_t g[kEntrySize];
  makeRaw(g, 0xFFFFFFFFu, 1);
  st.poke((0xFFFFFFFFu - 1) % 256, g);  // slot 254
  EventLog log(st);
  log.begin(1);
  TEST_ASSERT_NOT_EQUAL_MESSAGE(0, log.nextSeq(), "nextSeq() is 0 after recovering seq 0xFFFFFFFF");
  TEST_ASSERT_EQUAL_UINT32(1, log.nextSeq());
  TEST_ASSERT_TRUE(appendTagged(log, 1));
  TEST_ASSERT_TRUE(appendTagged(log, 2));
  EventLog log2(st);
  log2.begin(2);
  // After a reboot the next seq must not go backwards.
  TEST_ASSERT_EQUAL_UINT32(3, log2.nextSeq());
}

// Puts a valid entry with `seq` in its correct slot, as if written earlier.
static void pokeSeq(FakeStorage &st, uint32_t seq) {
  uint8_t g[kEntrySize];
  makeRaw(g, seq, 1);
  st.poke((seq - 1) % st.capacity(), g);
}

void test_maxseq_append_refuses_once_exhausted() {
  FakeStorage st(8);
  pokeSeq(st, kMaxSeq - 1);
  EventLog log(st);
  log.begin(1);
  TEST_ASSERT_EQUAL_UINT32(kMaxSeq, log.nextSeq());
  TEST_ASSERT_TRUE(appendTagged(log, 1));  // the last valid seq
  TEST_ASSERT_EQUAL_size_t((kMaxSeq - 1) % 8, st.lastWriteSlot);
  TEST_ASSERT_EQUAL_UINT32(kMaxSeq + 1, log.nextSeq());
  size_t writes = st.writes;
  for (uint32_t t = 2; t < 6; t++) {
    TEST_ASSERT_FALSE(appendTagged(log, t));
    TEST_ASSERT_FALSE(log.append(EventType::Boot, 0, nullptr, 0));
  }
  TEST_ASSERT_EQUAL_size_t_MESSAGE(writes, st.writes, "refused append wrote to storage");
  TEST_ASSERT_EQUAL_UINT32(kMaxSeq + 1, log.nextSeq());  // never wraps to 0
  TEST_ASSERT_EQUAL_UINT32(kMaxSeq, log.durableSeq());
  auto all = dumpPaged(log);
  assertSeqs({kMaxSeq - 1, kMaxSeq}, seqsOf(all));
  assertTagged(all[1], 1, 1);
}

void test_maxseq_exhausted_after_reboot_stays_refused() {
  FakeStorage st(256);
  pokeSeq(st, kMaxSeq);
  EventLog log(st);
  log.begin(1);
  TEST_ASSERT_EQUAL_UINT32(kMaxSeq + 1, log.nextSeq());
  TEST_ASSERT_EQUAL_UINT32(kMaxSeq, log.durableSeq());
  size_t writes = st.writes;
  TEST_ASSERT_FALSE(appendTagged(log, 1));
  TEST_ASSERT_EQUAL_size_t(writes, st.writes);
  log.markDurable(0xFFFFFFFFu);
  TEST_ASSERT_EQUAL_UINT32(kMaxSeq, log.durableSeq());
  LogEntry e[2];
  TEST_ASSERT_EQUAL_size_t(1, log.readFrom(0, e, 2));
  TEST_ASSERT_EQUAL_UINT32(kMaxSeq, e[0].seq);
  TEST_ASSERT_EQUAL_size_t(0, log.readFrom(0xFFFFFFFFu, e, 2));
}

// ---------------------------------------------------------------- durability gating

void test_durable_append_not_readable_until_markDurable() {
  FakeStorage st(8);
  EventLog log(st);
  log.begin(1);
  TEST_ASSERT_EQUAL_UINT32(0, log.durableSeq());
  for (uint32_t t = 1; t <= 3; t++) TEST_ASSERT_TRUE(appendBuffered(log, t));
  LogEntry out[8];
  TEST_ASSERT_EQUAL_size_t(0, log.readFrom(0, out, 8));
  TEST_ASSERT_EQUAL_size_t(0, log.readFrom(3, out, 8));
  log.markDurable(2);
  TEST_ASSERT_EQUAL_UINT32(2, log.durableSeq());
  assertSeqs({1, 2}, seqsOf(dumpPaged(log)));
  TEST_ASSERT_EQUAL_size_t(0, log.readFrom(3, out, 8));
  log.markDurable(3);
  auto all = dumpPaged(log);
  assertSeqs({1, 2, 3}, seqsOf(all));
  for (auto &e : all) assertTagged(e, e.seq, 1);
}

void test_durable_markDurable_clamps_and_never_moves_backwards() {
  FakeStorage st(8);
  EventLog log(st);
  log.begin(1);
  log.markDurable(5);  // nothing issued yet
  TEST_ASSERT_EQUAL_UINT32(0, log.durableSeq());
  for (uint32_t t = 1; t <= 5; t++) appendBuffered(log, t);
  log.markDurable(3);
  TEST_ASSERT_EQUAL_UINT32(3, log.durableSeq());
  log.markDurable(1);
  TEST_ASSERT_EQUAL_UINT32(3, log.durableSeq());
  log.markDurable(0);
  TEST_ASSERT_EQUAL_UINT32(3, log.durableSeq());
  log.markDurable(6);  // == nextSeq(): not issued
  TEST_ASSERT_EQUAL_UINT32(5, log.durableSeq());
  log.markDurable(0xFFFFFFFFu);
  TEST_ASSERT_EQUAL_UINT32(5, log.durableSeq());
  appendBuffered(log, 6);
  TEST_ASSERT_EQUAL_UINT32(5, log.durableSeq());  // append alone does not advance it
  log.markDurable(4);
  TEST_ASSERT_EQUAL_UINT32(5, log.durableSeq());
}

void test_durable_after_reboot_is_highest_recovered() {
  FakeStorage st(8);
  {
    EventLog log(st);
    log.begin(1);
    for (uint32_t t = 1; t <= 10; t++) appendBuffered(log, t);
    log.markDurable(4);  // storage is write-through, so 5..10 are on flash too
  }
  {
    EventLog log(st);
    log.begin(2);
    TEST_ASSERT_EQUAL_UINT32(10, log.durableSeq());
    TEST_ASSERT_EQUAL_UINT32(11, log.nextSeq());
    assertSeqs(range(3, 10), seqsOf(dumpPaged(log)));
  }
  // Newest slot unreadable: durable stops at what was actually recovered.
  st.unreadable[(10 - 1) % 8] = true;
  EventLog log(st);
  log.begin(3);
  TEST_ASSERT_EQUAL_UINT32(9, log.durableSeq());
  // Empty storage: nothing durable.
  FakeStorage empty(8);
  EventLog log2(empty);
  log2.begin(1);
  TEST_ASSERT_EQUAL_UINT32(0, log2.durableSeq());
}

void test_durable_lost_buffered_writes_were_never_visible_so_reuse_is_invisible() {
  // Power is lost before the flush: entries 6 and 7 vanish, and their seqs
  // are issued again after reboot. Because 6 and 7 were never readable, a
  // client paging throughout sees each seq exactly once and misses nothing.
  FakeStorage st(16);
  st.buffered = true;
  std::vector<LogEntry> client;
  uint32_t cursor = 0;
  auto page = [&](EventLog &log) {
    LogEntry buf[8];
    size_t n;
    while ((n = log.readFrom(cursor, buf, 8)) > 0) {
      for (size_t i = 0; i < n; i++) client.push_back(buf[i]);
      cursor = buf[n - 1].seq + 1;
    }
  };
  {
    EventLog log(st);
    log.begin(1);
    for (uint32_t t = 1; t <= 5; t++) TEST_ASSERT_TRUE(appendBuffered(log, t));
    st.flush();
    log.markDurable(log.nextSeq() - 1);
    TEST_ASSERT_TRUE(appendBuffered(log, 6));  // accepted, still in RAM
    TEST_ASSERT_TRUE(appendBuffered(log, 7));
    page(log);
    assertSeqs(range(1, 5), seqsOf(client));
    st.loseBuffer();  // power loss before the next flush
  }
  EventLog log(st);
  log.begin(2);
  TEST_ASSERT_EQUAL_UINT32(6, log.nextSeq());
  TEST_ASSERT_EQUAL_UINT32(5, log.durableSeq());
  for (uint32_t t = 106; t <= 108; t++) TEST_ASSERT_TRUE(appendBuffered(log, t));
  page(log);
  assertSeqs(range(1, 5), seqsOf(client));  // nothing new until the flush
  st.flush();
  log.markDurable(log.nextSeq() - 1);
  page(log);
  assertSeqs(range(1, 8), seqsOf(client));
  for (uint32_t i = 0; i < 5; i++) assertTagged(client[i], i + 1, 1);
  for (uint32_t i = 5; i < 8; i++) assertTagged(client[i], 101 + i, 2);  // the new events
}

void test_durable_ring_overwrite_by_buffered_appends_hides_old_entries() {
  // Appends that are not durable yet still claim their ring slots, so the
  // durable entries they replaced are gone and nothing newer is shown.
  FakeStorage st(4);
  st.buffered = true;
  EventLog log(st);
  log.begin(1);
  for (uint32_t t = 1; t <= 4; t++) appendBuffered(log, t);
  st.flush();
  log.markDurable(4);
  assertSeqs(range(1, 4), seqsOf(dumpPaged(log)));
  for (uint32_t t = 5; t <= 6; t++) appendBuffered(log, t);  // over slots 0, 1
  assertSeqs({3, 4}, seqsOf(dumpPaged(log)));
  st.flush();
  log.markDurable(6);
  auto all = dumpPaged(log);
  assertSeqs(range(3, 6), seqsOf(all));
  for (auto &e : all) assertTagged(e, e.seq, 1);
}

// ---------------------------------------------------------------- runner

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_pack_exact_layout_little_endian);
  RUN_TEST(test_pack_matches_ble_spec_worked_example);
  RUN_TEST(test_pack_unpack_roundtrip);
  RUN_TEST(test_unpack_rejects_seq_zero);
  RUN_TEST(test_unpack_rejects_seq_above_kMaxSeq);
  RUN_TEST(test_unpack_accepts_exactly_the_six_known_types);
  RUN_TEST(test_unpack_rejects_erased_and_zeroed);
  RUN_TEST(test_append_stamps_fields_and_placement);
  RUN_TEST(test_append_truncates_long_data);
  RUN_TEST(test_append_zero_pads_short_data);
  RUN_TEST(test_append_null_data);
  RUN_TEST(test_wrap_capacity_1);
  RUN_TEST(test_wrap_capacity_2);
  RUN_TEST(test_wrap_capacity_8);
  RUN_TEST(test_wrap_capacity_256_many_times);
  RUN_TEST(test_read_empty_log);
  RUN_TEST(test_read_cursors);
  RUN_TEST(test_read_paging_by_8_full_256);
  RUN_TEST(test_read_paging_by_8_with_gaps);
  RUN_TEST(test_read_paging_with_appends_between_pages);
  RUN_TEST(test_reboot_empty_storage);
  RUN_TEST(test_reboot_partial_and_new_bootcount);
  RUN_TEST(test_reboot_wrapped_many_times);
  RUN_TEST(test_reboot_wrapped_exactly_at_boundary);
  RUN_TEST(test_reboot_corrupted_storage);
  RUN_TEST(test_reboot_all_slots_unreadable);
  RUN_TEST(test_reboot_after_capacity_change_ignores_misplaced);
  RUN_TEST(test_fault_failed_write_consumes_seq_and_hides_stale);
  RUN_TEST(test_fault_failed_write_unwrapped_slot);
  RUN_TEST(test_fault_unreadable_slot_skipped_on_read);
  RUN_TEST(test_fault_capacity_zero);
  RUN_TEST(test_power_cut_clean);
  RUN_TEST(test_power_cut_whole_entry_landed);
  RUN_TEST(test_power_cut_torn_writes);
  RUN_TEST(limitation_size_is_upper_bound);
  RUN_TEST(limitation_seq_reused_after_failed_newest_write);
  RUN_TEST(limitation_seq_reused_after_newest_corrupted);
  RUN_TEST(limitation_garbage_that_looks_valid_is_accepted);
  RUN_TEST(test_reboot_garbage_above_kMaxSeq_is_ignored);
  RUN_TEST(limitation_torn_write_mixes_new_seq_with_old_payload);
  RUN_TEST(test_seq_never_zero_even_after_max_seq_in_storage);
  RUN_TEST(test_maxseq_append_refuses_once_exhausted);
  RUN_TEST(test_maxseq_exhausted_after_reboot_stays_refused);
  RUN_TEST(test_durable_append_not_readable_until_markDurable);
  RUN_TEST(test_durable_markDurable_clamps_and_never_moves_backwards);
  RUN_TEST(test_durable_after_reboot_is_highest_recovered);
  RUN_TEST(test_durable_lost_buffered_writes_were_never_visible_so_reuse_is_invisible);
  RUN_TEST(test_durable_ring_overwrite_by_buffered_appends_hides_old_entries);
  return UNITY_END();
}

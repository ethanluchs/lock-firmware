// Host-side unit tests for lib/lock_protocol (wire encoders), against
// docs/ble-spec.md v1.1 (§3 UUIDs, §4.4 status, §4.5 info, §4.6 log).
// Requirement IDs refer to docs/v1-spec.md.
//
// Run: pio test -e native -f test_protocol

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "event_log.h"
#include "lock_protocol.h"
#include "lock_types.h"

using lock::CmdResult;
using lock::kNone;
using lock::LockState;
using lock::log::EventType;
using lock::log::LogEntry;
namespace proto = lock::proto;

void setUp() {}
void tearDown() {}

static const uint8_t kGuard = 0xA5;

// ---------------------------------------------------------------------------
// UUIDs (ble-spec §3 quick reference / GATT table; BLE-001, BLE-008)
// ---------------------------------------------------------------------------

// ble-spec writes them upper-case and says comparison is case-insensitive.
static void assertUuidCaseInsensitive(const char *specUpper, const char *actual) {
  TEST_ASSERT_NOT_NULL(actual);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(strlen(specUpper), strlen(actual), specUpper);
  for (size_t i = 0; specUpper[i]; ++i) {
    char a = actual[i];
    if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 'a' + 'A');
    if (a != specUpper[i]) {
      char msg[128];
      snprintf(msg, sizeof msg, "UUID mismatch at index %u: spec %s, code %s",
               static_cast<unsigned>(i), specUpper, actual);
      TEST_FAIL_MESSAGE(msg);
    }
  }
}

void test_uuid_service_matches_ble_spec() {
  assertUuidCaseInsensitive("8F2A1C00-5B7E-4D19-9A3F-6C2E81D4B70A", proto::kServiceUuid);
}
void test_uuid_command_char_matches_ble_spec() {
  assertUuidCaseInsensitive("8F2A1C01-5B7E-4D19-9A3F-6C2E81D4B70A", proto::kCommandCharUuid);
}
void test_uuid_status_char_matches_ble_spec() {
  assertUuidCaseInsensitive("8F2A1C02-5B7E-4D19-9A3F-6C2E81D4B70A", proto::kStatusCharUuid);
}
void test_uuid_info_char_matches_ble_spec() {
  assertUuidCaseInsensitive("8F2A1C03-5B7E-4D19-9A3F-6C2E81D4B70A", proto::kInfoCharUuid);
}
void test_uuid_log_char_matches_ble_spec() {
  assertUuidCaseInsensitive("8F2A1C04-5B7E-4D19-9A3F-6C2E81D4B70A", proto::kLogCharUuid);
}

// ---------------------------------------------------------------------------
// Constants (ble-spec §4.4-4.6; BLE-003)
// ---------------------------------------------------------------------------

void test_protocol_version_is_1_1_and_sizes_match_spec() {
  TEST_ASSERT_EQUAL_UINT8(1, proto::kProtocolMajor);
  TEST_ASSERT_EQUAL_UINT8(1, proto::kProtocolMinor);
  TEST_ASSERT_EQUAL_size_t(4, proto::kStatusLen);
  TEST_ASSERT_EQUAL_size_t(5, proto::kInfoLen);
  TEST_ASSERT_EQUAL_size_t(4, proto::kLogCursorLen);
  TEST_ASSERT_EQUAL_size_t(8, proto::kLogPageMaxEntries);
  TEST_ASSERT_EQUAL_size_t(16, lock::log::kEntrySize);
  TEST_ASSERT_EQUAL_size_t(129, proto::kLogPageMaxLen);
}

// ---------------------------------------------------------------------------
// Status (ble-spec §4.4; BLE-002)
// ---------------------------------------------------------------------------

static void checkStatus(LockState st, uint8_t op, uint8_t res, uint8_t seq,
                        const uint8_t expected[4]) {
  uint8_t out[proto::kStatusLen + 2];
  memset(out, kGuard, sizeof out);
  proto::Status s{st, op, res, seq};
  proto::encodeStatus(s, out);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, 4);
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(kGuard, out[4], "encodeStatus wrote past 4 bytes");
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(kGuard, out[5], "encodeStatus wrote past 4 bytes");
}

// Every row of the §4.4 worked-examples table.
void test_status_worked_examples_from_ble_spec_4_4() {
  const uint8_t r1[] = {0x00, 0xFF, 0xFF, 0x00};
  checkStatus(LockState::Locked, kNone, kNone, 0, r1);
  const uint8_t r2[] = {0x01, 0x01, 0x00, 0x01};
  checkStatus(LockState::Unlocking, 0x01, static_cast<uint8_t>(CmdResult::Ok), 1, r2);
  const uint8_t r3[] = {0x02, 0x01, 0x00, 0x01};
  checkStatus(LockState::Unlocked, 0x01, static_cast<uint8_t>(CmdResult::Ok), 1, r3);
  const uint8_t r4[] = {0x02, 0x01, 0x01, 0x02};
  checkStatus(LockState::Unlocked, 0x01, static_cast<uint8_t>(CmdResult::NoChange), 2, r4);
  const uint8_t r5[] = {0x02, 0x07, 0x03, 0x03};
  checkStatus(LockState::Unlocked, 0x07, static_cast<uint8_t>(CmdResult::UnknownCommand), 3, r5);
  const uint8_t r6[] = {0x02, 0x01, 0x04, 0x04};
  checkStatus(LockState::Unlocked, 0x01, static_cast<uint8_t>(CmdResult::Malformed), 4, r6);
  const uint8_t r7[] = {0x03, 0x00, 0x00, 0x05};
  checkStatus(LockState::Locking, 0x00, static_cast<uint8_t>(CmdResult::Ok), 5, r7);
  const uint8_t r8[] = {0x03, 0x01, 0x02, 0x06};
  checkStatus(LockState::Locking, 0x01, static_cast<uint8_t>(CmdResult::Busy), 6, r8);
  const uint8_t r9[] = {0x00, 0x01, 0x02, 0x06};
  checkStatus(LockState::Locked, 0x01, static_cast<uint8_t>(CmdResult::Busy), 6, r9);
}

void test_status_fields_are_independent_bytes_in_order_state_op_result_seq() {
  const uint8_t e[] = {0x03, 0xAB, 0xCD, 0xFE};
  checkStatus(LockState::Locking, 0xAB, 0xCD, 0xFE, e);
  const uint8_t e2[] = {0x02, 0xFF, 0xFF, 0xFF};
  checkStatus(LockState::Unlocked, 0xFF, 0xFF, 0xFF, e2);
}

// ---------------------------------------------------------------------------
// Info (ble-spec §4.5; BLE-003, DIAG-003)
// ---------------------------------------------------------------------------

void test_info_example_from_ble_spec_4_5_protocol_1_1_firmware_0_2_3() {
  uint8_t out[proto::kInfoLen + 2];
  memset(out, kGuard, sizeof out);
  proto::encodeInfo(0, 2, 3, out);
  const uint8_t expected[] = {0x01, 0x01, 0x00, 0x02, 0x03};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, 5);
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(kGuard, out[5], "encodeInfo wrote past 5 bytes");
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(kGuard, out[6], "encodeInfo wrote past 5 bytes");
}

void test_info_firmware_bytes_are_major_minor_patch_in_order() {
  uint8_t out[proto::kInfoLen];
  proto::encodeInfo(0xFE, 0x80, 0x7F, out);
  const uint8_t expected[] = {0x01, 0x01, 0xFE, 0x80, 0x7F};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, 5);
}

// ---------------------------------------------------------------------------
// decodeLogCursor (ble-spec §4.6 step 1; BLE-004)
// ---------------------------------------------------------------------------

void test_cursor_4_bytes_decodes_little_endian() {
  const uint8_t b[] = {0x78, 0x56, 0x34, 0x12};
  uint32_t c = 0;
  TEST_ASSERT_TRUE(proto::decodeLogCursor(b, 4, &c));
  TEST_ASSERT_EQUAL_HEX32(0x12345678u, c);
}

void test_cursor_spec_examples_00000000_and_03000000() {
  uint32_t c = 0xDEADBEEF;
  const uint8_t zero[] = {0, 0, 0, 0};
  TEST_ASSERT_TRUE(proto::decodeLogCursor(zero, 4, &c));
  TEST_ASSERT_EQUAL_UINT32(0, c);
  const uint8_t three[] = {0x03, 0, 0, 0};
  TEST_ASSERT_TRUE(proto::decodeLogCursor(three, 4, &c));
  TEST_ASSERT_EQUAL_UINT32(3, c);
}

void test_cursor_max_and_high_bit_values() {
  uint32_t c = 0;
  const uint8_t ff[] = {0xFF, 0xFF, 0xFF, 0xFF};
  TEST_ASSERT_TRUE(proto::decodeLogCursor(ff, 4, &c));
  TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, c);
  const uint8_t hi[] = {0x00, 0x00, 0x00, 0x80};
  TEST_ASSERT_TRUE(proto::decodeLogCursor(hi, 4, &c));
  TEST_ASSERT_EQUAL_HEX32(0x80000000u, c);
  const uint8_t lo[] = {0x01, 0x00, 0x00, 0x00};
  TEST_ASSERT_TRUE(proto::decodeLogCursor(lo, 4, &c));
  TEST_ASSERT_EQUAL_HEX32(0x00000001u, c);
}

void test_cursor_wrong_lengths_rejected_and_cursor_untouched() {
  const uint8_t b[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  const size_t lens[] = {0, 1, 2, 3, 5, 6, 8, 255, 512};
  for (size_t len : lens) {
    uint32_t c = 0xCAFEF00Du;
    char msg[48];
    snprintf(msg, sizeof msg, "len %u", static_cast<unsigned>(len));
    // Pass a pointer into a buffer that is only read for len <= 8; for longer
    // lengths the decoder must reject before reading anything.
    TEST_ASSERT_FALSE_MESSAGE(proto::decodeLogCursor(b, len, &c), msg);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0xCAFEF00Du, c, msg);
  }
}

void test_cursor_null_data_rejected_and_cursor_untouched() {
  uint32_t c = 0x11223344u;
  TEST_ASSERT_FALSE(proto::decodeLogCursor(nullptr, 4, &c));
  TEST_ASSERT_EQUAL_HEX32(0x11223344u, c);
  TEST_ASSERT_FALSE(proto::decodeLogCursor(nullptr, 0, &c));
  TEST_ASSERT_EQUAL_HEX32(0x11223344u, c);
}

void test_cursor_null_out_pointer_rejected_without_crash() {
  const uint8_t b[] = {1, 2, 3, 4};
  TEST_ASSERT_FALSE(proto::decodeLogCursor(b, 4, nullptr));
}

// ---------------------------------------------------------------------------
// encodeLogPage (ble-spec §4.6 step 2, entry layout; LOG-003, BLE-004)
// ---------------------------------------------------------------------------

static LogEntry makeEntry(uint32_t seq, uint16_t boot, uint32_t ms, EventType t, uint8_t d0,
                          uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4) {
  LogEntry e;
  e.seq = seq;
  e.bootCount = boot;
  e.msSinceBoot = ms;
  e.type = t;
  e.data[0] = d0;
  e.data[1] = d1;
  e.data[2] = d2;
  e.data[3] = d3;
  e.data[4] = d4;
  return e;
}

// Exact bytes of the §4.6 worked example (two entries).
void test_log_page_worked_example_from_ble_spec_4_6() {
  LogEntry es[2] = {
      makeEntry(1, 1, 1234, EventType::Boot, 0x01, 0x00, 0x00, 0, 0),
      makeEntry(2, 1, 2236, EventType::StateChange, 0x03, 0x00, 0, 0, 0),
  };
  const uint8_t expected[33] = {
      0x02,
      0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0xD2, 0x04, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
      0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0xBC, 0x08, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00,
  };
  uint8_t out[proto::kLogPageMaxLen + 4];
  memset(out, kGuard, sizeof out);
  size_t n = proto::encodeLogPage(es, 2, out, sizeof out);
  TEST_ASSERT_EQUAL_size_t(33, n);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, 33);
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(kGuard, out[33], "wrote past the end of the page");
}

// Every multi-byte field uses distinct bytes so a byte-order or offset error
// shows up as a specific mismatch.
void test_log_page_every_field_little_endian_at_documented_offset() {
  LogEntry e = makeEntry(0x0A0B0C0Du, 0x1E1Fu, 0x21222324u, EventType::Disconnect, 0x31, 0x32,
                         0x33, 0x34, 0x35);
  const uint8_t expected[17] = {
      0x01,                    // count
      0x0D, 0x0C, 0x0B, 0x0A,  // [0..3]  seq LE
      0x1F, 0x1E,              // [4..5]  bootCount LE
      0x24, 0x23, 0x22, 0x21,  // [6..9]  msSinceBoot LE
      0x05,                    // [10]    type
      0x31, 0x32, 0x33, 0x34, 0x35,  // [11..15] data, verbatim
  };
  uint8_t out[17 + 2];
  memset(out, kGuard, sizeof out);
  TEST_ASSERT_EQUAL_size_t(17, proto::encodeLogPage(&e, 1, out, sizeof out));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, 17);
  TEST_ASSERT_EQUAL_HEX8(kGuard, out[17]);
}

void test_log_page_max_field_values() {
  LogEntry e = makeEntry(0xFFFFFFFFu, 0xFFFFu, 0xFFFFFFFFu, EventType::IdleTimeout, 0xFF, 0xFF,
                         0xFF, 0xFF, 0xFF);
  uint8_t expected[17];
  memset(expected, 0xFF, sizeof expected);
  expected[0] = 0x01;
  expected[11] = 0x06;
  uint8_t out[17];
  TEST_ASSERT_EQUAL_size_t(17, proto::encodeLogPage(&e, 1, out, sizeof out));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, 17);
}

static void fillEntries(LogEntry *es, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    uint8_t k = static_cast<uint8_t>(i + 1);
    es[i] = makeEntry(0x01000000u * k + k, static_cast<uint16_t>(0x0100 * k + k),
                      0x00010000u * k + 0x10 + k, EventType::CommandResult, k, 0x10 + k, 0x20 + k,
                      0x30 + k, 0x40 + k);
  }
}

static void buildExpectedPage(const LogEntry *es, size_t n, uint8_t *exp) {
  // Hand-rolled LE packing independent of log::pack().
  exp[0] = static_cast<uint8_t>(n);
  for (size_t i = 0; i < n; ++i) {
    uint8_t *p = exp + 1 + 16 * i;
    for (int b = 0; b < 4; ++b) p[b] = static_cast<uint8_t>(es[i].seq >> (8 * b));
    for (int b = 0; b < 2; ++b) p[4 + b] = static_cast<uint8_t>(es[i].bootCount >> (8 * b));
    for (int b = 0; b < 4; ++b) p[6 + b] = static_cast<uint8_t>(es[i].msSinceBoot >> (8 * b));
    p[10] = static_cast<uint8_t>(es[i].type);
    for (int b = 0; b < 5; ++b) p[11 + b] = es[i].data[b];
  }
}

void test_log_page_full_8_entries_is_129_bytes_oldest_first() {
  LogEntry es[8];
  fillEntries(es, 8);
  uint8_t expected[129];
  buildExpectedPage(es, 8, expected);
  uint8_t out[129 + 3];
  memset(out, kGuard, sizeof out);
  TEST_ASSERT_EQUAL_size_t(129, proto::encodeLogPage(es, 8, out, sizeof out));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, 129);
  TEST_ASSERT_EQUAL_HEX8(kGuard, out[129]);
}

void test_log_page_count_above_8_is_capped_to_first_8_entries() {
  LogEntry es[12];
  fillEntries(es, 12);
  uint8_t expected[129];
  buildExpectedPage(es, 8, expected);
  uint8_t out[1 + 12 * 16];
  memset(out, kGuard, sizeof out);
  TEST_ASSERT_EQUAL_size_t(129, proto::encodeLogPage(es, 12, out, sizeof out));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, 129);
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(kGuard, out[129], "entries beyond the cap were written");
}

void test_log_page_huge_count_capped_without_overflowing_needed_size() {
  LogEntry es[8];
  fillEntries(es, 8);
  uint8_t out[129];
  TEST_ASSERT_EQUAL_size_t(129, proto::encodeLogPage(es, SIZE_MAX, out, sizeof out));
  TEST_ASSERT_EQUAL_HEX8(8, out[0]);
}

void test_log_page_partial_counts_1_to_7_sizes_and_bytes() {
  LogEntry es[7];
  fillEntries(es, 7);
  for (size_t n = 1; n <= 7; ++n) {
    uint8_t expected[129];
    buildExpectedPage(es, n, expected);
    uint8_t out[130];
    memset(out, kGuard, sizeof out);
    size_t len = 1 + 16 * n;
    char msg[32];
    snprintf(msg, sizeof msg, "count %u", static_cast<unsigned>(n));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(len, proto::encodeLogPage(es, n, out, sizeof out), msg);
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(expected, out, len, msg);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(kGuard, out[len], msg);
  }
}

void test_log_page_empty_page_is_single_zero_byte() {
  // ble-spec: "count == 0 means there are no more entries".
  LogEntry es[1];
  fillEntries(es, 1);
  uint8_t out[4];
  memset(out, kGuard, sizeof out);
  TEST_ASSERT_EQUAL_size_t(1, proto::encodeLogPage(es, 0, out, sizeof out));
  TEST_ASSERT_EQUAL_HEX8(0x00, out[0]);
  TEST_ASSERT_EQUAL_HEX8(kGuard, out[1]);
}

void test_log_page_empty_page_with_null_entries() {
  uint8_t out[1] = {kGuard};
  TEST_ASSERT_EQUAL_size_t(1, proto::encodeLogPage(nullptr, 0, out, 1));
  TEST_ASSERT_EQUAL_HEX8(0x00, out[0]);
}

void test_log_page_null_entries_with_nonzero_count_is_empty_page() {
  // Header comment: a null entry list can only describe an empty page.
  uint8_t out[129];
  memset(out, kGuard, sizeof out);
  TEST_ASSERT_EQUAL_size_t(1, proto::encodeLogPage(nullptr, 5, out, sizeof out));
  TEST_ASSERT_EQUAL_HEX8(0x00, out[0]);
  TEST_ASSERT_EQUAL_HEX8(kGuard, out[1]);
}

void test_log_page_buffer_one_byte_too_small_returns_0_and_writes_nothing() {
  LogEntry es[8];
  fillEntries(es, 8);
  for (size_t n = 0; n <= 8; ++n) {
    size_t need = 1 + 16 * n;
    uint8_t out[129];
    memset(out, kGuard, sizeof out);
    char msg[48];
    snprintf(msg, sizeof msg, "count %u cap %u", static_cast<unsigned>(n),
             static_cast<unsigned>(need - 1));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, proto::encodeLogPage(es, n, out, need - 1), msg);
    for (size_t i = 0; i < sizeof out; ++i) TEST_ASSERT_EQUAL_HEX8_MESSAGE(kGuard, out[i], msg);
  }
}

void test_log_page_capped_count_needs_only_129_bytes_not_count_times_16() {
  LogEntry es[10];
  fillEntries(es, 10);
  uint8_t out[129];
  TEST_ASSERT_EQUAL_size_t(129, proto::encodeLogPage(es, 10, out, 129));
  TEST_ASSERT_EQUAL_size_t(0, proto::encodeLogPage(es, 10, out, 128));
}

void test_log_page_null_out_returns_0() {
  LogEntry es[1];
  fillEntries(es, 1);
  TEST_ASSERT_EQUAL_size_t(0, proto::encodeLogPage(es, 1, nullptr, 129));
  TEST_ASSERT_EQUAL_size_t(0, proto::encodeLogPage(es, 0, nullptr, 129));
}

void test_log_page_zero_capacity_returns_0() {
  uint8_t out[1] = {kGuard};
  TEST_ASSERT_EQUAL_size_t(0, proto::encodeLogPage(nullptr, 0, out, 0));
  TEST_ASSERT_EQUAL_HEX8(kGuard, out[0]);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_uuid_service_matches_ble_spec);
  RUN_TEST(test_uuid_command_char_matches_ble_spec);
  RUN_TEST(test_uuid_status_char_matches_ble_spec);
  RUN_TEST(test_uuid_info_char_matches_ble_spec);
  RUN_TEST(test_uuid_log_char_matches_ble_spec);
  RUN_TEST(test_protocol_version_is_1_1_and_sizes_match_spec);
  RUN_TEST(test_status_worked_examples_from_ble_spec_4_4);
  RUN_TEST(test_status_fields_are_independent_bytes_in_order_state_op_result_seq);
  RUN_TEST(test_info_example_from_ble_spec_4_5_protocol_1_1_firmware_0_2_3);
  RUN_TEST(test_info_firmware_bytes_are_major_minor_patch_in_order);
  RUN_TEST(test_cursor_4_bytes_decodes_little_endian);
  RUN_TEST(test_cursor_spec_examples_00000000_and_03000000);
  RUN_TEST(test_cursor_max_and_high_bit_values);
  RUN_TEST(test_cursor_wrong_lengths_rejected_and_cursor_untouched);
  RUN_TEST(test_cursor_null_data_rejected_and_cursor_untouched);
  RUN_TEST(test_cursor_null_out_pointer_rejected_without_crash);
  RUN_TEST(test_log_page_worked_example_from_ble_spec_4_6);
  RUN_TEST(test_log_page_every_field_little_endian_at_documented_offset);
  RUN_TEST(test_log_page_max_field_values);
  RUN_TEST(test_log_page_full_8_entries_is_129_bytes_oldest_first);
  RUN_TEST(test_log_page_count_above_8_is_capped_to_first_8_entries);
  RUN_TEST(test_log_page_huge_count_capped_without_overflowing_needed_size);
  RUN_TEST(test_log_page_partial_counts_1_to_7_sizes_and_bytes);
  RUN_TEST(test_log_page_empty_page_is_single_zero_byte);
  RUN_TEST(test_log_page_empty_page_with_null_entries);
  RUN_TEST(test_log_page_null_entries_with_nonzero_count_is_empty_page);
  RUN_TEST(test_log_page_buffer_one_byte_too_small_returns_0_and_writes_nothing);
  RUN_TEST(test_log_page_capped_count_needs_only_129_bytes_not_count_times_16);
  RUN_TEST(test_log_page_null_out_returns_0);
  RUN_TEST(test_log_page_zero_capacity_returns_0);
  return UNITY_END();
}

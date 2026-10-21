// ecm_inventory_test.cpp — host test for the opcode census.
//
// No board, no bus, no ESPHome: every FrameRecord is built by hand, so each
// fixed-size dimension of the census can be driven exactly to its edge — the
// length table, the payload-hash set, the per-index value bitmaps, the key table
// and the slot-opcode table. What happens AT the boundary is the point: a noisy
// bus must not be able to grow this structure, and the counts are supposed to
// stay honest when the sets saturate.
//
//     clang++ -std=c++20 -Wall -Wextra -Wpedantic -Iinclude \
//             -o /tmp/ecm_inventory_test test/ecm_inventory_test.cpp
//     /tmp/ecm_inventory_test
//
// Section 5 is the one that matters most. The write opcodes (0x64 flame,
// 0x65 pilot, 0x67 height) live at payload byte 2 of a 0xBC frame and are a
// SEPARATE NAMESPACE from the frame command byte. Nothing else in this firmware
// looks at that byte, so if a new one ever appears, this census is the only
// thing that would notice.
//
// What this test cannot say: whether the ECM ever emits such a byte, or that a
// byte it emits lands on the wire the way the captures say. Those need the
// appliance.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ecm_inventory.h"

// ---------------------------------------------------------------------------
// Tiny test harness (same shape as ecm_reconciler_test.cpp)
// ---------------------------------------------------------------------------

static int g_passed = 0;
static int g_failed = 0;
static std::vector<std::string> g_failures;

static void check(bool ok, const std::string &label, const std::string &detail = "") {
  if (ok) {
    ++g_passed;
    std::printf("  PASS  %s\n", label.c_str());
  } else {
    ++g_failed;
    g_failures.push_back(label);
    std::printf("  FAIL  %s\n", label.c_str());
    if (!detail.empty()) {
      std::printf("          %s\n", detail.c_str());
    }
  }
}

// ---------------------------------------------------------------------------
// Names and helpers
// ---------------------------------------------------------------------------

using ecm::telemetry::DIR_D0;
using ecm::telemetry::DIR_D1;
using ecm::telemetry::FrameRecord;
using ecm::telemetry::KeyStats;
using ecm::telemetry::OpcodeInventory;
using ecm::telemetry::SlotOpStats;

namespace flag = ecm::telemetry::flag;
namespace invflag = ecm::telemetry::invflag;
namespace notable = ecm::telemetry::notable;

/// A FrameRecord as the parser would have left one: a real 0x4B header, a real
/// CRC-16 stored in the last two bytes, and the CRC_OK flag the census gates on.
///
/// The payload is `plen` bytes from `payload`, so raw_len is 6 + plen and
/// FrameRecord::payload_len() agrees with it. Lengths are therefore driven by
/// driving the payload, which is how the length table gets exercised.
static FrameRecord make_rec(uint8_t dir, uint8_t dev, uint8_t cmd, const uint8_t *payload,
                            size_t plen, uint32_t t_ms, uint8_t seq = 0,
                            bool crc_valid = true) {
  FrameRecord r;
  const size_t total = 6 + plen;
  r.raw[0] = ecm::START;
  r.raw[1] = dev;
  r.raw[2] = seq;
  r.raw[3] = cmd;
  for (size_t i = 0; i < plen; ++i) {
    r.raw[4 + i] = payload[i];
  }
  const uint16_t c = ecm::crc16_modbus(r.raw, total - 2);
  r.raw[total - 2] = static_cast<uint8_t>(c & 0xFF);
  r.raw[total - 1] = static_cast<uint8_t>((c >> 8) & 0xFF);
  r.raw_len = static_cast<uint8_t>(total);
  r.crc = c;
  r.dir = dir;
  r.dev = dev;
  r.seq = seq;
  r.cmd = cmd;
  r.t_ms = t_ms;
  if (crc_valid) {
    r.flags |= flag::CRC_OK;
  }
  return r;
}

/// A 0xBC frame carrying a write opcode at payload byte 2 — the shape the real
/// module's reply has (it echoes `02 04` and then appends the 4-byte slot).
///
/// `slot_op` is deliberately a parameter and not derived from anything: it is
/// the byte whose namespace the header exists to keep separate.
static FrameRecord bc_rec(uint8_t dir, uint8_t dev, uint8_t slot_op, uint32_t t_ms,
                          uint8_t value = 0x00, size_t plen = 4) {
  const uint8_t p[4] = {0x02, 0x04, slot_op, value};
  return make_rec(dir, dev, ecm::cmd::WRITE_CARRIER, p, plen, t_ms);
}

/// The i-th distinct 3-byte payload for the saturation tests: only byte 0
/// varies, so each i is a different payload at a constant length.
static FrameRecord nth_payload(unsigned i, uint32_t t_ms) {
  const uint8_t p[3] = {static_cast<uint8_t>(i), 0x00, 0x00};
  return make_rec(DIR_D0, 0x09, ecm::cmd::STATUS, p, 3, t_ms);
}

/// value_bits[i] is a 256-bit set of the byte VALUES seen at payload index i.
/// The test reads it with its own accessor rather than the header's private
/// test_bit_, so the bitmap is checked as a plain array of bytes.
static bool vbit(const KeyStats &k, size_t index, uint8_t value) {
  return (k.value_bits[index][value >> 3] & (1u << (value & 7u))) != 0;
}

/// Find one slot-opcode entry, so an assertion about it cannot silently pass
/// against whichever entry happens to sit at a fixed index.
static const SlotOpStats *find_slot(const OpcodeInventory &inv, uint8_t dir, uint8_t dev,
                                    uint8_t op) {
  for (size_t i = 0; i < ecm::telemetry::MAX_SLOT_OPS; ++i) {
    const SlotOpStats &s = inv.slot_op(i);
    if (s.used && s.dir == dir && s.dev == dev && s.op == op) {
      return &s;
    }
  }
  return nullptr;
}

static bool has(const std::string &haystack, const std::string &needle) {
  return haystack.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// 1. The key table
// ---------------------------------------------------------------------------

static void test_key_table() {
  std::printf("\n== 1. the key table: one entry per (dir, dev, cmd) ==\n");

  OpcodeInventory inv;
  const uint8_t p[2] = {0x02, 0x04};

  // NOTE the 2-byte payload: a 0xBC frame that short carries no slot, so this
  // section sees the key table on its own.
  const uint8_t first = inv.observe(make_rec(DIR_D0, 0x01, ecm::cmd::WRITE_CARRIER, p, 2, 1000));
  check((first & notable::NEW_KEY) != 0, "the first sighting of (D0, 0x01, 0xBC) reports NEW_KEY");
  check((first & notable::NEW_PAYLOAD) != 0, "and NEW_PAYLOAD: its payload is new by definition");
  check((first & notable::UNEXPECTED_LEN) == 0,
        "but NOT UNEXPECTED_LEN — the first length establishes the length for the key");
  check(inv.key_count() == 1, "and the table holds exactly one key");

  const uint8_t again = inv.observe(make_rec(DIR_D0, 0x01, ecm::cmd::WRITE_CARRIER, p, 2, 1200));
  check((again & notable::NEW_KEY) == 0, "the same key a second time does NOT report NEW_KEY");
  check((again & notable::NEW_PAYLOAD) == 0, "nor a new payload (the bytes are identical)");
  check(inv.key_count() == 1, "and no second entry was created");

  inv.observe(make_rec(DIR_D0, 0x01, ecm::cmd::WRITE_CARRIER, p, 2, 1500));

  KeyStats k;
  const bool have = inv.lookup(DIR_D0, 0x01, ecm::cmd::WRITE_CARRIER, &k);
  check(have, "lookup() finds a key that exists");
  check(k.occurrences == 3, "occurrences counts every frame, not just the new ones",
        "got " + std::to_string(k.occurrences));
  check(k.first_ms == 1000, "first_ms stays at the FIRST sighting", "got " + std::to_string(k.first_ms));
  check(k.last_ms == 1500, "last_ms tracks the latest", "got " + std::to_string(k.last_ms));
  check(k.dir == DIR_D0 && k.dev == 0x01 && k.cmd == ecm::cmd::WRITE_CARRIER, "the key's own bytes are stored");
  check(std::strcmp(k.channel(), "D0") == 0, "and channel() names the direction the analyser uses (D0)");
  check(k.used == 1, "the entry is marked used");

  // dir, dev and cmd are ALL part of the key: a neighbour on any one of them is
  // a different opcode census, not the same one.
  check(!inv.lookup(DIR_D1, 0x01, ecm::cmd::WRITE_CARRIER, nullptr),
        "lookup() reports false for the other direction (dir is part of the key)");
  check(!inv.lookup(DIR_D0, 0x02, ecm::cmd::WRITE_CARRIER, nullptr), "and false for a different dev");
  check(!inv.lookup(DIR_D0, 0x01, ecm::cmd::STATUS, nullptr), "and false for a different cmd");
  check(inv.key_count() == 1, "a failed lookup creates nothing — lookup() is read-only");

  inv.observe(make_rec(DIR_D1, 0x01, ecm::cmd::WRITE_CARRIER, p, 2, 2000));
  KeyStats d1;
  const bool have_d1 = inv.lookup(DIR_D1, 0x01, ecm::cmd::WRITE_CARRIER, &d1);
  check(inv.key_count() == 2 && have_d1 && std::strcmp(d1.channel(), "D1") == 0 && d1.occurrences == 1,
        "the same cmd in the other direction is a separate key, with its own channel() and count",
        "key_count " + std::to_string(inv.key_count()) + ", occurrences " +
            std::to_string(d1.occurrences));

  // A known frame cmd is not flagged unknown; an unrecognised one is. `role` in
  // the JSON is derived from exactly this bit.
  check(!k.has(invflag::UNKNOWN_CMD), "a known cmd (0xBC) is not flagged UNKNOWN_CMD");
  OpcodeInventory u;
  u.observe(make_rec(DIR_D0, 0x01, 0x99, p, 2, 0));
  KeyStats uk;
  const bool have_u = u.lookup(DIR_D0, 0x01, 0x99, &uk);
  check(have_u && uk.has(invflag::UNKNOWN_CMD), "an unrecognised cmd (0x99) IS flagged UNKNOWN_CMD");

  // clear() is a full reset, and everything afterwards behaves like a fresh boot.
  inv.clear();
  check(inv.key_count() == 0 && inv.slot_op_count() == 0, "clear() empties both tables");
  check(inv.overflowed() == 0 && inv.slot_overflowed() == 0 && inv.known_slot_seen() == 0,
        "and resets every counter, including the aggregate count of known slot opcodes");
  const uint8_t after = inv.observe(make_rec(DIR_D0, 0x01, ecm::cmd::WRITE_CARRIER, p, 2, 9000));
  KeyStats fresh;
  const bool have_fresh = inv.lookup(DIR_D0, 0x01, ecm::cmd::WRITE_CARRIER, &fresh);
  check((after & notable::NEW_KEY) != 0, "so the next frame is new again");
  check(have_fresh && fresh.occurrences == 1 && fresh.first_ms == 9000 && fresh.last_ms == 9000,
        "with the count and the clock restarted from zero", "occurrences " + std::to_string(fresh.occurrences));
}

// ---------------------------------------------------------------------------
// 2. The length histogram
// ---------------------------------------------------------------------------

static void test_length_histogram() {
  std::printf("\n== 2. the length histogram: a new length is a finding, the first one is not ==\n");

  OpcodeInventory inv;
  uint8_t pay[16];
  for (size_t i = 0; i < sizeof(pay); ++i) {
    pay[i] = static_cast<uint8_t>(0xA0 + i);
  }
  // raw_len = 6 + plen, so this drives the frame length through the payload.
  const auto frame = [&pay](size_t raw_len, uint32_t t) {
    return make_rec(DIR_D1, 0x03, ecm::cmd::STATUS, pay, raw_len - 6, t);
  };

  check((inv.observe(frame(8, 100)) & notable::UNEXPECTED_LEN) == 0,
        "the FIRST length for a key is not a finding — it establishes the length");
  check((inv.observe(frame(8, 200)) & notable::UNEXPECTED_LEN) == 0, "nor is a repeat of it");
  check((inv.observe(frame(12, 300)) & notable::UNEXPECTED_LEN) != 0,
        "but a SECOND, different length on an already-known key IS a finding");

  KeyStats k;
  const bool have = inv.lookup(DIR_D1, 0x03, ecm::cmd::STATUS, &k);
  check(have && static_cast<size_t>(k.len_n) == 2, "two lengths are now recorded");
  check(k.len_vals[0] == 8 && k.len_vals[1] == 12, "in the order they were first seen");
  check(k.len_counts[0] == 2 && k.len_counts[1] == 1, "with the per-length frame counts kept",
        "counts " + std::to_string(k.len_counts[0]) + ", " + std::to_string(k.len_counts[1]));
  check(k.has(invflag::LENGTH_VARIED), "and LENGTH_VARIED set");

  check((inv.observe(frame(12, 400)) & notable::UNEXPECTED_LEN) == 0,
        "a repeat of a KNOWN length is never a finding");
  check((inv.observe(frame(10, 500)) & notable::UNEXPECTED_LEN) != 0, "a third distinct length is another one");
  check((inv.observe(frame(14, 600)) & notable::UNEXPECTED_LEN) != 0, "and a fourth fills the table (LEN_CAP)");

  // Past LEN_CAP the table cannot grow — but every further distinct length must
  // STILL be reported. That is the whole value of the table on a bus whose frame
  // shapes keep changing: the answer must not be lost just because the list is.
  const uint8_t past1 = inv.observe(frame(16, 700));
  check((past1 & notable::UNEXPECTED_LEN) != 0,
        "a fifth distinct length is still reported with the table saturated");
  const uint8_t past2 = inv.observe(frame(18, 800));
  check((past2 & notable::UNEXPECTED_LEN) != 0, "and so is a sixth");

  KeyStats full;
  const bool have_full = inv.lookup(DIR_D1, 0x03, ecm::cmd::STATUS, &full);
  check(have_full && static_cast<size_t>(full.len_n) == ecm::telemetry::LEN_CAP,
        "the table did not grow past LEN_CAP", "len_n " + std::to_string(full.len_n));
  bool kept = full.len_n == 4 && full.len_vals[0] == 8 && full.len_vals[1] == 12 && full.len_vals[2] == 10 &&
              full.len_vals[3] == 14;
  check(kept, "and the four it kept are the first four seen");
  check(full.has(invflag::LENGTH_VARIED), "LENGTH_VARIED is set");
  // The length table and the payload set have their OWN flags. They were once a
  // single bit, which meant a saturated LENGTH table told a consumer its PAYLOAD
  // set was incomplete when it was not — two different tables, two different
  // claims, so two flags.
  check(full.has(invflag::LENGTH_SET_TRUNCATED), "and a saturated LENGTH table sets LENGTH_SET_TRUNCATED");
  check(!full.has(invflag::PAYLOAD_SET_TRUNCATED),
        "and NOT the payload-set flag: this key's payload set was never truncated");

  // len_counts is a uint8_t: a length seen more than 255 times must CLAMP, not
  // wrap back to zero and understate a length that is by far the most common.
  OpcodeInventory many;
  for (int i = 0; i < 300; ++i) {
    many.observe(frame(8, static_cast<uint32_t>(i)));
  }
  KeyStats kc;
  const bool have_kc = many.lookup(DIR_D1, 0x03, ecm::cmd::STATUS, &kc);
  check(have_kc && kc.occurrences == 300, "300 identical frames are all counted",
        "occurrences " + std::to_string(kc.occurrences));
  check(kc.len_counts[0] == 255, "and the per-length count clamped at 255 instead of wrapping to 44",
        "len_counts[0] " + std::to_string(kc.len_counts[0]));

  // The histogram is per key: a length that is new for one key is not a finding
  // for another, which is what makes "a known command arriving at a NEW length"
  // a statement about that command.
  OpcodeInventory other;
  check((other.observe(make_rec(DIR_D0, 0x04, ecm::cmd::STATUS, pay, 6, 0)) & notable::UNEXPECTED_LEN) == 0,
        "a different key's first sighting of that same length is not a finding either");
}

// ---------------------------------------------------------------------------
// 3. The payload census
// ---------------------------------------------------------------------------

static void test_payload_census() {
  std::printf("\n== 3. the payload census: the distinct count, the example, and saturation ==\n");

  OpcodeInventory inv;
  const uint8_t a[2] = {0x01, 0x02};
  const uint8_t b[2] = {0x01, 0x03};

  check((inv.observe(make_rec(DIR_D1, 0x05, ecm::cmd::STATUS, a, 2, 10)) & notable::NEW_PAYLOAD) != 0,
        "the first payload of a key is new");
  check((inv.observe(make_rec(DIR_D1, 0x05, ecm::cmd::STATUS, a, 2, 20)) & notable::NEW_PAYLOAD) == 0,
        "an identical repeat is not");
  check((inv.observe(make_rec(DIR_D1, 0x05, ecm::cmd::STATUS, b, 2, 30)) & notable::NEW_PAYLOAD) != 0,
        "and a different one is");

  KeyStats k;
  const bool have = inv.lookup(DIR_D1, 0x05, ecm::cmd::STATUS, &k);
  check(have && k.distinct_payloads == 2, "distinct_payloads counts exactly: 3 frames, 2 payloads",
        "got " + std::to_string(k.distinct_payloads));
  check(static_cast<size_t>(k.payload_hash_n) == 2, "and two hashes are stored");
  // `example` is the evidence field: it keeps the FIRST payload seen, whole, so
  // a later reader can see what the key looked like when it was new — not what
  // it looked like most recently.
  check(static_cast<size_t>(k.example_len) == 2 && k.example[0] == 0x01 && k.example[1] == 0x02,
        "and `example` still holds the FIRST payload, not the latest");

  // A payload longer than EXAMPLE_CAP is clipped. example_len sits immediately
  // after example[] in the struct, so a missing clamp would have written past
  // the array and shown up here as 30 rather than EXAMPLE_CAP.
  OpcodeInventory wide;
  uint8_t long_p[30];
  for (size_t i = 0; i < sizeof(long_p); ++i) {
    long_p[i] = static_cast<uint8_t>(i + 1);
  }
  wide.observe(make_rec(DIR_D0, 0x07, ecm::cmd::STATUS, long_p, sizeof(long_p), 0));
  KeyStats wk;
  const bool have_wk = wide.lookup(DIR_D0, 0x07, ecm::cmd::STATUS, &wk);
  check(have_wk && static_cast<size_t>(wk.example_len) == ecm::telemetry::EXAMPLE_CAP,
        "a 30-byte payload is CLIPPED to EXAMPLE_CAP, not overflowed",
        "example_len " + std::to_string(wk.example_len));
  bool prefix_ok = true;
  for (size_t i = 0; i < ecm::telemetry::EXAMPLE_CAP; ++i) {
    if (wk.example[i] != long_p[i]) {
      prefix_ok = false;
    }
  }
  check(prefix_ok, "and what it kept is the FIRST EXAMPLE_CAP bytes of it");

  // --- saturation ---------------------------------------------------------
  OpcodeInventory sat;
  for (unsigned i = 0; i < 12; ++i) {
    sat.observe(nth_payload(i, 100 + i));
  }
  const auto read_sat = [&sat]() {
    KeyStats s;
    sat.lookup(DIR_D0, 0x09, ecm::cmd::STATUS, &s);
    return s;
  };
  KeyStats sk = read_sat();
  // The count FREEZES at the cap, and that freeze is the point: a count that
  // kept rising would be counting repeats it could no longer recognise, and so
  // could exceed the TRUE distinct count — wrong in the unsafe direction. A
  // frozen count is a lower bound; PAYLOAD_SET_TRUNCATED is what says so.
  check(sk.distinct_payloads == ecm::telemetry::PAYLOAD_HASH_CAP,
        "12 distinct payloads past a cap of 8 leave distinct_payloads frozen AT the cap",
        "got " + std::to_string(sk.distinct_payloads));
  check(static_cast<size_t>(sk.payload_hash_n) == ecm::telemetry::PAYLOAD_HASH_CAP,
        "and the stored SET stopped at PAYLOAD_HASH_CAP too", "payload_hash_n " + std::to_string(sk.payload_hash_n));
  check(sk.distinct_payloads == sk.payload_hash_n,
        "so distinct_payloads == payload_hash_n holds unconditionally");
  check(sk.has(invflag::PAYLOAD_SET_TRUNCATED), "and the saturation is flagged rather than left silent");
  check(!sk.has(invflag::LENGTH_SET_TRUNCATED) && !sk.has(invflag::LENGTH_VARIED),
        "while the LENGTH flags stay clear — every one of those frames was the same length");

  // A payload that DID fit is still recognised after the set saturates.
  const uint32_t before_known = read_sat().distinct_payloads;
  sat.observe(nth_payload(0, 500));
  check(read_sat().distinct_payloads == before_known && read_sat().occurrences == 13,
        "a repeat of a payload inside the cap is still deduped after saturation",
        "distinct went " + std::to_string(before_known) + " -> " + std::to_string(read_sat().distinct_payloads));

  // A payload that did NOT fit must never move the count again. This is exactly
  // the case that used to inflate it: payload #9 was seen (and counted) while
  // the set was already full, so its hash was never stored and every later
  // repeat was counted as a fresh distinct payload.
  const uint32_t before_dup = read_sat().distinct_payloads;
  sat.observe(nth_payload(8, 900));
  sat.observe(nth_payload(8, 950));
  const uint32_t after_dup = read_sat().distinct_payloads;
  check(after_dup == before_dup,
        "re-feeding a payload first seen past the cap does NOT move the count (it used to go 8 -> 10)",
        "distinct went " + std::to_string(before_dup) + " -> " + std::to_string(after_dup) + " on two repeats");

  // ...but the frame is still flagged as one worth looking at. Past saturation
  // NEW_PAYLOAD means "not in the stored set", NOT "provably new": this is an
  // opcode-discovery tool, and going silent on new bytes would defeat it.
  const uint8_t fresh_mask = sat.observe(nth_payload(20, 1000));
  check((fresh_mask & notable::NEW_PAYLOAD) != 0 &&
            read_sat().distinct_payloads == ecm::telemetry::PAYLOAD_HASH_CAP,
        "a brand-new payload past the cap still sets NEW_PAYLOAD while the count stays frozen");
}

// ---------------------------------------------------------------------------
// 4. The per-index value bitmaps
// ---------------------------------------------------------------------------

static void test_value_bitmaps() {
  std::printf("\n== 4. value_bits[]: a 256-bit set of VALUES, one set per payload index ==\n");

  OpcodeInventory inv;
  const uint8_t p0[5] = {0x41, 0x02, 0x41, 0x00, 0xFF};
  inv.observe(make_rec(DIR_D1, 0x0B, ecm::cmd::STATUS, p0, 5, 10));

  KeyStats k;
  const bool have = inv.lookup(DIR_D1, 0x0B, ecm::cmd::STATUS, &k);
  check(have && vbit(k, 0, 0x41), "index 0 has a bit for the VALUE 0x41");
  check(!vbit(k, 0, 0x02), "and NOT for 0x02, which appeared at a different index — the mapping is by index, not a union");
  check(vbit(k, 1, 0x02) && !vbit(k, 1, 0x41), "index 1 is its own set: 0x02 present, 0x41 absent");
  check(vbit(k, 2, 0x41), "so the same value at two indices is recorded in both sets");
  check(vbit(k, 3, 0x00), "a zero byte is a value like any other (bit 0 of the set)");
  check(vbit(k, 4, 0xFF), "and 0xFF lands in the LAST byte of the 256-bit set");
  check(!vbit(k, 0, 0x42), "a value never seen at that index reads as absent");
  check(!vbit(k, 5, 0x00), "and an index the payload never reached is entirely empty");

  // The sets accumulate across frames and each index only ever gains its own
  // index's values.
  const uint8_t p1[5] = {0x00, 0x02, 0x41, 0x01, 0xFE};
  inv.observe(make_rec(DIR_D1, 0x0B, ecm::cmd::STATUS, p1, 5, 20));
  KeyStats k2;
  const bool have2 = inv.lookup(DIR_D1, 0x0B, ecm::cmd::STATUS, &k2);
  check(have2 && vbit(k2, 0, 0x41) && vbit(k2, 0, 0x00),
        "the sets accumulate: two payloads put two values in index 0");
  check(vbit(k2, 3, 0x00) && vbit(k2, 3, 0x01) && !vbit(k2, 3, 0x02),
        "index 3 gained both of ITS values and nothing else");
  check(!vbit(k2, 4, 0x00), "and index 4 kept its single value");

  // VALUE_INDEX_CAP is a hard stop. value_bits is [VALUE_INDEX_CAP][32], so an
  // index past the cap would write OFF THE END of the array — into whatever the
  // struct holds next. Nothing beyond the cap may be recorded, and nothing may
  // appear in a neighbouring bitmap either.
  OpcodeInventory deep;
  uint8_t pw[16];
  for (size_t i = 0; i < sizeof(pw); ++i) {
    pw[i] = static_cast<uint8_t>(0x80 + i);  // 0x80..0x8F, one per index
  }
  deep.observe(make_rec(DIR_D0, 0x0C, ecm::cmd::STATUS, pw, sizeof(pw), 0));
  KeyStats kw;
  const bool have_w = deep.lookup(DIR_D0, 0x0C, ecm::cmd::STATUS, &kw);
  check(have_w && vbit(kw, 0, 0x80) && vbit(kw, 11, 0x8B),
        "bytes below VALUE_INDEX_CAP are recorded (index 11 is the last one covered)");
  // 0x8C..0x8F appear ONLY at indices 12..15, so finding any of them in a bitmap
  // below the cap would mean an out-of-range write landed in a live neighbour.
  bool leaked = false;
  for (size_t i = 0; i < ecm::telemetry::VALUE_INDEX_CAP; ++i) {
    for (unsigned v = 0x8C; v <= 0x8F; ++v) {
      if (vbit(kw, i, static_cast<uint8_t>(v))) {
        leaked = true;
      }
    }
  }
  check(!leaked,
        "and a byte at index >= VALUE_INDEX_CAP is recorded in NO bitmap at all (a write at value_bits[12] "
        "would run off the array)");
}

// ---------------------------------------------------------------------------
// 5. Slot opcodes — the blind spot this header exists to close
// ---------------------------------------------------------------------------

static void test_slot_opcodes() {
  std::printf("\n== 5. slot opcodes: the byte the frame-cmd check cannot see ==\n");

  // --- 5a. the blind spot itself -----------------------------------------
  // 0x66 now has a documented ECM handler (config byte + ~1.5 s delay), but it
  // is deliberately not in the implemented set: its human purpose is unknown,
  // so a sighting must still be recorded rather than counted as routine.
  check(!ecm::telemetry::is_known_slot_op(0x66),
        "0x66 is NOT an implemented slot opcode (documented but never emitted)");
  OpcodeInventory inv;
  const uint8_t m = inv.observe(bc_rec(DIR_D1, 0x03, 0x66, 1000));
  check((m & notable::NEW_SLOT_OP) != 0,
        "a 0xBC frame carrying 0x66 at payload byte 2 reports NEW_SLOT_OP");
  check((m & notable::NEW_KEY) != 0, "and the 0xBC frame itself is censused normally as well");
  check(inv.slot_op_count() == 1, "the slot table holds one entry");
  const SlotOpStats *s66 = find_slot(inv, DIR_D1, 0x03, 0x66);
  check(s66 != nullptr && s66->occurrences == 1 && s66->first_ms == 1000 && s66->last_ms == 1000,
        "keyed by (dir, dev, op), with the sighting time recorded");
  KeyStats bc;
  const bool have_bc = inv.lookup(DIR_D1, 0x03, ecm::cmd::WRITE_CARRIER, &bc);
  check(have_bc && bc.has(invflag::HAD_NEW_SLOT_OP),
        "and the 0xBC key itself is marked as the one that introduced it");

  // --- 5b. the same unknown op again -------------------------------------
  const uint8_t again = inv.observe(bc_rec(DIR_D1, 0x03, 0x66, 1200));
  check((again & notable::NEW_SLOT_OP) == 0, "the SAME unknown op a second time is not new");
  const SlotOpStats *s66b = find_slot(inv, DIR_D1, 0x03, 0x66);
  check(s66b != nullptr && s66b->occurrences == 2 && s66b->last_ms == 1200, "...but IS counted");
  check(inv.slot_op_count() == 1, "and the table did not gain an entry");

  // --- 5c. known ops are counted, never tabulated, never "new" ------------
  OpcodeInventory known;
  const struct {
    uint8_t op;
    const char *name;
  } known_ops[4] = {{0x00, "idle"}, {0x64, "flame"}, {0x65, "pilot"}, {0x67, "height"}};
  bool never_new = true;
  for (size_t i = 0; i < 4; ++i) {
    for (int n = 0; n < 3; ++n) {
      const uint8_t mask = known.observe(bc_rec(DIR_D1, 0x03, known_ops[i].op, static_cast<uint32_t>(n)));
      if ((mask & notable::NEW_SLOT_OP) != 0) {
        never_new = false;
      }
    }
  }
  check(never_new, "0x00 idle, 0x64 flame, 0x65 pilot and 0x67 height NEVER report NEW_SLOT_OP");
  // Known opcodes take NO table slot. Idle 0x00 rides in the slot of every
  // command-bearing reply, so tabulating it would spend the table on a value
  // that can never be a finding — and the table exists for the ones that can.
  check(known.slot_op_count() == 0, "and they take NO slot in the slot table");
  check(known.known_slot_seen() == 12, "they are counted in aggregate instead (4 opcodes x 3 frames)",
        "known_slot_seen " + std::to_string(known.known_slot_seen()));
  check(known.slot_overflowed() == 0, "so idle traffic cannot overflow a table it never touches");
  bool absent = true;
  for (size_t i = 0; i < 4; ++i) {
    if (find_slot(known, DIR_D1, 0x03, known_ops[i].op) != nullptr) {
      absent = false;
    }
  }
  check(absent, "and none of the four is findable in the slot table");
  KeyStats known_bc;
  const bool have_known = known.lookup(DIR_D1, 0x03, ecm::cmd::WRITE_CARRIER, &known_bc);
  check(have_known && !known_bc.has(invflag::HAD_NEW_SLOT_OP),
        "and a known opcode never marks the key as having introduced one");

  // The regression this reservation exists for. Idle traffic from several
  // devices used to fill the table one idle per (dir, dev) and push a genuinely
  // NEW opcode into slot_overflowed_, where it is never recorded at all — the
  // exact blind spot the table was added to close.
  OpcodeInventory busy;
  for (uint8_t dev = 0x01; dev <= 0x06; ++dev) {
    for (int n = 0; n < 5; ++n) {
      busy.observe(bc_rec(DIR_D1, dev, 0x00, static_cast<uint32_t>(n)));
    }
  }
  check(busy.slot_op_count() == 0 && busy.slot_overflowed() == 0 && busy.known_slot_seen() == 30,
        "30 idle frames from 6 devices occupy no slot and overflow nothing",
        "slots " + std::to_string(busy.slot_op_count()) + ", overflowed " +
            std::to_string(busy.slot_overflowed()) + ", known " + std::to_string(busy.known_slot_seen()));
  check((busy.observe(bc_rec(DIR_D1, 0x03, 0x66, 1000)) & notable::NEW_SLOT_OP) != 0 &&
            find_slot(busy, DIR_D1, 0x03, 0x66) != nullptr,
        "and a NEW opcode arriving after all that idle traffic is still recorded, not overflowed");

  // --- 5d. short payloads ------------------------------------------------
  // payload[2] must not be read unless it exists: payload_len() >= 3 is the
  // gate, so a 2-byte (or empty) 0xBC payload is not a slot at all.
  OpcodeInventory short_inv;
  check((short_inv.observe(bc_rec(DIR_D1, 0x03, 0x66, 0, 0x00, 2)) & notable::NEW_SLOT_OP) == 0,
        "a 0xBC frame with only 2 payload bytes reports no slot opcode");
  check((short_inv.observe(bc_rec(DIR_D1, 0x03, 0x66, 0, 0x00, 0)) & notable::NEW_SLOT_OP) == 0,
        "nor does one with no payload at all (no read past the frame)");
  check(short_inv.slot_op_count() == 0 && short_inv.slot_overflowed() == 0 &&
            short_inv.known_slot_seen() == 0,
        "and neither the slot table nor the known-slot counter moved for either");
  check(short_inv.key_count() == 1, "while the frame's own key is still created");
  // The boundary is inclusive on the other side: exactly 3 payload bytes IS a slot.
  OpcodeInventory three;
  check((three.observe(bc_rec(DIR_D1, 0x03, 0x66, 0, 0x00, 3)) & notable::NEW_SLOT_OP) != 0,
        "at exactly 3 payload bytes the census DOES fire (the boundary is inclusive)");

  // --- 5e. a separate namespace ------------------------------------------
  // The point of the separate table: a slot opcode must never become a frame
  // key. It has no frame cmd byte at all, so a frame-cmd-shaped entry for it
  // would be a fabricated opcode.
  OpcodeInventory ns;
  ns.observe(bc_rec(DIR_D1, 0x03, 0x99, 1000));
  ns.observe(bc_rec(DIR_D0, 0x05, 0x99, 1100));
  check(ns.key_count() == 2, "two 0xBC frames made two keys — the 0x99 slot opcodes made none");
  check(!ns.lookup(DIR_D1, 0x03, 0x99, nullptr), "there is no key with the slot opcode as its cmd");
  bool any_99 = false;
  for (size_t i = 0; i < ecm::telemetry::MAX_KEYS; ++i) {
    if (ns.key(i).used && ns.key(i).cmd == 0x99) {
      any_99 = true;
    }
  }
  check(!any_99, "and none anywhere in the key table");
  check(ns.slot_op_count() == 2, "while the slot table has both — dir and dev are part of ITS key too");
  check(find_slot(ns, DIR_D0, 0x05, 0x99) != nullptr, "including the D0/dev 0x05 one");
  // The same byte as a FRAME cmd is a different thing entirely, which is exactly
  // why one check cannot cover both.
  OpcodeInventory fc;
  const uint8_t p2[2] = {0x02, 0x04};
  fc.observe(make_rec(DIR_D1, 0x03, 0x99, p2, 2, 0));
  check(fc.key_count() == 1 && fc.lookup(DIR_D1, 0x03, 0x99, nullptr),
        "0x99 AS A FRAME CMD does become a key — the two namespaces disagree, hence two tables");
  check(fc.slot_op_count() == 0, "and as a frame cmd it is not a slot opcode");

  // --- 5f. the position is fixed: payload byte 2, nothing else ------------
  OpcodeInventory pos;
  const uint8_t everywhere[4] = {0x66, 0x66, 0x00, 0x66};  // 0x66 at every index EXCEPT 2
  check((pos.observe(make_rec(DIR_D1, 0x03, ecm::cmd::WRITE_CARRIER, everywhere, 4, 0)) & notable::NEW_SLOT_OP) == 0,
        "0x66 at payload[0], [1] and [3] is not a slot opcode — the byte is read at index 2 only");
  check(pos.slot_op_count() == 0 && pos.known_slot_seen() == 1,
        "and the only slot byte it saw was the idle 0x00 that really was at index 2 (counted, not tabulated)");

  // --- 5g. the table caps, and the drop is counted ------------------------
  OpcodeInventory full;
  bool all_new = true;
  for (size_t i = 0; i < ecm::telemetry::MAX_SLOT_OPS; ++i) {
    const uint8_t op = static_cast<uint8_t>(0x01 + i);
    if ((full.observe(bc_rec(DIR_D1, 0x03, op, static_cast<uint32_t>(i))) & notable::NEW_SLOT_OP) == 0) {
      all_new = false;
    }
  }
  check(all_new, "MAX_SLOT_OPS distinct unknown opcodes are all recorded and all report NEW_SLOT_OP");
  check(full.slot_op_count() == ecm::telemetry::MAX_SLOT_OPS, "the slot table holds exactly MAX_SLOT_OPS");
  check(full.slot_overflowed() == 0, "with no overflow yet");
  const uint8_t ninth = full.observe(bc_rec(DIR_D1, 0x03, 0x09, 900));
  check((ninth & notable::NEW_SLOT_OP) == 0, "a ninth distinct opcode reports nothing — there is nowhere to put it");
  check(full.slot_op_count() == ecm::telemetry::MAX_SLOT_OPS, "the table did not grow");
  check(find_slot(full, DIR_D1, 0x03, 0x09) == nullptr, "the dropped opcode is genuinely absent");
  check(full.slot_overflowed() == 1,
        "and the drop is COUNTED, not silent — observe() has no bit for it, so this counter is the only sign");
  // A repeat of an opcode that IS in the table is still matched after saturation,
  // which is what stops a full table from turning into a permanent miss.
  const uint8_t repeat_in = full.observe(bc_rec(DIR_D1, 0x03, 0x01, 950));
  const SlotOpStats *s01 = find_slot(full, DIR_D1, 0x03, 0x01);
  check((repeat_in & notable::NEW_SLOT_OP) == 0 && s01 != nullptr && s01->occurrences == 2,
        "and a slot opcode already in the table is still counted once the table is full");

  // --- 5h. documented sharp edge: the census does not check direction -----
  // observe() fires on ANY 0xBC frame with 3+ payload bytes, not only on the
  // reply that actually carries a write. Today's recorded 0xBC REQUESTS are
  // exactly 2 payload bytes, so this cannot happen on the real bus — but it is
  // real behaviour, and if a request ever grows a third payload byte, its value
  // would be censused as a write opcode. Asserted so that day fails loudly.
  OpcodeInventory req;
  const uint8_t req_payload[3] = {0x02, 0x04, 0x66};
  check((req.observe(make_rec(DIR_D0, 0x02, ecm::cmd::WRITE_CARRIER, req_payload, 3, 0)) & notable::NEW_SLOT_OP) != 0,
        "a D0 (request-direction) 0xBC frame with a 3rd payload byte IS censused as a slot opcode too "
        "(direction is not part of the gate)");

  // --- 5i. observe_slot_ can be called directly --------------------------
  // It is public, and it is the only way to census an opcode seen outside a
  // frame the census would parse.
  OpcodeInventory direct;
  check(direct.observe_slot_(DIR_D1, 0x03, 0x66, 5),
        "observe_slot_() directly: a first sighting of an UNKNOWN op returns true");
  check(!direct.observe_slot_(DIR_D1, 0x03, 0x66, 6), "the second returns false");
  check(!direct.observe_slot_(DIR_D1, 0x03, 0x64, 7),
        "and a KNOWN opcode never returns true, not even on its first sighting");
  check(direct.slot_op_count() == 1 && direct.known_slot_seen() == 1,
        "the unknown one is tabulated and the known one only counted");
}

// ---------------------------------------------------------------------------
// 6. A CRC-invalid frame is ignored entirely
// ---------------------------------------------------------------------------

static void test_crc_invalid_ignored() {
  std::printf("\n== 6. a frame that failed CRC is ignored in EVERY dimension ==\n");

  // A corrupt frame has no trustworthy cmd byte and no trustworthy payload, so
  // censusing it would INVENT opcodes that do not exist — and an invented
  // opcode is exactly what this census is supposed to surface as real. The
  // same reasoning keeps the firmware silent on a bad frame.
  OpcodeInventory inv;
  const uint8_t p[4] = {0x02, 0x04, 0x66, 0x00};

  const uint8_t mask = inv.observe(make_rec(DIR_D1, 0x03, ecm::cmd::WRITE_CARRIER, p, 4, 500, 0, false));
  check(mask == 0, "a frame without CRC_OK returns 0 — the whole mask, not just the new-key bit",
        "mask " + std::to_string(mask));
  check(inv.key_count() == 0, "it creates no key");
  check(inv.slot_op_count() == 0, "and censuses no slot opcode, even though payload[2] is an unknown 0x66");
  check(inv.overflowed() == 0 && inv.slot_overflowed() == 0, "and moves neither overflow counter");
  check(!inv.lookup(DIR_D1, 0x03, ecm::cmd::WRITE_CARRIER, nullptr), "lookup() finds nothing afterwards");

  // The same frame with the flag set is new in BOTH namespaces and counted
  // exactly once — proving the invalid one was ignored outright rather than
  // half-applied (a half-applied frame would show up as occurrences == 2).
  const uint8_t good = inv.observe(make_rec(DIR_D1, 0x03, ecm::cmd::WRITE_CARRIER, p, 4, 500, 0, true));
  KeyStats k;
  const bool have = inv.lookup(DIR_D1, 0x03, ecm::cmd::WRITE_CARRIER, &k);
  const bool slot_new = (good & notable::NEW_SLOT_OP) != 0;
  check((good & notable::NEW_KEY) != 0 && slot_new,
        "the identical CRC-valid frame is new in both the key and the slot namespace");
  check(have && k.occurrences == 1, "with occurrences 1, not 2");
  check(inv.slot_op_count() == 1, "and one slot entry, not two");
}

// ---------------------------------------------------------------------------
// 7. The key table caps
// ---------------------------------------------------------------------------

static void test_key_table_overflow() {
  std::printf("\n== 7. the key table caps at MAX_KEYS and counts what it refused ==\n");

  OpcodeInventory inv;
  const uint8_t p[2] = {0x01, 0x02};

  bool all_new = true;
  for (size_t i = 0; i < ecm::telemetry::MAX_KEYS; ++i) {
    const uint8_t mask = inv.observe(make_rec(DIR_D0, static_cast<uint8_t>(i + 1), ecm::cmd::STATUS, p, 2,
                                              static_cast<uint32_t>(i)));
    if ((mask & notable::NEW_KEY) == 0) {
      all_new = false;
    }
  }
  check(all_new, "the first MAX_KEYS distinct keys are all stored, and all report NEW_KEY");
  check(inv.key_count() == ecm::telemetry::MAX_KEYS, "the table holds exactly MAX_KEYS");
  check(inv.overflowed() == 0, "with no overflow yet");

  bool found_all = true;
  for (size_t i = 0; i < ecm::telemetry::MAX_KEYS; ++i) {
    if (!inv.lookup(DIR_D0, static_cast<uint8_t>(i + 1), ecm::cmd::STATUS, nullptr)) {
      found_all = false;
    }
  }
  check(found_all, "and every one of them is still findable by lookup()");

  // Past the cap the frame is refused WHOLE — no partial census of a length, a
  // payload or a slot. The caller gets exactly one bit and nothing else.
  bool exact_mask = true;
  for (size_t i = 0; i < 5; ++i) {
    const uint8_t mask = inv.observe(make_rec(DIR_D0, static_cast<uint8_t>(ecm::telemetry::MAX_KEYS + 1 + i),
                                              ecm::cmd::STATUS, p, 2, 5000));
    if (mask != notable::KEY_TABLE_FULL) {
      exact_mask = false;
    }
  }
  check(exact_mask, "a key past the cap returns EXACTLY notable::KEY_TABLE_FULL, no other bit");
  check(inv.overflowed() == 5, "overflowed() counts every refused frame",
        "got " + std::to_string(inv.overflowed()));
  // Fixed-size by construction: keys_ is an array, so "does not grow" is a
  // property of the type, and this is the runtime half of that assertion.
  check(inv.key_count() == ecm::telemetry::MAX_KEYS, "and the table did NOT grow");
  check(!inv.lookup(DIR_D0, static_cast<uint8_t>(ecm::telemetry::MAX_KEYS + 1), ecm::cmd::STATUS, nullptr),
        "the refused key is genuinely not in the table");

  // The counter is per FRAME, not per lost key: a repeat of a refused key is
  // refused again and counted again, so "how much traffic could not be
  // censused" stays answerable.
  const uint8_t repeat = inv.observe(make_rec(DIR_D0, static_cast<uint8_t>(ecm::telemetry::MAX_KEYS + 1),
                                              ecm::cmd::STATUS, p, 2, 6000));
  check(repeat == notable::KEY_TABLE_FULL && inv.overflowed() == 6,
        "a repeat of a refused key is refused and counted again (the counter is per frame)");

  // Clearing makes room again: the refusal is not sticky.
  inv.clear();
  const uint8_t freed = inv.observe(make_rec(DIR_D0, 1, ecm::cmd::STATUS, p, 2, 7000));
  check((freed & notable::NEW_KEY) != 0 && inv.overflowed() == 0,
        "after clear() the same key is storable again, and the overflow counter is back to zero");
}

// ---------------------------------------------------------------------------
// 8. A clipped frame: the census reads only what was STORED
// ---------------------------------------------------------------------------

static void test_clipped_record() {
  std::printf("\n== 8. a clipped frame: the census reads only the bytes that were copied ==\n");

  // A frame longer than RAW_CAP: the telemetry layer copies RAW_CAP bytes, sets
  // TRUNCATED, and keeps the real length in raw_len. payload_len() is bounded by
  // what is actually stored, so nothing here may be read past raw[RAW_CAP].
  FrameRecord a = make_rec(DIR_D1, 0x0E, ecm::cmd::STATUS, nullptr, 0, 100);
  for (size_t i = 0; i < ecm::telemetry::RAW_CAP; ++i) {
    a.raw[i] = static_cast<uint8_t>(0x30 + (i & 7));
  }
  a.flags |= flag::TRUNCATED;
  a.raw_len = 200;
  check(a.stored_len() == ecm::telemetry::RAW_CAP && a.payload_len() == ecm::telemetry::RAW_CAP - 6,
        "a record declaring 200 bytes reports a payload length of only the stored bytes",
        "payload_len " + std::to_string(a.payload_len()));

  OpcodeInventory inv;
  inv.observe(a);
  FrameRecord b = a;
  b.raw_len = 240;  // a longer DECLARATION, but the same 80 stored bytes
  b.t_ms = 200;
  const uint8_t mask = inv.observe(b);
  KeyStats k;
  const bool have = inv.lookup(DIR_D1, 0x0E, ecm::cmd::STATUS, &k);
  check((mask & notable::NEW_PAYLOAD) == 0 && have && k.distinct_payloads == 1,
        "two clipped frames with identical stored bytes are ONE payload, not two hashes over "
        "bytes that were never copied",
        "distinct_payloads " + std::to_string(k.distinct_payloads));
  // NOTE: the length histogram uses the DECLARED length, deliberately — 200 and
  // 240 really were the wire lengths, and the tail is what was lost. So a key
  // whose frames are always the same length can still report UNEXPECTED_LEN here
  // if the clipped tails differ. Documented, not asserted as desirable.
  check((mask & notable::UNEXPECTED_LEN) != 0 &&
            static_cast<size_t>(k.len_n) == 2 && k.len_vals[0] == 200 && k.len_vals[1] == 240,
        "while the length histogram records the DECLARED length of each clipped frame (200, then 240)");
  check(static_cast<size_t>(k.example_len) == ecm::telemetry::EXAMPLE_CAP,
        "and the example is clipped to EXAMPLE_CAP, as for any long payload");
}

// ---------------------------------------------------------------------------
// 9a. One entry as JSON
// ---------------------------------------------------------------------------

static void test_json_key() {
  std::printf("\n== 9a. one entry: the JSON contract the map pipeline reads ==\n");

  OpcodeInventory inv;
  const uint8_t p[2] = {0x02, 0x04};
  inv.observe(make_rec(DIR_D1, 0x03, ecm::cmd::STATUS, p, 2, 1000));

  KeyStats k;
  const bool have = inv.lookup(DIR_D1, 0x03, ecm::cmd::STATUS, &k);
  char buf[1024];
  const bool ok = have && ecm::telemetry::key_to_json(k, buf, sizeof(buf));
  const std::string got = ok ? std::string(buf) : std::string("<no output>");
  // Whole-string equality, not a handful of substrings: this is the shape the
  // map pipeline parses, so a moved comma is a real defect and must fail here.
  const std::string want =
      "{\"key\": \"D1/0x03/0xDC\", \"dir\": \"D1\", \"dev\": \"0x03\", \"cmd\": \"0xDC\", "
      "\"occurrences\": 1, \"frame_len\": 8, \"lengths_seen\": [8], \"lengths_varied\": false, "
      "\"distinct_payloads\": 1, \"payload_set_truncated\": false, \"role\": \"observed\", \"new\": false, "
      "\"values_observed\": {\"0\": [\"0x02\"],\"1\": [\"0x04\"]}, \"values_truncated\": false, "
      "\"first_ms\": 1000, \"last_ms\": 1000}";
  check(ok && got == want, "a known command serializes byte-for-byte as the schema expects", got);
  check(has(got, "\"dev\": \"0x03\""),
        "dev is a QUOTED 0x%02X string, not a number — the Python side re-spells it");
  check(has(got, "\"cmd\": \"0xDC\""), "and so is cmd");
  check(has(got, "\"role\": \"observed\""), "and a known cmd carries role observed");
  check(has(got, "\"values_truncated\": false"), "an entry that fits raises no truncation flag");

  // The same key one payload later: two values at index 0, one at index 1. This
  // is the shape a reader uses to answer "what has byte 0 ever been?".
  const uint8_t p2[2] = {0x03, 0x04};
  inv.observe(make_rec(DIR_D1, 0x03, ecm::cmd::STATUS, p2, 2, 1500));
  KeyStats k2;
  const bool have2 = inv.lookup(DIR_D1, 0x03, ecm::cmd::STATUS, &k2);
  char buf2[1024];
  const bool ok2 = have2 && ecm::telemetry::key_to_json(k2, buf2, sizeof(buf2));
  const std::string got2 = ok2 ? std::string(buf2) : std::string("<no output>");
  const std::string want2 =
      "{\"key\": \"D1/0x03/0xDC\", \"dir\": \"D1\", \"dev\": \"0x03\", \"cmd\": \"0xDC\", "
      "\"occurrences\": 2, \"frame_len\": 8, \"lengths_seen\": [8], \"lengths_varied\": false, "
      "\"distinct_payloads\": 2, \"payload_set_truncated\": false, \"role\": \"observed\", \"new\": false, "
      "\"values_observed\": {\"0\": [\"0x02\",\"0x03\"],\"1\": [\"0x04\"]}, \"values_truncated\": false, "
      "\"first_ms\": 1000, \"last_ms\": 1500}";
  check(ok2 && got2 == want2, "two payloads list both values per index, in value order", got2);

  // An unrecognised frame cmd is labelled unknown and NOTHING ELSE: the whole
  // point of the census is not to guess what a new byte might mean.
  OpcodeInventory u;
  u.observe(make_rec(DIR_D0, 0x02, 0x99, p, 2, 7));
  KeyStats uk;
  const bool have_u = u.lookup(DIR_D0, 0x02, 0x99, &uk);
  char ubuf[1024];
  const bool uok = have_u && ecm::telemetry::key_to_json(uk, ubuf, sizeof(ubuf));
  const std::string ugot = uok ? std::string(ubuf) : std::string("<no output>");
  check(has(ugot, "\"role\": \"unknown\""), "an unrecognised frame cmd serializes as role unknown", ugot);
  check(has(ugot, "\"cmd\": \"0x99\"") && has(ugot, "\"key\": \"D0/0x02/0x99\""),
        "with dev and cmd still quoted 0x%02X, and the key spelled the analyser's way");
  check(!has(ugot, "\"role\": \"observed\""), "and never both roles — the label is one value, not a hedge");

  // --- values_truncated: the LISTING is capped, the record is not ---------
  // The bitmap always holds all 256 possible values; only the emitted list is
  // bounded, at VALUES_PER_INDEX_CAP. A consumer that sees values_truncated must
  // not read the list as the complete set — a single key's uncapped listing has
  // been measured at ~18 KB, which is why the cap exists.
  OpcodeInventory vol;
  for (unsigned i = 0; i < 20; ++i) {
    const uint8_t v[1] = {static_cast<uint8_t>(i)};
    vol.observe(make_rec(DIR_D0, 0x11, ecm::cmd::STATUS, v, 1, i));
  }
  KeyStats vk;
  const bool have_v = vol.lookup(DIR_D0, 0x11, ecm::cmd::STATUS, &vk);
  check(have_v && vbit(vk, 0, 0x13), "the BITMAP still recorded all 20 values at index 0 (0x13 is the 20th)");
  char vbuf[1024];
  const bool vok = have_v && ecm::telemetry::key_to_json(vk, vbuf, sizeof(vbuf));
  const std::string vgot = vok ? std::string(vbuf) : std::string("<no output>");
  check(vok && has(vgot, "\"values_truncated\": true"),
        "but the JSON reports values_truncated: true once an index spills past the cap", vgot);
  check(has(vgot,
            "\"0\": [\"0x00\",\"0x01\",\"0x02\",\"0x03\",\"0x04\",\"0x05\",\"0x06\",\"0x07\","
            "\"0x08\",\"0x09\",\"0x0A\",\"0x0B\",\"0x0C\",\"0x0D\",\"0x0E\",\"0x0F\"]"),
        "and lists exactly the first VALUES_PER_INDEX_CAP values, in order");
  check(!has(vgot, "\"0x10\""), "the 17th value is genuinely absent from the list", vgot);

  // --- the buffer bound ---------------------------------------------------
  // `need` is the exact content length, so the boundary can be approached from
  // both sides: the NUL is part of the budget, and one byte less must fail
  // rather than write into the caller's memory.
  const size_t need = std::strlen(buf);
  char exact[1024];
  check(ecm::telemetry::key_to_json(k, exact, need + 1) && std::strcmp(exact, buf) == 0,
        "a buffer of exactly len+1 fits and produces identical bytes");
  check(!ecm::telemetry::key_to_json(k, exact, need), "one byte short returns false (the NUL needs a byte too)");
  check(!ecm::telemetry::key_to_json(k, exact, 1), "and a 1-byte buffer is rejected outright (cap < 2)");
  check(!ecm::telemetry::key_to_json(k, nullptr, 100), "a null buffer returns false rather than crashing");

  char guarded[512];
  std::memset(guarded, 0x5A, sizeof(guarded));
  const bool small = ecm::telemetry::key_to_json(k, guarded, 40);
  bool intact = true;
  for (size_t i = 40; i < sizeof(guarded); ++i) {
    if (static_cast<unsigned char>(guarded[i]) != 0x5A) {
      intact = false;
    }
  }
  check(!small, "a 40-byte buffer is too small for a real entry, and says so with false");
  check(intact, "and not one byte past cap was touched (the guard pattern is intact)");
}

// ---------------------------------------------------------------------------
// 8b. The whole census as JSON
// ---------------------------------------------------------------------------

static void test_json_inventory() {
  std::printf("\n== 9b. the whole census: one array, and never a partial one ==\n");

  OpcodeInventory inv;
  const uint8_t p[2] = {0x02, 0x04};
  inv.observe(bc_rec(DIR_D1, 0x03, 0x66, 1000));                      // an unknown SLOT opcode
  inv.observe(make_rec(DIR_D0, 0x01, ecm::cmd::STATUS, p, 2, 2000));  // a known frame cmd
  inv.observe(make_rec(DIR_D0, 0x07, 0x99, p, 2, 3000));              // an unrecognised frame cmd

  char big[4096];
  const bool ok = ecm::telemetry::inventory_to_json(inv, big, sizeof(big));
  const std::string s = ok ? std::string(big) : std::string("<no output>");
  check(ok, "the census serializes into a large buffer", s);
  check(!s.empty() && s.front() == '[' && s.back() == ']', "as one JSON array");
  // Offset 1: the array's own '[' comes first.
  check(s.compare(1, std::strlen("{\"key\": \"D1/0x03/0xBC\""), "{\"key\": \"D1/0x03/0xBC\"") == 0,
        "starting with the first key in table order, right after the opening bracket", s);
  check(has(s, "\"role\": \"observed\"") && has(s, "\"role\": \"unknown\""),
        "carrying both frame keys, one observed and one unknown");
  // The slot opcode must be its OWN object in its own namespace, so it can never
  // be mistaken for a frame command by anything reading the array.
  check(has(s, "{\"key\": \"slot/D1/0x03/0x66\", \"namespace\": \"slot_op\", \"occurrences\": 1, "
               "\"known\": false, \"first_ms\": 1000, \"last_ms\": 1000}"),
        "and the slot opcode as a separate object, marked namespace slot_op and known: false", s);
  check(s.find("\"namespace\": \"slot_op\"") > s.rfind("\"role\":"),
        "which comes after every frame key");

  const size_t need = std::strlen(big);
  char exact[4096];
  check(ecm::telemetry::inventory_to_json(inv, exact, need + 1) && std::strlen(exact) == need,
        "a buffer of exactly len+1 fits and produces the same bytes");
  // The contract that matters: a partial array would look complete to a reader,
  // so a short buffer must be a hard false, not a truncated dump.
  check(!ecm::telemetry::inventory_to_json(inv, exact, need),
        "one byte short returns FALSE rather than emitting a partial array that looks complete");
  check(!ecm::telemetry::inventory_to_json(inv, nullptr, 100), "a null buffer returns false");

  char guarded[4096];
  std::memset(guarded, 0x5A, sizeof(guarded));
  const bool short_call = ecm::telemetry::inventory_to_json(inv, guarded, need);
  bool intact = (need < sizeof(guarded));
  for (size_t i = need; i < sizeof(guarded); ++i) {
    if (static_cast<unsigned char>(guarded[i]) != 0x5A) {
      intact = false;
    }
  }
  check(!short_call && intact, "and the failing call stayed inside the boundary it was given");

  OpcodeInventory empty;
  char tiny[8];
  check(ecm::telemetry::inventory_to_json(empty, tiny, sizeof(tiny)) && std::strcmp(tiny, "[]") == 0,
        "an empty census is [] (no keys, no slot opcodes)");
  check(!ecm::telemetry::inventory_to_json(empty, tiny, 2), "and a 2-byte buffer is too small even for that");

  // --- one big key, and a modest buffer ----------------------------------
  // Each entry is serialized in place at the current write position, so the
  // caller's capacity is the ONLY limit. A fixed per-key scratch used to live
  // here; because one entry can exceed 1 KB once the value lists are included, a
  // key that varies across a few payload indices made the WHOLE dump return
  // false at any caller buffer size. This is the regression test for that: the
  // entry below is bigger than any plausible scratch, and a 1 KB buffer is
  // enough.
  OpcodeInventory wide;
  for (unsigned i = 0; i < 20; ++i) {
    const uint8_t w[4] = {static_cast<uint8_t>(i), static_cast<uint8_t>(i + 0x20),
                          static_cast<uint8_t>(i + 0x40), static_cast<uint8_t>(i + 0x60)};
    wide.observe(make_rec(DIR_D0, 0x11, ecm::cmd::STATUS, w, 4, i));
  }
  KeyStats wk;
  const bool have_w = wide.lookup(DIR_D0, 0x11, ecm::cmd::STATUS, &wk);
  char one_big[4096];
  const bool one_ok = have_w && ecm::telemetry::key_to_json(wk, one_big, sizeof(one_big));
  check(one_ok, "key_to_json on that one key succeeds with a 4 KB buffer");
  check(one_ok && std::strlen(one_big) > 640, "and its entry alone is well over 640 bytes",
        "entry length " + std::to_string(one_ok ? std::strlen(one_big) : 0));

  char modest[1024];
  const bool modest_ok = ecm::telemetry::inventory_to_json(wide, modest, sizeof(modest));
  check(modest_ok, "a MODEST 1 KB buffer dumps that whole census — one big key no longer fails the array");
  const std::string wide_json = modest_ok ? std::string(modest) : std::string("<no output>");
  check(modest_ok && wide_json.size() > 640 && has(wide_json, "\"values_truncated\": true") &&
            wide_json.front() == '[' && wide_json.back() == ']',
        "and it is a complete array carrying that key's own values_truncated flag",
        wide_json.size() > 200 ? wide_json.substr(0, 200) + "..." : wide_json);

  // The capacity boundary still holds for the big entry: exactly right is
  // enough, one byte less is a hard false.
  const size_t wide_need = wide_json.size();
  char exact_big[2048];
  check(ecm::telemetry::inventory_to_json(wide, exact_big, wide_need + 1) &&
            std::strcmp(exact_big, modest) == 0,
        "a buffer of exactly that length + 1 is enough, byte for byte");
  check(!ecm::telemetry::inventory_to_json(wide, exact_big, wide_need),
        "one byte short still returns false, however big the entry is");
}

// ---------------------------------------------------------------------------

// Fixed-size by construction: `a noisy bus must not be able to grow this` is a
// property of the type, not of a run. This is the canary for it — it fires at
// compile time if a cap is raised, because the ESP32 pays for every byte.
static_assert(sizeof(OpcodeInventory) < 12 * 1024,
              "the census must stay inside its RAM budget; raising the caps costs the ESP32 real memory");

int main() {
  std::printf("=== ecm opcode inventory: host test ===\n");
  std::printf("    no board, no bus, no ESPHome: every FrameRecord is built by hand, so\n");
  std::printf("    each bound in the census is driven exactly to its edge.\n");
  std::printf("    sizeof(KeyStats) = %zu bytes, sizeof(OpcodeInventory) = %zu bytes\n",
              sizeof(KeyStats), sizeof(OpcodeInventory));

  test_key_table();
  test_length_histogram();
  test_payload_census();
  test_value_bitmaps();
  test_slot_opcodes();
  test_crc_invalid_ignored();
  test_key_table_overflow();
  test_clipped_record();
  test_json_key();
  test_json_inventory();

  std::printf("\n============================================================\n");
  if (g_failed == 0) {
    std::printf("RESULT: all %d checks passed\n", g_passed);
  } else {
    std::printf("RESULT: %d FAILED, %d passed\n", g_failed, g_passed);
    for (const auto &f : g_failures) {
      std::printf("  - %s\n", f.c_str());
    }
  }
  std::printf("============================================================\n");
  return g_failed == 0 ? 0 : 1;
}

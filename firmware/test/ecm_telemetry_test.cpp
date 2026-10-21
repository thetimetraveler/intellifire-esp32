// ecm_telemetry_test.cpp — host test for the bus flight recorder.
//
// No ESPHome, no board, no UART, no clock. The header is pure C++20 on purpose
// so the two things that can actually be checked off-hardware are checked here:
//
//   1. the ring's per-channel bookkeeping (index, gap_ms, eviction), and
//   2. the JSON line, which is a CONTRACT with tools/ecm_frames.py and
//      tools/ecm_diff.py rather than a convenience.
//
// (2) is the part worth being pedantic about. The collector writes these lines
// straight into frames_D0.jsonl / frames_D1.jsonl and tools/ecm_diff.py parses
// them, so a key renamed, reordered, or emitted as a NUMBER where the Python
// writes a STRING would corrupt the analysis SILENTLY — the line would still be
// valid JSON and would still parse. That is why several checks below compare a
// whole literal line instead of "contains dev". The expected strings were
// derived by reading tools/ecm_frames.py::Frame.to_json() and its dataclass
// field order, not by running this serializer and pasting back what came out.
//
// Build:  clang++ -std=c++20 -Wall -Wextra -Wpedantic -Iinclude -O1 \
//                   -o /tmp/ecm_telemetry_test test/ecm_telemetry_test.cpp \
//                   && /tmp/ecm_telemetry_test

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "ecm_telemetry.h"

// ---------------------------------------------------------------------------
// Structure — asserted at compile time AND at run time
// ---------------------------------------------------------------------------

// 104 = 4*3 (t_ms, gap_ms, index) + 2*2 (crc, latency_ms) + 6*1 (raw_len, dir,
// dev, seq, cmd, flags) + 80 (raw), padded from 102 up to a 4-byte boundary.
// Asserting the exact number (rather than "big enough") is deliberate: it
// decides both the ring's RAM cost and the cost of the by-value push(), so a
// field added without thinking about padding would silently change both.
static_assert(sizeof(ecm::telemetry::FrameRecord) == 104,
              "FrameRecord changed size — decide deliberately, then update this test");
static_assert(std::is_trivially_copyable_v<ecm::telemetry::FrameRecord>,
              "FrameRecord must stay trivially copyable: push() stores it by value on the reply path");
static_assert(ecm::telemetry::RAW_CAP == ecm::MAX_FRAME_LEN,
              "RAW_CAP is the parser's ceiling on purpose — see the header's rationale");

// ---------------------------------------------------------------------------
// Tiny test harness (same shape as ecm_framing_test.cpp)
// ---------------------------------------------------------------------------

static int g_pass = 0, g_fail = 0;

static void check(bool ok, const char *label, const char *detail = "") {
  if (ok) {
    ++g_pass;
    std::printf("  PASS  %s\n", label);
  } else {
    ++g_fail;
    std::printf("  FAIL  %s%s%s\n", label, *detail ? "\n          " : "", detail);
  }
}

// ---------------------------------------------------------------------------
// Recorded frames, reused as the realistic inputs
// ---------------------------------------------------------------------------

// A recorded 8-byte 0xBC poll frame (D0). Using captured bytes rather than made
// up ones means the serializer is exercised on a line that really does occur.
static const uint8_t kBcFrame[] = {0x4B, 0x02, 0x5E, 0xBC, 0x02, 0x04, 0xA4, 0xCF};
static const size_t kBcLen = sizeof(kBcFrame);

// A recorded 31-byte 0xDC status frame (D0): the longest frame seen on this bus.
static const uint8_t kDcFrame[] = {
    0x4B, 0x01, 0x5D, 0xDC, 0x00, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0xFF};
static const size_t kDcLen = sizeof(kDcFrame);

/// Build a record the way handle_frame_() would: bytes in, fields derived. The
/// dev/seq/cmd/crc are taken from the bytes so a test cannot accidentally assert
/// against a record whose payload disagrees with its own raw array.
static ecm::telemetry::FrameRecord make_rec(uint8_t dir, uint32_t t_ms,
                                            const uint8_t *bytes, size_t len) {
  ecm::telemetry::FrameRecord r;
  r.dir = dir;
  r.t_ms = t_ms;
  r.raw_len = static_cast<uint8_t>(len);
  std::memcpy(r.raw, bytes, len);
  if (len >= 4) {
    r.dev = bytes[1];
    r.seq = bytes[2];
    r.cmd = bytes[3];
  }
  if (len >= 6) {
    r.crc = static_cast<uint16_t>(bytes[len - 2] | (bytes[len - 1] << 8));
  }
  return r;
}

/// Count the integers in the JSON array that follows `open`, e.g. "\"raw\": [".
/// -1 when the array is absent. This proves the serializer emitted the right
/// NUMBER of values, not merely something that looks like an array.
static int count_array_ints(const char *json, const char *open) {
  const char *p = std::strstr(json, open);
  if (p == nullptr) {
    return -1;
  }
  p += std::strlen(open);
  int count = 0;
  bool in_number = false;
  for (; *p != '\0' && *p != ']'; ++p) {
    const bool digit = (*p >= '0' && *p <= '9');
    if (digit && !in_number) {
      ++count;
    }
    in_number = digit;
  }
  return count;
}

/// Count the bytes in the `hex` string (two hex digits per byte, single-space
/// separated). Used to prove the raw_len > RAW_CAP case clips at 80.
static int count_hex_bytes(const char *json) {
  const char *p = std::strstr(json, "\"hex\": \"");
  if (p == nullptr) {
    return -1;
  }
  p += 8;
  int count = 0;
  while (*p != '\0' && *p != '"') {
    ++count;
    p += 2;              // two hex digits per byte
    if (*p == ' ') {
      ++p;               // exactly one space between bytes
    }
  }
  return count;
}

// ---------------------------------------------------------------------------
// 1. Ring: fill, wrap, evict, clear
// ---------------------------------------------------------------------------

static void test_ring_fill_and_evict() {
  std::printf("\n== 1. the ring: fill, wrap, evict, clear ==\n");

  // -- fewer than capacity --
  {
    ecm::telemetry::TelemetryRing<4> ring;
    check(ring.count() == 0 && ring.capacity() == 4 && !ring.full() && ring.evicted() == 0,
          "a fresh ring is empty, has capacity 4, is not full, and has evicted nothing");

    for (uint32_t i = 0; i < 3; ++i) {
      ring.push(make_rec(ecm::telemetry::DIR_D0, 100u * (i + 1), kBcFrame, kBcLen));
    }
    check(ring.count() == 3, "three pushes -> count 3");
    check(!ring.full() && ring.evicted() == 0, "3 < 4: not full, and nothing evicted");
    check(ring.at(0).t_ms == 100 && ring.at(1).t_ms == 200 && ring.at(2).t_ms == 300,
          "at() iterates OLDEST FIRST (100, 200, 300) — the analyser reads the ring as a timeline");
  }

  // -- exactly capacity: the boundary between "filled a slot" and "overwrote one" --
  {
    ecm::telemetry::TelemetryRing<4> ring;
    for (uint32_t i = 0; i < 4; ++i) {
      ring.push(make_rec(ecm::telemetry::DIR_D0, 100u * (i + 1), kBcFrame, kBcLen));
    }
    check(ring.count() == 4 && ring.full(), "exactly N pushes: count == N and full() is true");
    check(ring.evicted() == 0,
          "the Nth push FILLED a slot rather than overwriting one, so nothing is counted lost");
    check(ring.at(0).t_ms == 100 && ring.at(3).t_ms == 400, "oldest-first is still 100..400");
  }

  // -- more than capacity: the oldest records are the ones that go --
  {
    ecm::telemetry::TelemetryRing<4> ring;
    for (uint32_t i = 0; i < 6; ++i) {
      ring.push(make_rec(ecm::telemetry::DIR_D0, 100u * (i + 1), kBcFrame, kBcLen));
    }
    check(ring.count() == 4, "six pushes into a 4-ring: count saturates at capacity");
    check(ring.full(), "and it reports full");
    check(ring.evicted() == 2, "exactly two pushes overwrote a slot, so exactly two records were evicted");
    check(ring.at(0).t_ms == 300,
          "the survivors are the NEWEST four: at(0) is t = 300, so it is 100 and 200 that were dropped");
    check(ring.at(3).t_ms == 600, "and at(3) is the newest (600)");
  }

  // -- clear() resets EVERYTHING, including the per-direction bookkeeping --
  {
    ecm::telemetry::TelemetryRing<4> ring;
    for (uint32_t i = 0; i < 6; ++i) {
      ring.push(make_rec(ecm::telemetry::DIR_D0, 100u * (i + 1), kBcFrame, kBcLen));
    }
    ring.clear();
    check(ring.count() == 0 && ring.evicted() == 0 && !ring.full(),
          "clear() empties the ring: count 0, evicted 0, not full");

    // The per-channel state must reset too, or the first frame after a clear
    // would inherit a stale index and a gap measured from a frame that is no
    // longer in the ring — a gap the analysis could not explain.
    const ecm::telemetry::FrameRecord a = ring.push(make_rec(ecm::telemetry::DIR_D0, 5000, kBcFrame, kBcLen));
    const ecm::telemetry::FrameRecord b = ring.push(make_rec(ecm::telemetry::DIR_D1, 5100, kBcFrame, kBcLen));
    check(a.index == 0 && b.index == 0,
          "after clear(), the first frame on EACH channel is index 0 again");
    check(a.gap_ms == ecm::telemetry::NO_GAP && b.gap_ms == ecm::telemetry::NO_GAP,
          "and neither has a gap: clear() wiped the last-timestamp state as well");
    check(ring.count() == 2 && ring.evicted() == 0, "and the ring is usable again");
  }
}

// ---------------------------------------------------------------------------
// 2. index and gap_ms are PER CHANNEL
// ---------------------------------------------------------------------------

static void test_per_channel_bookkeeping() {
  std::printf("\n== 2. index and gap are PER CHANNEL, not global ==\n");

  // An interleaved D0/D1 sequence, exactly as the two directions of a real
  // exchange arrive. The gaps are chosen so a wrong implementation is visible:
  // every frame is 100 ms after the previous frame on the OTHER channel and
  // 200 ms after the previous frame on its OWN channel.
  ecm::telemetry::TelemetryRing<8> ring;
  ring.push(make_rec(ecm::telemetry::DIR_D0, 1000, kBcFrame, kBcLen));
  ring.push(make_rec(ecm::telemetry::DIR_D1, 1100, kBcFrame, kBcLen));
  ring.push(make_rec(ecm::telemetry::DIR_D0, 1200, kBcFrame, kBcLen));
  ring.push(make_rec(ecm::telemetry::DIR_D1, 1300, kBcFrame, kBcLen));
  ring.push(make_rec(ecm::telemetry::DIR_D0, 1400, kBcFrame, kBcLen));

  check(ring.at(0).index == 0 && ring.at(0).gap_ms == ecm::telemetry::NO_GAP,
        "D0 @1000: index 0, NO_GAP — the first frame on D0 has no predecessor");
  check(ring.at(1).index == 0 && ring.at(1).gap_ms == ecm::telemetry::NO_GAP,
        "D1 @1100: index 0, NO_GAP — D1's first frame does NOT inherit D0's history");
  check(ring.at(2).index == 1 && ring.at(2).gap_ms == 200,
        "D0 @1200: index 1, gap 200 — measured to the previous D0 (1000), NOT to the D1 at 1100");
  check(ring.at(3).index == 1 && ring.at(3).gap_ms == 200,
        "D1 @1300: index 1, gap 200 — measured to the previous D1 (1100)");
  check(ring.at(4).index == 2 && ring.at(4).gap_ms == 200,
        "D0 @1400: index 2, gap 200");
  check(ring.at(4).index == 2,
        "the index is a PER-CHANNEL counter: a single global counter would have produced 4 here");

  check(std::strcmp(ring.at(0).channel(), "D0") == 0 && std::strcmp(ring.at(1).channel(), "D1") == 0,
        "channel() spells the direction the way the analyser's channel names do (D0/D1)");

  // -- the wrap-safe gap arithmetic --------------------------------------
  // millis() is a uint32_t that wraps roughly every 49.7 days, so a record's
  // t_ms being LESS than its predecessor's is a real, expected event and not a
  // corruption. The subtraction is unsigned, so the delta is correct across the
  // wrap — this is asserted rather than assumed because a "sane" signed
  // implementation would produce a negative gap or a huge bogus one here.
  {
    ecm::telemetry::TelemetryRing<4> ring;
    ring.push(make_rec(ecm::telemetry::DIR_D0, 0xFFFFFF00u, kBcFrame, kBcLen));  // near the wrap
    const ecm::telemetry::FrameRecord r = ring.push(make_rec(ecm::telemetry::DIR_D0, 0x00000064u, kBcFrame, kBcLen));
    check(r.t_ms < 0xFFFFFF00u, "premises: the second frame's t_ms is LESS than the first's (the counter wrapped)");
    check(r.gap_ms == 356,
          "and the gap is still the correct small positive number: (0x64 - 0xFFFFFF00) mod 2^32 == 356 ms");
  }
}

// ---------------------------------------------------------------------------
// 3. Serialization — byte-exact against tools/ecm_frames.py
// ---------------------------------------------------------------------------

static void test_serialization() {
  std::printf("\n== 3. the JSON line is byte-exact against the Python contract ==\n");

  // The pinned vectors are real, so the serializer is being tested on frames
  // that occur on the wire rather than on convenient synthetic ones.
  check(ecm::crc_ok(kBcFrame, kBcLen) && ecm::crc_ok(kDcFrame, kDcLen),
        "the two recorded vectors really are CRC-valid frames");

  // -- the whole line, for a D0 frame that WAS answered --------------------
  // Note the array separators: the serializer emits ',' with no space, while
  // json.dumps in the Python tool writes ', '. Both are valid JSON and
  // ecm_diff parses rather than string-compares, so only the KEY SET and ORDER
  // are contractual — but the literals below must match THIS code exactly.
  {
    ecm::telemetry::FrameRecord r = make_rec(ecm::telemetry::DIR_D0, 200, kBcFrame, kBcLen);
    r.index = 7;                                    // set by the ring in real use
    r.gap_ms = ecm::telemetry::NO_GAP;              // first frame on this channel
    r.latency_ms = 3;                               // answered 3 ms after the last byte

    char buf[1024];
    const size_t n = ecm::telemetry::to_json(r, buf, sizeof(buf));
    const char *expect =
        "{\"channel\": \"D0\", \"t\": 0.200000, \"dev\": \"0x02\", \"seq\": 94, \"cmd\": \"0xBC\", "
        "\"payload\": [2,4], \"raw\": [75,2,94,188,2,4,164,207], \"crc\": 53156, \"index\": 7, "
        "\"gap_before\": null, \"ambiguous\": false, \"alt_lengths\": [], \"key\": \"D0/0x02/0xBC\", "
        "\"hex\": \"4B 02 5E BC 02 04 A4 CF\", \"latency_ms\": 3}";

    check(n == std::strlen(expect), "the return value is the line's byte length (the NUL is not counted)");
    check(std::strcmp(buf, expect) == 0,
          "the WHOLE D0 line matches the literal, key order and all",
          buf);  // on failure this prints the actual line, which is the useful half
  }

  // -- the whole line, for a D1 reply: a real gap, and NO latency ----------
  {
    ecm::telemetry::FrameRecord r = make_rec(ecm::telemetry::DIR_D1, 358, kBcFrame, kBcLen);
    r.index = 0;
    r.gap_ms = 158;          // 158 ms after the previous D1 frame
    r.latency_ms = 42;       // set to a REAL value on purpose: it must still be omitted

    char buf[1024];
    const size_t n = ecm::telemetry::to_json(r, buf, sizeof(buf));
    const char *expect =
        "{\"channel\": \"D1\", \"t\": 0.358000, \"dev\": \"0x02\", \"seq\": 94, \"cmd\": \"0xBC\", "
        "\"payload\": [2,4], \"raw\": [75,2,94,188,2,4,164,207], \"crc\": 53156, \"index\": 0, "
        "\"gap_before\": 0.158000, \"ambiguous\": false, \"alt_lengths\": [], \"key\": \"D1/0x02/0xBC\", "
        "\"hex\": \"4B 02 5E BC 02 04 A4 CF\"}";

    check(n == std::strlen(expect), "the D1 line's return value is its byte length");
    check(std::strcmp(buf, expect) == 0, "the WHOLE D1 line matches the literal", buf);
    check(std::strstr(buf, "latency_ms") == nullptr,
          "latency_ms is D0-only: a D1 record carries no latency key even when the field is set");
  }

  // -- dev and cmd MUST be quoted strings --------------------------------
  // This is the specific mismatch the header warns about. The Python re-spells
  // dev/cmd as "0x%02X" strings, and ecm_diff tolerates both forms, so emitting
  // integers here would pass every other test in this file and still mis-key
  // the map downstream. Hence the explicit check, not just the literal above.
  {
    ecm::telemetry::FrameRecord r = make_rec(ecm::telemetry::DIR_D0, 1, kDcFrame, kDcLen);
    char buf[2048];
    check(ecm::telemetry::to_json(r, buf, sizeof(buf)) > 0, "premises: the DC frame serialized");
    check(std::strstr(buf, "\"dev\": \"0x01\"") != nullptr,
          "dev is the STRING \"0x01\", not the integer 1");
    check(std::strstr(buf, "\"cmd\": \"0xDC\"") != nullptr,
          "cmd is the STRING \"0xDC\", not the integer 220");
    check(std::strstr(buf, "\"dev\": 1,") == nullptr && std::strstr(buf, "\"cmd\": 220,") == nullptr,
          "and the integer spellings are absent");

    // A recorded 31-byte frame exercises a long raw/payload/hex at once.
    check(std::strstr(buf, "\"payload\": [0,23,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]") != nullptr,
          "the DC frame's 25-byte payload is emitted between the header and the CRC");
    check(std::strstr(buf, "\"key\": \"D0/0x01/0xDC\"") != nullptr, "key is channel/dev/cmd, 0x-prefixed");
    check(std::strstr(buf, "\"hex\": \"4B 01 5D DC 00 17 00 00 00 00 00 04 00 00 00 00 "
                           "00 00 00 00 00 00 00 00 00 00 00 00 00 09 FF\"") != nullptr,
          "hex is all 31 bytes of the frame, space-separated uppercase");
    check(std::strstr(buf, "\"crc\": 65289") != nullptr,
          "crc is the parsed little-endian value (0xFF09), as a bare number");
  }

  // -- the constants the contract fixes -------------------------------------
  {
    ecm::telemetry::FrameRecord r = make_rec(ecm::telemetry::DIR_D0, 0, kBcFrame, kBcLen);
    char buf[1024];
    ecm::telemetry::to_json(r, buf, sizeof(buf));
    check(std::strstr(buf, "\"ambiguous\": false") != nullptr && std::strstr(buf, "\"alt_lengths\": []") != nullptr,
          "ambiguous is false and alt_lengths is [] — honestly constant, per the header's argument");
    check(std::strstr(buf, "\"gap_before\": null") != nullptr,
          "a record with NO_GAP emits gap_before: null, not 4294.967295");
  }

  // -- gap_before is a DECIMAL NUMBER OF SECONDS when there is one ----------
  {
    ecm::telemetry::FrameRecord r = make_rec(ecm::telemetry::DIR_D0, 0, kBcFrame, kBcLen);
    r.gap_ms = 1234;
    char buf[1024];
    ecm::telemetry::to_json(r, buf, sizeof(buf));
    check(std::strstr(buf, "\"gap_before\": 1.234000") != nullptr,
          "gap_before is seconds with six decimals (1234 ms -> 1.234000), matching the Python's seconds unit");
  }

  // -- latency_ms is present ONLY for an answered D0 record -----------------
  {
    ecm::telemetry::FrameRecord r = make_rec(ecm::telemetry::DIR_D0, 0, kBcFrame, kBcLen);
    r.latency_ms = ecm::telemetry::NO_LATENCY;
    char buf[1024];
    ecm::telemetry::to_json(r, buf, sizeof(buf));
    check(std::strstr(buf, "latency_ms") == nullptr,
          "an unanswered D0 record (NO_LATENCY) omits latency_ms rather than emitting 65535");
  }

  // -- a frame too short to have a payload ---------------------------------
  // raw_len < 6 means there is no room for a header + payload + CRC, so the
  // payload loop must not run at all. Reading raw[4..] here would read the
  // bytes that are not part of the frame — and for a 4-byte frame, raw[80]
  // would be past the array entirely.
  {
    const uint8_t stub[] = {0x4B, 0x01, 0x5D, 0xDC};   // 4 bytes: header only
    ecm::telemetry::FrameRecord r = make_rec(ecm::telemetry::DIR_D0, 0, stub, sizeof(stub));
    char buf[1024];
    const size_t n = ecm::telemetry::to_json(r, buf, sizeof(buf));
    check(n > 0, "a 4-byte frame still serializes (it is data, not an error)");
    check(std::strstr(buf, "\"payload\": []") != nullptr,
          "a frame shorter than 6 bytes yields an EMPTY payload array, not garbage");
    check(count_array_ints(buf, "\"payload\": [") == 0,
          "the payload array has exactly zero entries");
    check(count_array_ints(buf, "\"raw\": [") == 4,
          "while raw still reports all 4 bytes that were actually captured");
    check(std::strstr(buf, "\"crc\": 0") != nullptr,
          "and crc is 0: a frame this short has no CRC bytes to have parsed");
  }
}

// ---------------------------------------------------------------------------
// 4. Bounds — the serializer must clip, never overrun
// ---------------------------------------------------------------------------

static void test_bounds() {
  std::printf("\n== 4. bounds: a short buffer and an over-long frame ==\n");

  ecm::telemetry::FrameRecord r = make_rec(ecm::telemetry::DIR_D0, 200, kBcFrame, kBcLen);

  // -- a buffer far too small, with guard bytes on BOTH sides ---------------
  // The dangerous path is not "returns 0 immediately" but "writes some, then
  // stops": that is where an off-by-one overruns the caller's buffer. 16 bytes
  // is big enough for the serializer to get partway through the key sequence.
  {
    const size_t kGuard = 8;
    const size_t kCap = 16;
    uint8_t arena[kGuard + kCap + kGuard];
    std::memset(arena, 0xA5, sizeof(arena));
    char *buf = reinterpret_cast<char *>(arena + kGuard);

    const size_t n = ecm::telemetry::to_json(r, buf, kCap);
    check(n == 0, "to_json into a 16-byte buffer returns 0 (no partial line is ever reported as success)");

    bool before_ok = true;
    for (size_t i = 0; i < kGuard; ++i) {
      if (arena[i] != 0xA5) {
        before_ok = false;
      }
    }
    bool after_ok = true;
    for (size_t i = kGuard + kCap; i < sizeof(arena); ++i) {
      if (arena[i] != 0xA5) {
        after_ok = false;
      }
    }
    check(before_ok, "the 8 guard bytes BEFORE the buffer are untouched");
    check(after_ok, "the 8 guard bytes AFTER the buffer are untouched (no write past buf[cap-1])");
  }

  // -- capacities too small to hold even a NUL ------------------------------
  {
    char one[1] = {'X'};
    check(ecm::telemetry::to_json(r, one, 1) == 0, "cap == 1 (no room for a terminator) returns 0");
    check(one[0] == 'X', "and writes nothing at all");
    check(ecm::telemetry::to_json(r, one, 0) == 0, "cap == 0 returns 0");
    check(one[0] == 'X', "and writes nothing at all either");
  }

  // -- a null buffer is refused, not dereferenced ---------------------------
  {
    check(ecm::telemetry::to_json(r, nullptr, 1024) == 0,
          "a null buffer returns 0 rather than crashing");
  }

  // -- a raw_len LONGER than the array must clip at RAW_CAP -----------------
  // raw_len is a uint8_t and can in principle be set to anything; the ring must
  // never serialise past raw[RAW_CAP - 1]. The count of emitted values is the
  // observable proof that it clipped instead of reading whatever followed.
  {
    ecm::telemetry::FrameRecord big;
    big.dir = ecm::telemetry::DIR_D0;
    for (size_t i = 0; i < ecm::telemetry::RAW_CAP; ++i) {
      big.raw[i] = static_cast<uint8_t>(i + 1);
    }
    big.raw_len = 200;  // longer than RAW_CAP (80): must NOT be believed

    char buf[2048];
    const size_t n = ecm::telemetry::to_json(big, buf, sizeof(buf));
    check(n > 0 && n < sizeof(buf), "an over-long raw_len still serializes within the buffer");
    check(count_array_ints(buf, "\"raw\": [") == static_cast<int>(ecm::telemetry::RAW_CAP),
          "exactly RAW_CAP (80) raw values are emitted — the array was CLIPPED, not read past");
    check(count_hex_bytes(buf) == static_cast<int>(ecm::telemetry::RAW_CAP),
          "and exactly 80 bytes appear in hex, so no out-of-bounds byte reached the line");
    check(std::strstr(buf, ",80], \"crc\": ") != nullptr,
          "the last emitted raw value is raw[79] == 80 (the array stopped at the real end)");
  }
}

// ---------------------------------------------------------------------------
// 5. Structure, at run time as well as compile time
// ---------------------------------------------------------------------------

static void test_structure() {
  std::printf("\n== 5. the record's shape ==\n");

  check(sizeof(ecm::telemetry::FrameRecord) == 104,
        "sizeof(FrameRecord) is 104 bytes, as the static_assert requires");
  check(std::is_trivially_copyable_v<ecm::telemetry::FrameRecord>,
        "FrameRecord is trivially copyable (push() relies on it)");
  check(ecm::telemetry::RAW_CAP == 80, "RAW_CAP is the 80-byte parser ceiling");

  // The sentinels are part of the JSON contract (NO_GAP becomes null), so pin
  // their values: a change here changes what the downstream tool sees.
  check(ecm::telemetry::NO_GAP == 0xFFFFFFFFu, "NO_GAP is 0xFFFFFFFF");
  check(ecm::telemetry::NO_LATENCY == 0xFFFFu, "NO_LATENCY is 0xFFFF");
  check(ecm::telemetry::DIR_D0 == 0 && ecm::telemetry::DIR_D1 == 1,
        "the direction constants are 0 and 1 — they index the ring's per-direction arrays");
}

int main() {
  std::printf("\n=== ecm telemetry: host test ===\n");
  std::printf("    synthetic records, no board, no UART, no clock.\n\n");

  test_ring_fill_and_evict();
  test_per_channel_bookkeeping();
  test_serialization();
  test_bounds();
  test_structure();

  std::printf("\n============================================================\n");
  if (g_fail) {
    std::printf("RESULT: %d FAILED, %d passed\n", g_fail, g_pass);
  } else {
    std::printf("RESULT: all %d checks passed\n", g_pass);
  }
  std::printf("============================================================\n");
  return g_fail ? 1 : 0;
}

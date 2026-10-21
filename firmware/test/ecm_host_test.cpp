// ecm_host_test.cpp — host build of the protocol core.
//
// No ESPHome, no board, no hardware. It runs on a Mac against vectors taken from
// the module's own recorded behaviour, because the real module already answered
// these exact requests — its replies are ground truth.
//
//     clang++ -std=c++20 -Wall -Wextra -O1 -o /tmp/ecm_host_test test/ecm_host_test.cpp
//     /tmp/ecm_host_test                       # built-in vectors
//     /tmp/ecm_host_test /path/to/captures     # + replay every recorded exchange
//
// The replayed vectors below are byte-for-byte from
// `captures/preserved-run-manual-0319/` and
// `captures/run-manual-20261016-112657/` (`frames_D0.jsonl` / `frames_D1.jsonl`).
//
// This mirrors `tools/ecm_responder.py --selftest`, which is the validated
// reference. Run both; they must agree.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../include/ecm_protocol.h"

// ---------------------------------------------------------------------------
// Tiny test harness
// ---------------------------------------------------------------------------

static int g_passed = 0;
static int g_failed = 0;
static std::vector<std::string> g_failures;

static std::string hex(const uint8_t *data, size_t len) {
  std::string out;
  char buf[4];
  for (size_t i = 0; i < len; ++i) {
    std::snprintf(buf, sizeof(buf), "%02X", data[i]);
    if (i != 0) {
      out += ' ';
    }
    out += buf;
  }
  return out;
}

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

static bool bytes_equal(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen) {
  return alen == blen && std::memcmp(a, b, alen) == 0;
}

// ---------------------------------------------------------------------------
// One recorded exchange: what the ECM sent, and what the real module answered
// ---------------------------------------------------------------------------

struct Vector {
  const char *what;
  std::vector<uint8_t> request;
  std::vector<uint8_t> module_reply;
};

// The module's replies here are ground truth: real hardware, real bus.
// Returned by reference from a function-local static: callers hold references
// into these vectors (recorded_vectors()[2].request), and a returned temporary
// would dangle at the end of the full expression.
static const std::vector<Vector> &recorded_vectors() {
  static const std::vector<Vector> vectors = {
      {"0x20 power-on / boot frame",
       {0x4B, 0x01, 0x00, 0x20, 0x03, 0x01, 0x01, 0xDA, 0x45},
       {0x4B, 0x03, 0x00, 0x20, 0x03, 0x01, 0x8B, 0x5A}},

      {"0xBC write carrier, idle slot",
       {0x4B, 0x02, 0xDE, 0xBC, 0x02, 0x04, 0x8D, 0x0F},
       {0x4B, 0x04, 0xDE, 0xBC, 0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x30, 0xCF}},

      {"0xDC status report (31-byte request)",
       {0x4B, 0x01, 0xDD, 0xDC, 0x00, 0x17, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x60},
       {0x4B, 0x03, 0xDD, 0xDC, 0x00, 0x17, 0xF0, 0x38}},

      {"0x64 routine frame",
       {0x4B, 0x01, 0xDF, 0x64, 0x00, 0x01, 0x64, 0x6B, 0x4D},
       {0x4B, 0x03, 0xDF, 0x64, 0x00, 0x01, 0xF0, 0x6B}},

      {"0x6E routine frame (one trailing 00)",
       {0x4B, 0x02, 0xE5, 0x6E, 0x00, 0x01, 0xE1, 0x71},
       {0x4B, 0x04, 0xE5, 0x6E, 0x00, 0x01, 0x00, 0xB1, 0x2E}},

      {"0xE1 height announcement",
       {0x4B, 0x01, 0xD3, 0xE1, 0x00, 0x01, 0x04, 0x52, 0x68},
       {0x4B, 0x03, 0xD3, 0xE1, 0x00, 0x01, 0xE2, 0xD2}},

      {"0xE4 flame announcement",
       {0x4B, 0x01, 0x28, 0xE4, 0x00, 0x01, 0x00, 0xB6, 0xB3},
       {0x4B, 0x03, 0x28, 0xE4, 0x00, 0x01, 0xC3, 0xF7}},

      // The pilot announcement, both transitions, from
      // `captures/run-manual-20261016-152343/`: the ECM answers a pilot write
      // with 0xDF ~0.18 s later, echo-only, exactly like 0xE4 and 0xE1. These
      // are the two frames that used to be answered with silence — and counted
      // as MISMATCH by the replay — before 0xDF was learned.
      {"0xDF pilot announcement (pilot ON)",
       {0x4B, 0x01, 0xFC, 0xDF, 0x00, 0x01, 0x01, 0x4A, 0x45},
       {0x4B, 0x03, 0xFC, 0xDF, 0x00, 0x01, 0x8B, 0xCA}},

      {"0xDF pilot announcement (pilot OFF)",
       {0x4B, 0x01, 0x37, 0xDF, 0x00, 0x01, 0x00, 0x2E, 0x55},
       {0x4B, 0x03, 0x37, 0xDF, 0x00, 0x01, 0xB5, 0xEE}},
  };
  return vectors;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_crc() {
  std::printf("\n== CRC-16/MODBUS, against real frames ==\n");

  // Every recorded frame must CRC-validate, and the stored CRC is the frame's
  // last two bytes read little-endian.
  for (const auto &v : recorded_vectors()) {
    const bool ok = ecm::crc_ok(v.request.data(), v.request.size());
    check(ok, std::string("CRC validates: ") + v.what, hex(v.request.data(), v.request.size()));
  }

  // An independent, bitwise implementation of the same algorithm (the one in
  // tools/ecm_analyse.py) must agree with the table-driven one for every length.
  auto bitwise = [](const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
      crc ^= data[i];
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001) : static_cast<uint16_t>(crc >> 1);
      }
    }
    return crc;
  };

  bool all_agree = true;
  for (const auto &v : recorded_vectors()) {
    for (size_t len = 1; len <= v.request.size(); ++len) {
      if (ecm::crc16_modbus(v.request.data(), len) != bitwise(v.request.data(), len)) {
        all_agree = false;
      }
    }
  }
  check(all_agree, "table-driven CRC == bitwise reference for every prefix length");

  // A corrupted byte must break validation.
  auto corrupt = recorded_vectors()[3].request;
  corrupt[5] ^= 0x01;
  check(!ecm::crc_ok(corrupt.data(), corrupt.size()), "a flipped payload bit fails CRC");
}

static void test_parse_rejects() {
  std::printf("\n== parse_request rejects what it must ==\n");

  ecm::Frame frame;
  const auto good = recorded_vectors()[3].request;

  check(!ecm::parse_request(nullptr, 0, frame), "nullptr + length 0 rejected");
  check(!ecm::parse_request(good.data(), 0, frame), "length 0 rejected");
  check(!ecm::parse_request(good.data(), 5, frame), "length 5 (< MIN_FRAME_LEN) rejected");
  check(!ecm::parse_request(good.data(), 6, frame), "wrong-length-but-long-enough rejected on CRC");

  auto bad_start = good;
  bad_start[0] = 0x4C;
  check(!ecm::parse_request(bad_start.data(), bad_start.size(), frame), "wrong start byte rejected");

  auto bad_crc = good;
  bad_crc.back() ^= 0xFF;
  check(!ecm::parse_request(bad_crc.data(), bad_crc.size(), frame), "bad CRC rejected");

  // The 31-byte frame is longer than an 8-byte one: proves no fixed-length
  // assumption crept in (the old analysis tool silently skipped this frame).
  const auto &dc = recorded_vectors()[2].request;
  const bool ok = ecm::parse_request(dc.data(), dc.size(), frame);
  check(ok && frame.payload_len == 25 && frame.cmd == 0xDC, "31-byte DC frame parses, payload_len 25");
}

static void test_reply_identity() {
  std::printf("\n== build_reply is byte-identical to the real module ==\n");

  for (const auto &v : recorded_vectors()) {
    ecm::Frame frame;
    if (!ecm::parse_request(v.request.data(), v.request.size(), frame)) {
      check(false, std::string("parse failed for ") + v.what);
      continue;
    }

    uint8_t reply[ecm::MAX_REPLY_LEN];
    const size_t len = ecm::build_reply(frame, nullptr, reply, sizeof(reply));

    if (len == 0) {
      check(false, std::string("no reply built for ") + v.what);
      continue;
    }

    // For a BC reply the module may have injected a command into the slot; those
    // are recorded command EVENTS, not idle replies. Our responder has nothing
    // queued, so the head must still match and the slot must be idle.
    if (frame.cmd == 0xBC) {
      const bool head_ok = bytes_equal(reply, 6, v.module_reply.data(), 6);
      check(head_ok, "BC reply head matches (dev+2, seq copied, echo)", std::string("ours ") +
                                                                           hex(reply, 6) + "\n          real " +
                                                                           hex(v.module_reply.data(), 6));
      const bool slot_idle = reply[6] == 0 && reply[7] == 0 && reply[8] == 0 && reply[9] == 0;
      check(slot_idle, "idle BC reply carries a zeroed slot");
    }

    check(bytes_equal(reply, len, v.module_reply.data(), v.module_reply.size()),
          std::string("reply identical: ") + v.what,
          std::string("ours ") + hex(reply, len) + "\n          real " + hex(v.module_reply.data(), v.module_reply.size()));
  }
}

static void test_slots() {
  std::printf("\n== the 0xBC write slot ==\n");

  const auto idle = ecm::slot_idle();
  check(idle.b[0] == 0 && idle.b[1] == 0 && idle.b[2] == 0 && idle.b[3] == 0, "slot_idle is 00 00 00 00");

  const auto on = ecm::slot_flame(true);
  const auto off = ecm::slot_flame(false);
  check(on.b[0] == 0x64 && on.b[1] == 0x00 && on.b[2] == 0x01 && on.b[3] == 0x00, "slot_flame(on) = 64 00 01 00");
  check(off.b[0] == 0x64 && off.b[1] == 0x00 && off.b[2] == 0x00 && off.b[3] == 0x00, "slot_flame(off) = 64 00 00 00");

  // Pilot: opcode 0x65, the same 01/00 value encoding. Captured and verified on
  // both transitions, so these two constants are the whole pilot command.
  const auto pilot_on = ecm::slot_pilot(true);
  const auto pilot_off = ecm::slot_pilot(false);
  check(pilot_on.b[0] == 0x65 && pilot_on.b[1] == 0x00 && pilot_on.b[2] == 0x01 && pilot_on.b[3] == 0x00,
        "slot_pilot(on) = 65 00 01 00");
  check(pilot_off.b[0] == 0x65 && pilot_off.b[1] == 0x00 && pilot_off.b[2] == 0x00 && pilot_off.b[3] == 0x00,
        "slot_pilot(off) = 65 00 00 00");
  // The opcode and its ECM announcement are two different namespaces: 0x65 is
  // written, 0xDF is what the ECM says back. Guard the literal, not a transform.
  check(ecm::op::PILOT != ecm::cmd::PILOT_ANNOUNCE && ecm::cmd::PILOT_ANNOUNCE == 0xDF,
        "the pilot write opcode 0x65 and its announcement 0xDF are distinct constants");

  // App 1-5 -> wire 0-4. The off-by-one between the app and the wire is a real
  // trap: the ECM indexes height from zero and the app shows it from one.
  bool mapping_ok = true;
  for (uint8_t app = 1; app <= 5; ++app) {
    const auto slot = ecm::slot_height(app);
    if (slot.b[0] != 0x67 || slot.b[1] != 0x00 || slot.b[2] != app - 1 || slot.b[3] != 0x00) {
      mapping_ok = false;
    }
  }
  check(mapping_ok, "slot_height(app 1..5) = 67 00 (app-1) 00");

  // Out of range clamps to the nearest legal value instead of wrapping.
  check(ecm::slot_height(0).b[2] == 0x00, "slot_height(0) clamps to internal 0");
  check(ecm::slot_height(99).b[2] == 0x04, "slot_height(99) clamps to internal 4");
  check(ecm::slot_height_internal(3).b[2] == 0x03, "slot_height_internal(3) -> 03");

  // A BC reply carrying each slot, and the whole frame round-trips.
  ecm::Frame frame;
  const auto &bc = recorded_vectors()[1].request;
  if (ecm::parse_request(bc.data(), bc.size(), frame)) {
    uint8_t reply[ecm::MAX_REPLY_LEN];

    const auto height3 = ecm::slot_height(4);  // app 4 -> internal 3
    const size_t len = ecm::build_reply(frame, &height3, reply, sizeof(reply));
    const uint8_t expected[12] = {0x4B, 0x04, 0xDE, 0xBC, 0x02, 0x04, 0x67, 0x00, 0x03, 0x00, 0, 0};
    check(len == 12 && std::memcmp(reply, expected, 10) == 0,
          "BC reply with height 3 has the documented shape (67 00 03 00)",
          hex(reply, len));

    // The same carrier, carrying the pilot write instead.
    const auto pilot1 = ecm::slot_pilot(true);
    const size_t plen = ecm::build_reply(frame, &pilot1, reply, sizeof(reply));
    const uint8_t p_expected[12] = {0x4B, 0x04, 0xDE, 0xBC, 0x02, 0x04, 0x65, 0x00, 0x01, 0x00, 0, 0};
    check(plen == 12 && std::memcmp(reply, p_expected, 10) == 0,
          "BC reply with pilot ON has the documented shape (65 00 01 00)",
          hex(reply, plen));

    // The frame we build must itself parse — CRC stored what the CRC computed.
    ecm::Frame round_trip;
    check(ecm::parse_request(reply, len, round_trip) && round_trip.cmd == 0xBC &&
              round_trip.dev == frame.dev + 2 && round_trip.seq == frame.seq,
          "a built reply re-parses: dev+2, seq copied, CRC valid");

    // A slot is ignored for every command that is not the write carrier.
    const auto &dc = recorded_vectors()[2].request;
    ecm::Frame dc_frame;
    if (ecm::parse_request(dc.data(), dc.size(), dc_frame)) {
      uint8_t with_slot[ecm::MAX_REPLY_LEN];
      uint8_t without[ecm::MAX_REPLY_LEN];
      const size_t a = ecm::build_reply(dc_frame, &height3, with_slot, sizeof(with_slot));
      const size_t b = ecm::build_reply(dc_frame, nullptr, without, sizeof(without));
      check(a == b && bytes_equal(with_slot, a, without, b),
            "a pending write does not leak into a non-BC reply");
    }
  }
}

static void test_stay_silent() {
  std::printf("\n== unknown input means SILENCE, never a guess ==\n");

  // A syntactically valid frame with a command byte we do not know.
  uint8_t unknown[8] = {0x4B, 0x02, 0x10, 0x99, 0x02, 0x04, 0x00, 0x00};
  const uint16_t crc = ecm::crc16_modbus(unknown, 6);
  unknown[6] = static_cast<uint8_t>(crc & 0xFF);
  unknown[7] = static_cast<uint8_t>(crc >> 8);

  ecm::Frame frame;
  const bool parsed = ecm::parse_request(unknown, sizeof(unknown), frame);
  uint8_t reply[ecm::MAX_REPLY_LEN];
  check(parsed, "an unknown command still parses (we must be able to count it)");
  check(!ecm::is_known_cmd(0x99), "0x99 is not a known command");
  check(ecm::build_reply(frame, nullptr, reply, sizeof(reply)) == 0, "unknown command -> no reply at all");

  // When we did not hear a request, we cannot know it happened — silence.
  check(ecm::build_reply(frame, nullptr, reply, sizeof(reply)) == 0, "silence is a return of 0, not an error");

  // A CRC-valid frame with no room to echo anything cannot be answered.
  uint8_t no_payload[6] = {0x4B, 0x02, 0x11, 0xBC, 0x00, 0x00};
  const uint16_t crc2 = ecm::crc16_modbus(no_payload, 4);
  no_payload[4] = static_cast<uint8_t>(crc2 & 0xFF);
  no_payload[5] = static_cast<uint8_t>(crc2 >> 8);
  ecm::Frame empty;
  check(ecm::parse_request(no_payload, sizeof(no_payload), empty), "payload-less frame parses (len == 6)");
  check(ecm::build_reply(empty, nullptr, reply, sizeof(reply)) == 0, "payload-less frame -> no reply");

  // Output buffer too small: refuse rather than truncate a frame.
  ecm::Frame bc_frame;
  const auto &bc = recorded_vectors()[1].request;
  if (ecm::parse_request(bc.data(), bc.size(), bc_frame)) {
    uint8_t small[11];
    check(ecm::build_reply(bc_frame, nullptr, small, sizeof(small)) == 0,
          "a short output buffer refuses (11 < 12) rather than truncating");
    check(ecm::build_reply(bc_frame, nullptr, nullptr, 0) == 0, "a null output buffer refuses");
  }
}

static void test_new_announcements() {
  std::printf("\n== ECM announcements E0 / E2 / E5: answered, echo-only ==\n");

  // These three have never appeared in a capture, because this firmware never
  // sends the slots that provoke them (0x66, 0x68, 0x69). The vectors are
  // constructed from the ECM's announcement builders, which pass type 1, the
  // command, and a one-byte payload:
  //
  //     4B 01 <seq> <CMD> 00 01 <value> <crc>
  //
  // They are inferred, not replayed, and are labelled as such. The reply
  // follows the universal echo rule, so it is 8 bytes and independent of the
  // value byte. Staying silent on one would lose an exchange if it ever did
  // appear — which is exactly the failure 0xDF exposed before it was learned.
  struct Announcement {
    const char *what;
    uint8_t cmd;
    std::vector<uint8_t> request;
    std::vector<uint8_t> reply;
  };
  const Announcement cases[] = {
      {"0xE0 after slot 0x66 (config byte, value 0)",
       0xE0,
       {0x4B, 0x01, 0x00, 0xE0, 0x00, 0x01, 0x00, 0xD7, 0x85},
       {0x4B, 0x03, 0x00, 0xE0, 0x00, 0x01, 0x8B, 0x96}},
      {"0xE2 after slot 0x68 (heat-demand byte, value 0)",
       0xE2,
       {0x4B, 0x01, 0x00, 0xE2, 0x00, 0x01, 0x00, 0xD6, 0x3D},
       {0x4B, 0x03, 0x00, 0xE2, 0x00, 0x01, 0x2A, 0x56}},
      {"0xE5 after slot 0x69 (boolean, value 1)",
       0xE5,
       {0x4B, 0x01, 0x00, 0xE5, 0x00, 0x01, 0x01, 0x16, 0x89},
       {0x4B, 0x03, 0x00, 0xE5, 0x00, 0x01, 0x9B, 0x97}},
      {"0xE6 after slot 0x6B (one-byte getter)",
       0xE6,
       {0x4B, 0x01, 0x00, 0xE6, 0x00, 0x01, 0x00, 0xD7, 0x0D},
       {0x4B, 0x03, 0x00, 0xE6, 0x00, 0x01, 0x6B, 0x97}},
      {"0xE7 after slot 0x6C (two-byte payload)",
       0xE7,
       {0x4B, 0x01, 0x00, 0xE7, 0x00, 0x02, 0x00, 0x00, 0x80, 0x9E},
       {0x4B, 0x03, 0x00, 0xE7, 0x00, 0x02, 0x7A, 0x56}},
      {"0xE9 after slot 0x6D (one-byte getter)",
       0xE9,
       {0x4B, 0x01, 0x00, 0xE9, 0x00, 0x01, 0x00, 0xD4, 0x19},
       {0x4B, 0x03, 0x00, 0xE9, 0x00, 0x01, 0x5B, 0x94}},
      {"0xEA after slot 0x6E (four-byte payload)",
       0xEA,
       {0x4B, 0x01, 0x00, 0xEA, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0xB4, 0xA8},
       {0x4B, 0x03, 0x00, 0xEA, 0x00, 0x04, 0x6B, 0x97}},
  };

  for (const auto &c : cases) {
    ecm::Frame frame;
    const bool parsed = ecm::parse_request(c.request.data(), c.request.size(), frame);
    uint8_t reply[ecm::MAX_REPLY_LEN];
    const size_t len = parsed ? ecm::build_reply(frame, nullptr, reply, sizeof(reply)) : 0;
    check(ecm::is_known_cmd(c.cmd), std::string("is_known_cmd: ") + c.what);
    check(parsed, std::string("request parses: ") + c.what);
    check(len == c.reply.size() && bytes_equal(reply, len, c.reply.data(), c.reply.size()),
          std::string("echo-only reply: ") + c.what, hex(reply, len));
  }
}

static void test_command_word_and_type() {
  std::printf("\n== 16-bit command words, and the type must match ==\n");

  // The ECM's dispatcher compares (frame[4] << 8) | frame[3], so matching on
  // the low byte alone is wrong. These vectors exercise that distinction.
  struct Case {
    const char *what;
    std::vector<uint8_t> request;
    bool answered;
    std::vector<uint8_t> reply;  // empty when answered == false
  };
  const Case cases[] = {
      // Schedule announcement, word 0x01F4 (slot id 0xC8). Type 1, 2 data bytes.
      {"0x01F4 schedule announcement is answered echo-only",
       {0x4B, 0x01, 0x00, 0xF4, 0x01, 0x02, 0x80, 0x00, 0x65, 0x61},
       true,
       {0x4B, 0x03, 0x00, 0xF4, 0x01, 0x02, 0x8A, 0x03}},

      // 0x0323: type 1, one data byte. Its low byte 0x23 is otherwise mundane.
      {"0x0323 is answered echo-only",
       {0x4B, 0x01, 0x00, 0x23, 0x03, 0x01, 0x01, 0xDA, 0x01},
       true,
       {0x4B, 0x03, 0x00, 0x23, 0x03, 0x01, 0x7B, 0x5A}},

      // An unreferenced ECM announcement builder.
      {"0x00DE is answered echo-only",
       {0x4B, 0x01, 0x00, 0xDE, 0x00, 0x01, 0x00, 0xDA, 0x6D},
       true,
       {0x4B, 0x03, 0x00, 0xDE, 0x00, 0x01, 0xEA, 0x5A}},

      // The ECM's type-1 echo of a module time sync is answerable...
      {"0x00C6 type-1 echo is answered echo-only",
       {0x4B, 0x01, 0x00, 0xC6, 0x02, 0x04, 0x69, 0x55, 0xB9, 0x00, 0x26, 0x94},
       true,
       {0x4B, 0x03, 0x00, 0xC6, 0x02, 0x04, 0xAB, 0x3E}},

      // ...but the type-2 request for 8 epoch bytes is NOT: answering it with
      // the echo shape would make the ECM read a short, bogus epoch.
      {"0x02C6 type-2 request is NOT answered (wants 8 epoch bytes)",
       {0x4B, 0x02, 0x00, 0xC6, 0x02, 0x08, 0x96, 0xFB},
       false,
       {}},

      // 0x0321/0x0322 are real type-2 requests with no known expected payload.
      {"0x0321 type-2 request is NOT answered",
       {0x4B, 0x02, 0x00, 0x21, 0x03, 0x01, 0xE7, 0x5A},
       false,
       {}},

      // The boot frame is word 0x0320, not plain 0x20. A frame with the same
      // low byte and tag 0 is a different word and must not be guessed at.
      {"0x0020 (wrong tag) is NOT the boot frame and is NOT answered",
       {0x4B, 0x01, 0x00, 0x20, 0x00, 0x01, 0x01, 0x2A, 0x45},
       false,
       {}},
  };

  for (const auto &c : cases) {
    ecm::Frame frame;
    const bool parsed = ecm::parse_request(c.request.data(), c.request.size(), frame);
    uint8_t reply[ecm::MAX_REPLY_LEN];
    const size_t len = parsed ? ecm::build_reply(frame, nullptr, reply, sizeof(reply)) : 0;
    if (!c.answered) {
      check(parsed && len == 0, std::string("not answered: ") + c.what, hex(reply, len));
      check(parsed && !ecm::is_known_frame(frame), std::string("is_known_frame false: ") + c.what);
    } else {
      check(parsed && len == c.reply.size() &&
                bytes_equal(reply, len, c.reply.data(), c.reply.size()),
            std::string("answered echo-only: ") + c.what, hex(reply, len));
      check(parsed && ecm::is_known_frame(frame), std::string("is_known_frame true: ") + c.what);
    }
  }

  // Announcement words are a property of the WORD; requests are a property of
  // the word AND the type. 0x02C6 is both, which is why it is in both lists.
  check(ecm::is_announcement_word(0x0320) && ecm::is_announcement_word(0x00DC) &&
            ecm::is_announcement_word(0x02C6) && !ecm::is_announcement_word(0x02BC),
        "announcement words: boot/status true, BC false");
  check(ecm::is_replyable_request_word(0x02BC) && ecm::is_replyable_request_word(0x006E) &&
            !ecm::is_replyable_request_word(0x02C6) && !ecm::is_replyable_request_word(0x00D2),
        "replyable requests: BC and 6E only — C6/D2 requests need data we do not have");
  check(ecm::is_announcement_word(0x01F4) && ecm::is_announcement_word(0x0262) &&
            !ecm::is_announcement_word(0x01F5) && !ecm::is_announcement_word(0x0264),
        "schedule family: every EVEN word 0x01F4-0x0262, nothing outside it");
}

static void test_seq_reset_tolerated() {
  std::printf("\n== ECM seq reset: tolerated, never predicted ==\n");

  // The ECM can zero its exchange counter at any time. A reply
  // must simply copy whatever seq arrived. Build two BC requests, one at 0xFF
  // and one at 0x00, and assert both are answered with their own seq. There is
  // no state in this header, so this also proves nothing carries across frames.
  auto make_bc = [](uint8_t seq, uint8_t out[8]) {
    out[0] = 0x4B;
    out[1] = 0x02;
    out[2] = seq;
    out[3] = 0xBC;
    out[4] = 0x02;
    out[5] = 0x04;
    const uint16_t crc = ecm::crc16_modbus(out, 6);
    out[6] = static_cast<uint8_t>(crc & 0xFF);
    out[7] = static_cast<uint8_t>(crc >> 8);
  };

  const uint8_t seqs[3] = {0xFF, 0x00, 0x01};  // wrap, reset, resume
  bool all_ok = true;
  std::string detail;
  for (uint8_t seq : seqs) {
    uint8_t request[8];
    make_bc(seq, request);
    ecm::Frame frame;
    uint8_t reply[ecm::MAX_REPLY_LEN];
    const bool parsed = ecm::parse_request(request, sizeof(request), frame);
    const size_t len = parsed ? ecm::build_reply(frame, nullptr, reply, sizeof(reply)) : 0;
    if (!parsed || len != 12 || reply[2] != seq) {
      all_ok = false;
      detail += "seq " + std::to_string(seq) + " failed; ";
    }
  }
  check(all_ok, "seq 0xFF -> 0x00 -> 0x01: each poll is answered with its own seq copied", detail);
}

static void test_status_decode() {
  std::printf("\n== the 0xDC status payload field map ==\n");

  ecm::Frame frame;
  const auto &dc = recorded_vectors()[2].request;
  if (!ecm::parse_request(dc.data(), dc.size(), frame)) {
    check(false, "DC frame did not parse");
    return;
  }

  check(ecm::has_status_fields(frame), "the DC frame carries the mapped fields");

  // This captured frame was taken with the fireplace burning at app height 1:
  // pilot on, height internal 0, not yet hot, flame on.
  check(ecm::status_byte(frame, ecm::status::PILOT) == 0x01, "byte 5 = pilot on");
  check(ecm::status_byte(frame, ecm::status::CONFIG) == 0x00, "byte 6 = config byte (constant 0 here)");
  check(ecm::status_byte(frame, ecm::status::HEIGHT) == 0x00, "byte 7 = height internal 0 (app 1)");
  check(ecm::status_byte(frame, ecm::status::HEAT_DEMAND) == 0x00, "byte 8 = heat-demand byte");
  check(ecm::status_byte(frame, ecm::status::FLAG_9) == 0x00,
        "byte 9 = 0 (32-bit deadline-armed flag; the old \"hot\" label is unsupported)");
  check(ecm::status_byte(frame, ecm::status::FLAME) == 0x01, "byte 10 = flame on");

  // Reading past the end of a short payload must not read memory.
  const auto &short_frame = recorded_vectors()[3].request;  // 9 bytes, payload_len 3
  ecm::Frame short_parsed;
  if (ecm::parse_request(short_frame.data(), short_frame.size(), short_parsed)) {
    check(!ecm::has_status_fields(short_parsed), "a non-DC frame has no status fields");
    check(ecm::status_byte(short_parsed, ecm::status::FLAME) == 0, "an out-of-range status read returns 0");
  }
}

// ---------------------------------------------------------------------------
// Optional: replay a whole capture, exactly as the Python reference does
// ---------------------------------------------------------------------------

struct Recorded {
  double t{0.0};
  int seq{-1};
  int cmd{-1};
  std::vector<uint8_t> raw;
};

// Minimal read of the fields the replay needs out of a frames_*.jsonl line.
static bool parse_jsonl_line(const std::string &line, Recorded &out) {
  auto find_number = [&](const char *key, double &value) {
    const size_t at = line.find(key);
    if (at == std::string::npos) {
      return false;
    }
    value = std::strtod(line.c_str() + at + std::strlen(key), nullptr);
    return true;
  };

  double t = 0, seq = 0;
  if (!find_number("\"t\": ", t) || !find_number("\"seq\": ", seq)) {
    return false;
  }

  // Offsets are derived from the prefixes themselves — a hardcoded +9 that is
  // really +8 silently shifts every byte by one and "parses" a wrong frame.
  const char *const cmd_prefix = "\"cmd\": \"0x";
  const char *const raw_prefix = "\"raw\": [";
  const size_t cmd_at = line.find(cmd_prefix);
  const size_t raw_at = line.find(raw_prefix);
  if (cmd_at == std::string::npos || raw_at == std::string::npos) {
    return false;
  }

  out.t = t;
  out.seq = static_cast<int>(seq);
  out.cmd = static_cast<int>(std::strtol(line.c_str() + cmd_at + std::strlen(cmd_prefix), nullptr, 16));

  out.raw.clear();
  const char *p = line.c_str() + raw_at + std::strlen(raw_prefix);
  while (*p != ']' && *p != '\0') {
    if (*p >= '0' && *p <= '9') {
      out.raw.push_back(static_cast<uint8_t>(std::strtol(p, const_cast<char **>(&p), 10)));
    } else {
      ++p;
    }
  }
  return !out.raw.empty();
}

static std::vector<Recorded> load_jsonl(const std::string &path) {
  std::vector<Recorded> records;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    Recorded record;
    if (parse_jsonl_line(line, record)) {
      records.push_back(std::move(record));
    }
  }
  return records;
}

static void replay_captures(const std::string &captures_dir) {
  std::printf("\n== replay recorded captures (%s) ==\n", captures_dir.c_str());

  int total_compared = 0;
  int total_identical = 0;
  int total_injections = 0;
  int total_unpaired = 0;
  int total_mismatched = 0;

  namespace fs = std::filesystem;
  std::vector<fs::path> dirs;
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(captures_dir, ec)) {
    if (entry.is_directory()) {
      dirs.push_back(entry.path());
    }
  }
  std::sort(dirs.begin(), dirs.end());

  for (const auto &dir : dirs) {
    const auto d0_path = dir / "frames_D0.jsonl";
    const auto d1_path = dir / "frames_D1.jsonl";
    if (!fs::exists(d0_path) || !fs::exists(d1_path)) {
      continue;
    }

    const auto requests = load_jsonl(d0_path.string());
    auto replies = load_jsonl(d1_path.string());
    std::sort(replies.begin(), replies.end(), [](const Recorded &a, const Recorded &b) { return a.t < b.t; });

    // Pair by TIME ORDER, never by (seq, cmd): seq wraps and parks at 0, so the
    // same pair recurs constantly and first-match pairing would silently compare
    // a request against an unrelated reply.
    std::vector<bool> used(replies.size(), false);
    int identical = 0, injections = 0, unpaired = 0, mismatched = 0;

    for (const auto &req : requests) {
      size_t found = replies.size();
      for (size_t i = 0; i < replies.size(); ++i) {
        if (!used[i] && replies[i].seq == req.seq && replies[i].cmd == req.cmd && replies[i].t >= req.t) {
          found = i;
          break;
        }
      }
      if (found == replies.size()) {
        ++unpaired;
        continue;
      }
      used[found] = true;
      const auto &real = replies[found];

      ecm::Frame frame;
      if (!ecm::parse_request(req.raw.data(), req.raw.size(), frame)) {
        ++mismatched;  // the capture itself did not validate: our problem, loudly
        continue;
      }

      uint8_t ours[ecm::MAX_REPLY_LEN];
      const size_t len = ecm::build_reply(frame, nullptr, ours, sizeof(ours));
      if (len == 0) {
        ++mismatched;
        continue;
      }

      ++total_compared;

      if (req.cmd == 0xBC && real.raw.size() >= 10) {
        // The module's own injections are command EVENTS; with nothing queued,
        // the idle slot is the correct output. Compare the head either way.
        if (!bytes_equal(ours, 6, real.raw.data(), 6)) {
          ++mismatched;
          continue;
        }
        const bool injected =
            real.raw[6] != 0 || real.raw[7] != 0 || real.raw[8] != 0 || real.raw[9] != 0;
        if (injected) {
          ++injections;
          continue;
        }
      }

      if (bytes_equal(ours, len, real.raw.data(), real.raw.size())) {
        ++identical;
      } else {
        ++mismatched;
      }
    }

    std::printf("  %-42s %5d identical  %3d injections  %3d unpaired  %3d MISMATCH\n", dir.filename().string().c_str(),
                identical, injections, unpaired, mismatched);

    total_identical += identical;
    total_injections += injections;
    total_unpaired += unpaired;
    total_mismatched += mismatched;
  }

  std::printf("\n  compared %d exchanges: %d byte-identical, %d module command injections (correctly NOT reproduced), "
              "%d unpaired, %d MISMATCH\n",
              total_compared, total_identical, total_injections, total_unpaired, total_mismatched);

  check(total_compared > 0, "captures were found and replayed");
  check(total_mismatched == 0, "every replayed reply matches the real module byte-for-byte");
  check(total_identical > 0, "a substantial number of replies were actually compared");
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
  std::printf("=== ecm protocol core: host test ===\n");
  std::printf("    mirrors tools/ecm_responder.py --selftest; the recorded replies\n");
  std::printf("    below came from the real module on the real bus.\n");

  test_crc();
  test_parse_rejects();
  test_reply_identity();
  test_slots();
  test_new_announcements();
  test_command_word_and_type();
  test_seq_reset_tolerated();
  test_stay_silent();
  test_status_decode();

  if (argc > 1) {
    replay_captures(argv[1]);
  } else {
    std::printf("\n(pass a captures directory to also replay every recorded exchange)\n");
  }

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

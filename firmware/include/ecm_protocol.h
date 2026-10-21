// ecm_protocol.h — the IntelliFire ECM <-> Wi-Fi module serial protocol.
//
// PURE C++20. No ESPHome, no Arduino, no board. This file compiles standalone:
//
//     clang++ -std=c++20 -Wall -Wextra -fsyntax-only include/ecm_protocol.h
//
// It exists as a separate, dependency-free unit so the wire format can be
// unit-tested on a Mac against the module's own recorded behaviour before any
// of it runs on hardware connected to a gas appliance.
//
// ---------------------------------------------------------------------------
// WHAT THIS IS PORTED FROM
// ---------------------------------------------------------------------------
// byte-for-byte from `tools/ecm_responder.py` (the validated reference) and the
// findings in `docs/PROTOCOL_DECODED.md` / `docs/COMMAND_MAP.md`.
//
// The rule, verified on 642/642 exchange pairs and replayed against 2,522
// recorded exchanges:
//
//     reply payload = request payload[0:2]   (echoed verbatim)
//                   + a per-command suffix
//     reply type    = request type + 2
//     reply seq     = request seq, copied
//
// The second byte is a TYPE/DIRECTION byte, not a device address: 1 =
// announcement, 2 = request from the ECM, 3 = acknowledgement, 4 = the module's
// reply. This was proven from the ECM's own RX dispatcher, which branches on
// `frame[1] == 4` for the module's replies and on the high byte of the payload
// word, not on a device id. The field is still called `dev` below for wire and
// test compatibility; the arithmetic is unchanged.
//
// ---------------------------------------------------------------------------
// THE COMMAND IS 16 BITS, NOT 8
// ---------------------------------------------------------------------------
// The ECM's RX dispatcher reads `cmd = frame[3]` AND `tag = frame[4]` and
// compares `(tag << 8) | cmd`. Its own producer writes both bytes from a 16-bit
// command word. So a "command byte" alone is an approximation: 0x01F4 and
// 0x00F4 are different commands that share a low byte, and the boot frame is
// really 0x0320. `Frame::cmd_word()` returns the full value and reply matching
// uses it. The low byte is still exposed as `cmd` for the telemetry census.
//
// Two consequences that this header now enforces:
//   * the documented TYPE for a command word must match the frame's type byte
//     (a 0x02C6 time-sync *request* is not the 0x00C6 echo announcement), and
//   * every type-1 frame is answered with the two-byte echo and nothing else.
//     That is the general rule for every type-1 frame observed so far, so a
//     newly seen announcement is answerable without inventing a reply shape.
//
// CRC-16/MODBUS (poly 0xA001 reflected, init 0xFFFF), computed over the reply
// from the 0x4B start byte inclusive, stored little-endian.
//
// ---------------------------------------------------------------------------
// RECEIVE-PATH REQUIREMENTS
// ---------------------------------------------------------------------------
// The ECM can recover from a UART overrun or a desynchronised byte stream by
// resetting its receive path. That reset also zeroes its exchange counter.
// Two consequences are structural here:
//
//   * `seq` may jump back to 0 at any time. Never predict, validate or
//     resynchronise it — copy it into the reply and nothing else.
//   * Never transmit a byte before START. A stray byte is exactly the kind of
//     desynchronised input that tears the link down. This header only ever
//     builds complete frames beginning with START; keep it that way.
//
// A 0xBC poll does NOT reset the ECM's 9-hour safety deadline. Only slot
// commands that set the activity latch do. Polling the appliance is not
// "using" it.
//
// ---------------------------------------------------------------------------
// THE ONE STRUCTURAL FACT
// ---------------------------------------------------------------------------
// The module is a POLLED SLAVE. It never initiates a frame. Its only write
// channel is the 4-byte data slot inside its reply to the 0xBC request. There
// is no "send a command" call in this header, because the protocol has no such
// operation.
//
// ---------------------------------------------------------------------------
// SCOPE AND SAFETY
// ---------------------------------------------------------------------------
// This header parses and builds frames. It holds no state, drives no pin, and
// makes no decision about whether a write is permitted — that is the
// component's job (see components/fireplace_ecm/fireplace_ecm.h).
//
// The ECM owns ignition, flame sensing and the gas valve. This code is a
// control path, never a safety device. It can command; it can never bypass.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ecm {

// --------------------------------------------------------------------------
// Wire constants
// --------------------------------------------------------------------------

/// Frame start byte. Every frame begins with it; nothing else does.
inline constexpr uint8_t START = 0x4B;

/// Shortest frame that can exist: 4 header + 0 payload + 2 CRC.
inline constexpr size_t MIN_FRAME_LEN = 6;

/// Longest frame we will accept. The longest ever observed on this bus is the
/// 31-byte 0xDC status frame; the analysis tool uses the same 80-byte ceiling
/// (an earlier 24-byte limit silently skipped the DC frame).
inline constexpr size_t MAX_FRAME_LEN = 80;

/// Longest reply we can build: 4 header + 2 echo + 4 slot + 2 CRC = 12.
inline constexpr size_t MAX_REPLY_LEN = 12;

/// Frame type/direction — the frame's 2nd byte.
namespace frame_type {
inline constexpr uint8_t ANNOUNCE = 1;  // ECM -> module, carries data
inline constexpr uint8_t REQUEST = 2;   // ECM -> module, byte 5 = bytes wanted
inline constexpr uint8_t ACK = 3;       // module -> ECM, echo only
inline constexpr uint8_t REPLY = 4;     // module -> ECM, echo + requested bytes
}  // namespace frame_type

/// Command bytes (the frame's 4th byte). Prefer the full 16-bit word when
/// deciding whether a frame is answerable; see `is_announcement_word()` / `is_replyable_request_word()`.
namespace cmd {
inline constexpr uint8_t POWER_ON = 0x20;         // low byte of boot word 0x0320
inline constexpr uint8_t FLAME_ANNOUNCE = 0xE4;   // ECM -> module, value 0/1
inline constexpr uint8_t HEIGHT_ANNOUNCE = 0xE1;  // ECM -> module, value 0-4
inline constexpr uint8_t PILOT_ANNOUNCE = 0xDF;   // ECM -> module, value 0/1
// Announcements emitted unconditionally after a slot command. They are NOT
// acks: the ECM sends them even when the handler did nothing, so they must
// never be used to confirm a command. E0/E2/E5 carry the same one-byte value
// the 0xDC status frame reports (see status::CONFIG and status::HEAT_DEMAND);
// E7 and EA carry wider values. The human meanings of E5-EA are unresolved.
inline constexpr uint8_t ANNOUNCE_E0 = 0xE0;  // after slot 0x66; value = status byte 6
inline constexpr uint8_t ANNOUNCE_E2 = 0xE2;  // after slot 0x68; value = status byte 8
inline constexpr uint8_t ANNOUNCE_E5 = 0xE5;  // after slot 0x69; value 0/1, meaning unknown
inline constexpr uint8_t ANNOUNCE_E6 = 0xE6;  // after slot 0x6B
inline constexpr uint8_t ANNOUNCE_E7 = 0xE7;  // after slot 0x6C; 2-byte payload
inline constexpr uint8_t ANNOUNCE_E9 = 0xE9;  // after slot 0x6D
inline constexpr uint8_t ANNOUNCE_EA = 0xEA;  // after slot 0x6E; payload LE16(value) 00 00
// Additional announcement opcodes recognised defensively. Not all have been
// observed on the wire; they are answered on the same echo rule as the others.
inline constexpr uint8_t ANNOUNCE_DE = 0xDE;
inline constexpr uint8_t ANNOUNCE_E3 = 0xE3;
inline constexpr uint8_t ANNOUNCE_EE = 0xEE;
inline constexpr uint8_t ANNOUNCE_EF = 0xEF;
inline constexpr uint8_t ANNOUNCE_F0 = 0xF0;
inline constexpr uint8_t ANNOUNCE_F1 = 0xF1;
inline constexpr uint8_t ANNOUNCE_F2 = 0xF2;
// The 0x01F4-0x0262 family (56 words, 2-byte payload) is emitted by the
// schedule slot ids 0xC8-0xFF. Matching is on the full word, not this byte.
inline constexpr uint8_t ANNOUNCE_SCHEDULE_LO = 0xF4;
inline constexpr uint8_t ANNOUNCE_SCHEDULE_HI = 0x62;
// Type-1 echoes the ECM builds when the module sends it a C6 or D2 frame. We
// never originate either, so these are unreachable today — but a type-2 0xC6
// request must NOT be answered with this shape, which is why type is checked.
inline constexpr uint8_t ANNOUNCE_C6_ECHO = 0xC6;
inline constexpr uint8_t ANNOUNCE_D2_ECHO = 0xD2;
// 0x0323: type 1, one data byte, callers exist in the D0 task and event path.
// Its low byte is ordinary, so it is matched on the full word.
inline constexpr uint8_t ANNOUNCE_0323_LO = 0x23;
inline constexpr uint8_t WRITE_CARRIER = 0xBC;    // carries the module's write slot
inline constexpr uint8_t STATUS = 0xDC;           // the ECM's state report
inline constexpr uint8_t ROUTINE_64 = 0x64;       // purpose UNKNOWN
inline constexpr uint8_t ROUTINE_6E = 0x6E;       // purpose UNKNOWN
}  // namespace cmd

/// The module's write opcodes, as they appear in the 0xBC reply slot.
/// NOTE: these are a DIFFERENT NAMESPACE from the frame command bytes above.
/// The correspondence was learned empirically and there is NO constant offset
/// between the two: flame is 0x64 -> 0xE4 (+0x80) but pilot is 0x65 -> 0xDF and
/// height is 0x67 -> 0xE1 (both +0x7A). With three pairs no rule can be
/// established, so treat them as two separately-learned namespaces and never
/// derive one byte from the other.
///
/// The slot namespace extends beyond the three implemented opcodes. ONLY the
/// three below are implemented here, because only they are both understood and
/// verified on the wire. The others are recognised and deliberately NOT
/// emitted; their effects range from unknown to flame-affecting:
///
///   0x66  writes status byte 6 and sleeps ~1.5 s; human purpose unknown.
///   0x68  writes status byte 8; a NONZERO EDGE RAISES INTERNAL FAULT 6.
///   0x69  stores a truthiness flag; may reach the flame-level path indirectly.
///   0x6A  ignores its value and makes the ECM request 10 bytes over 0xD2.
///   0x6B  0 clears a flag; otherwise an indirect path into flame-level control.
///   0x6C  writes flame-level state; also an indirect flame-level path.
///   0x6D  0 clears a stored value; always delays ~1 s.
///   0x6E  stores a minutes-like value.
///   0x6F  makes the ECM emit a type-2 0xC6 time-sync request.
///
/// The 0x69/0x6B/0x6C path is the important one to leave alone: it can drive
/// the flame level without a flame command. Sending one of these speculatively
/// could change the flame.
///
/// Emitting one of these would be a guess on a gas appliance. The census in
/// ecm_inventory.h treats everything outside the implemented three as
/// noteworthy, precisely so a sighting is recorded instead of guessed at.
namespace op {
inline constexpr uint8_t FLAME = 0x64;
inline constexpr uint8_t PILOT = 0x65;
inline constexpr uint8_t CONFIG_BYTE = 0x66;   // documented, NOT EMITTED
inline constexpr uint8_t HEIGHT = 0x67;
inline constexpr uint8_t HEAT_DEMAND = 0x68;   // documented, NOT EMITTED (fault edge)
inline constexpr uint8_t DIAG = 0x69;          // documented, NOT EMITTED
inline constexpr uint8_t TEXT_REQUEST = 0x6A;  // documented, NOT EMITTED
}  // namespace op

/// Payload byte indices of the 0xDC status frame (0-based). The parser's
/// payload is 25 bytes for the 31-byte frame (4 header + 2 CRC); the first two
/// are the command-high byte and the length 0x17, leaving 23 status data bytes.
/// The ECM sends this unprompted; we never ask for it. Only the indices below
/// are interpreted.
namespace status {
inline constexpr size_t PILOT = 5;   // 0x01 pilot on / 0x00 off   (confirmed)
// The data path for both of these is high confidence; the human labels are
// medium/low confidence. They are exposed for diagnosis only — no control
// decision reads them.
inline constexpr size_t CONFIG = 6;       // also written by slot 0x66 (label unknown)
inline constexpr size_t HEIGHT = 7;  // 0x00-0x04, INTERNAL 0-based (confirmed)
inline constexpr size_t HEAT_DEMAND = 8;  // also written by slots 0x64/0x68 (label medium)
// CORRECTED 2026-10-20: this byte was inherited as "hot". It reports that a
// 32-bit tick deadline is armed; no read site ties it to a thermostat or heat
// call. The "hot" label is NOT supported, so it is exposed as an unnamed
// diagnostic flag, not a heat sensor.
// (Named FLAG_9 rather than HOT in part because ESPHome's helpers.h defines
// `HOT` as a macro; a namespace cannot protect a macro expansion.)
inline constexpr size_t FLAG_9 = 9;  // 32-bit deadline armed      (source high, meaning unknown)
inline constexpr size_t FLAME = 10;  // 0x01 flame on / 0x00 off   (confirmed)
}  // namespace status

// --------------------------------------------------------------------------
// CRC-16/MODBUS — table driven
// --------------------------------------------------------------------------

namespace detail {

struct CrcTable {
  uint16_t entry[256];
  constexpr CrcTable() : entry() {
    for (int i = 0; i < 256; ++i) {
      uint16_t crc = static_cast<uint16_t>(i);
      for (int bit = 0; bit < 8; ++bit) {
        if (crc & 0x0001u) {
          crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001u);
        } else {
          crc = static_cast<uint16_t>(crc >> 1);
        }
      }
      entry[i] = crc;
    }
  }
};

inline constexpr CrcTable CRC_TABLE{};

}  // namespace detail

/// MODBUS CRC-16: polynomial 0xA001 (reflected), init 0xFFFF, no final xor.
/// Equivalent to the bitwise loop in `tools/ecm_analyse.py::crc16_modbus` and
/// to the table-free loop in `tools/ecm_responder.py`.
inline uint16_t crc16_modbus(const uint8_t *data, size_t len) noexcept {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc = static_cast<uint16_t>((crc >> 8) ^ detail::CRC_TABLE.entry[(crc ^ data[i]) & 0xFF]);
  }
  return crc;
}

/// CRC of a complete frame (all bytes including the trailing CRC bytes) equals
/// the CRC stored little-endian in its last two bytes.
inline bool crc_ok(const uint8_t *frame, size_t len) noexcept {
  if (frame == nullptr || len < MIN_FRAME_LEN) {
    return false;
  }
  const uint16_t computed = crc16_modbus(frame, len - 2);
  const uint16_t stored = static_cast<uint16_t>(frame[len - 2] | (static_cast<uint16_t>(frame[len - 1]) << 8));
  return computed == stored;
}

// --------------------------------------------------------------------------
// Parsing
// --------------------------------------------------------------------------

/// A parsed, CRC-valid request. `payload` points INTO the caller's buffer — this
/// struct never owns memory and is only valid while that buffer is unchanged.
struct Frame {
  uint8_t dev{0};   ///< frame byte 1: the TYPE/direction field (historical name)
  uint8_t seq{0};
  uint8_t cmd{0};   ///< frame byte 3: command word LOW byte
  uint8_t cmd_hi{0};  ///< frame byte 4: command word HIGH byte ("tag")
  const uint8_t *payload{nullptr};
  size_t payload_len{0};

  /// The full 16-bit command word the ECM itself compares on.
  constexpr uint16_t cmd_word() const noexcept {
    return static_cast<uint16_t>(cmd | (static_cast<uint16_t>(cmd_hi) << 8));
  }
};

/// Validate and parse one complete ECM request.
///
/// Rejects anything that is not a CRC-valid frame: a wrong start byte, a frame
/// shorter than MIN_FRAME_LEN, a frame longer than MAX_FRAME_LEN, or a CRC
/// mismatch. Returns false and leaves `out` untouched on rejection — a caller
/// that gets false must stay SILENT on the bus, never guess.
inline bool parse_request(const uint8_t *buf, size_t len, Frame &out) noexcept {
  if (buf == nullptr || len < MIN_FRAME_LEN || len > MAX_FRAME_LEN) {
    return false;
  }
  if (buf[0] != START) {
    return false;
  }
  if (!crc_ok(buf, len)) {
    return false;
  }

  Frame parsed;
  parsed.dev = buf[1];
  parsed.seq = buf[2];
  parsed.cmd = buf[3];
  parsed.cmd_hi = buf[4];
  parsed.payload = buf + 4;
  parsed.payload_len = len - 6;  // 4 header + 2 CRC

  out = parsed;
  return true;
}

/// The ECM's power-on frame is command word 0x0320 (low byte 0x20, high byte
/// 0x03). Match the FULL word: the schedule announcement 0x0220 shares the low
/// byte and must not be mistaken for a boot.
inline bool is_boot_frame(const Frame &f) noexcept {
  return f.cmd_word() == 0x0320;
}

// --------------------------------------------------------------------------
// The 16-bit command word and its documented frame type
// --------------------------------------------------------------------------

/// True for a command word the ECM emits as a TYPE-1 announcement, which is
/// always answered with the two-byte echo and nothing else.
///
/// Type matters because several words exist in BOTH directions-of-meaning:
/// 0x02C6 is the ECM's type-1 echo of a module time sync AND its type-2
/// request for one; 0x00D2 likewise. A type-1 frame is answerable; a type-2
/// request is not, because it demands data this firmware does not have (a
/// 4+-byte big-endian epoch for C6, up to 10 bytes for D2). Answering a C6
/// request with the echo shape would make the ECM read a short, bogus epoch
/// into its schedule clock — so the type check is a safety check, not style.
inline constexpr bool is_announcement_word(uint16_t word) noexcept {
  switch (word) {
    case 0x0320:  // boot
    case 0x0064:  // periodic announcement, constant data 0x64
    case 0x00DC:  // status report (and its 2-byte variant)
    case 0x00DF:  // pilot
    case 0x00E1:  // height
    case 0x00E4:  // flame
    case 0x00DE:
    case 0x00E0:
    case 0x00E2:
    case 0x00E3:
    case 0x00E5:
    case 0x00E6:
    case 0x00E7:
    case 0x00E9:
    case 0x00EA:
    case 0x00EE:
    case 0x00EF:
    case 0x00F0:
    case 0x00F1:
    case 0x00F2:
    case 0x02C6:  // type-1 echo of a module time sync
    case 0x00D2:  // type-1 echo of a module text frame
    case 0x0323:  // one data byte, constant 1
      return true;
    default:
      break;
  }
  // The schedule announcements 0x01F4-0x0262: every even word in the range,
  // emitted by slot ids 0xC8-0xFF with a 2-byte value payload.
  return word >= 0x01F4 && word <= 0x0262 && (word & 1u) == 0;
}

/// True for a command word the ECM emits as a TYPE-2 request this firmware can
/// actually answer: 0x02BC wants the 4-byte slot, 0x006E wants one byte.
/// 0x02C6/0x00D2 requests are deliberately absent — see above.
inline constexpr bool is_replyable_request_word(uint16_t word) noexcept {
  return word == 0x02BC || word == 0x006E;
}

/// True when this exact frame is one we can answer with confidence: the word is
/// known, the TYPE is one we answer for that word, and it carries the two
/// payload bytes every reply must echo.
inline bool is_known_frame(const Frame &f) noexcept {
  if (f.payload_len < 2) {
    return false;
  }
  if (f.dev == frame_type::ANNOUNCE) {
    return is_announcement_word(f.cmd_word());
  }
  if (f.dev == frame_type::REQUEST) {
    return is_replyable_request_word(f.cmd_word());
  }
  return false;
}

/// True for every command BYTE we know how to answer. This is a census helper
/// only: 0x20 here means the boot frame's low byte, and it cannot distinguish
/// 0x01F4 from 0x00F4. Reply decisions use `is_announcement_word()` / `is_replyable_request_word()` on the full word.
inline bool is_known_cmd(uint8_t command) noexcept {
  switch (command) {
    case cmd::POWER_ON:
    case cmd::FLAME_ANNOUNCE:
    case cmd::HEIGHT_ANNOUNCE:
    case cmd::PILOT_ANNOUNCE:
    case cmd::ANNOUNCE_E0:
    case cmd::ANNOUNCE_E2:
    case cmd::ANNOUNCE_E5:
    case cmd::ANNOUNCE_E6:
    case cmd::ANNOUNCE_E7:
    case cmd::ANNOUNCE_E9:
    case cmd::ANNOUNCE_EA:
    case cmd::WRITE_CARRIER:
    case cmd::STATUS:
    case cmd::ROUTINE_64:
    case cmd::ROUTINE_6E:
      return true;
    default:
      return false;
  }
}

// --------------------------------------------------------------------------
// The 0xBC data slot — the module's only write channel
// --------------------------------------------------------------------------

/// The 4-byte slot placed in a 0xBC reply. Two little-endian uint16 words:
/// (opcode, value). Idle is `00 00 00 00`; the zero bytes are the high bytes
/// of those words, not flags or reserved fields. Do not "tidy" them away.
struct Slot {
  uint8_t b[4];
};

/// No command pending. This is what the real module sends when idle.
/// Held as a namespace-scope constant (static storage), so taking a pointer to
/// its bytes is safe beyond the current full-expression.
inline constexpr Slot SLOT_IDLE = {{0x00, 0x00, 0x00, 0x00}};

constexpr Slot slot_idle() noexcept { return SLOT_IDLE; }

/// Flame write, opcode 0x64, value 0x01 on / 0x00 off.
constexpr Slot slot_flame(bool on) noexcept {
  return Slot{{op::FLAME, 0x00, static_cast<uint8_t>(on ? 0x01 : 0x00), 0x00}};
}

/// Pilot write, opcode 0x65, value 0x01 on / 0x00 off.
///
/// Captured and verified on both transitions: the ECM answers the write ~0.18 s
/// later with a 0xDF announcement carrying the same value, and status byte 5
/// then reports it. Note that 0xDF is the ECM's OWN command namespace and is not
/// derived from this opcode (see the note on `namespace op` above).
constexpr Slot slot_pilot(bool on) noexcept {
  return Slot{{op::PILOT, 0x00, static_cast<uint8_t>(on ? 0x01 : 0x00), 0x00}};
}

/// Height write, opcode 0x67.
///
/// Takes the height as the APP shows it (1-5) and writes the INTERNAL value
/// (app - 1) that the wire carries. App 1 -> wire 0, app 5 -> wire 4.
/// Out-of-range input is clamped rather than wrapped: sending a wrapped height
/// to a gas appliance is worse than sending the nearest legal one.
constexpr Slot slot_height(uint8_t app_height) noexcept {
  uint8_t internal = 0;
  if (app_height > 5) {
    internal = 4;
  } else if (app_height > 0) {
    internal = static_cast<uint8_t>(app_height - 1);
  }
  return Slot{{op::HEIGHT, 0x00, internal, 0x00}};
}

/// Height write from an already-internal value (0-4), for callers that keep the
/// internal representation (as the reconciler does).
constexpr Slot slot_height_internal(uint8_t internal) noexcept {
  return Slot{{op::HEIGHT, 0x00, static_cast<uint8_t>(internal > 4 ? 4 : internal), 0x00}};
}

// --------------------------------------------------------------------------
// Reply construction
// --------------------------------------------------------------------------

inline constexpr uint8_t SUFFIX_6E[1] = {0x00};

/// Build the module's reply to one CRC-valid request.
///
/// `slot` overrides the 4-byte 0xBC data slot; nullptr means idle. It is
/// ignored for every other command (no other command carries a write).
///
/// Returns the total reply length, or 0 if no reply should be sent. 0 means
/// STAY SILENT, and that is deliberate: for an unknown command, or a request
/// too malformed to answer, guessing on a gas appliance is worse than silence —
/// the ECM parks its counter and resyncs on its own, which is behaviour we have
/// observed it do.
///
/// The reply is byte-identical to `ecm_responder.py::build_reply`.
inline size_t build_reply(const Frame &req, const Slot *slot, uint8_t *out, size_t out_cap) noexcept {
  // A reply echoes the request's first two payload bytes, so a request without
  // them cannot be answered even if its CRC is valid.
  if (req.payload == nullptr || req.payload_len < 2 || out == nullptr) {
    return 0;
  }

  // Match on the full 16-bit command word AND the documented frame type.
  // A word we know with the wrong type is not answerable: 0x02C6 as a type-2
  // request wants a 4-byte big-endian epoch, not the two-byte echo its type-1
  // sibling carries.
  if (!is_known_frame(req)) {
    return 0;
  }

  const uint16_t word = req.cmd_word();
  const uint8_t *suffix = nullptr;
  size_t suffix_len = 0;

  switch (word) {
    case 0x02BC:  // write carrier, type 2
      suffix_len = 4;
      // SLOT_IDLE has static storage duration; a temporary here would dangle.
      suffix = (slot != nullptr) ? slot->b : SLOT_IDLE.b;
      break;
    case 0x006E:  // routine request, one byte wanted back, type 2
      suffix_len = 1;
      suffix = SUFFIX_6E;
      break;
    default:
      // Every other known frame is a type-1 announcement or status frame:
      // echo the first two payload bytes and append nothing. This is the
      // general rule for the type-1 frames observed so far, including the
      // schedule family 0x01F4-0x0262 and the never-yet-seen 0xDE/E3/EE-F2
      // announcements. None of those has been captured, because this firmware never
      // sends the slots that provoke them; they are answered on the strength of
      // the builder rule, and the host test labels them as inferred.
      suffix_len = 0;
      break;
  }

  const size_t total = 6 + suffix_len + 2;  // header + echo + suffix + CRC
  if (out_cap < total) {
    return 0;
  }

  out[0] = START;
  out[1] = static_cast<uint8_t>(req.dev + 2);
  out[2] = req.seq;
  out[3] = req.cmd;
  out[4] = req.payload[0];
  out[5] = req.payload[1];
  for (size_t i = 0; i < suffix_len; ++i) {
    out[6 + i] = suffix[i];
  }

  const uint16_t crc = crc16_modbus(out, 6 + suffix_len);
  out[6 + suffix_len] = static_cast<uint8_t>(crc & 0xFF);
  out[7 + suffix_len] = static_cast<uint8_t>((crc >> 8) & 0xFF);

  return total;
}

/// Convenience overload for the common "idle slot" case.
inline size_t build_reply(const Frame &req, uint8_t *out, size_t out_cap) noexcept {
  return build_reply(req, nullptr, out, out_cap);
}

// --------------------------------------------------------------------------
// Status payload helpers
// --------------------------------------------------------------------------

/// True when `f` is a 0xDC status frame long enough to carry the mapped fields.
inline bool has_status_fields(const Frame &f) noexcept {
  return f.cmd == cmd::STATUS && f.payload != nullptr && f.payload_len > status::FLAME;
}

/// Non-zero means "on" for the mapped boolean fields. A payload shorter than the
/// field index reads as 0 (unknown/off) rather than reading out of bounds.
inline uint8_t status_byte(const Frame &f, size_t index) noexcept {
  if (f.payload == nullptr || index >= f.payload_len) {
    return 0;
  }
  return f.payload[index];
}

}  // namespace ecm

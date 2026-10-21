// ecm_inventory.h — the opcode census.
//
// PURE C++20. No ESPHome, no board, no allocation. Host-tested by
// test/ecm_inventory_test.cpp.
//
// WHAT IT IS FOR
// --------------
// "Track new opcodes so their behaviour can be defined later." An opcode that
// has never been seen before must be RECORDED, not interpreted and not merely
// counted — the previous implementation did `unknown_cmds_++` and returned,
// throwing away the byte, the payload and the length. This keeps enough of each
// key to characterise it later.
//
// ⚠ THE BLIND SPOT THIS CLOSES
// ---------------------------
// is_known_cmd() covers the FRAME command byte. But the write opcodes
// (0x64 flame, 0x65 pilot, 0x67 height) are a SEPARATE NAMESPACE: they ride in
// the payload of the 0xBC reply, at payload byte 2. A genuinely new write
// opcode — 0x66 sits between pilot and height — would NEVER appear as a frame
// cmd byte, so the check that exists to catch new opcodes could not see it.
// observe_slot() censuses that byte separately.
//
// "Known" here means IMPLEMENTED AND SAFE TO EMIT, not "exists in the ECM".
// The slot range extends past the three implemented opcodes, but only
// 0x64/0x65/0x67 are understood well enough to send. Everything outside those
// three — 0x66, 0x68–0x6F, and the weekly-schedule ids 0xC8–0xFF — stays in
// the unknown table on purpose: if one is ever sighted, that is a real finding
// worth recording, not a routine value to be counted and forgotten.
//
// EVERY DIMENSION IS BOUNDED
// --------------------------
// A noisy or hostile bus must not be able to grow this structure. Keys are
// capped with an overflow bucket; distinct lengths, distinct payload hashes and
// per-index value sets are all fixed-size.
//
// When a set saturates, its COUNT FREEZES and the corresponding flag is set.
// That is deliberate: a count that kept rising past the cap would be counting
// repeats it could no longer recognise, and so could exceed the true distinct
// count. A frozen count is a lower bound you can trust; a rising one is neither
// exact nor bounded. `occurrences` is always exact.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ecm_protocol.h"
#include "ecm_telemetry.h"

namespace ecm {
namespace telemetry {

/// Distinct (dir, dev, cmd) keys tracked. Observed keys number about 16
/// (8 per direction); the headroom is for opcodes nobody has seen yet.
inline constexpr size_t MAX_KEYS = 24;

/// Distinct frame lengths tracked per key. A known command arriving at a new
/// length is a real finding — it is how a changed payload shape announces
/// itself — so this is worth catching even for "boring" frames.
inline constexpr size_t LEN_CAP = 4;

/// Distinct payloads tracked per key. `distinct_payloads` stays an exact count;
/// only the set of hashes is capped, and saturating sets a flag.
inline constexpr size_t PAYLOAD_HASH_CAP = 8;

/// Payload indices given a value bitmap. 12 covers every index this project has
/// mapped or suspects: the 0xBC write slot occupies 0..3, and the status fields
/// are PILOT=5, CONFIG=6, HEIGHT=7, HEAT_DEMAND=8, FLAG_9=9, FLAME=10.
///
/// NOTE: the census key is (dir, dev, cmd LOW BYTE), so command words that
/// share a low byte (0x00F4 vs 0x01F4) are merged in one key. That is a
/// deliberate limitation: no such pair has ever been observed, and the raw frame
/// in the ring record still carries the full word. Reply decisions use the full
/// word (ecm_protocol.h `is_announcement_word` / `is_replyable_request_word`) and are
/// not affected.
inline constexpr size_t VALUE_INDEX_CAP = 12;

/// 256 bits — one per possible byte value. Fixed, so it cannot grow.
inline constexpr size_t VALUE_BYTES = 32;

/// First-seen payload kept for the map's evidence field.
inline constexpr size_t EXAMPLE_CAP = 24;

/// Values LISTED per payload index when serializing. The bitmap itself always
/// records all 256 possibilities — this bounds only the output. Without it a
/// single key (the 0xDC status frame varies across most byte values) would
/// serialize to roughly 18 KB, which is far past what one MQTT payload or one
/// stack buffer should carry. When the list is cut, the object says so.
inline constexpr size_t VALUES_PER_INDEX_CAP = 16;

/// Write opcodes tracked separately (dir, dev, op).
inline constexpr size_t MAX_SLOT_OPS = 8;

namespace invflag {
inline constexpr uint8_t LENGTH_VARIED = 1u << 0;
inline constexpr uint8_t PAYLOAD_SET_TRUNCATED = 1u << 1;
inline constexpr uint8_t UNKNOWN_CMD = 1u << 2;
inline constexpr uint8_t HAD_NEW_SLOT_OP = 1u << 3;
/// The LENGTH table saturated. Distinct from PAYLOAD_SET_TRUNCATED: these were
/// once the same bit, which meant a saturated length table reported the PAYLOAD
/// set as truncated — telling a consumer its payload set was incomplete when it
/// was not. Two different tables, two different flags.
inline constexpr uint8_t LENGTH_SET_TRUNCATED = 1u << 4;
}  // namespace invflag

/// What observe() found noteworthy, as a bitmask.
namespace notable {
inline constexpr uint8_t NEW_KEY = 1u << 0;
inline constexpr uint8_t UNEXPECTED_LEN = 1u << 1;
inline constexpr uint8_t NEW_PAYLOAD = 1u << 2;
inline constexpr uint8_t KEY_TABLE_FULL = 1u << 3;
inline constexpr uint8_t NEW_SLOT_OP = 1u << 4;
}  // namespace notable

/// The slot byte values that are IMPLEMENTED. 0x00 is the idle slot — every
/// reply that carries no command uses it, so it is by far the most common value
/// and must never be mistaken for a new opcode.
///
/// This is deliberately narrower than "the ECM has a handler for it". Sending
/// anything outside 0x64/0x65/0x67 without understanding it would be a guess;
/// 0x68 in particular raises internal fault 6 on a nonzero change, and
/// 0xC8–0xFF program the weekly schedule. See the namespace comment in
/// ecm_protocol.h for the documented-but-not-emitted ids.
inline bool is_known_slot_op(uint8_t op) noexcept {
  // `op::` is ecm::op from ecm_protocol.h — a DIFFERENT namespace from cmd::,
  // and deliberately not derivable from it. See that file's comment.
  return op == 0x00 || op == op::FLAME || op == op::PILOT || op == op::HEIGHT;
}

/// Per-key census. ~472 bytes; see the memory note in the header comment.
struct KeyStats {
  uint8_t used{0};
  uint8_t dir{0};
  uint8_t dev{0};
  uint8_t cmd{0};
  uint8_t flags{0};

  uint32_t occurrences{0};
  uint32_t first_ms{0};
  uint32_t last_ms{0};
  uint32_t distinct_payloads{0};

  uint8_t len_vals[LEN_CAP]{};
  uint8_t len_counts[LEN_CAP]{};
  uint8_t len_n{0};

  uint32_t payload_hash[PAYLOAD_HASH_CAP]{};
  uint8_t payload_hash_n{0};

  uint8_t example[EXAMPLE_CAP]{};
  uint8_t example_len{0};

  /// value_bits[i] is a 256-bit set of the byte values seen at payload index i.
  uint8_t value_bits[VALUE_INDEX_CAP][VALUE_BYTES]{};

  const char *channel() const noexcept { return this->dir == DIR_D1 ? "D1" : "D0"; }
  bool has(uint8_t f) const noexcept { return (this->flags & f) != 0; }
};

/// A write opcode seen inside a 0xBC slot. Kept out of KeyStats because it is
/// not a frame command byte and must never be conflated with one.
struct SlotOpStats {
  uint8_t used{0};
  uint8_t dir{0};
  uint8_t dev{0};
  uint8_t op{0};
  uint32_t occurrences{0};
  uint32_t first_ms{0};
  uint32_t last_ms{0};
  // No payload sample here on purpose. The full frame — including this byte —
  // is in the ring record that triggered the census, and the alert carries its
  // hex. Duplicating a clipped copy here would only add a second, worse source
  // of the same thing.
};

class OpcodeInventory {
 public:
  /// Census one frame. Returns a bitmask of `notable::*`.
  ///
  /// Frames that failed CRC are ignored: a corrupt frame has no trustworthy cmd
  /// or payload, and censusing it would invent opcodes that do not exist. That
  /// is the same reasoning that keeps the firmware silent on a bad frame.
  uint8_t observe(const FrameRecord &rec) noexcept {
    if (!rec.has(flag::CRC_OK)) {
      return 0;
    }

    uint8_t out = 0;
    KeyStats *k = this->find_or_create_(rec.dir, rec.dev, rec.cmd);
    if (k == nullptr) {
      this->overflowed_++;
      return notable::KEY_TABLE_FULL;
    }

    const bool is_new = (k->occurrences == 0);
    if (is_new) {
      k->dir = rec.dir;
      k->dev = rec.dev;
      k->cmd = rec.cmd;
      out |= notable::NEW_KEY;
      if (!ecm::is_known_cmd(rec.cmd)) {
        k->flags |= invflag::UNKNOWN_CMD;
      }
    }
    k->used = 1;
    k->occurrences++;
    if (is_new) {
      k->first_ms = rec.t_ms;
    }
    k->last_ms = rec.t_ms;

    if (this->note_length_(k, rec.raw_len, is_new)) {
      out |= notable::UNEXPECTED_LEN;
    }
    if (this->note_payload_(k, rec)) {
      out |= notable::NEW_PAYLOAD;
    }

    // The 0xBC reply carries the write opcode at payload byte 2. Census it in
    // the separate namespace — see the header comment for why this cannot be
    // done with the frame cmd check.
    if (rec.cmd == cmd::WRITE_CARRIER && rec.payload_len() >= 3) {
      if (this->observe_slot_(rec.dir, rec.dev, rec.payload()[2], rec.t_ms)) {
        out |= notable::NEW_SLOT_OP;
        k->flags |= invflag::HAD_NEW_SLOT_OP;
      }
    }
    return out;
  }

  /// Census a write opcode seen at payload[2] of a 0xBC frame. Returns true the
  /// first time this (dir, dev, op) is seen with an op outside the known set.
  bool observe_slot_(uint8_t dir, uint8_t dev, uint8_t op, uint32_t t_ms) noexcept {
    // KNOWN slot opcodes are counted in aggregate only, and deliberately never
    // take a table slot.
    //
    // This is not an optimisation. Idle (0x00) rides in the slot of every
    // command-bearing reply, so a handful of devices would fill the whole table
    // with idles — and the genuinely new opcode this table exists to catch
    // would land in the overflow counter instead, i.e. never be recorded at
    // all. That is precisely the blind spot the table was added to close, so
    // the table is reserved for opcodes that are NOT known. Their counts remain
    // derivable from the frame records.
    if (is_known_slot_op(op)) {
      this->known_slot_seen_++;
      return false;
    }

    bool first_sighting = false;
    SlotOpStats *s = nullptr;
    for (size_t i = 0; i < MAX_SLOT_OPS; ++i) {
      SlotOpStats &c = this->slots_[i];
      if (c.used && c.dir == dir && c.dev == dev && c.op == op) {
        s = &c;
        break;
      }
      if (!c.used && s == nullptr) {
        s = &c;  // remember the first free slot, keep scanning for a match
      }
    }
    if (s == nullptr) {
      this->slot_overflowed_++;
      return false;
    }
    if (!s->used) {
      s->used = 1;
      s->dir = dir;
      s->dev = dev;
      s->op = op;
      s->first_ms = t_ms;
      first_sighting = true;
    }
    s->occurrences++;
    s->last_ms = t_ms;
    return first_sighting;
  }

  /// How many known slot opcodes were seen (idle, flame, pilot, height), which
  /// are counted rather than tabulated. See observe_slot_.
  uint32_t known_slot_seen() const noexcept { return this->known_slot_seen_; }

  size_t key_count() const noexcept {
    size_t n = 0;
    for (const KeyStats &k : this->keys_) {
      n += k.used ? 1 : 0;
    }
    return n;
  }
  size_t slot_op_count() const noexcept {
    size_t n = 0;
    for (const SlotOpStats &s : this->slots_) {
      n += s.used ? 1 : 0;
    }
    return n;
  }
  uint32_t overflowed() const noexcept { return this->overflowed_; }
  uint32_t slot_overflowed() const noexcept { return this->slot_overflowed_; }

  const KeyStats &key(size_t i) const noexcept { return this->keys_[i]; }
  const SlotOpStats &slot_op(size_t i) const noexcept { return this->slots_[i]; }

  /// Look up an existing key without creating one. Returns false if absent.
  bool lookup(uint8_t dir, uint8_t dev, uint8_t cmd, KeyStats *out) const noexcept {
    for (const KeyStats &k : this->keys_) {
      if (k.used && k.dir == dir && k.dev == dev && k.cmd == cmd) {
        if (out != nullptr) {
          *out = k;
        }
        return true;
      }
    }
    return false;
  }

  void clear() noexcept {
    for (KeyStats &k : this->keys_) {
      k = KeyStats{};
    }
    for (SlotOpStats &s : this->slots_) {
      s = SlotOpStats{};
    }
    this->overflowed_ = 0;
    this->slot_overflowed_ = 0;
    this->known_slot_seen_ = 0;
  }

 private:
  static uint32_t fnv1a_(const uint8_t *p, size_t n) noexcept {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) {
      h ^= p[i];
      h *= 16777619u;
    }
    return h;
  }

  static void set_bit_(uint8_t *bits, uint8_t value) noexcept {
    bits[value >> 3] |= static_cast<uint8_t>(1u << (value & 7u));
  }
  static bool test_bit_(const uint8_t *bits, uint8_t value) noexcept {
    return (bits[value >> 3] & (1u << (value & 7u))) != 0;
  }

  KeyStats *find_or_create_(uint8_t dir, uint8_t dev, uint8_t cmd) noexcept {
    KeyStats *free_slot = nullptr;
    for (KeyStats &k : this->keys_) {
      if (k.used && k.dir == dir && k.dev == dev && k.cmd == cmd) {
        return &k;
      }
      if (!k.used && free_slot == nullptr) {
        free_slot = &k;
      }
    }
    return free_slot;  // nullptr when the table is full — counted, never grown
  }

  /// Returns true if this length had not been seen before ON AN ALREADY-KNOWN
  /// key. The first sighting of a key establishes its length, which is not a
  /// finding; a later second length is.
  bool note_length_(KeyStats *k, uint8_t len, bool is_new) noexcept {
    for (uint8_t i = 0; i < k->len_n; ++i) {
      if (k->len_vals[i] == len) {
        if (k->len_counts[i] < 255) {
          k->len_counts[i]++;
        }
        return false;
      }
    }
    if (k->len_n < LEN_CAP) {
      k->len_vals[k->len_n] = len;
      k->len_counts[k->len_n] = 1;
      k->len_n++;
    } else {
      // The length table is full and this is yet another distinct length.
      // LENGTH_SET_TRUNCATED, not PAYLOAD_SET_TRUNCATED — see the flag comment.
      k->flags |= invflag::LENGTH_VARIED | invflag::LENGTH_SET_TRUNCATED;
      return !is_new;
    }
    if (k->len_n > 1) {
      k->flags |= invflag::LENGTH_VARIED;
    }
    return !is_new;
  }

  /// Returns true when this payload is distinct from every one seen so far.
  bool note_payload_(KeyStats *k, const FrameRecord &rec) noexcept {
    const uint8_t *p = rec.payload();
    const size_t plen = rec.payload_len();
    const uint32_t h = fnv1a_(p, plen);

    if (k->occurrences == 1) {  // first ever sighting: keep it whole
      const size_t n = plen < EXAMPLE_CAP ? plen : EXAMPLE_CAP;
      for (size_t i = 0; i < n; ++i) {
        k->example[i] = p[i];
      }
      k->example_len = static_cast<uint8_t>(n);
    }

    // Per-index value sets, whether or not this payload is distinct — the
    // bitmap is what answers "has byte 7 ever been anything but 4?".
    const size_t limit = plen < VALUE_INDEX_CAP ? plen : VALUE_INDEX_CAP;
    for (size_t i = 0; i < limit; ++i) {
      set_bit_(k->value_bits[i], p[i]);
    }

    for (uint8_t i = 0; i < k->payload_hash_n; ++i) {
      if (k->payload_hash[i] == h) {
        return false;
      }
    }
    // The count moves ONLY when the hash is stored, so
    // distinct_payloads == payload_hash_n holds unconditionally.
    //
    // This was once an unconditional increment, and that was wrong: a payload
    // first seen AFTER the set saturated was never stored, so its later repeats
    // were unrecognisable and got counted again. The total could exceed the true
    // distinct count — not merely imprecise but not even a lower bound, in a
    // field the map schema documents as "how many distinct payloads were seen".
    // Past the cap the count now freezes and PAYLOAD_SET_TRUNCATED says so.
    if (k->payload_hash_n < PAYLOAD_HASH_CAP) {
      k->payload_hash[k->payload_hash_n++] = h;
      k->distinct_payloads++;
    } else {
      k->flags |= invflag::PAYLOAD_SET_TRUNCATED;
    }
    return true;
  }

  KeyStats keys_[MAX_KEYS]{};
  SlotOpStats slots_[MAX_SLOT_OPS]{};
  uint32_t overflowed_{0};
  uint32_t slot_overflowed_{0};
  uint32_t known_slot_seen_{0};
};

// ---------------------------------------------------------------------------
// Serialization — the `<prefix>/inventory` topic
// ---------------------------------------------------------------------------

namespace detail {
/// Bounded writer shared by both serializers here.
struct InvOut {
  char *buf;
  size_t cap;
  size_t n{0};
  bool ok() const noexcept { return this->buf != nullptr && this->n <= this->cap; }
  size_t room() const noexcept { return this->n <= this->cap ? this->cap - this->n : 0; }
  void str(const char *s) noexcept {
    const size_t l = std::strlen(s);
    if (this->room() < l) {
      this->n = this->cap + 1;
      return;
    }
    std::memcpy(this->buf + this->n, s, l);
    this->n += l;
  }
  void ch(char c) noexcept {
    if (this->room() < 1) {
      this->n = this->cap + 1;
      return;
    }
    this->buf[this->n++] = c;
  }
  template <typename... Args>
  void fmt(const char *f, Args... args) noexcept {
    char tmp[64];
    const int k = std::snprintf(tmp, sizeof(tmp), f, args...);
    if (k < 0 || this->room() < static_cast<size_t>(k)) {
      this->n = this->cap + 1;
      return;
    }
    std::memcpy(this->buf + this->n, tmp, static_cast<size_t>(k));
    this->n += static_cast<size_t>(k);
  }
};
}  // namespace detail

/// One census entry as a JSON object, shaped for tools/ecm_map_schema.json's
/// `commands` block so the live census can feed the map pipeline directly.
inline bool key_to_json(const KeyStats &k, char *buf, size_t cap) noexcept {
  if (buf == nullptr || cap < 2) {
    return false;
  }
  detail::InvOut o{buf, cap - 1};
  const bool unknown = k.has(invflag::UNKNOWN_CMD);

  o.str("{\"key\": \"");
  o.str(k.channel());
  o.fmt("/0x%02X/0x%02X\"", k.dev, k.cmd);
  o.str(", \"dir\": \"");
  o.str(k.channel());
  o.str("\", \"dev\": ");
  o.fmt("\"0x%02X\"", k.dev);
  o.str(", \"cmd\": ");
  o.fmt("\"0x%02X\"", k.cmd);
  o.fmt(", \"occurrences\": %u", k.occurrences);
  o.str(", \"frame_len\": ");
  if (k.len_n > 0) {
    o.fmt("%u", k.len_vals[0]);
  } else {
    o.str("null");
  }
  o.str(", \"lengths_seen\": [");
  for (uint8_t i = 0; i < k.len_n; ++i) {
    if (i) o.ch(',');
    o.fmt("%u", k.len_vals[i]);
  }
  o.fmt("], \"lengths_varied\": %s", k.has(invflag::LENGTH_VARIED) ? "true" : "false");
  o.fmt(", \"distinct_payloads\": %u", k.distinct_payloads);
  o.fmt(", \"payload_set_truncated\": %s",
        k.has(invflag::PAYLOAD_SET_TRUNCATED) ? "true" : "false");

  // `role` follows the schema's closed enum. An unrecognised command byte is
  // labelled unknown and nothing more — the whole point is not to guess.
  o.str(", \"role\": \"");
  o.str(unknown ? "unknown" : "observed");
  o.str("\", \"new\": false");

  o.str(", \"values_observed\": {");
  bool first_index = true;
  bool any_truncated = false;
  for (size_t i = 0; i < VALUE_INDEX_CAP; ++i) {
    bool any = false;
    for (size_t b = 0; b < VALUE_BYTES && !any; ++b) {
      any = k.value_bits[i][b] != 0;
    }
    if (!any) {
      continue;
    }
    if (!first_index) {
      o.ch(',');
    }
    first_index = false;
    o.fmt("\"%zu\": [", i);
    bool first_val = true;
    size_t emitted = 0;
    for (unsigned v = 0; v < 256; ++v) {
      if (k.value_bits[i][v >> 3] & (1u << (v & 7u))) {
        if (emitted >= VALUES_PER_INDEX_CAP) {
          any_truncated = true;
          break;
        }
        if (!first_val) {
          o.ch(',');
        }
        first_val = false;
        o.fmt("\"0x%02X\"", v);
        emitted++;
      }
    }
    o.ch(']');
  }
  // Additive to the schema. A consumer that sees this must not treat the list
  // as the complete set — the true count is on the bitmap, not here.
  o.fmt("}, \"values_truncated\": %s", any_truncated ? "true" : "false");

  o.fmt(", \"first_ms\": %u, \"last_ms\": %u}", k.first_ms, k.last_ms);
  if (!o.ok()) {
    return false;
  }
  buf[o.n] = '\0';
  return true;
}

/// The whole census as a JSON array. Written into `buf`; returns false if the
/// buffer was too small (the caller should then dump keys individually rather
/// than emit a partial array that looks complete).
inline bool inventory_to_json(const OpcodeInventory &inv, char *buf, size_t cap) noexcept {
  if (buf == nullptr || cap < 2) {
    return false;
  }
  detail::InvOut o{buf, cap - 1};
  o.ch('[');
  bool first = true;
  for (size_t i = 0; i < MAX_KEYS; ++i) {
    const KeyStats &k = inv.key(i);
    if (!k.used) {
      continue;
    }
    if (!first) {
      o.ch(',');
    }
    first = false;
    // Serialize straight into the output at the current write position. A
    // fixed scratch buffer here was a latent bug: a key with several varying
    // payload indices serializes to well over 1 KB once value lists are
    // included, so a too-small scratch made the WHOLE dump fail at any caller
    // buffer size, no matter how large. Writing in place removes the arbitrary
    // limit and makes the caller's capacity the only one.
    if (!key_to_json(k, o.buf + o.n, o.room() + 1)) {
      return false;
    }
    o.n += std::strlen(o.buf + o.n);
  }
  // Slot opcodes are a separate namespace and are emitted as their own objects
  // so they can never be mistaken for frame commands.
  for (size_t i = 0; i < MAX_SLOT_OPS; ++i) {
    const SlotOpStats &s = inv.slot_op(i);
    if (!s.used) {
      continue;
    }
    if (!first) {
      o.ch(',');
    }
    first = false;
    o.str("{\"key\": \"slot/");
    o.str(s.dir == DIR_D1 ? "D1" : "D0");
    o.fmt("/0x%02X/0x%02X\", \"namespace\": \"slot_op\", \"occurrences\": %u",
          s.dev, s.op, s.occurrences);
    o.fmt(", \"known\": %s", is_known_slot_op(s.op) ? "true" : "false");
    o.fmt(", \"first_ms\": %u, \"last_ms\": %u}", s.first_ms, s.last_ms);
  }
  o.ch(']');
  if (!o.ok()) {
    return false;
  }
  buf[o.n] = '\0';
  return true;
}

}  // namespace telemetry
}  // namespace ecm

// ecm_telemetry.h — the bus flight recorder: a fixed-size record per frame, a
// ring that keeps the most recent ones, and a serializer that emits exactly the
// JSON shape the project's existing tooling already understands.
//
// PURE C++20. No ESPHome, no Arduino, no board, no clock, no allocation. This
// file compiles standalone and is exercised by test/ecm_telemetry_test.cpp.
//
// WHY THIS EXISTS
// ---------------
// The component previously kept five lifetime counters and nothing else. When a
// command reported "sent" and never reached the appliance, there was no way to
// ask what had actually gone on the wire — answering it required clipping a
// logic analyser to the board. Unknown opcodes were counted and their bytes
// discarded. This header is the record that makes those questions answerable.
//
// THE JSON CONTRACT IS LOAD-BEARING
// ---------------------------------
// FrameJson emits the same key set, in the same order, as
// tools/ecm_frames.py's Frame.to_json(). That is not cosmetic: the collector
// writes these lines straight into frames_D0.jsonl / frames_D1.jsonl, and
// tools/ecm_diff.py parses them. A mismatch would round-trip silently while
// mis-keying the analysis. In particular `dev` and `cmd` MUST be "0x%02X"
// STRINGS, not integers — the Python re-spells them and ecm_diff tolerates both
// forms, so getting this wrong would pass a naive round-trip test and still
// corrupt the map.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ecm_protocol.h"

namespace ecm {
namespace telemetry {

// ---------------------------------------------------------------------------
// Record
// ---------------------------------------------------------------------------

/// Longest frame an accepted request can be. Deliberately the parser's ceiling
/// rather than the longest frame ever *observed* (the 0xDC status frame, ~31
/// bytes): this whole mechanism exists to capture frames nobody has seen yet,
/// so truncating them would defeat its purpose. Costs ~5 KB over a 40-byte
/// field at the default ring size, which the ESP32-C6 can afford.
inline constexpr size_t RAW_CAP = MAX_FRAME_LEN;  // 80

/// Direction, matching the analyser's channel naming. These strings are emitted
/// verbatim into the JSON `channel` field and into the map key, so they are not
/// free to change.
inline constexpr uint8_t DIR_D0 = 0;  // ECM -> device
inline constexpr uint8_t DIR_D1 = 1;  // device -> ECM

/// Sentinel for "this RX frame was never answered".
inline constexpr uint16_t NO_LATENCY = 0xFFFF;

/// Sentinel for "no previous frame on this channel" (JSON `gap_before: null`).
inline constexpr uint32_t NO_GAP = 0xFFFFFFFFu;

/// Flag bits. Kept in one byte so the record stays compact.
namespace flag {
inline constexpr uint8_t CRC_OK = 1u << 0;         // parsed and CRC-valid
inline constexpr uint8_t KNOWN_CMD = 1u << 1;      // cmd byte is in is_known_cmd()
inline constexpr uint8_t REPLIED = 1u << 2;        // we sent a reply to this frame
inline constexpr uint8_t TRUNCATED = 1u << 3;      // frame longer than RAW_CAP
inline constexpr uint8_t UNKNOWN_CMD = 1u << 4;    // CRC-valid but unrecognised
inline constexpr uint8_t UNEXPECTED_LEN = 1u << 5; // new length for a known key
inline constexpr uint8_t NEW_KEY = 1u << 6;        // first sighting of (dir,dev,cmd)
}  // namespace flag

/// One bus event. Fixed size, trivial to copy, no indirection — pushing one is
/// a single struct assignment and the ring never allocates.
///
/// NOTE the deliberate absence of a `std::string` or any owning member: this
/// struct is created inside handle_frame_(), which runs on the reply's critical
/// path. Nothing here may allocate.
struct FrameRecord {
  uint32_t t_ms{0};      ///< millis() at the last byte in (RX) or at write (TX)
  uint32_t gap_ms{NO_GAP};  ///< since the previous frame ON THIS CHANNEL
  uint32_t index{0};     ///< this channel's frame sequence number, per boot
  uint16_t crc{0};       ///< parsed (RX) or computed (TX); 0 when not valid
  uint16_t latency_ms{NO_LATENCY};  ///< RX only: last byte -> write_array
  uint8_t raw_len{0};
  uint8_t dir{DIR_D0};
  uint8_t dev{0};
  uint8_t seq{0};
  uint8_t cmd{0};
  uint8_t flags{0};
  uint8_t raw[RAW_CAP]{};

  bool has(uint8_t f) const noexcept { return (this->flags & f) != 0; }
  const char *channel() const noexcept { return this->dir == DIR_D1 ? "D1" : "D0"; }

  /// Payload length as the parser defines it: everything between the 4-byte
  /// header and the 2-byte CRC. Zero when the frame is too short to have one.
  ///
  /// Bounded by what is ACTUALLY STORED, not by the original length. On a
  /// clipped frame (raw_len > RAW_CAP) the bytes past the cap were never copied,
  /// so trusting raw_len here would walk the payload into whatever the array
  /// happens to hold past its stored extent — in bounds of the array, but
  /// describing data that does not exist.
  size_t stored_len() const noexcept {
    return this->raw_len <= RAW_CAP ? static_cast<size_t>(this->raw_len) : RAW_CAP;
  }
  size_t payload_len() const noexcept {
    const size_t stored = this->stored_len();
    return stored > 6 ? stored - 6 : 0;
  }
  const uint8_t *payload() const noexcept { return this->raw + 4; }
};

// ---------------------------------------------------------------------------
// Ring
// ---------------------------------------------------------------------------

/// A fixed-capacity ring of FrameRecords. Wrap overwrites the oldest and counts
/// the loss, so "we dropped records" is visible rather than silent.
///
/// This is a RECENT-HISTORY recorder, not an archive: it lives in RAM and is
/// lost on reboot. Durability is the collector's job (it writes frames to disk).
/// Do not let this be over-trusted — say "the last N seconds", never "the log".
template <size_t N>
class TelemetryRing {
 public:
  static_assert(N > 0, "ring must hold at least one record");

  /// Record `rec`, filling in this channel's sequence index and the inter-frame
  /// gap from the ring's own per-direction bookkeeping (the caller does not have
  /// to track either). Returns the stored record by value for the caller's use.
  FrameRecord push(FrameRecord rec) noexcept {
    const uint8_t d = rec.dir & 1u;
    rec.index = this->next_index_[d];
    rec.gap_ms = this->have_prev_[d]
                     ? (rec.t_ms - this->last_t_ms_[d])  // wrap-safe unsigned delta
                     : NO_GAP;
    this->next_index_[d] = this->next_index_[d] + 1;
    this->last_t_ms_[d] = rec.t_ms;
    this->have_prev_[d] = true;

    this->buf_[this->head_] = rec;
    this->head_ = (this->head_ + 1) % N;
    if (this->count_ == N) {
      this->evicted_++;
    } else {
      this->count_++;
    }
    return rec;
  }

  /// Oldest-first access. `i` must be < count().
  const FrameRecord &at(size_t i) const noexcept {
    return this->buf_[(this->head_ + N - this->count_ + i) % N];
  }

  size_t count() const noexcept { return this->count_; }
  size_t capacity() const noexcept { return N; }
  uint32_t evicted() const noexcept { return this->evicted_; }
  bool full() const noexcept { return this->count_ == N; }

  void clear() noexcept {
    this->head_ = 0;
    this->count_ = 0;
    this->evicted_ = 0;
    this->have_prev_[0] = this->have_prev_[1] = false;
    this->last_t_ms_[0] = this->last_t_ms_[1] = 0;
    this->next_index_[0] = this->next_index_[1] = 0;
  }

 private:
  FrameRecord buf_[N]{};
  size_t head_{0};
  size_t count_{0};
  uint32_t evicted_{0};
  bool have_prev_[2]{false, false};
  uint32_t last_t_ms_[2]{0, 0};
  uint32_t next_index_[2]{0, 0};
};

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

namespace detail {

/// Bounded append. Every write goes through here so the serializer can never
/// overrun the caller's buffer — it stops and reports what it managed instead,
/// and the caller's `truncated` flag (or the short return) says so.
struct Out {
  char *buf;
  size_t cap;
  size_t n{0};

  bool ok() const noexcept { return this->buf != nullptr && this->n <= this->cap; }
  size_t room() const noexcept { return this->n <= this->cap ? this->cap - this->n : 0; }

  void raw_bytes(const char *s, size_t len) noexcept {
    if (this->room() < len) {
      this->n = this->cap + 1;  // poisoned: signal overflow, write nothing more
      return;
    }
    std::memcpy(this->buf + this->n, s, len);
    this->n += len;
  }
  void str(const char *s) noexcept { this->raw_bytes(s, std::strlen(s)); }

  void ch(char c) noexcept { this->raw_bytes(&c, 1); }

  /// printf-style, bounded. `tmp` keeps the formatted text off the stack frame
  /// of every call site.
  template <typename... Args>
  void fmt(const char *f, Args... args) noexcept {
    char tmp[64];
    const int k = std::snprintf(tmp, sizeof(tmp), f, args...);
    if (k < 0) {
      this->n = this->cap + 1;
      return;
    }
    this->raw_bytes(tmp, static_cast<size_t>(k));
  }
};

}  // namespace detail

/// Serialize one record into `buf` as a single JSON line (no trailing newline),
/// matching tools/ecm_frames.py Frame.to_json() key-for-key. Returns the number
/// of bytes written, or 0 if the record is unusable or the buffer too small.
///
/// `gap_before` is emitted as null when the record has no predecessor, and
/// `latency_ms` is additive (the Python has no such key; extra keys are
/// ignored by ecm_diff, which is why adding it is safe).
inline size_t to_json(const FrameRecord &r, char *buf, size_t cap) noexcept {
  if (buf == nullptr || cap < 2) {
    return 0;
  }
  detail::Out o{buf, cap - 1};  // reserve a byte for the NUL
  const size_t n = r.raw_len <= RAW_CAP ? r.raw_len : RAW_CAP;

  o.str("{\"channel\": \"");
  o.str(r.channel());
  o.str("\", \"t\": ");
  o.fmt("%.6f", static_cast<double>(r.t_ms) / 1000.0);
  o.str(", \"dev\": ");
  o.fmt("\"0x%02X\"", r.dev);
  o.str(", \"seq\": ");
  o.fmt("%u", static_cast<unsigned>(r.seq));
  o.str(", \"cmd\": ");
  o.fmt("\"0x%02X\"", r.cmd);

  // payload = everything between the 4-byte header and the 2-byte CRC
  o.str(", \"payload\": [");
  {
    const size_t plen = r.payload_len();
    for (size_t i = 0; i < plen && (4 + i) < n; ++i) {
      if (i) o.ch(',');
      o.fmt("%u", static_cast<unsigned>(r.raw[4 + i]));
    }
  }
  o.str("], \"raw\": [");
  for (size_t i = 0; i < n; ++i) {
    if (i) o.ch(',');
    o.fmt("%u", static_cast<unsigned>(r.raw[i]));
  }
  o.str("], \"crc\": ");
  o.fmt("%u", static_cast<unsigned>(r.crc));
  o.str(", \"index\": ");
  o.fmt("%u", static_cast<unsigned>(r.index));
  o.str(", \"gap_before\": ");
  if (r.gap_ms == NO_GAP) {
    o.str("null");
  } else {
    o.fmt("%.6f", static_cast<double>(r.gap_ms) / 1000.0);
  }

  // The device frames by idle gap rather than by a length sweep, so it never
  // has the analyser's ambiguity about where a frame ended. These two are
  // therefore honestly constant here, not defaulted.
  o.str(", \"ambiguous\": false, \"alt_lengths\": []");

  o.str(", \"key\": \"");
  o.str(r.channel());
  o.fmt("/0x%02X/0x%02X\"", r.dev, r.cmd);

  o.str(", \"hex\": \"");
  for (size_t i = 0; i < n; ++i) {
    if (i) o.ch(' ');
    o.fmt("%02X", static_cast<unsigned>(r.raw[i]));
  }
  o.str("\"");

  // Additive: not in Frame.to_json(), ignored by ecm_diff, and the only way to
  // attribute a reply latency to a specific frame.
  if (r.dir == DIR_D0 && r.latency_ms != NO_LATENCY) {
    o.str(", \"latency_ms\": ");
    o.fmt("%u", static_cast<unsigned>(r.latency_ms));
  }

  o.str("}");
  if (!o.ok()) {
    return 0;
  }
  buf[o.n] = '\0';
  return o.n;
}

}  // namespace telemetry
}  // namespace ecm

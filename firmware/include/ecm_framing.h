#pragma once
// Frame delimiting for the ECM's UART — pure C++20, no ESPHome dependencies.
//
// This is the layer BETWEEN the UART and the protocol core. It answers one
// question: "where does a frame end?" Everything else (is it valid? what does
// it mean?) belongs to ecm_protocol.h and the reconciler.
//
// Why an idle gap and not a CRC scan
// ----------------------------------
// The obvious alternative is to scan for a CRC-valid prefix at each candidate
// length. It is rejected here on purpose: a 31-byte frame offers ~26 candidate
// lengths, and a CRC-16 matches a *prefix* by chance roughly 1/65536 per
// candidate — about 0.015% per frame. On a gas appliance, mis-framing means
// answering the WRONG COMMAND. The gap rule cannot mis-frame; it can only be
// late, and we have the budget for that.
//
// The numbers that make it safe (measured, not assumed):
//   * intra-frame byte gap at 9600-8-N-1 = 10 bit-periods = 1.042 ms
//   * inter-frame gap between exchanges  = min 158.6 ms, median 340 ms
//   * GAP_MS = 8  ->  ~8x above a real byte gap, ~20x below the shortest
//                      inter-exchange gap. Wide margins on both sides.
//
// Cost of the rule: a frame is only delivered after the gap elapses, so it
// completes one loop iteration (~16 ms) after the last byte rather than
// immediately. Against the ~160 ms reply budget that is affordable.
//
// READ-ONLY with respect to the bus: this class only assembles bytes it is
// given. It has no transmit path.

#include <cstddef>
#include <cstdint>

namespace ecm {

/// Assembles bytes into whole frames using an idle-gap rule.
///
/// Usage from the component loop:
///   1. drain the UART: for each byte -> push(byte, now_ms)
///   2. call tick(now_ms) once; if it returns true, frame()/length() hold a
///      complete frame to hand to the protocol core
///
/// The frame stays valid until the NEXT push(), which begins a new frame.
/// Copy it if you need to keep it.
class FrameAssembler {
 public:
  /// Idle time that ends a frame. See the header comment for the margins.
  static constexpr uint32_t GAP_MS = 8;

  /// Longest frame we will accumulate before declaring the stream desynced.
  /// The longest real frame is 31 bytes (the 0xDC status frame); 80 leaves
  /// generous headroom while still bounding a garbage stream.
  static constexpr size_t MAX_LEN = 80;

  /// Start byte. Mirrors ecm::START in the protocol core, duplicated here so
  /// this unit stays dependency-free and independently testable.
  static constexpr uint8_t START = 0x4B;

  /// Feed one received byte. `now_ms` is a monotonic millisecond clock.
  /// Returns true if this byte completed a frame — which normally does NOT
  /// happen, because completion is detected by tick().
  bool push(uint8_t byte, uint32_t now_ms) {
    if (this->delivered_) {
      // A frame was handed out by tick() and not yet superseded. The next byte
      // begins a NEW frame. Without this the byte would be appended to the
      // completed frame and two consecutive frames would run together — caught
      // by the host test, which is the only reason it is not in the firmware.
      this->delivered_ = false;
      this->len_ = 0;
    }
    if (this->len_ == 0) {
      if (byte != START) {
        // Idle line noise, or a byte left over from a partial frame. Discard.
        this->resyncs_++;
        return false;
      }
      this->buf_[0] = byte;
      this->len_ = 1;
      this->last_byte_ms_ = now_ms;
      return false;
    }
    if (this->len_ >= MAX_LEN) {
      // We have been fed a continuous stream with no gap — we are not looking
      // at frames. Drop it and resynchronise on this byte (which may itself be
      // a start byte).
      this->overflows_++;
      this->restart_with_(byte, now_ms);
      return false;
    }
    // Append. A START byte inside a payload is DATA, not a new frame — this is
    // the whole reason the gap rule exists, and the reason a naive "scan for
    // 0x4B" parser mis-frames on a payload containing one.
    this->buf_[this->len_++] = byte;
    this->last_byte_ms_ = now_ms;
    return false;
  }

  /// Call once per loop iteration. Returns true when the idle gap has elapsed
  /// and a complete frame is available in frame()/length().
  bool tick(uint32_t now_ms) {
    // A frame is handed out ONCE. Without this guard every loop iteration after
    // the gap re-delivers the same frame until the next byte arrives — roughly
    // 10-20 times per ECM poll at a ~16 ms loop against a 160-340 ms poll gap.
    // Each re-delivery re-counts the frame and re-answers the poll, flooding
    // the bus. `push()` clears the flag when the next byte begins a new frame.
    if (this->delivered_) {
      return false;
    }
    if (this->len_ == 0) {
      return false;
    }
    if (static_cast<uint32_t>(now_ms - this->last_byte_ms_) < GAP_MS) {
      return false;
    }
    this->completed_++;
    this->delivered_ = true;
    return true;
  }

  /// Clear the assembler without counting anything. For run()/setup().
  void reset() {
    this->len_ = 0;
    this->delivered_ = false;
  }

  /// The assembled frame. Valid until the next push().
  const uint8_t *frame() const { return this->buf_; }
  size_t length() const { return this->len_; }

  /// Timestamp of the most recent byte appended to the current frame.
  /// The component measures reply latency from the last byte of the poll, so it
  /// needs this rather than the time tick() fired (which is GAP_MS later).
  uint32_t last_byte_ms() const { return this->last_byte_ms_; }

  /// Bytes discarded before a start byte was seen.
  uint32_t resyncs() const { return this->resyncs_; }
  /// Runs of >MAX_LEN bytes with no gap.
  uint32_t overflows() const { return this->overflows_; }
  /// Frames delivered by tick().
  uint32_t completed() const { return this->completed_; }

 private:
  void restart_with_(uint8_t byte, uint32_t now_ms) {
    if (byte == START) {
      this->buf_[0] = byte;
      this->len_ = 1;
      this->last_byte_ms_ = now_ms;
    } else {
      this->len_ = 0;
    }
  }

  uint8_t buf_[MAX_LEN]{};
  size_t len_{0};
  uint32_t last_byte_ms_{0};
  uint32_t resyncs_{0};
  uint32_t overflows_{0};
  uint32_t completed_{0};
  /// Set by tick() when it hands out a frame; consumed by the next push().
  bool delivered_{false};
};

}  // namespace ecm

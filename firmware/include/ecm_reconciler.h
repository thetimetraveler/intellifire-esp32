// ecm_reconciler.h — the write-decision policy for the 0xBC reply slot.
//
// PURE C++20. No ESPHome, no Arduino, no board, no clock, no pin. It compiles
// and runs on a host:
//
//     clang++ -std=c++20 -Wall -Wextra -Wpedantic -fsyntax-only include/ecm_reconciler.h
//     clang++ -std=c++20 -Wall -Wextra -O1 -o /tmp/ecm_reconciler_test test/ecm_reconciler_test.cpp
//
// Why the clock and the switch are PARAMETERS rather than members: the firmware
// reads the physical write-enable pin immediately before calling
// slot_for_bc_reply(), so the switch state used for the decision is the state at
// the decision — not the state at the top of the loop. That is what makes "a
// switch flipped mid-exchange is honoured" a real guarantee instead of a
// per-loop approximation. Nothing here reads a global, so the whole policy is
// executable and assertable against a synthetic clock with no hardware at all.
//
// ---------------------------------------------------------------------------
// WHAT THIS IS LIFTED FROM
// ---------------------------------------------------------------------------
// The logic that used to live inline in
// `components/fireplace_ecm/fireplace_ecm.cpp::choose_slot_()` and in the
// confirmation half of `handle_status_()`. It was reasoned through and
// compile-checked, never executed. Behaviour is meant to be bit-for-bit
// identical; `test/ecm_reconciler_test.cpp` now pins it, including the
// boundaries a reading of the code alone gets wrong.
//
// ---------------------------------------------------------------------------
// THE POLICY, IN THE ORDER IT IS APPLIED
// ---------------------------------------------------------------------------
//  1. All three desired values start UNKNOWN, so "no writes at boot" is a
//     structural property rather than a policy an OTA could quietly change: with
//     no intent there is nothing to reconcile, so a reboot cannot re-command the
//     appliance. See "why a reboot is silent" in the README.
//  2. The single write slot is awarded in a fixed order: FLAME, then PILOT, then
//     HEIGHT. Flame is decided BEFORE height because flame GATES height's
//     legality; pilot sits between them because it is a plain on/off peer of
//     flame (it needs no precondition) but a flame request must never wait
//     behind it. An outstanding flame request therefore always wins the slot,
//     exactly as it did before the pilot opcode was known.
//  3. Height is only written while the flame is BURNING. The ECM ignores it
//     otherwise, and resets its stored height to maximum on flame-off, so a
//     height write into a cold appliance is a no-op at best.
//  4. Intent that changed while it was in flight is SUPERSEDED: dropped, never
//     queued, because the protocol has exactly one write slot and a queue would
//     be a fiction.
//  5. Retry no sooner than MIN_RETRY_MS. Give up after MAX_ATTEMPTS or
//     COMMAND_TIMEOUT_MS and LATCH A FAULT, so a write that never lands is loud
//     rather than retried forever. See the note on the two give-up conditions
//     below.
//  6. write_enabled == false never emits a non-idle slot, and reports a distinct
//     outcome so an open switch is visible rather than silent.
//  7. A latched fault still honours an OFF write (flame-off or pilot-off).
//     Refusing to turn something off is the wrong direction to fail in.
//     Everything else is blocked — and while the fault is latched ONLY the OFF
//     writes are considered as targets at all, so a standing flame-ON cannot
//     starve a pilot-OFF of the slot.
//  8. Confirmation comes from the ECM's own status frame and nothing else: the
//     protocol has no separate ack, so a write is "confirmed" exactly when the
//     appliance reports having done it.
//
// ---------------------------------------------------------------------------
// WHAT THIS DELIBERATELY DOES NOT DO: enforce "pilot before flame"
// ---------------------------------------------------------------------------
// request_pilot() / op::PILOT expose the pilot WRITE, but this firmware does NOT
// refuse, delay or reorder a flame request on the grounds that the ECM reports
// the pilot off. That is a decision, not an omission:
//
//  1. The ECM owns the ignition interlock. The stock module did not enforce
//     ordering on this link, and a drop-in replacement that did would diverge
//     from stock — which is the one thing a drop-in replacement must not do.
//  2. The Home Assistant automation layer already owns that policy:
//     the reference keypad automation refuses to start the flame while the
//     pilot is off, and speaks a message saying so.
//  3. Duplicating the policy here would give two places to change it, and the two
//     could conflict. A firmware that silently declines a request Home Assistant
//     believes it made is worse than one that makes the request and reports what
//     it saw.
//
// It is NOT silent, though. A flame-ON emitted while the ECM REPORTS the pilot
// off produces OutcomeKind::FlameWithoutPilot, so the condition surfaces in the
// Home Assistant log instead of looking like an ignition that simply did not
// happen. The write itself is never blocked by it. (An UNKNOWN pilot does not
// trigger it: "unknown" is not "reports off", and before the first status frame
// there is nothing truthful to report.)
//
// ---------------------------------------------------------------------------
// SAFETY FRAME
// ---------------------------------------------------------------------------
// The ECM owns ignition, flame sensing and the gas valve. This is a control
// path, never a safety device. It can command; it can never bypass.
#pragma once

#include <cstdint>

#include "ecm_protocol.h"

namespace ecm {

/// Three-valued: an unread state is NOT "off". Collapsing Unknown into Off is
/// how a boot-time write appears from nowhere.
enum class Tri : uint8_t { Unknown, Off, On };

/// What the ECM last reported, as read out of one 0xDC status frame by the
/// caller. The caller passes -1 for the height when the frame carried no usable
/// height byte (the mapped field is 0..4); that leaves the stored value alone,
/// exactly as `if (height <= 4) last_height_ = height;` did.
///
/// `pilot` follows the same convention as `flame`: it is assigned as given, and
/// the default Unknown is what a caller that did not read byte 5 passes. A
/// caller that HAS read the frame must pass Tri::On/Tri::Off, or a pilot request
/// can never be confirmed against it.
struct EcmObservation {
  Tri flame = Tri::Unknown;
  Tri pilot = Tri::Unknown;
  int8_t height_internal = -1;  // -1 unknown, else 0..4 (INTERNAL, not app)
};

/// What the last write decision did. `None` means "nothing happened" and is the
/// initial state; it never overwrites a more informative outcome, which is what
/// keeps the text sensor's last-outcome sticky across idle polls.
///
/// `FlameWithoutPilot` is deliberately its own kind rather than a log line: a
/// flame-ON commanded while the ECM reports the pilot off still goes out (the
/// ECM owns the interlock and Home Assistant owns the policy — see the header),
/// so this is how that condition becomes VISIBLE instead of looking like an
/// ignition that quietly did not happen.
enum class OutcomeKind : uint8_t {
  None,
  Sent,
  Retrying,
  Confirmed,
  RejectedSwitchOpen,
  Faulted,
  Superseded,
  FlameWithoutPilot
};

struct Outcome {
  OutcomeKind kind{OutcomeKind::None};
  uint8_t attempts{0};  // attempts at the moment of the outcome; 0 if none
};

class Reconciler {
 public:
  // ---- Timing budget -----------------------------------------------------
  // The ECM's next poll starts >= ~158 ms after the end of the previous one
  // (measured min 158.6 ms, median 340 ms), and the ECM tolerates seconds of
  // silence before it parks its counter. These are this component's own retry
  // policy, not protocol deadlines.
  static constexpr uint32_t MIN_RETRY_MS = 2000;        // gap between attempts
  static constexpr uint32_t COMMAND_TIMEOUT_MS = 8000;  // give up after this long
  static constexpr uint8_t MAX_ATTEMPTS = 5;            // attempts before fault
  //
  // NOTE ON THE TWO GIVE-UP CONDITIONS, because they do not both do work: the
  // timeout is tested FIRST, and with MIN_RETRY_MS = 2000 / COMMAND_TIMEOUT_MS =
  // 8000 it always fires at the same poll the 5th attempt would have been made.
  // So a command is sent at most COMMAND_TIMEOUT_MS / MIN_RETRY_MS = 4 times and
  // faults at the 8 s mark; `attempts >= MAX_ATTEMPTS` is unreachable with these
  // constants. That is the behaviour being preserved, and the test asserts the
  // 4-send count rather than the nominal 5. Raising MAX_ATTEMPTS above
  // COMMAND_TIMEOUT_MS / MIN_RETRY_MS would make it real.

  // ---- Intent ------------------------------------------------------------
  /// Ask for the flame on or off. Records intent only; nothing is written here.
  /// If a flame write is already in flight its value is now stale, so it is
  /// dropped.
  void request_flame(bool on) noexcept;

  /// Ask for the pilot on or off. Records intent only; nothing is written here.
  /// Same Tri treatment and same drop-if-in-flight rule as request_flame().
  ///
  /// NOTE: this does NOT gate the flame. Requesting the flame on while the ECM
  /// reports the pilot off emits the flame write and reports
  /// OutcomeKind::FlameWithoutPilot — see "WHAT THIS DELIBERATELY DOES NOT DO"
  /// at the top of this file.
  void request_pilot(bool on) noexcept;

  /// Ask for a height as the APP shows it (1-5); stores internal = app - 1.
  /// Out-of-range input CLAMPS rather than wrapping — a wrapped height sent to a
  /// gas appliance is worse than the nearest legal one.
  void request_height_app(int app_height) noexcept;

  /// The requested height as the app shows it (1-5), or -1 if none was
  /// requested. This is INTENT, not observation.
  int height_app() const noexcept;

  /// A status frame arrived. May confirm an in-flight request.
  void observe(const EcmObservation &o) noexcept;

  /// Answer a 0xBC poll. `now_ms` is the caller's clock; `write_enabled` is the
  /// physical switch, read by the caller immediately before this call.
  Slot slot_for_bc_reply(uint32_t now_ms, bool write_enabled) noexcept;

  // ---- Fault -------------------------------------------------------------
  bool fault_latched() const noexcept { return this->fault_latched_; }
  /// Clear the latch AND drop standing intent. A stale request that failed to
  /// land must not be re-sent by the act of clearing its alarm: without this,
  /// pressing "Clear Command Fault" re-arms intent that may be hours old.
  void clear_fault() noexcept {
    this->fault_latched_ = false;
    this->desired_flame_ = Tri::Unknown;
    this->desired_pilot_ = Tri::Unknown;
    this->desired_height_ = -1;
    this->drop_inflight_();
  }

  /// The most recent non-`None` outcome. Sticky across idle polls.
  Outcome last_outcome() const noexcept { return this->last_outcome_; }

  // ---- Read-backs, for diagnostics and tests -----------------------------
  Tri desired_flame() const noexcept { return this->desired_flame_; }
  Tri desired_pilot() const noexcept { return this->desired_pilot_; }
  int desired_height_internal() const noexcept { return this->desired_height_; }
  Tri reported_flame() const noexcept { return this->last_flame_; }
  Tri reported_pilot() const noexcept { return this->last_pilot_; }
  int reported_height_internal() const noexcept { return this->last_height_; }
  bool inflight_active() const noexcept { return this->inflight_.active; }
  uint8_t inflight_attempts() const noexcept { return this->inflight_.attempts; }

 private:
  /// One command awaiting acknowledgement. At most one can exist, because the
  /// protocol has exactly one write slot.
  struct Inflight {
    bool active{false};
    uint8_t cmd{0};  // ecm::op::FLAME, ecm::op::PILOT or ecm::op::HEIGHT
    uint8_t value{0};
    uint8_t attempts{0};
    uint32_t first_sent_ms{0};
    uint32_t last_sent_ms{0};
  };

  void set_outcome_(OutcomeKind kind, uint8_t attempts) noexcept {
    this->last_outcome_ = Outcome{kind, attempts};
  }
  void drop_inflight_() noexcept { this->inflight_ = Inflight{}; }
  /// Drop in-flight intent because it is no longer what is wanted. Reported as
  /// its own outcome: the write is abandoned, not retried.
  void supersede_inflight_() noexcept {
    const bool was_active = this->inflight_.active;
    const uint8_t attempts = this->inflight_.attempts;
    this->drop_inflight_();
    if (was_active) {
      this->set_outcome_(OutcomeKind::Superseded, attempts);
    }
  }
  void confirm_inflight_() noexcept {
    // Confirmation COMPLETES the request. Clearing flame/pilot intent here is
    // what stops the firmware re-asserting a command the appliance has already
    // executed — without it, an appliance-initiated shutdown (its own safety
    // timer, the wall keypad, the RF remote) reads as a failed command and is
    // silently reversed. Height is deliberately left standing: it is a
    // remembered user preference, re-applied when the burner next lights, not a
    // standing command.
    const uint8_t cmd = this->inflight_.cmd;
    const uint8_t attempts = this->inflight_.attempts;
    this->drop_inflight_();
    if (cmd == op::FLAME) {
      this->desired_flame_ = Tri::Unknown;
    } else if (cmd == op::PILOT) {
      this->desired_pilot_ = Tri::Unknown;
    }
    this->set_outcome_(OutcomeKind::Confirmed, attempts);
  }
  void fail_inflight_() noexcept {
    const uint8_t attempts = this->inflight_.attempts;
    this->drop_inflight_();
    this->fault_latched_ = true;
    this->set_outcome_(OutcomeKind::Faulted, attempts);
  }

  // ---- Intent ----
  Tri desired_flame_{Tri::Unknown};
  Tri desired_pilot_{Tri::Unknown};
  int8_t desired_height_{-1};  // internal 0..4, -1 unknown

  // ---- ECM-reported state ----
  Tri last_flame_{Tri::Unknown};
  Tri last_pilot_{Tri::Unknown};
  int8_t last_height_{-1};  // internal 0..4, -1 unknown

  Inflight inflight_{};
  bool fault_latched_{false};
  Outcome last_outcome_{};
};

// ---------------------------------------------------------------------------
// Intent
// ---------------------------------------------------------------------------

inline void Reconciler::request_flame(bool on) noexcept {
  const Tri want = on ? Tri::On : Tri::Off;
  if (this->desired_flame_ == want) {
    return;
  }
  this->desired_flame_ = want;

  // Superseded intent is dropped, not queued. If a flame command is already in
  // flight its value is now stale, so abandon it: the ECM's next status frame
  // decides what still needs doing.
  if (this->inflight_.active && this->inflight_.cmd == op::FLAME) {
    this->supersede_inflight_();
  }
}

inline void Reconciler::request_pilot(bool on) noexcept {
  const Tri want = on ? Tri::On : Tri::Off;
  if (this->desired_pilot_ == want) {
    return;
  }
  this->desired_pilot_ = want;

  // Same rule as flame: an in-flight pilot write whose value is now stale is
  // abandoned, not queued.
  if (this->inflight_.active && this->inflight_.cmd == op::PILOT) {
    this->supersede_inflight_();
  }
}

inline void Reconciler::request_height_app(int app_height) noexcept {
  if (app_height < 1) {
    app_height = 1;
  } else if (app_height > 5) {
    app_height = 5;
  }
  const int8_t internal = static_cast<int8_t>(app_height - 1);
  if (this->desired_height_ == internal) {
    return;
  }
  this->desired_height_ = internal;

  if (this->inflight_.active && this->inflight_.cmd == op::HEIGHT) {
    this->supersede_inflight_();
  }
}

inline int Reconciler::height_app() const noexcept {
  if (this->desired_height_ < 0) {
    return -1;  // nothing requested — unknown, not zero
  }
  return static_cast<int>(this->desired_height_) + 1;  // internal -> app 1-5
}

// ---------------------------------------------------------------------------
// Observation — the ECM's status frame is the only acknowledgement there is
// ---------------------------------------------------------------------------

inline void Reconciler::observe(const EcmObservation &o) noexcept {
  this->last_flame_ = o.flame;
  this->last_pilot_ = o.pilot;
  if (o.height_internal >= 0 && o.height_internal <= 4) {
    this->last_height_ = static_cast<int8_t>(o.height_internal);
  }

  if (!this->inflight_.active) {
    return;
  }

  if (this->inflight_.cmd == op::FLAME) {
    const bool want_on = this->inflight_.value != 0;
    // The `!= Unknown` guard is redundant on the real path — a status frame
    // always carries a definite flame byte — but without it an Unknown state
    // would satisfy a flame-OFF request (false == false) and silently abandon
    // the attempt to put a fire out. Confirmations may only ever be made by
    // positive evidence.
    if (this->last_flame_ != Tri::Unknown && (this->last_flame_ == Tri::On) == want_on) {
      this->confirm_inflight_();
    }
  } else if (this->inflight_.cmd == op::PILOT) {
    // Identical rule, identical reason. A pilot-OFF is never confirmed by an
    // Unknown pilot byte, so an attempt to turn the pilot off is never silently
    // abandoned on the strength of a frame that did not actually say so.
    const bool want_on = this->inflight_.value != 0;
    if (this->last_pilot_ != Tri::Unknown && (this->last_pilot_ == Tri::On) == want_on) {
      this->confirm_inflight_();
    }
  } else if (this->inflight_.cmd == op::HEIGHT) {
    if (this->last_height_ >= 0 && static_cast<uint8_t>(this->last_height_) == this->inflight_.value) {
      this->confirm_inflight_();
    }
  }
}

// ---------------------------------------------------------------------------
// The write decision — the only place one is made
// ---------------------------------------------------------------------------

inline Slot Reconciler::slot_for_bc_reply(uint32_t now_ms, bool write_enabled) noexcept {
  // --- 0. A latched fault stops NEW writes until a human clears it — that is
  //        its whole purpose, so a command that never lands stays loud instead
  //        of retrying forever. The one exception is an OFF write: refusing to
  //        honour an "off" is the wrong direction to fail in, and a bounded
  //        number of off attempts is not the retry storm the latch exists to
  //        prevent.
  //
  //        The latch is applied where the target is CHOSEN (step 1) rather than
  //        as an early return, because with three commands an early "is an off
  //        request standing?" test is no longer equivalent: a standing flame-ON
  //        would return idle before a standing pilot-OFF was ever looked at, and
  //        the pilot-OFF would be starved of the slot it is entitled to.
  const bool faulted = this->fault_latched_;

  // --- 1. What, if anything, does the desired state want written? ---------
  //        Order is the arbitration order: FLAME, then PILOT, then HEIGHT.
  uint8_t target_op = 0;
  uint8_t target_value = 0;
  bool have_target = false;

  if (this->desired_flame_ != Tri::Unknown) {
    const bool want_on = (this->desired_flame_ == Tri::On);
    const bool reported_on = (this->last_flame_ == Tri::On);
    // While faulted, a flame-ON is not a target at all; a flame-OFF still is.
    // A write is only targeted once the ECM has actually reported this field.
    // The component additionally suppresses writes until it has seen a status
    // frame at all (fireplace_ecm.cpp, the WRITE_CARRIER block); this guard is
    // the unit-level equivalent for a reconciler driven directly.
    if ((!faulted || !want_on) && (this->last_flame_ == Tri::Unknown || reported_on != want_on)) {
      target_op = op::FLAME;
      target_value = want_on ? 0x01 : 0x00;
      have_target = true;
    }
  }

  // Pilot is decided NEXT: it is a plain on/off peer of flame with no
  // precondition of its own, but an outstanding flame request must never wait
  // behind it, so flame keeps the slot. Height comes last because it is only
  // legal while the flame is burning.
  if (!have_target && this->desired_pilot_ != Tri::Unknown) {
    const bool want_on = (this->desired_pilot_ == Tri::On);
    const bool reported_on = (this->last_pilot_ == Tri::On);
    if ((!faulted || !want_on) && (this->last_pilot_ == Tri::Unknown || reported_on != want_on)) {
      target_op = op::PILOT;
      target_value = want_on ? 0x01 : 0x00;
      have_target = true;
    }
  }

  if (!have_target && this->desired_height_ >= 0) {
    // Flame GATES height's legality: the ECM ignores a height write while the
    // flame is off, and resets the stored height to maximum on flame-off.
    // Writing height into that is a no-op at best — and a height write is never
    // an OFF write, so a latched fault blocks it entirely.
    if (!faulted && this->last_flame_ == Tri::On && this->last_height_ != this->desired_height_) {
      target_op = op::HEIGHT;
      target_value = static_cast<uint8_t>(this->desired_height_);
      have_target = true;
    }
  }

  // --- 2. Reconcile anything already in flight against that target --------
  if (this->inflight_.active) {
    if (!have_target ||
        !(this->inflight_.cmd == target_op && this->inflight_.value == target_value)) {
      // The intent changed, or the ECM's own status has already satisfied it.
      // Superseded intent is DROPPED, not queued.
      this->supersede_inflight_();
    } else if (now_ms - this->inflight_.first_sent_ms >= COMMAND_TIMEOUT_MS ||
               this->inflight_.attempts >= MAX_ATTEMPTS) {
      this->fail_inflight_();
      return slot_idle();
    } else if (now_ms - this->inflight_.last_sent_ms < MIN_RETRY_MS) {
      return slot_idle();  // sent; giving the ECM its ~0.18 s to answer
    }
    // Otherwise fall through and retry this poll.
  }

  // --- 3. Nothing wanted --------------------------------------------------
  if (!have_target) {
    return slot_idle();
  }

  // --- 4. The physical switch, read by the caller IMMEDIATELY before this
  //        call. An open switch means this device is electrically severed from
  //        the ECM's TX line, and it is re-read every time rather than cached,
  //        so a switch flipped mid-exchange is honoured on this poll.
  //
  //        An OFF write is exempt. In the installed build the sense wire severs
  //        nothing (TX runs through a fixed resistor), so a broken D2 sense wire
  //        would otherwise remove the firmware's last-resort OFF while the link
  //        stayed live — the same wrong direction the fault latch refuses to
  //        fail in. A genuine interlock severs TX anyway, so exempting OFF costs
  //        nothing where the switch is real.
  const bool off_write =
      (target_op == op::FLAME || target_op == op::PILOT) && target_value == 0;
  if (!write_enabled && !off_write) {
    this->set_outcome_(OutcomeKind::RejectedSwitchOpen, this->inflight_.attempts);
    return slot_idle();
  }

  // --- 5. Claim or refresh the in-flight record, then build the slot ------
  if (this->inflight_.active) {
    this->inflight_.attempts++;
    this->inflight_.last_sent_ms = now_ms;
  } else {
    this->inflight_ = Inflight{};
    this->inflight_.active = true;
    this->inflight_.cmd = target_op;
    this->inflight_.value = target_value;
    this->inflight_.attempts = 1;
    this->inflight_.first_sent_ms = now_ms;
    this->inflight_.last_sent_ms = now_ms;
  }

  // A flame-ON aimed at an appliance whose ECM reports the pilot OFF is still
  // written — this firmware does not enforce the ordering (see the header) — but
  // it is reported as its own outcome so the condition is visible in the Home
  // Assistant log instead of looking like an ignition that simply did not
  // happen. Only a reported OFF triggers it: an UNKNOWN pilot is not a report.
  if (target_op == op::FLAME && target_value != 0 && this->last_pilot_ == Tri::Off) {
    this->set_outcome_(OutcomeKind::FlameWithoutPilot, this->inflight_.attempts);
  } else {
    this->set_outcome_(this->inflight_.attempts <= 1 ? OutcomeKind::Sent : OutcomeKind::Retrying,
                       this->inflight_.attempts);
  }

  if (target_op == op::FLAME) {
    return slot_flame(target_value != 0);
  }
  if (target_op == op::PILOT) {
    return slot_pilot(target_value != 0);
  }
  return slot_height_internal(target_value);
}

}  // namespace ecm

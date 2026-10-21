// ecm_reconciler_test.cpp — host test for the write-decision policy.
//
// No ESPHome, no board, no hardware, no sleeping, no millis(). The clock is a
// uint32_t the test owns and advances by hand, so every retry boundary is
// asserted exactly rather than observed approximately on a real timer — which
// is the only way to check a boundary at all.
//
//     clang++ -std=c++20 -Wall -Wextra -O1 -o /tmp/ecm_reconciler_test test/ecm_reconciler_test.cpp
//     /tmp/ecm_reconciler_test
//
// This closes the project's largest confidence gap: the reconciler in
// include/ecm_reconciler.h had been reasoned through and compile-checked but
// never executed. It decides the ONE thing in this firmware that can move a gas
// appliance, so "it looked right" was never good enough.
//
// What this test does NOT cover, and cannot: the UART, the framing, the pin
// polarity, the ECM's real timing, or whether the ECM accepts a write at all.
// Those need the appliance.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../include/ecm_reconciler.h"

// ---------------------------------------------------------------------------
// Tiny test harness (same shape as ecm_host_test.cpp)
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
// Slot helpers — the wire bytes are the only thing that actually matters
// ---------------------------------------------------------------------------

static std::string slot_hex(const ecm::Slot &s) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02X %02X %02X %02X", s.b[0], s.b[1], s.b[2], s.b[3]);
  return buf;
}

static bool is_idle(const ecm::Slot &s) { return s.b[0] == 0 && s.b[1] == 0 && s.b[2] == 0 && s.b[3] == 0; }

static bool slot_is(const ecm::Slot &s, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return s.b[0] == a && s.b[1] == b && s.b[2] == c && s.b[3] == d;
}

/// A status frame as the component would read it out of the 0xDC payload:
/// `height_internal` is -1 when the frame carried no usable height byte.
///
/// `pilot` defaults to Unknown, which is exactly what the component passes
/// before the first status frame and what every pre-pilot test in this file
/// therefore keeps seeing — a defaulted parameter rather than a second helper,
/// so the pilot cannot silently change what the flame/height tests assert.
static ecm::EcmObservation obs(bool flame_on, int height_internal,
                               ecm::Tri pilot = ecm::Tri::Unknown) {
  ecm::EcmObservation o;
  o.flame = flame_on ? ecm::Tri::On : ecm::Tri::Off;
  o.pilot = pilot;
  o.height_internal = static_cast<int8_t>(height_internal);
  return o;
}

/// A reconciler plus the clock and switch it does not own. Nothing is global,
/// so each test starts from a genuinely fresh appliance.
struct Rig {
  ecm::Reconciler rec;
  uint32_t now{0};
  bool write_enabled{true};

  ecm::Slot poll() { return this->rec.slot_for_bc_reply(this->now, this->write_enabled); }
  void advance(uint32_t ms) { this->now += ms; }
  /// Advance to an absolute time, so a test reads as a timeline.
  void at(uint32_t t) { this->now = t; }
};

/// Poll `count` times, MIN_RETRY_MS apart, counting the non-idle slots. Used
/// where a test cares that nothing is EVER written, not just right now.
static int poll_many(Rig &r, int count, uint32_t step_ms = ecm::Reconciler::MIN_RETRY_MS) {
  int writes = 0;
  for (int i = 0; i < count; ++i) {
    if (!is_idle(r.poll())) {
      ++writes;
    }
    r.advance(step_ms);
  }
  return writes;
}

// ---------------------------------------------------------------------------
// 1. No writes at boot — structural, not a policy
// ---------------------------------------------------------------------------

static void test_no_writes_at_boot() {
  std::printf("\n== 1. a fresh reconciler never writes ==\n");

  Rig r;
  const int writes = poll_many(r, 200);
  check(writes == 0, "200 polls over 400 s with no request: every slot is idle",
        "non-idle slots: " + std::to_string(writes));
  check(!r.rec.fault_latched(), "a fresh reconciler has latched no fault");
  check(r.rec.last_outcome().kind == ecm::OutcomeKind::None, "a fresh reconciler reports no outcome at all",
        "kind " + std::to_string(static_cast<int>(r.rec.last_outcome().kind)));
  check(r.rec.height_app() == -1, "height_app() is -1 (unknown), not 1, before anything is requested",
        "got " + std::to_string(r.rec.height_app()));
  check(r.rec.desired_flame() == ecm::Tri::Unknown, "desired flame starts Unknown, so there is nothing to reconcile");

  // The write-enable switch is only consulted when there IS something to write,
  // so an open switch with no intent must not manufacture an outcome either.
  Rig open_rig;
  open_rig.write_enabled = false;
  check(poll_many(open_rig, 10) == 0 && open_rig.rec.last_outcome().kind == ecm::OutcomeKind::None,
        "an OPEN switch with nothing requested is still silent (no outcome, no slot)");

  // And nothing is written with the switch enabled either — the guarantee is not
  // coming from the switch.
  Rig enabled_rig;
  enabled_rig.write_enabled = true;
  check(poll_many(enabled_rig, 10) == 0, "an ENABLED switch with nothing requested writes nothing");
}

// ---------------------------------------------------------------------------
// 2. Flame — and the exact bytes
// ---------------------------------------------------------------------------

static void test_flame_slot() {
  std::printf("\n== 2. a flame request produces exactly 64 00 01 00 ==\n");

  Rig r;
  r.rec.request_flame(true);
  const ecm::Slot s = r.poll();
  check(slot_is(s, 0x64, 0x00, 0x01, 0x00), "flame-on slot is exactly 64 00 01 00", "got " + slot_hex(s));
  check(r.rec.last_outcome().kind == ecm::OutcomeKind::Sent && r.rec.last_outcome().attempts == 1,
        "outcome is Sent, attempts = 1");

  // The slot is only useful if it reaches the wire. Build the real reply to a
  // real recorded 0xBC request and look at bytes 6..9.
  const uint8_t bc_request[8] = {0x4B, 0x02, 0xDE, 0xBC, 0x02, 0x04, 0x8D, 0x0F};
  ecm::Frame frame;
  if (ecm::parse_request(bc_request, sizeof(bc_request), frame)) {
    uint8_t reply[ecm::MAX_REPLY_LEN];
    const size_t len = ecm::build_reply(frame, &s, reply, sizeof(reply));
    const bool wire_ok = len == 12 && reply[6] == 0x64 && reply[7] == 0x00 && reply[8] == 0x01 && reply[9] == 0x00 &&
                         ecm::crc_ok(reply, len);
    check(wire_ok, "the same slot lands in the 0xBC reply as 64 00 01 00 (CRC valid)",
          "reply " + std::to_string(len) + " bytes");
  } else {
    check(false, "the recorded 0xBC vector failed to parse");
  }

  Rig off_rig;
  off_rig.rec.request_flame(false);
  const ecm::Slot off = off_rig.poll();
  check(slot_is(off, 0x64, 0x00, 0x00, 0x00), "flame-off is the same opcode with value 00", "got " + slot_hex(off));
}

// ---------------------------------------------------------------------------
// 3. The height off-by-one, in both directions
// ---------------------------------------------------------------------------

static void test_height_off_by_one() {
  std::printf("\n== 3. height: app 5 -> 0x04, app 1 -> 0x00 (the off-by-one) ==\n");

  // Height is only legal while the flame burns, so each rig starts with the ECM
  // reporting a burning appliance.
  {
    Rig r;
    r.rec.observe(obs(true, 0));  // burning at app height 1
    r.rec.request_height_app(5);
    const ecm::Slot s = r.poll();
    check(slot_is(s, 0x67, 0x00, 0x04, 0x00), "app height 5 -> 67 00 04 00", "got " + slot_hex(s));
  }
  {
    Rig r;
    r.rec.observe(obs(true, 4));  // burning at app height 5
    r.rec.request_height_app(1);
    const ecm::Slot s = r.poll();
    check(slot_is(s, 0x67, 0x00, 0x00, 0x00), "app height 1 -> 67 00 00 00", "got " + slot_hex(s));
  }

  // Every app value, in the same direction, with the reported height unknown so
  // each request is always a genuine change. A wrap or an off-by-one shows up
  // as one wrong byte here.
  bool all_ok = true;
  std::string detail;
  for (int app = 1; app <= 5; ++app) {
    Rig r;
    r.rec.observe(obs(true, -1));  // status frame carried no height byte
    r.rec.request_height_app(app);
    const ecm::Slot s = r.poll();
    if (!slot_is(s, 0x67, 0x00, static_cast<uint8_t>(app - 1), 0x00)) {
      all_ok = false;
      detail += "app " + std::to_string(app) + " -> " + slot_hex(s) + "; ";
    }
  }
  check(all_ok, "app 1..5 -> 67 00 (app - 1) 00 for every value", detail);

  // The conversion in the other direction, on the request side.
  bool round_trip = true;
  detail.clear();
  for (int app = 1; app <= 5; ++app) {
    Rig r;
    r.rec.request_height_app(app);
    if (r.rec.height_app() != app) {
      round_trip = false;
      detail += "requested " + std::to_string(app) + " reads back " + std::to_string(r.rec.height_app()) + "; ";
    }
  }
  check(round_trip, "request_height_app(n) round-trips: height_app() == n", detail);

  // Clamping, not wrapping: a wrapped height sent to a gas appliance is worse
  // than the nearest legal one.
  Rig clamp;
  clamp.rec.request_height_app(99);
  check(clamp.rec.height_app() == 5, "request_height_app(99) clamps to app 5");
  Rig clamp_low;
  clamp_low.rec.request_height_app(0);
  check(clamp_low.rec.height_app() == 1, "request_height_app(0) clamps to app 1");
}

// ---------------------------------------------------------------------------
// 4. Retry timing, at the boundary
// ---------------------------------------------------------------------------

static void test_retry_timing() {
  std::printf("\n== 4. retry gap: not a millisecond sooner than MIN_RETRY_MS ==\n");

  Rig r;
  r.rec.request_flame(true);

  const ecm::Slot first = r.poll();  // t = 0
  check(slot_is(first, 0x64, 0x00, 0x01, 0x00), "t = 0: the first attempt is sent");

  r.at(ecm::Reconciler::MIN_RETRY_MS - 1);
  check(is_idle(r.poll()), "t = MIN_RETRY_MS - 1: idle (1999 ms is too soon)");

  // A poll inside the gap must not disturb the attempt count or the clock.
  check(r.rec.inflight_attempts() == 1, "polling inside the gap does not consume an attempt");

  r.at(ecm::Reconciler::MIN_RETRY_MS);
  const ecm::Slot second = r.poll();
  check(slot_is(second, 0x64, 0x00, 0x01, 0x00), "t = MIN_RETRY_MS exactly: sent again (the boundary is inclusive)",
        "got " + slot_hex(second));
  check(r.rec.last_outcome().kind == ecm::OutcomeKind::Retrying && r.rec.last_outcome().attempts == 2,
        "outcome is Retrying, attempts = 2");

  // The second retry is measured from the SECOND send, not the first.
  r.at(2 * ecm::Reconciler::MIN_RETRY_MS - 1);
  check(is_idle(r.poll()), "t = 2*MIN_RETRY_MS - 1: idle (the gap is measured from the last send)");
  r.at(2 * ecm::Reconciler::MIN_RETRY_MS);
  const ecm::Slot third = r.poll();
  check(slot_is(third, 0x64, 0x00, 0x01, 0x00) && r.rec.inflight_attempts() == 3,
        "t = 2*MIN_RETRY_MS exactly: third attempt");
}

// ---------------------------------------------------------------------------
// 5. Give-up, and the fault latch
// ---------------------------------------------------------------------------

static void test_give_up() {
  std::printf("\n== 5. give-up: the fault latches and further polls are idle ==\n");

  Rig r;
  r.rec.request_flame(true);

  // Poll exactly on the retry cadence: t = 0, 2000, 4000, 6000 are attempts
  // 1..4; t = 8000 hits COMMAND_TIMEOUT_MS and gives up.
  int sends = 0;
  for (uint32_t t = 0; t <= ecm::Reconciler::COMMAND_TIMEOUT_MS; t += ecm::Reconciler::MIN_RETRY_MS) {
    r.at(t);
    if (!is_idle(r.poll())) {
      ++sends;
    }
  }

  // This is the faithful behaviour, and it is worth stating plainly because the
  // two give-up conditions do not both do work: the timeout is tested first and
  // always fires on the poll the 5th attempt would have been made on, so
  // MAX_ATTEMPTS = 5 is unreachable at COMMAND_TIMEOUT_MS / MIN_RETRY_MS = 4.
  check(sends == 4, "4 attempts were sent before the 8 s timeout gave up (MAX_ATTEMPTS=5 is unreachable here)",
        "sends: " + std::to_string(sends));

  const ecm::Outcome o = r.rec.last_outcome();
  check(o.kind == ecm::OutcomeKind::Faulted && o.attempts == 4, "outcome is Faulted with the attempt count",
        "kind " + std::to_string(static_cast<int>(o.kind)) + ", attempts " + std::to_string(o.attempts));
  check(r.rec.fault_latched(), "the fault is latched");
  check(!r.rec.inflight_active(), "the in-flight record is dropped (nothing is retried behind the fault)");

  // Everything after the latch is silence, however long the appliance runs.
  const int after = poll_many(r, 100);
  check(after == 0, "every poll after the latch is idle (100 polls over 200 s)", "non-idle: " + std::to_string(after));

  // The latch is only released by a human. Clearing it ALSO drops the intent
  // that failed, so the same command is not re-sent by the act of clearing.
  r.rec.clear_fault();
  check(!r.rec.fault_latched(), "clear_fault() releases the latch");
  check(r.rec.desired_flame() == ecm::Tri::Unknown,
        "clear_fault() drops the standing intent that failed");
  check(poll_many(r, 5) == 0, "after clearing, nothing is re-sent");
}

// ---------------------------------------------------------------------------
// 6. Supersede
// ---------------------------------------------------------------------------

static void test_supersede() {
  std::printf("\n== 6. supersede: dropped, never queued ==\n");

  // (a) Both requests before any poll: the superseded ON never reaches the bus.
  {
    Rig r;
    r.rec.request_flame(true);
    r.rec.request_flame(false);
    const ecm::Slot s = r.poll();
    check(slot_is(s, 0x64, 0x00, 0x00, 0x00), "6a: flame-on then flame-off before any poll -> the first slot is OFF, never ON",
          "got " + slot_hex(s));
  }

  // (b) The ON is already in flight and then superseded.
  {
    Rig r;
    r.rec.request_flame(true);
    const ecm::Slot sent = r.poll();
    check(slot_is(sent, 0x64, 0x00, 0x01, 0x00), "6b: the ON was sent (attempt 1)");
    check(r.rec.inflight_active() && r.rec.inflight_attempts() == 1, "6b: the ON is in flight");

    r.rec.request_flame(false);
    check(r.rec.last_outcome().kind == ecm::OutcomeKind::Superseded, "6b: superseding the ON is reported as Superseded, not retried");
    check(!r.rec.inflight_active(), "6b: the superseded ON is dropped from flight");

    // The user's last word was OFF, so OFF is what goes out — and the
    // superseded ON is not re-sent alongside it.
    //
    // Dropping the in-flight record also DISCARDS its retry clock: the
    // replacement is a different command, not a retry of the old one, so it is
    // not held back by the dead attempt's 2 s gap. That is asserted here on
    // purpose, because it is the kind of thing a later "optimisation" gets
    // wrong: the ON's cadence must not be inherited by the OFF.
    const uint32_t t_off = ecm::Reconciler::MIN_RETRY_MS - 1;
    r.at(t_off);
    const ecm::Slot s = r.poll();
    check(slot_is(s, 0x64, 0x00, 0x00, 0x00),
          "6b: the slot carried after the supersede is OFF, not the stale ON (sent immediately: the dead attempt's "
          "retry clock is discarded with it)",
          "got " + slot_hex(s));

    // ...and the NEW command's own gap then applies, measured from its send.
    r.at(t_off + 1);
    check(is_idle(r.poll()), "6b: 1 ms after the OFF was sent, idle — the replacement starts its own retry clock");
    r.at(t_off + ecm::Reconciler::MIN_RETRY_MS);
    check(slot_is(r.poll(), 0x64, 0x00, 0x00, 0x00) && r.rec.inflight_attempts() == 2,
          "6b: MIN_RETRY_MS after the OFF was sent, the OFF is retried (attempt 2)");
  }

  // (c) The ECM then reports the flame ON — which is exactly what the
  // superseded command asked for. THIS IS THE CASE WHERE THE USER ASKED FOR
  // OFF: the report is stale relative to the user's intent, so the reconciler
  // must go on commanding OFF. It must not "settle" on a state the user has
  // already overridden.
  {
    Rig r;
    r.rec.request_flame(true);
    r.poll();  // ON in flight
    r.rec.request_flame(false);  // supersedes it
    r.rec.observe(obs(true, 0));  // the appliance is burning
    check(r.rec.reported_flame() == ecm::Tri::On && r.rec.desired_flame() == ecm::Tri::Off,
          "6c premises: the ECM reports ON while the user's standing intent is OFF");
    const ecm::Slot s = r.poll();
    check(slot_is(s, 0x64, 0x00, 0x00, 0x00),
          "6c: user asked OFF, ECM reports flame ON -> the reconciler still commands OFF (intent, not staleness)",
          "got " + slot_hex(s));

    // And once the ECM agrees, it settles.
    r.rec.observe(obs(false, 0));
    check(r.rec.last_outcome().kind == ecm::OutcomeKind::Confirmed, "6c: the OFF confirms against a status frame reporting flame off");
    const int writes = poll_many(r, 20);
    check(writes == 0, "6c: with the flame off as asked, the reconciler settles (20 polls, no writes)");
  }

  // (d) The mirror of (c): the user's LAST word was ON, so the same flame-ON
  // report settles rather than triggering anything. Same status frame, opposite
  // intent, opposite decision — which is the point.
  {
    Rig r;
    r.rec.request_flame(true);
    r.poll();
    r.rec.observe(obs(true, 0));
    check(r.rec.last_outcome().kind == ecm::OutcomeKind::Confirmed, "6d: a flame-ON report confirms a flame-ON request");
    const int writes = poll_many(r, 20);
    check(writes == 0, "6d: and nothing further is commanded (the last request was ON, and the ECM is ON)");
  }
}

// ---------------------------------------------------------------------------
// 7. Confirmation
// ---------------------------------------------------------------------------

static void test_confirmation() {
  std::printf("\n== 7. the ECM's own status frame is the only acknowledgement ==\n");

  {
    Rig r;
    r.rec.request_flame(true);
    r.poll();
    r.rec.observe(obs(true, 0));
    check(r.rec.last_outcome().kind == ecm::OutcomeKind::Confirmed && r.rec.last_outcome().attempts == 1,
          "flame request confirmed on the first attempt by a status frame reporting flame on");
    check(!r.rec.inflight_active(), "the request clears when confirmed");
    check(poll_many(r, 20) == 0, "subsequent polls are idle");
  }

  {
    Rig r;
    r.rec.observe(obs(true, 0));
    r.rec.request_height_app(5);
    r.poll();
    check(r.rec.inflight_active(), "the height request is in flight");
    r.rec.observe(obs(true, 4));  // the ECM reports internal 4 == app 5
    check(r.rec.last_outcome().kind == ecm::OutcomeKind::Confirmed,
          "a height request is confirmed when the ECM reports the wanted INTERNAL height (4, i.e. app 5)");
    check(poll_many(r, 20) == 0, "subsequent polls are idle");
  }

  // A status frame that reports the OTHER value must not confirm anything.
  {
    Rig r;
    r.rec.observe(obs(true, 0));
    r.rec.request_height_app(5);
    r.poll();
    r.rec.observe(obs(true, 1));  // wrong height (app 2)
    check(r.rec.inflight_active() && r.rec.last_outcome().kind != ecm::OutcomeKind::Confirmed,
          "a status frame reporting a DIFFERENT height does not confirm the request");
    r.at(ecm::Reconciler::MIN_RETRY_MS);
    check(slot_is(r.poll(), 0x67, 0x00, 0x04, 0x00), "and it is retried at the normal cadence");
  }

  // A flame-off status frame reports the SYNTHETIC height 4, not the stored
  // height. It must not confirm an in-flight app-height-5 (internal 4)
  // request: the ECM ignores height writes while the burner is off, so the
  // match is not evidence that the write landed. The request stays in flight
  // and confirms only when the burner is actually burning at that height.
  {
    Rig r;
    r.rec.observe(obs(true, 0));
    r.rec.request_height_app(5);
    r.poll();
    check(r.rec.inflight_active(), "height 5 is in flight");
    r.rec.observe(obs(false, 4));  // flame off -> the status getter's synthetic 4
    check(r.rec.inflight_active() && r.rec.last_outcome().kind != ecm::OutcomeKind::Confirmed,
          "a flame-off status reporting the synthetic height 4 does NOT confirm an in-flight height 5");
    r.rec.observe(obs(true, 4));  // now burning at internal 4 == app 5
    check(r.rec.last_outcome().kind == ecm::OutcomeKind::Confirmed,
          "the same height confirms once the flame is ON");
  }

  // One more, deliberately documented rather than assumed: a confirmed request
  // is NOT a one-shot. The intent stands, so if the appliance later reports a
  // state the user did not ask for.
  {
    Rig r;
    r.rec.request_flame(true);
    r.poll();
    r.rec.observe(obs(true, 0));
    check(poll_many(r, 5) == 0, "a confirmed flame-on settles while the ECM keeps reporting on");
    check(r.rec.desired_flame() == ecm::Tri::Unknown,
          "confirmation COMPLETES the request: intent is no longer standing");
    r.rec.observe(obs(false, 0));  // the appliance stopped burning on its own
    check(poll_many(r, 5) == 0,
          "if the flame later reports OFF, the completed intent is NOT commanded again");
  }
}

// ---------------------------------------------------------------------------
// 8. Height needs a burning flame
// ---------------------------------------------------------------------------

static void test_height_needs_flame() {
  std::printf("\n== 8. height is not written while the flame is off ==\n");

  {
    Rig r;
    r.rec.request_height_app(5);  // asked for before any status frame at all
    check(poll_many(r, 30) == 0, "with no status frame yet, a height request writes nothing (30 polls over 60 s)");
    check(!r.rec.fault_latched(),
          "and nothing latches a fault for it: no write was ever attempted, so no attempt can time out");
  }

  {
    Rig r;
    r.rec.observe(obs(false, 4));  // the door is shut; the ECM reports a cold appliance
    r.rec.request_height_app(5);
    check(poll_many(r, 30) == 0, "with the ECM reporting flame OFF, the height request writes nothing");
    check(!r.rec.fault_latched(), "still no fault: an ignored height request is not a failed one");

    // The same request, the moment the appliance is burning.
    r.rec.observe(obs(true, 0));
    const ecm::Slot s = r.poll();
    check(slot_is(s, 0x67, 0x00, 0x04, 0x00), "once the ECM reports flame ON, the standing request goes out as 67 00 04 00",
          "got " + slot_hex(s));
  }

  // Flame is decided BEFORE height: an unsatisfied flame request occupies the
  // one slot instead of the height request being queued behind it.
  {
    Rig r;
    r.rec.observe(obs(true, 0));
    r.rec.request_height_app(5);
    r.rec.request_flame(false);
    const ecm::Slot s = r.poll();
    check(slot_is(s, 0x64, 0x00, 0x00, 0x00), "flame wins the single slot when both are outstanding", "got " + slot_hex(s));
  }
}

// ---------------------------------------------------------------------------
// 9. The write-enable gate
// ---------------------------------------------------------------------------

static void test_write_enable_gate() {
  std::printf("\n== 9. the write-enable switch, read at the moment of decision ==\n");

  Rig r;
  r.write_enabled = false;
  r.rec.request_flame(true);

  check(poll_many(r, 10) == 0, "switch OPEN: 10 polls, never a non-idle slot");
  check(r.rec.last_outcome().kind == ecm::OutcomeKind::RejectedSwitchOpen, "the outcome is RejectedSwitchOpen",
        "kind " + std::to_string(static_cast<int>(r.rec.last_outcome().kind)));
  check(!r.rec.inflight_active(), "nothing was claimed in flight while the switch was open");

  // The switch closes between one poll and the next: the same outstanding
  // request goes out immediately, as a first attempt — being blocked is not a
  // retry, and the 2 s gap and the 8 s timeout have not been running.
  r.write_enabled = true;
  const ecm::Slot s = r.poll();
  check(slot_is(s, 0x64, 0x00, 0x01, 0x00), "switch CLOSED on the very next poll: the same request is sent", "got " + slot_hex(s));
  check(r.rec.last_outcome().kind == ecm::OutcomeKind::Sent && r.rec.last_outcome().attempts == 1,
        "it is attempt 1, not attempt 11: the open switch consumed nothing");

  // And the reverse direction on an already-sent command: opening the switch
  // stops the retries without consuming attempts, so closing it resumes them
  // exactly where the cadence left off.
  {
    Rig r2;
    r2.rec.request_flame(true);
    r2.poll();  // sent at t = 0
    r2.write_enabled = false;
    r2.at(ecm::Reconciler::MIN_RETRY_MS);
    check(is_idle(r2.poll()) && r2.rec.last_outcome().kind == ecm::OutcomeKind::RejectedSwitchOpen,
          "an in-flight command whose retry is rejected reports RejectedSwitchOpen and sends nothing");
    check(r2.rec.inflight_attempts() == 1, "the rejected retry did not consume an attempt");
    r2.write_enabled = true;
    check(slot_is(r2.poll(), 0x64, 0x00, 0x01, 0x00), "closing the switch resumes the retry (still attempt 2 pending)");
  }

  // Documented sharp edge, preserved from the original: while a command is
  // outstanding, the give-up clock keeps running through rejected polls, so a
  // switch left open past COMMAND_TIMEOUT_MS latches a fault rather than
  // waiting forever for the switch.
  {
    Rig r3;
    r3.rec.request_flame(true);
    r3.poll();  // sent at t = 0
    r3.write_enabled = false;
    r3.at(ecm::Reconciler::COMMAND_TIMEOUT_MS);
    r3.poll();
    check(r3.rec.fault_latched(),
          "a switch left open past COMMAND_TIMEOUT_MS with a command outstanding latches a fault (loud, not silent)");
  }
}

// ---------------------------------------------------------------------------
// 10. A latched fault still honours flame-OFF
// ---------------------------------------------------------------------------

static void test_fault_still_allows_flame_off() {
  std::printf("\n== 10. a latched fault blocks everything except flame-OFF ==\n");

  // Latch a fault the only way it can be latched: a command that is never
  // acknowledged.
  Rig r;
  r.rec.observe(obs(true, 0));
  r.rec.request_height_app(5);
  for (uint32_t t = 0; t <= ecm::Reconciler::COMMAND_TIMEOUT_MS; t += ecm::Reconciler::MIN_RETRY_MS) {
    r.at(t);
    r.poll();
  }
  check(r.rec.fault_latched(), "setup: the unacknowledged height command latched a fault");

  // Blocked: flame-ON.
  r.rec.request_flame(true);
  check(poll_many(r, 20) == 0, "with the fault latched, a flame-ON request writes nothing (20 polls over 40 s)");

  // Blocked: height, even with the flame burning and the request outstanding.
  // (Flame is ON here and already reported on, so nothing else can be the
  // reason the slot is idle.)
  r.rec.observe(obs(true, 0));
  r.rec.request_height_app(3);
  check(r.rec.desired_height_internal() == 2 && r.rec.reported_height_internal() == 0,
        "setup: a height change is genuinely outstanding (wanted 2, reported 0)");
  check(poll_many(r, 20) == 0, "with the fault latched, a height request writes nothing");

  // NOT blocked: flame-OFF. Refusing to turn a fire off is the wrong direction
  // to fail in.
  r.rec.request_flame(false);
  const ecm::Slot s = r.poll();
  check(slot_is(s, 0x64, 0x00, 0x00, 0x00), "with the fault latched, flame-OFF IS emitted: 64 00 00 00", "got " + slot_hex(s));

  // And it can still be confirmed, through the latch.
  r.rec.observe(obs(false, 4));
  check(r.rec.last_outcome().kind == ecm::OutcomeKind::Confirmed, "the flame-OFF confirms through the latched fault");
  check(poll_many(r, 20) == 0, "with the flame off as asked, and the fault still latched, polls are idle");
  check(r.rec.fault_latched(), "confirming the OFF did not clear the latch — only a human does that");

  // Clearing the latch restores normal service.
  r.rec.clear_fault();
  r.rec.request_flame(true);
  const ecm::Slot back = r.poll();
  check(slot_is(back, 0x64, 0x00, 0x01, 0x00), "after clear_fault(), a new flame-ON request is sent again", "got " + slot_hex(back));
}

// ---------------------------------------------------------------------------
// 11. Pilot — and the exact bytes
// ---------------------------------------------------------------------------

static void test_pilot_slot() {
  std::printf("\n== 11. a pilot request produces exactly 65 00 01 00 ==\n");

  Rig r;
  r.rec.request_pilot(true);
  const ecm::Slot s = r.poll();
  check(slot_is(s, 0x65, 0x00, 0x01, 0x00), "pilot-on slot is exactly 65 00 01 00", "got " + slot_hex(s));
  check(r.rec.last_outcome().kind == ecm::OutcomeKind::Sent && r.rec.last_outcome().attempts == 1,
        "outcome is Sent, attempts = 1");

  // The slot is only useful if it reaches the wire. Build the real reply to the
  // same recorded 0xBC request the flame test uses and look at bytes 6..9.
  const uint8_t bc_request[8] = {0x4B, 0x02, 0xDE, 0xBC, 0x02, 0x04, 0x8D, 0x0F};
  ecm::Frame frame;
  if (ecm::parse_request(bc_request, sizeof(bc_request), frame)) {
    uint8_t reply[ecm::MAX_REPLY_LEN];
    const size_t len = ecm::build_reply(frame, &s, reply, sizeof(reply));
    const bool wire_ok = len == 12 && reply[6] == 0x65 && reply[7] == 0x00 && reply[8] == 0x01 && reply[9] == 0x00 &&
                         ecm::crc_ok(reply, len);
    check(wire_ok, "the same slot lands in the 0xBC reply as 65 00 01 00 (CRC valid)",
          "reply " + std::to_string(len) + " bytes");
  } else {
    check(false, "the recorded 0xBC vector failed to parse");
  }

  Rig off_rig;
  off_rig.rec.request_pilot(false);
  const ecm::Slot off = off_rig.poll();
  check(slot_is(off, 0x65, 0x00, 0x00, 0x00), "pilot-off is the same opcode with value 00", "got " + slot_hex(off));

  // A pilot request is not a flame request: the two opcodes are separate
  // namespaces over the same slot, and neither is derived from the other.
  Rig cross;
  cross.rec.request_pilot(true);
  check(cross.poll().b[0] == 0x65, "the pilot never emits the flame opcode 0x64");
  Rig cross2;
  cross2.rec.request_flame(true);
  check(cross2.poll().b[0] == 0x64, "and the flame never emits the pilot opcode 0x65");
}

// ---------------------------------------------------------------------------
// 12. Pilot confirmation, and the retry until it comes
// ---------------------------------------------------------------------------

static void test_pilot_confirmation() {
  std::printf("\n== 12. the pilot confirms against status byte 5, and is retried until it does ==\n");

  {
    Rig r;
    r.rec.observe(obs(true, 0, ecm::Tri::Off));  // the ECM reports the pilot off
    r.rec.request_pilot(true);
    check(slot_is(r.poll(), 0x65, 0x00, 0x01, 0x00), "t = 0: the pilot-on is sent");
    check(r.rec.inflight_active(), "the pilot request is in flight");

    // A status frame STILL reporting the pilot off must not confirm it.
    r.rec.observe(obs(true, 0, ecm::Tri::Off));
    check(r.rec.inflight_active() && r.rec.last_outcome().kind != ecm::OutcomeKind::Confirmed,
          "a status frame still reporting the pilot off does not confirm the request");
    r.at(ecm::Reconciler::MIN_RETRY_MS - 1);
    check(is_idle(r.poll()), "t = MIN_RETRY_MS - 1: idle (1999 ms is too soon)");
    r.at(ecm::Reconciler::MIN_RETRY_MS);
    check(slot_is(r.poll(), 0x65, 0x00, 0x01, 0x00) && r.rec.inflight_attempts() == 2,
          "t = MIN_RETRY_MS exactly: retried (attempt 2)");

    // An UNKNOWN pilot must not confirm it either — the same "positive evidence
    // only" rule the flame confirmation carries. Without that rule a pilot-OFF
    // would be silently abandoned by a frame that never mentioned the pilot.
    r.rec.observe(obs(true, 0, ecm::Tri::Unknown));
    check(r.rec.inflight_active(), "an Unknown pilot does not confirm the request");

    // The ECM reports the pilot lit: that, and only that, confirms.
    r.rec.observe(obs(true, 0, ecm::Tri::On));
    check(r.rec.last_outcome().kind == ecm::OutcomeKind::Confirmed,
          "a status frame reporting the pilot ON confirms it");
    check(!r.rec.inflight_active(), "the request clears when confirmed");
    check(poll_many(r, 20) == 0, "and subsequent polls are idle");
  }

  {
    // The mirror direction: a pilot-OFF is confirmed only by a pilot-OFF report.
    Rig r;
    r.rec.observe(obs(false, -1, ecm::Tri::On));
    r.rec.request_pilot(false);
    check(slot_is(r.poll(), 0x65, 0x00, 0x00, 0x00), "pilot-off is sent as 65 00 00 00");
    r.rec.observe(obs(false, -1, ecm::Tri::On));
    check(r.rec.inflight_active(), "still reporting the pilot ON does not confirm the pilot-OFF");
    r.rec.observe(obs(false, -1, ecm::Tri::Off));
    check(r.rec.last_outcome().kind == ecm::OutcomeKind::Confirmed,
          "a status frame reporting the pilot OFF confirms it");
    check(poll_many(r, 20) == 0, "and it settles");
  }
}

// ---------------------------------------------------------------------------
// 13. The write-enable gate, on the pilot
// ---------------------------------------------------------------------------

static void test_pilot_write_enable_gate() {
  std::printf("\n== 13. the write-enable gate applies to the pilot exactly as to flame ==\n");

  Rig r;
  r.write_enabled = false;
  r.rec.request_pilot(true);
  check(poll_many(r, 10) == 0, "switch OPEN: 10 polls, never a non-idle pilot slot");
  check(r.rec.last_outcome().kind == ecm::OutcomeKind::RejectedSwitchOpen, "the outcome is RejectedSwitchOpen",
        "kind " + std::to_string(static_cast<int>(r.rec.last_outcome().kind)));
  check(!r.rec.inflight_active(), "nothing was claimed in flight while the switch was open");

  r.write_enabled = true;
  const ecm::Slot after = r.poll();
  check(slot_is(after, 0x65, 0x00, 0x01, 0x00), "switch CLOSED on the next poll: the same pilot request is sent",
        "got " + slot_hex(after));
  check(r.rec.last_outcome().kind == ecm::OutcomeKind::Sent && r.rec.last_outcome().attempts == 1,
        "it is attempt 1, not attempt 11: the open switch consumed nothing");
}

// ---------------------------------------------------------------------------
// 14. A latched fault still honours pilot-OFF
// ---------------------------------------------------------------------------

static void test_fault_still_allows_pilot_off() {
  std::printf("\n== 14. a latched fault blocks everything except an OFF write, flame or pilot ==\n");

  // Latch a fault the only way it can be latched: a command that is never
  // acknowledged. Height is used so that the pilot is left reported ON — which
  // is what makes a subsequent pilot-OFF a real change rather than a no-op.
  Rig r;
  r.rec.observe(obs(true, 0, ecm::Tri::On));
  r.rec.request_height_app(5);
  for (uint32_t t = 0; t <= ecm::Reconciler::COMMAND_TIMEOUT_MS; t += ecm::Reconciler::MIN_RETRY_MS) {
    r.at(t);
    r.poll();
  }
  check(r.rec.fault_latched(), "setup: the unacknowledged height command latched a fault");
  check(r.rec.reported_pilot() == ecm::Tri::On, "setup: the ECM reports the pilot ON");

  // Blocked: a flame-ON.
  r.rec.request_flame(true);
  check(poll_many(r, 20) == 0, "with the fault latched, a flame-ON request writes nothing (20 polls over 40 s)");

  // Blocked: height, even with the flame burning and the request outstanding.
  r.rec.request_height_app(3);
  check(r.rec.desired_height_internal() == 2 && r.rec.reported_height_internal() == 0,
        "setup: a height change is genuinely outstanding (wanted 2, reported 0)");
  check(poll_many(r, 20) == 0, "with the fault latched, a height request writes nothing");

  // NOT blocked: pilot-OFF. Refusing to turn something off is the wrong
  // direction to fail in — and it must not be starved by the standing flame-ON
  // target that the latch is refusing to write.
  r.rec.request_pilot(false);
  const ecm::Slot s = r.poll();
  check(slot_is(s, 0x65, 0x00, 0x00, 0x00),
        "with the fault latched AND a flame-ON standing, pilot-OFF IS emitted: 65 00 00 00", "got " + slot_hex(s));

  // And it can still be confirmed, through the latch.
  r.rec.observe(obs(false, -1, ecm::Tri::Off));
  check(r.rec.last_outcome().kind == ecm::OutcomeKind::Confirmed, "the pilot-OFF confirms through the latched fault");
  check(r.rec.fault_latched(), "confirming the OFF did not clear the latch — only a human does that");

  // Clearing the latch restores normal service. It ALSO drops the standing
  // flame-ON that the latch was holding back: clearing an alarm must not
  // re-arm a stale command.
  r.rec.clear_fault();
  check(r.rec.desired_flame() == ecm::Tri::Unknown,
        "clear_fault() drops the standing flame-ON intent");
  check(poll_many(r, 3) == 0, "after clear_fault(), nothing is re-sent");
}

// ---------------------------------------------------------------------------
// 15. Precedence in the single slot
// ---------------------------------------------------------------------------

static void test_slot_precedence() {
  std::printf("\n== 15. precedence: flame > pilot > height in the one write slot ==\n");

  // All three outstanding at once.
  {
    Rig r;
    r.rec.observe(obs(true, 0, ecm::Tri::On));
    r.rec.request_height_app(5);
    r.rec.request_pilot(false);
    r.rec.request_flame(false);
    const ecm::Slot s = r.poll();
    check(slot_is(s, 0x64, 0x00, 0x00, 0x00), "all three outstanding: flame takes the slot", "got " + slot_hex(s));
  }

  // Nothing for flame to do: pilot wins over the standing height request.
  {
    Rig r;
    r.rec.observe(obs(true, 0, ecm::Tri::On));
    r.rec.request_height_app(5);
    r.rec.request_pilot(false);
    const ecm::Slot s = r.poll();
    check(slot_is(s, 0x65, 0x00, 0x00, 0x00),
          "with the flame settled, pilot takes the slot over the standing height request", "got " + slot_hex(s));

    // ...and the height request is not lost. It goes out once the pilot has.
    r.rec.observe(obs(true, 0, ecm::Tri::Off));
    check(r.rec.last_outcome().kind == ecm::OutcomeKind::Confirmed, "the pilot-OFF confirms on the next status frame");
    check(slot_is(r.poll(), 0x67, 0x00, 0x04, 0x00), "then the standing height request goes out (67 00 04 00)");
  }

  // Height still needs a burning flame, with the pilot in the picture or not.
  {
    Rig r;
    r.rec.observe(obs(false, 4, ecm::Tri::On));
    r.rec.request_height_app(1);
    check(poll_many(r, 10) == 0, "with the flame OFF, a height request writes nothing, pilot or no pilot");
  }
}

// ---------------------------------------------------------------------------
// 16. Flame with the pilot reported off — visible, and never blocked
// ---------------------------------------------------------------------------

static void test_flame_without_pilot() {
  std::printf("\n== 16. flame with the pilot reported off: written, and reported distinctly ==\n");

  // The ECM reports the pilot OFF. The flame-ON is STILL written — the ECM owns
  // the ignition interlock and Home Assistant owns the ordering policy, so this
  // firmware does not enforce "pilot before flame" — but the outcome is its own
  // kind, so the condition is visible instead of looking like an ignition that
  // simply did not happen.
  Rig r;
  r.rec.observe(obs(false, -1, ecm::Tri::Off));
  r.rec.request_flame(true);
  const ecm::Slot s = r.poll();
  check(slot_is(s, 0x64, 0x00, 0x01, 0x00), "the flame-ON write STILL goes out (the condition does not block it)",
        "got " + slot_hex(s));
  check(r.rec.last_outcome().kind == ecm::OutcomeKind::FlameWithoutPilot,
        "and the outcome is the distinct FlameWithoutPilot, not an ordinary Sent",
        "kind " + std::to_string(static_cast<int>(r.rec.last_outcome().kind)));
  check(r.rec.inflight_active() && r.rec.inflight_attempts() == 1,
        "it is genuinely in flight, not refused: the retry/confirm machinery is unchanged");

  // The retry says the same thing, carrying the attempt count.
  r.at(ecm::Reconciler::MIN_RETRY_MS);
  check(slot_is(r.poll(), 0x64, 0x00, 0x01, 0x00) && r.rec.last_outcome().kind == ecm::OutcomeKind::FlameWithoutPilot &&
            r.rec.last_outcome().attempts == 2,
        "the retry is still a flame write and still reports FlameWithoutPilot (attempt 2)");

  // It confirms normally: the ECM's status frame remains the only ack.
  r.rec.observe(obs(true, 0, ecm::Tri::Off));
  check(r.rec.last_outcome().kind == ecm::OutcomeKind::Confirmed,
        "the write confirms normally against a status frame reporting the flame on");

  // The control: with the pilot reported ON, the identical request is a plain Sent.
  {
    Rig r2;
    r2.rec.observe(obs(false, -1, ecm::Tri::On));
    r2.rec.request_flame(true);
    r2.poll();
    check(r2.rec.last_outcome().kind == ecm::OutcomeKind::Sent,
          "with the pilot reported ON, a flame-ON is an ordinary Sent");
  }

  // An UNKNOWN pilot is not a report of off: before the first status frame
  // there is nothing truthful to report, so the distinct outcome must not fire.
  {
    Rig r3;
    r3.rec.request_flame(true);
    r3.poll();
    check(r3.rec.last_outcome().kind == ecm::OutcomeKind::Sent, "an UNKNOWN pilot does not produce FlameWithoutPilot");
  }

  // It is a flame-ON condition only. A flame-OFF is never tagged with it.
  {
    Rig r4;
    r4.rec.observe(obs(true, 0, ecm::Tri::Off));
    r4.rec.request_flame(false);
    r4.poll();
    check(r4.rec.last_outcome().kind == ecm::OutcomeKind::Sent, "a flame-OFF with the pilot off is an ordinary Sent");
  }

  // And the new opcode must not let the pilot starve: flame still goes first,
  // and the outstanding pilot-ON gets the very next slot.
  {
    Rig r5;
    r5.rec.observe(obs(false, -1, ecm::Tri::Off));
    r5.rec.request_pilot(true);
    r5.rec.request_flame(true);
    check(slot_is(r5.poll(), 0x64, 0x00, 0x01, 0x00),
          "flame and pilot both requested from cold: flame takes the slot first");
    r5.rec.observe(obs(true, 0, ecm::Tri::Off));  // the flame lights; the pilot still reports off
    check(slot_is(r5.poll(), 0x65, 0x00, 0x01, 0x00),
          "once the flame is settled, the outstanding pilot-ON gets the slot (flame does not starve it)");
  }
}

// ---------------------------------------------------------------------------

int main() {
  std::printf("=== ecm write reconciler: host test ===\n");
  std::printf("    synthetic clock, no board, no hardware. This is the code that\n");
  std::printf("    decides whether a write goes out on a gas appliance's bus.\n");

  test_no_writes_at_boot();
  test_flame_slot();
  test_height_off_by_one();
  test_retry_timing();
  test_give_up();
  test_supersede();
  test_confirmation();
  test_height_needs_flame();
  test_write_enable_gate();
  test_fault_still_allows_flame_off();
  test_pilot_slot();
  test_pilot_confirmation();
  test_pilot_write_enable_gate();
  test_fault_still_allows_pilot_off();
  test_slot_precedence();
  test_flame_without_pilot();

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

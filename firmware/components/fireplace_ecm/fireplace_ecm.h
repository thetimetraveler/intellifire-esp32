// fireplace_ecm.h — ESPHome component: the ECM's polled slave, reimplemented.
//
// This is a replacement for the IntelliFire Wi-Fi module. It answers the ECM's
// polls on the UART and carries commands in the 0xBC reply's 4-byte data slot.
// The wire format lives in include/ecm_protocol.h and the write-decision policy
// in include/ecm_reconciler.h — both pure C++20 and host-tested. This file is
// only the part that needs a board: UART framing, the write-enable pin, the
// counters, and the delegation of every write decision to the reconciler.
//
// ---------------------------------------------------------------------------
// WHERE THE DECISION LIVES
// ---------------------------------------------------------------------------
// The write-decision policy is NOT in this file. It is in
// include/ecm_reconciler.h — pure C++20, no ESPHome, host-tested, with the clock
// and the write-enable pin as parameters rather than globals. This class is the
// board side: UART framing, the pin read, the counters, and delegation to
// that reconciler. Read ecm_reconciler.h for the policy itself (why a queue
// would be a fiction, why all three desired values start unknown, why a latched
// fault still honours an off write, and why the pilot is write-only here and
// does NOT gate the flame).
//
// The one thing that has to stay here is the ORDER: the write-enable pin is read
// immediately before slot_for_bc_reply() is called, so the switch state used for
// the decision is the state at the decision. Caching it for the loop would turn
// "a switch flipped mid-exchange is honoured" into a per-loop approximation.
//
// ---------------------------------------------------------------------------
// SAFETY FRAME
// ---------------------------------------------------------------------------
// The ECM owns ignition, flame sensing and the gas valve. This component is a
// control path, never a safety device. It can command; it can never bypass.
// There is deliberately no flame-error or pilot-error entity: the ECM's fault
// codes are internal and are not transmitted on this link, and a stub
// hardwired to `false` would be a silently-never-firing fault alarm — worse
// than a missing entity.
#pragma once

#include <string>

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"

// The protocol core and the reconciler live in firmware/include/, outside this
// component's own directory. __init__.py adds that directory with a -I build
// flag; the relative fallback covers the case where the flag is ever dropped or
// the project is copied somewhere the absolute path no longer resolves, so that
// the failure is an obvious one to diagnose rather than a bare "file not found".
#if __has_include("ecm_reconciler.h")
#include "ecm_reconciler.h"
#include "ecm_telemetry.h"
#include "ecm_inventory.h"
#include "ecm_framing.h"
#else
#include "../../include/ecm_reconciler.h"
#include "../../include/ecm_telemetry.h"
#include "../../include/ecm_inventory.h"
#include "../../include/ecm_framing.h"
#endif

// The MQTT client is only linked when the mqtt: component is configured. Every
// use below is behind this guard, so the component still builds — and the host
// stub compile still passes — with no broker at all.
#ifdef USE_MQTT
#include "esphome/components/mqtt/mqtt_client.h"
#endif

namespace esphome {
namespace fireplace_ecm {

class FireplaceECM : public Component, public uart::UARTDevice {
 public:
  // ---- Timing budget. The ECM's next poll starts >= ~158 ms after the end of
  // the previous one (measured min 158.6 ms, median 340 ms), and the ECM
  // tolerates seconds of silence before it parks its counter. These are the
  // component's own retry policy, not protocol deadlines.
  //
  // The retry numbers belong to the reconciler (they are part of the policy, and
  // the host test asserts their boundaries). Re-exported here — single-sourced,
  // not copied — for dump_config() and for anything that already refers to them.
  static constexpr uint32_t LINK_TIMEOUT_MS = 5000;  // link_alive() window
  static constexpr uint32_t MIN_RETRY_MS = ecm::Reconciler::MIN_RETRY_MS;
  static constexpr uint32_t COMMAND_TIMEOUT_MS = ecm::Reconciler::COMMAND_TIMEOUT_MS;
  static constexpr uint8_t MAX_ATTEMPTS = ecm::Reconciler::MAX_ATTEMPTS;
  /// A poll older than this is never answered with a write. The ECM's own poll
  /// spacing is 158-340 ms, so a frame this stale arrived across a loop stall
  /// and a write into it would cross the ECM's counter park/resync.
  static constexpr uint32_t STALE_WRITE_MS = 1000;

  /// Bytes of one frame arrive ~1.04 ms apart at 9600-8-N-1, and the ECM's
  /// frames are back-to-back; the next frame is >= 158 ms away. So a gap this
  /// long after the last byte can only mean the frame has ended.
  /// Single-sourced from the assembler rather than restated, so the two cannot
  /// drift. The framing rule itself lives in include/ecm_framing.h.
  static constexpr uint32_t FRAME_GAP_MS = ecm::FrameAssembler::GAP_MS;

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  /// BCM/ESP32 GPIO number of the write-enable sense line (DPST pole B).
  void set_write_enable_pin(int pin) { this->write_enable_pin_ = pin; }

  // ------------------------------------------------------------------
  // State, read back from the ECM. Nothing here is optimistic: every one of
  // these comes from the bytes the ECM actually sent, so a rejected command
  // snaps Home Assistant back to the truth on its own.
  // ------------------------------------------------------------------

  /// Flame burning, per the ECM's status frame. False while unknown.
  bool flame_on() const { return this->last_flame_ == ecm::Tri::On; }
  /// True once a status frame has been seen (before that, "off" is a guess).
  bool flame_known() const { return this->last_flame_ != ecm::Tri::Unknown; }
  /// Pilot lit, per the ECM's status frame (byte 5). False while unknown.
  bool pilot_on() const { return this->pilot_ == ecm::Tri::On; }
  /// True once a status frame has been seen. The Pilot switch lambda uses this
  /// to report NOTHING rather than a confident "off" before the first frame —
  /// the same rule the Flame switch follows, so neither entity can lie at boot.
  bool pilot_known() const { return this->pilot_ != ecm::Tri::Unknown; }
  /// Status byte 9. Formerly labelled "hot"; it reports a 32-bit
  /// deadline-armed flag, so it is a diagnostic only.
  bool status_flag9() const { return this->status_flag9_; }

  /// Status bytes 6 and 8. Their data paths are proven (byte 6 = the byte
  /// written by slot 0x66; byte 8 = the heat-demand byte written by slots
  /// 0x64/0x68) but their human labels are not. Diagnostic read-backs only — no control decision reads them.
  /// Negative means "no status frame yet"; the wire range is 0-255.
  int16_t config_byte() const { return this->last_config_byte_; }
  bool config_byte_known() const { return this->last_config_byte_ >= 0; }
  int16_t heat_demand() const { return this->last_heat_demand_; }
  bool heat_demand_known() const { return this->last_heat_demand_ >= 0; }

  /// Height as the APP shows it (1-5) = internal + 1. NAN until a status frame
  /// has been seen. NOTE: the ECM reports a synthetic maximum (4) whenever the
  /// burner is off, so this reads 5 while the flame is off. Its stored height
  /// is restored on flame-on. That is faithful to the reported state, and it
  /// will look like a bug.
  float height_app() const;
  bool height_known() const { return this->last_height_ >= 0; }

  /// A CRC-valid ECM frame within LINK_TIMEOUT_MS. The ECM polls continuously,
  /// so this is the real "is the appliance there" signal.
  bool link_alive() const;

  /// The write-enable switch, re-read from the pin RIGHT NOW. HIGH (or an
  /// unwired pin, or an unsupported target) reads DISABLED — fail-safe by
  /// polarity, so an open switch, a broken sense wire and a failed-open switch
  /// all land on the safe side.
  bool write_enabled();

  // ------------------------------------------------------------------
  // Intent. These record what the user asked for; the next 0xBC reply carries
  // it if the write-enable switch allows. They never write anything themselves.
  // ------------------------------------------------------------------

  void request_flame(bool on);
  void request_pilot(bool on);
  void request_height_app(int app_height);  // 1-5, as the app shows it

  /// A latched fault means commands are abandoned until a human clears it. The
  /// fault exists so a write that never lands is LOUD rather than retried
  /// forever.
  bool fault_latched() const { return this->reconciler_.fault_latched(); }
  void clear_fault();

  // ---- Diagnostics -------------------------------------------------------
  uint32_t reply_latency_ms() const { return this->reply_latency_ms_; }
  uint32_t ecm_frames() const { return this->ecm_frames_; }
  /// Everything that went wrong on the wire: frames whose CRC failed to parse,
  /// PLUS runs of bytes with no idle gap (the stream was not frames at all).
  ///
  /// Those are two different faults and the sensor has always summed them. The
  /// sum is preserved here so the entity keeps its meaning, but the MQTT
  /// summary reports them separately — a rising total on its own cannot tell
  /// you which of the two you are looking at.
  uint32_t crc_errors() const { return this->crc_errors_ + this->assembler_.overflows(); }
  uint32_t parse_errors() const { return this->crc_errors_; }
  uint32_t framing_overflows() const { return this->assembler_.overflows(); }
  uint32_t unknown_cmds() const { return this->unknown_cmds_; }
  uint32_t ecm_resets() const { return this->ecm_resets_; }
  /// Times the ECM's exchange counter jumped back to 0 mid-stream. The ECM
  /// does this after a UART overrun or a desynchronised byte stream, so an
  /// occasional count is a link event, not a flame event. Nothing in this
  /// firmware depends on seq; the counter exists so the recovery path is
  /// observable instead of invisible. The boot frame is not counted here.
  uint32_t seq_resets() const { return this->ecm_seq_resets_; }
  std::string last_outcome() const { return this->last_outcome_; }

  // ------------------------------------------------------------------
  // Telemetry. Read-only observation of the bus: it never transmits, never
  // alters a reply, and never turns "record this" into "answer this".
  //
  // A ring keeps the most recent frames in both directions, so a fault can be
  // reconstructed after the fact instead of requiring an analyser to have been
  // attached. It lives in RAM and is lost on reboot — it is recent history,
  // not an archive. Durability is the collector's job.
  // ------------------------------------------------------------------

  static constexpr size_t TELEMETRY_RING = 128;
  using Ring = ecm::telemetry::TelemetryRing<TELEMETRY_RING>;

  /// A dump walks header -> chunks -> footer across successive loop iterations.
  /// It is a state machine and not a loop for one reason: emitting the whole
  /// ring at once would block the bus for as long as it took.
  enum class DumpState : uint8_t { Idle, Header, Chunks, Footer };

  const Ring &ring() const { return this->ring_; }
  const ecm::telemetry::OpcodeInventory &inventory() const { return this->inventory_; }
  uint32_t records_pushed() const { return this->records_pushed_; }
  uint32_t records_published() const { return this->records_published_; }

  /// Records that could not be published because MQTT was down or the queue was
  /// backed up. A NON-ZERO value is a real fault, which is why this — unlike a
  /// "new opcode" counter — is worth an HA entity.
  uint32_t publish_skipped() const { return this->publish_skipped_; }
  uint32_t boot_id() const { return this->boot_id_; }
  /// Frames we actually answered. `Ecm Frames` counts every CRC-valid frame
  /// INCLUDING ones we then refuse to answer (unknown command), so on its own it
  /// reads as healthy while the appliance gets silence. Watching this diverge
  /// from `Ecm Frames` is what makes that visible.
  uint32_t frames_replied() const { return this->frames_replied_; }
  bool dump_active() const { return this->dump_state_ != DumpState::Idle; }

  /// Ask for the whole ring to be published, chunk by chunk. Called from an MQTT
  /// command topic, an API service, or a button — every one of which only sets a
  /// flag, because publishing the ring synchronously would stall the bus.
  void request_dump() { this->dump_requested_ = true; }

  // ------------------------------------------------------------------
  // MQTT broker configuration, changeable at runtime.
  //
  // ESPHome compiles the broker in, and its client has no public "reconnect" —
  // it only re-establishes the socket once the current one drops. So a change
  // cannot take effect without a reboot, and set_mqtt_config() performs one.
  // The values are stored in flash first, so they survive it.
  //
  // What is stored OVERRIDES the compiled-in values. Erasing the device returns
  // it to whatever the YAML says, which is the intended escape hatch.
  // ------------------------------------------------------------------

  /// Store new broker settings and reboot to apply them. Called from the
  /// `set_mqtt_config` API service; the HA Developer Tools action reaches it.
  void set_mqtt_config(const std::string &broker, uint16_t port, const std::string &username,
                       const std::string &password);

  /// Apply any stored override to the live MQTT client. Called during startup,
  /// after ESPHome's own codegen has set the compiled-in values — see the
  /// priority note in the .cpp, which is the reason this is not in setup().
  void apply_stored_mqtt_config();

 protected:
  // ---- RX framing ----
  // Delimiting lives in ecm::FrameAssembler (include/ecm_framing.h), so the
  // code actually running is the code the host tests exercise. This class used
  // to carry its own inline copy of the same logic, which meant the 35 framing
  // checks guarded something that never executed on a board.
  void handle_frame_(const uint8_t *buf, size_t len);

  // ---- Frame handling ----
  void handle_status_(const ecm::Frame &frame);
  /// Build and transmit the reply. Returns the measured latency (or
  /// NO_LATENCY when nothing legal could be sent) and fills `tx_out` with the
  /// frame we put on the wire, so the TX side is recorded without needing to
  /// receive it back. The reply is written before anything is recorded, so the
  /// call itself stays on the critical path and the recording does not.
  uint32_t send_slot_(const ecm::Frame &frame, const ecm::Slot &slot, uint32_t received_ms,
                      ecm::telemetry::FrameRecord *tx_out);

  /// Push a record into the ring and count it. The single place records enter.
  void record_(const ecm::telemetry::FrameRecord &rec) {
    this->ring_.push(rec);
    this->records_pushed_++;
  }

  /// Map the reconciler's last outcome onto the text sensor's wording. A `None`
  /// outcome means "nothing happened" and deliberately leaves the last text
  /// standing, so an idle poll cannot erase what the previous one reported.
  void apply_outcome_();
  void set_outcome_(const std::string &text);

  // ---- Telemetry ----
  /// Fan the publish work for one loop iteration. Called at the END of loop(),
  /// after the reply for any frame this iteration has already gone out — that
  /// ordering is what keeps telemetry out of the reply's time budget.
  void publish_tick_();
  /// One step of the dump state machine: header, then one chunk per call, then
  /// footer. Separate from publish_tick_ so a dump can never emit more than a
  /// single chunk in one loop iteration.
  void publish_tick_dump_();
  /// True only when a broker is both configured and actually connected. Every
  /// publish path checks this first and gives up immediately otherwise — a dead
  /// broker must never make the loop wait.
  bool mqtt_ready_() const;
  bool publish_raw_(const char *topic, const char *payload, bool retain);
  void emit_summary_();
  void emit_inventory_();
  /// Raise an alert for a census finding. Rate-limited: an alert fires once per
  /// new key, not once per occurrence, so a hostile bus cannot flood the broker.
  void note_notable_(uint8_t notable, const ecm::telemetry::FrameRecord &rec);
  /// The CRC is the last two bytes of a frame, little-endian. Used to stamp
  /// records for frames the parser REJECTED — those have no parsed CRC but we
  /// still want the trailer for diagnosis.
  static uint16_t trailer_crc_(const uint8_t *buf, size_t len);

  // ---- RX state ----
  ecm::FrameAssembler assembler_{};

  // ---- The decision. Everything about WHAT to write lives in there; this
  //      class only decides WHEN to ask it, and with which pin reading. ----
  ecm::Reconciler reconciler_{};

  // ---- ECM-reported state, mirrored from the same status frame for the
  //      read-back entities (the reconciler keeps its own copy for the write
  //      decision). One source per frame: both are set from the same bytes in
  //      handle_status_(), so they cannot disagree. ----
  ecm::Tri last_flame_{ecm::Tri::Unknown};
  int8_t last_height_{-1};  // internal 0..4, -1 unknown
  ecm::Tri pilot_{ecm::Tri::Unknown};
  bool status_flag9_{false};
  bool have_status_{false};
  int16_t last_config_byte_{-1};   // status byte 6, -1 unknown
  int16_t last_heat_demand_{-1};   // status byte 8, -1 unknown

  // ---- Board ----
  int write_enable_pin_{-1};

  // ---- Liveness / metrics ----
  bool have_link_{false};
  uint32_t last_valid_frame_ms_{0};
  uint32_t reply_latency_ms_{0};
  uint32_t ecm_frames_{0};
  uint32_t crc_errors_{0};
  uint32_t unknown_cmds_{0};
  uint32_t ecm_resets_{0};
  uint32_t ecm_seq_resets_{0};
  uint32_t frames_replied_{0};
  // Previous exchange counter, for observing a reset-to-zero. This is a
  // diagnostic only: seq is never validated, predicted, or resynchronised.
  uint8_t last_seq_{0};
  bool have_seq_{false};
  std::string last_outcome_{"idle"};

  // ---- Telemetry. Fixed-size, allocation-free: nothing here is touched on the
  //      reply's critical path except one struct copy and one bounded census. ----
  Ring ring_{};
  ecm::telemetry::OpcodeInventory inventory_{};
  uint32_t boot_id_{0};
  uint32_t boot_t0_ms_{0};     ///< millis() at setup, so `t` starts near zero
  uint32_t records_pushed_{0};
  uint32_t records_published_{0};
  uint32_t publish_skipped_{0};
  uint32_t last_summary_ms_{0};
  uint32_t last_inventory_ms_{0};
  bool dump_requested_{false};
  DumpState dump_state_{DumpState::Idle};
  size_t dump_cursor_{0};
  /// Walks the census one key per inventory tick. A whole census does not fit
  /// in one payload, and emitting it whole would stall the loop.
  size_t inventory_cursor_{0};
};

}  // namespace fireplace_ecm
}  // namespace esphome

// fireplace_ecm.cpp — UART framing, the write-enable pin, and delegation.
//
// This file is the board-side half and nothing else. The protocol (CRC,
// parsing, reply construction, slots) is in include/ecm_protocol.h; the policy
// that decides whether a write goes out is in include/ecm_reconciler.h. Both are
// pure C++20 and host-tested. Read those first.

#include "fireplace_ecm.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef USE_ESP32
#include <driver/gpio.h>
#include <esp_random.h>
#endif

#ifdef USE_MQTT
#include "esphome/core/application.h"
#include "esphome/core/preferences.h"
#endif

namespace esphome {
namespace fireplace_ecm {

static const char *const TAG = "fireplace_ecm";

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

float FireplaceECM::get_setup_priority() const {
  // Just after the UART bus itself is configured.
  return setup_priority::BUS - 1.0f;
}

void FireplaceECM::setup() {
  this->assembler_.reset();

  // A random boot id, so "the device rebooted" can be told apart from "the
  // broker connection blipped". They look identical on the wire otherwise, and
  // this project has already been bitten by conflating the two. The collector
  // opens a new run directory per boot id and never merges across one.
#ifdef USE_ESP32
  this->boot_id_ = esp_random();
#else
  this->boot_id_ = static_cast<uint32_t>(millis()) | 1u;
#endif
  this->boot_t0_ms_ = millis();

#ifdef USE_ESP32
  if (this->write_enable_pin_ >= 0 && this->write_enable_pin_ < 32) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << this->write_enable_pin_);
    cfg.mode = GPIO_MODE_INPUT;
    // Pull-up: an open switch, a cut sense wire or a failed-open switch all
    // read HIGH, and write_enabled() treats HIGH as DISABLED.
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
  } else {
    ESP_LOGE(TAG, "write_enable_pin is not a usable ESP32 GPIO (%d) — writes are DISABLED",
             this->write_enable_pin_);
  }
#else
  ESP_LOGE(TAG, "This component only drives the write-enable pin on ESP32 targets — writes are DISABLED");
#endif

  // Nothing is requested at boot, and nothing will be until a user asks. That
  // is structural: see the header.
  this->set_outcome_("idle");

  ESP_LOGCONFIG(TAG, "Fireplace ECM responder up; writes are %s",
                this->write_enabled() ? "ENABLED" : "DISABLED (write-enable switch open or unwired)");
}

void FireplaceECM::dump_config() {
  ESP_LOGCONFIG(TAG, "Fireplace ECM (replacement Wi-Fi module, polled slave):");
  ESP_LOGCONFIG(TAG, "  Write-enable sense pin: %d (LOW = enabled)", this->write_enable_pin_);
  ESP_LOGCONFIG(TAG, "  Writes currently: %s", this->write_enabled() ? "ENABLED" : "DISABLED");
  ESP_LOGCONFIG(TAG, "  Retry: %u ms between attempts, %u attempts max, %u ms timeout",
                (unsigned) MIN_RETRY_MS, (unsigned) MAX_ATTEMPTS, (unsigned) COMMAND_TIMEOUT_MS);
  ESP_LOGCONFIG(TAG, "  Link timeout: %u ms", (unsigned) LINK_TIMEOUT_MS);
  // The reconciler's own read-backs: this class no longer holds the intent, so
  // it asks for it. (Called directly rather than via a local, so the log line
  // stays warning-clean against the stub headers, where a logging macro expands
  // to nothing and would leave the local unused.)
  ESP_LOGCONFIG(TAG, "  Desired state: flame %s, pilot %s, height %s (no writes unless a user asks)",
                this->reconciler_.desired_flame() == ecm::Tri::Unknown
                    ? "unknown"
                    : (this->reconciler_.desired_flame() == ecm::Tri::On ? "on" : "off"),
                this->reconciler_.desired_pilot() == ecm::Tri::Unknown
                    ? "unknown"
                    : (this->reconciler_.desired_pilot() == ecm::Tri::On ? "on" : "off"),
                this->reconciler_.desired_height_internal() >= 0 ? "set" : "unknown");
  ESP_LOGCONFIG(TAG, "  ECM state seen: %s (frames %u, crc errors %u, unknown cmds %u, boot frames %u)",
                this->have_status_ ? "yes" : "not yet", (unsigned) this->ecm_frames_, (unsigned) this->crc_errors_,
                (unsigned) this->unknown_cmds_, (unsigned) this->ecm_resets_);
  ESP_LOGCONFIG(TAG, "  The ECM owns ignition, flame sensing and the gas valve; this is a control path only.");
}

// ---------------------------------------------------------------------------
// RX framing
// ---------------------------------------------------------------------------
//
// There is no length field to trust (the 0xDC frame's payload length byte holds
// only for dev-0x01 frames), so a frame is delimited by WHEN the bytes stop:
// bytes within a frame are ~1.04 ms apart at 9600-8-N-1, and the ECM's next
// frame is >= 158 ms later. A gap of FRAME_GAP_MS can only mean the frame ended.
//
// Deliberately NOT using a CRC scan to find the frame end early: a prefix of a
// real frame validates by chance about 1 time in 65536 per candidate length, and
// a false positive here means answering the wrong command on a gas appliance.
// One extra loop iteration of latency is free; we have ~10x headroom.

void FireplaceECM::loop() {
  uint8_t byte;
  const uint32_t now = millis();
  while (this->available() > 0) {
    if (!this->read_byte(&byte)) {
      break;
    }
    this->assembler_.push(byte, now);
  }
  // Where a frame ends is decided by ecm::FrameAssembler, so the code running
  // here is the code test/ecm_framing_test.cpp exercises. It delivers at most
  // one frame per call, which is all the bus can produce in one iteration
  // anyway — the ECM's frames are >= 158 ms apart.
  if (this->assembler_.tick(now)) {
    this->handle_frame_(this->assembler_.frame(), this->assembler_.length());
  }

  // Telemetry LAST, deliberately. By the time control reaches here, any reply
  // this iteration owed has already been written and its latency measured, so
  // the broker cannot end up inside the reply's ~160 ms budget. Do not move
  // this above the frame handling — that would put network work between a poll
  // arriving and its answer going out.
  this->publish_tick_();
}

// ---------------------------------------------------------------------------
// Frame handling
// ---------------------------------------------------------------------------

void FireplaceECM::handle_frame_(const uint8_t *buf, size_t len) {
  // The last byte of the frame is the closest thing we have to the moment the
  // ECM stopped transmitting; reply latency is measured from there.
  const uint32_t received_ms = this->assembler_.last_byte_ms();

  // Build the record BEFORE any early return, into fixed storage, with no
  // allocation and no network work. A frame we refuse to answer — an unknown
  // command, a bad CRC — is the one most worth having afterwards, and the
  // previous implementation discarded exactly those: it bumped a counter and
  // dropped the bytes, so "what was that?" had no answer anywhere.
  ecm::telemetry::FrameRecord rec;
  // Run-relative, so `t` means the same thing here as it does in the captured
  // frames the host tooling already parses.
  rec.t_ms = received_ms - this->boot_t0_ms_;
  rec.dir = ecm::telemetry::DIR_D0;
  const size_t n = len < ecm::telemetry::RAW_CAP ? len : ecm::telemetry::RAW_CAP;
  rec.raw_len = static_cast<uint8_t>(n);
  std::memcpy(rec.raw, buf, n);
  if (n < len) {
    rec.flags |= ecm::telemetry::flag::TRUNCATED;
  }

  ecm::Frame frame;
  if (!ecm::parse_request(buf, len, frame)) {
    // Bad start byte, too short, or CRC mismatch. STAY SILENT: the ECM parks
    // its exchange counter and resyncs on its own, so silence is safe, and
    // guessing on a gas appliance is not.
    this->crc_errors_++;
    rec.crc = trailer_crc_(buf, len);
    this->record_(rec);
    return;
  }

  rec.flags |= ecm::telemetry::flag::CRC_OK;
  rec.crc = trailer_crc_(buf, len);
  rec.dev = frame.dev;
  rec.seq = frame.seq;
  rec.cmd = frame.cmd;

  // The ECM resets its exchange counter to 0 after a UART overrun or a
  // desynchronised byte stream. Observe that here, and ONLY observe it:
  // nothing below validates or predicts seq.
  // Excluded: the first frame after our own boot (no previous seq to compare)
  // and the 0x0320 boot frame, which `Ecm Resets` already counts. A normal wrap
  // is 0xFF -> 0x00, so it is excluded too; a reset is any other jump to 0.
  if (this->have_seq_ && frame.seq == 0 && this->last_seq_ != 0xFF &&
      !ecm::is_boot_frame(frame)) {
    this->ecm_seq_resets_++;
  }
  this->last_seq_ = frame.seq;
  this->have_seq_ = true;

  rec.flags |= ecm::is_known_frame(frame) ? ecm::telemetry::flag::KNOWN_CMD
                                            : ecm::telemetry::flag::UNKNOWN_CMD;

  // Census BEFORE the early return below. This is the whole point of the
  // exercise: an unknown command gets its bytes, length and payload recorded,
  // instead of vanishing behind a counter.
  this->note_notable_(this->inventory_.observe(rec), rec);

  this->ecm_frames_++;
  this->last_valid_frame_ms_ = received_ms;
  this->have_link_ = true;

  if (!ecm::is_known_frame(frame)) {
    this->unknown_cmds_++;
    this->record_(rec);
    return;  // unknown command — counted and recorded, never guessed at
  }

  if (ecm::is_boot_frame(frame)) {
    // 0x0320 is the ECM's power-on frame (seq=0x00) and appears nowhere else.
    // Match the full word: schedule announcement 0x0220 shares the low byte.
    // We count it and reply to it like any other frame; note that a reboot does
    // NOT reset our intent, so a command a human asked for still lands once the
    // ECM is back — that is the user's intent persisting, not a boot-time write.
    this->ecm_resets_++;
  }

  if (ecm::has_status_fields(frame)) {
    this->handle_status_(frame);
  }

  ecm::Slot slot = ecm::slot_idle();
  if (frame.cmd == ecm::cmd::WRITE_CARRIER) {
    // At most one write, ever. The order here is the whole point: the clock and
    // the write-enable pin are read NOW, microseconds before the slot is built,
    // and both are passed in rather than looked up inside the policy. A switch
    // flipped since the last loop() is therefore honoured on this poll, and the
    // reconciler needs no board to be tested.
    //
    // Three gates before the policy is consulted at all:
    //  * `payload_len >= 2`  — build_reply() cannot answer a shorter frame, so
    //    consulting the policy would spend an attempt on a reply that is never
    //    transmitted (F9: false "sent", then a spurious fault latch).
    //  * `have_status_`      — never write against an appliance we have not yet
    //    observed. A command accepted in the window after boot would otherwise
    //    ride the first 0xBC, which can precede the first 0xDC.
    //  * freshness           — after a loop stall, a poll the assembler delivers
    //    seconds late must not carry a write across the ECM's counter
    //    park/resync.
    const uint32_t now = millis();
    const bool replyable = frame.payload_len >= 2;
    const bool observed = this->have_status_;
    const bool fresh = (now - received_ms) <= STALE_WRITE_MS;
    if (replyable && observed && fresh) {
      const bool write_enabled = this->write_enabled();
      slot = this->reconciler_.slot_for_bc_reply(now, write_enabled);
      this->apply_outcome_();
    }
  }

  // The reply goes out first and the recording happens after it, so the
  // transmit path keeps its timing and the telemetry rides along behind.
  ecm::telemetry::FrameRecord tx;
  const uint32_t latency = this->send_slot_(frame, slot, received_ms, &tx);
  if (latency != ecm::telemetry::NO_LATENCY) {
    rec.latency_ms = static_cast<uint16_t>(latency);
    rec.flags |= ecm::telemetry::flag::REPLIED;
  }
  // RX before TX, so a dump reads in the order the exchange actually happened.
  this->record_(rec);
  if (tx.raw_len != 0) {
    this->inventory_.observe(tx);
    this->record_(tx);
  }
}

void FireplaceECM::handle_status_(const ecm::Frame &frame) {
  // The ECM's own status report is the only acknowledgement this component
  // accepts. There is no separate ack frame in the protocol.
  this->pilot_ = (ecm::status_byte(frame, ecm::status::PILOT) != 0) ? ecm::Tri::On : ecm::Tri::Off;
  this->status_flag9_ = ecm::status_byte(frame, ecm::status::FLAG_9) != 0;
  this->have_status_ = true;

  // Bytes 6 and 8. The data path is high confidence; the human labels are not,
  // so these are exposed as
  // diagnostics and nothing acts on them. They are also the values the E0/E2
  // announcements carry, but the announcements are not acks and are not
  // consulted: the status frame remains the single source of truth.
  this->last_config_byte_ = static_cast<int16_t>(ecm::status_byte(frame, ecm::status::CONFIG));
  this->last_heat_demand_ = static_cast<int16_t>(ecm::status_byte(frame, ecm::status::HEAT_DEMAND));

  const uint8_t height = ecm::status_byte(frame, ecm::status::HEIGHT);

  ecm::EcmObservation observation;
  observation.flame = (ecm::status_byte(frame, ecm::status::FLAME) != 0) ? ecm::Tri::On : ecm::Tri::Off;
  // Byte 5 is always present in a 0xDC frame, so the pilot is never "unknown"
  // once a status frame has arrived. `pilot_` IS the read-back mirror, and it is
  // handed to the reconciler from the same single read, so entity and decision
  // cannot drift apart.
  observation.pilot = this->pilot_;
  // -1 means "this frame carried no usable height", which leaves the stored
  // height alone — the same thing `if (height <= 4)` did here before.
  observation.height_internal = (height <= 4) ? static_cast<int8_t>(height) : static_cast<int8_t>(-1);

  // The reconciler decides on this observation; the read-back entities mirror
  // it. Both come from the same bytes in the same place, so they cannot drift.
  this->last_flame_ = observation.flame;
  if (observation.height_internal >= 0) {
    this->last_height_ = observation.height_internal;
  }

  this->reconciler_.observe(observation);
  this->apply_outcome_();
}

// ---------------------------------------------------------------------------
// Outcome reporting
// ---------------------------------------------------------------------------
//
// The decision itself is in include/ecm_reconciler.h. All that is left here is
// wording: the reconciler returns a typed outcome, and this maps it onto the
// string the text sensor shows.

void FireplaceECM::apply_outcome_() {
  const ecm::Outcome outcome = this->reconciler_.last_outcome();
  char message[64];

  switch (outcome.kind) {
    case ecm::OutcomeKind::Sent:
      this->set_outcome_("sent");
      return;
    case ecm::OutcomeKind::Retrying:
      snprintf(message, sizeof(message), "retrying (attempt %u)", (unsigned) outcome.attempts);
      this->set_outcome_(message);
      return;
    case ecm::OutcomeKind::Confirmed:
      snprintf(message, sizeof(message), "confirmed in %u attempt%s", (unsigned) outcome.attempts,
               outcome.attempts == 1 ? "" : "s");
      this->set_outcome_(message);
      return;
    case ecm::OutcomeKind::RejectedSwitchOpen:
      this->set_outcome_("rejected: write switch open");
      return;
    case ecm::OutcomeKind::Faulted:
      snprintf(message, sizeof(message), "failed after %u attempts", (unsigned) outcome.attempts);
      this->set_outcome_(message);
      return;
    case ecm::OutcomeKind::Superseded:
      this->set_outcome_("superseded (intent changed)");
      return;
    case ecm::OutcomeKind::FlameWithoutPilot:
      // Deliberately its own message: the flame write still went out (this
      // firmware does not enforce the pilot-first ordering — Home Assistant's
      // automation.fireplace_keypad_control owns that policy), so without this
      // the poll would read as an ordinary "sent" and the reason a fire did not
      // light would never appear anywhere.
      if (outcome.attempts <= 1) {
        this->set_outcome_("flame requested but pilot reports off");
      } else {
        snprintf(message, sizeof(message), "flame requested but pilot reports off (attempt %u)",
                 (unsigned) outcome.attempts);
        this->set_outcome_(message);
      }
      return;
    case ecm::OutcomeKind::None:
    default:
      // Nothing happened on this poll — idle. Deliberately leaves the previous
      // text alone, so a quiet poll cannot erase the last thing that DID happen.
      return;
  }
}

uint32_t FireplaceECM::send_slot_(const ecm::Frame &frame, const ecm::Slot &slot,
                                  uint32_t received_ms, ecm::telemetry::FrameRecord *tx_out) {
  uint8_t reply[ecm::MAX_REPLY_LEN];
  const size_t len = ecm::build_reply(frame, &slot, reply, sizeof(reply));
  if (len == 0) {
    // Nothing legal to send — stay silent. NO_LATENCY (65535) doubles as "no
    // reply here"; the real budget is ~160 ms, so the sentinel cannot collide
    // with a measurement.
    return ecm::telemetry::NO_LATENCY;
  }

  // 12 bytes at 9600-8-N-1 drain in ~12.5 ms, inside the >= 158 ms the ECM
  // leaves before its next poll. No flush() here: blocking the loop for the
  // whole transmit buys nothing and the budget does not need it.
  this->write_array(reply, len);
  this->frames_replied_++;

  const uint32_t latency = millis() - received_ms;
  if (latency > this->reply_latency_ms_) {
    this->reply_latency_ms_ = latency;
  }

  // The TX record is built from the bytes we just handed to the UART, so
  // recording the reply costs no bus time at all — it is the D1 stream for
  // free, and it is what makes "did we actually say anything?" answerable.
  if (tx_out != nullptr) {
    tx_out->t_ms = millis() - this->boot_t0_ms_;
    tx_out->dir = ecm::telemetry::DIR_D1;
    tx_out->raw_len = static_cast<uint8_t>(len);
    std::memcpy(tx_out->raw, reply, len);
    tx_out->crc = trailer_crc_(reply, len);
    tx_out->dev = reply[1];
    tx_out->seq = reply[2];
    tx_out->cmd = reply[3];
    // Set by construction, not inferred: we built every byte of this frame.
    tx_out->flags |= ecm::telemetry::flag::CRC_OK | ecm::telemetry::flag::KNOWN_CMD;
  }
  return latency;
}

// ---------------------------------------------------------------------------
// Runtime MQTT broker configuration
// ---------------------------------------------------------------------------
//
// ESPHome compiles the broker in and its client has no public "reconnect" —
// start_connect_() is private, and the socket is only re-established after the
// current one drops. Changing the credentials therefore cannot take effect
// while connected, so applying a change requires a reboot. The values are
// stored in flash first, which is what makes the reboot worth it.
//
// ⚠ ORDERING, and why apply_stored_mqtt_config() is NOT called from setup():
// ESPHome's codegen emits the compiled-in set_broker_address()/set_username()/
// ... calls during the MQTT component's own setup, at priority AFTER_WIFI
// (200). This component runs at BUS - 1 (999), i.e. EARLIER, so anything we
// restored in setup() would simply be overwritten a moment later. The restore
// has to happen after the MQTT component's setup, which is why the YAML calls
// it from `esphome: on_boot:` at a priority below 200. If that wiring is ever
// dropped, this stops working silently — the device would connect to the
// compiled-in broker instead, and the stored values would look ignored.

#ifdef USE_MQTT
namespace {
/// Flash-stored broker override. A POD struct rather than std::string members,
/// which is the form ESPHome's preference store handles without ceremony.
struct MqttPrefs {
  char broker[64];
  char username[32];
  char password[64];
  uint16_t port;
  uint8_t valid;
};
constexpr uint32_t PREF_MQTT = 0x0ec1a001u;
}  // namespace
#endif

void FireplaceECM::set_mqtt_config(const std::string &broker, uint16_t port,
                                   const std::string &username, const std::string &password) {
#ifdef USE_MQTT
  MqttPrefs p{};
  // snprintf rather than strncpy: it always terminates, so an over-long value
  // is truncated rather than leaving an unterminated buffer in flash.
  std::snprintf(p.broker, sizeof(p.broker), "%s", broker.c_str());
  std::snprintf(p.username, sizeof(p.username), "%s", username.c_str());
  std::snprintf(p.password, sizeof(p.password), "%s", password.c_str());
  p.port = port != 0 ? port : 1883;
  p.valid = 1;

  auto pref = global_preferences->make_preference<MqttPrefs>(PREF_MQTT);
  const bool saved = pref.save(&p);
  global_preferences->sync();

  // Say so before rebooting: after the reboot the log that explained it is gone.
  ESP_LOGW(TAG, "MQTT broker stored (%s:%u) — rebooting to apply, because the client "
                "cannot reconnect in place. Save %s.",
           p.broker, static_cast<unsigned>(p.port), saved ? "ok" : "FAILED");
  if (saved) {
    App.safe_reboot();
  }
#else
  (void) broker;
  (void) port;
  (void) username;
  (void) password;
  ESP_LOGE(TAG, "set_mqtt_config called but this build has no MQTT component");
#endif
}

void FireplaceECM::apply_stored_mqtt_config() {
#ifdef USE_MQTT
  auto *client = mqtt::global_mqtt_client;
  if (client == nullptr) {
    return;
  }
  MqttPrefs p{};
  auto pref = global_preferences->make_preference<MqttPrefs>(PREF_MQTT);
  if (!pref.load(&p) || p.valid != 1 || p.broker[0] == '\0') {
    // Nothing stored: the compiled-in values stand. This is the state a wiped
    // device returns to, and it is the documented escape hatch if a stored
    // broker turns out to be unreachable.
    return;
  }
  client->set_broker_address(p.broker);
  client->set_broker_port(p.port);
  client->set_username(p.username);
  client->set_password(p.password);
  ESP_LOGI(TAG, "Applied stored MQTT broker override: %s:%u", p.broker,
           static_cast<unsigned>(p.port));
#endif
}

// ---------------------------------------------------------------------------
// Telemetry
// ---------------------------------------------------------------------------
//
// Everything below is deliberately OUTSIDE the reply path. handle_frame_ only
// records; the publishing happens here, called from the end of loop(), after
// any reply has already been written.

namespace {
/// Topic root. Deliberately independent of ESPHome's mqtt.topic_prefix: this is
/// a diagnostic stream with its own consumer (tools/ecm_collector.py), not an
/// entity namespace, and it should not move if the device is renamed.
constexpr const char *TOPIC_ROOT = "fireplace/ecm";
constexpr size_t CHUNK_RECORDS = 8;   ///< records per dump chunk
constexpr uint32_t SUMMARY_MS = 30000;
constexpr uint32_t INVENTORY_MS = 60000;
constexpr size_t MAX_PUBLISH_PER_LOOP = 4;
}  // namespace

uint16_t FireplaceECM::trailer_crc_(const uint8_t *buf, size_t len) {
  if (buf == nullptr || len < 2) {
    return 0;
  }
  return static_cast<uint16_t>(buf[len - 2] | (buf[len - 1] << 8));
}

bool FireplaceECM::mqtt_ready_() const {
#ifdef USE_MQTT
  auto *client = mqtt::global_mqtt_client;
  return client != nullptr && client->is_connected();
#else
  return false;
#endif
}

bool FireplaceECM::publish_raw_(const char *topic, const char *payload, bool retain) {
#ifdef USE_MQTT
  auto *client = mqtt::global_mqtt_client;
  if (client == nullptr || !client->is_connected()) {
    return false;
  }
  // The result matters: a queue rejection is a dropped record, and the caller
  // counts it as such. Ignoring it made publish_skipped_ understate losses.
  return client->publish(topic, payload, std::strlen(payload), 0, retain);
#else
  (void) topic;
  (void) payload;
  (void) retain;
  return false;
#endif
}

void FireplaceECM::note_notable_(uint8_t notable, const ecm::telemetry::FrameRecord &rec) {
  namespace T = ecm::telemetry;

  // The flags ride on the record whether or not an alert goes out, so a dump
  // taken later still shows which frames were the interesting ones.
  if ((notable & T::notable::NEW_KEY) != 0) {
    const_cast<T::FrameRecord &>(rec).flags |= T::flag::NEW_KEY;
  }
  if ((notable & T::notable::UNEXPECTED_LEN) != 0) {
    const_cast<T::FrameRecord &>(rec).flags |= T::flag::UNEXPECTED_LEN;
  }

  const char *kind = nullptr;
  if ((notable & T::notable::NEW_SLOT_OP) != 0) {
    // The one the frame-command check can never see. Highest precedence,
    // because a new WRITE opcode is the most consequential of the three.
    kind = "new_slot_op";
  } else if ((notable & T::notable::NEW_KEY) != 0) {
    kind = (rec.flags & T::flag::UNKNOWN_CMD) != 0 ? "new_unknown_cmd" : "new_key";
  } else if ((notable & T::notable::UNEXPECTED_LEN) != 0) {
    kind = "unexpected_len";
  } else if ((notable & T::notable::KEY_TABLE_FULL) != 0) {
    kind = "census_full";
  }
  if (kind == nullptr || !this->mqtt_ready_()) {
    return;
  }

  // One alert per new key, never per occurrence, and the payload carries the
  // bytes themselves — an alert that says only "something new happened" is
  // useless for working out what the new opcode does.
  char hex[3 * T::RAW_CAP + 1];
  size_t h = 0;
  const size_t n = rec.raw_len <= T::RAW_CAP ? rec.raw_len : T::RAW_CAP;
  for (size_t i = 0; i < n && h + 3 < sizeof(hex); ++i) {
    static const char *D = "0123456789ABCDEF";
    hex[h++] = D[(rec.raw[i] >> 4) & 0xF];
    hex[h++] = D[rec.raw[i] & 0xF];
    hex[h++] = ' ';
  }
  if (h > 0) {
    h--;
  }
  hex[h] = '\0';

  char payload[512];
  std::snprintf(payload, sizeof(payload),
                "{\"t\":%.3f,\"boot\":\"%08X\",\"kind\":\"%s\",\"dir\":\"%s\","
                "\"dev\":\"0x%02X\",\"cmd\":\"0x%02X\",\"len\":%u,\"hex\":\"%s\"}",
                static_cast<double>(rec.t_ms) / 1000.0, static_cast<unsigned>(this->boot_id_),
                kind, rec.channel(), rec.dev, rec.cmd, static_cast<unsigned>(rec.raw_len), hex);

  char topic[80];
  std::snprintf(topic, sizeof(topic), "%s/telemetry/alert", TOPIC_ROOT);
  this->publish_raw_(topic, payload, false);
}

void FireplaceECM::publish_tick_() {
  // Nothing to do, and nothing to wait for. Checking first means a broker that
  // is down costs one comparison per loop rather than a timeout.
  if (!this->mqtt_ready_()) {
    const uint32_t pending = this->records_pushed_ - this->records_published_;
    if (pending > 0 && !this->ring_.full()) {
      // Count the loss rather than letting it look like the bus went quiet.
      this->records_published_ = this->records_pushed_;
      this->publish_skipped_ += pending;
    }
    return;
  }

  // Static, not stack: the main-task stack size is an sdkconfig default and the
  // worst-case record serializes to ~1094 bytes (G8). 2048 covers it.
  static char line[2048];
  char topic[80];
  size_t budget = MAX_PUBLISH_PER_LOOP;

  // 1. Live frames, oldest first. Bounded per iteration so a burst can never
  //    hold the loop open.
  while (budget > 0 && this->records_published_ < this->records_pushed_) {
    // If the ring wrapped past what we had not sent, those records are gone.
    // Say so and resynchronise rather than publishing the wrong ones.
    if (this->records_pushed_ - this->records_published_ > this->ring_.count()) {
      this->publish_skipped_ +=
          static_cast<uint32_t>(this->records_pushed_ - this->records_published_ - this->ring_.count());
      this->records_published_ = this->records_pushed_ - this->ring_.count();
    }
    const size_t offset = this->records_published_ - (this->records_pushed_ - this->ring_.count());
    const ecm::telemetry::FrameRecord &rec = this->ring_.at(offset);
    if (ecm::telemetry::to_json(rec, line, sizeof(line)) == 0) {
      // A record that still does not fit must not park the stream: skip it and
      // count the loss, rather than `break`-ing on the same record forever.
      this->records_published_++;
      this->publish_skipped_++;
      continue;
    }
    std::snprintf(topic, sizeof(topic), "%s/frame", TOPIC_ROOT);
    if (!this->publish_raw_(topic, line, false)) {
      this->publish_skipped_++;
    }
    this->records_published_++;
    budget--;
  }

  // 2. One dump chunk per iteration while a dump is running.
  if (this->dump_state_ != DumpState::Idle) {
    this->publish_tick_dump_();
  } else if (this->dump_requested_) {
    this->dump_requested_ = false;
    if (this->ring_.count() > 0) {
      this->dump_state_ = DumpState::Header;
      this->dump_cursor_ = 0;
    }
  }

  // 3. Periodic aggregates.
  const uint32_t now = millis();
  if (now - this->last_summary_ms_ >= SUMMARY_MS) {
    this->last_summary_ms_ = now;
    this->emit_summary_();
  }
  if (now - this->last_inventory_ms_ >= INVENTORY_MS) {
    this->last_inventory_ms_ = now;
    this->emit_inventory_();
  }
}

void FireplaceECM::publish_tick_dump_() {
  const size_t count = this->ring_.count();
  char topic[80];

  switch (this->dump_state_) {
    case DumpState::Header: {
      char payload[256];
      const size_t chunks = (count + CHUNK_RECORDS - 1) / CHUNK_RECORDS;
      std::snprintf(payload, sizeof(payload),
                    "{\"boot\":\"%08X\",\"records\":%u,\"chunk_count\":%u,\"chunk_records\":%u}",
                    static_cast<unsigned>(this->boot_id_), static_cast<unsigned>(count),
                    static_cast<unsigned>(chunks), static_cast<unsigned>(CHUNK_RECORDS));
      std::snprintf(topic, sizeof(topic), "%s/telemetry/dump/header", TOPIC_ROOT);
      this->publish_raw_(topic, payload, false);
      this->dump_state_ = DumpState::Chunks;
      return;
    }
    case DumpState::Chunks: {
      if (this->dump_cursor_ >= count) {
        this->dump_state_ = DumpState::Footer;
        return;
      }
      static char payload[CHUNK_RECORDS * 512 + 32];
      size_t used = 0;
      payload[used++] = '[';
      size_t in_chunk = 0;
      while (this->dump_cursor_ < count && in_chunk < CHUNK_RECORDS) {
        // Static, not stack (G8); 2048 covers the measured worst case.
        static char one[2048];
        if (ecm::telemetry::to_json(this->ring_.at(this->dump_cursor_), one, sizeof(one)) == 0) {
          // Skip the un-serializable record and make progress. `break` here
          // left dump_cursor_ parked, underflowed the chunk index below, and
          // republished the same empty chunk every loop.
          this->dump_cursor_++;
          continue;
        }
        const size_t l = std::strlen(one);
        if (used + l + 2 >= sizeof(payload)) {
          break;  // never emit a truncated record; the footer will not come
        }
        if (in_chunk > 0) {
          payload[used++] = ',';
        }
        std::memcpy(payload + used, one, l);
        used += l;
        in_chunk++;
        this->dump_cursor_++;
      }
      payload[used++] = ']';
      payload[used] = '\0';
      const size_t chunk_index =
          this->dump_cursor_ == 0 ? 0 : (this->dump_cursor_ - 1) / CHUNK_RECORDS;
      std::snprintf(topic, sizeof(topic), "%s/telemetry/dump/chunk/%u", TOPIC_ROOT,
                    static_cast<unsigned>(chunk_index));
      if (!this->publish_raw_(topic, payload, false)) {
        this->publish_skipped_++;
      }
      return;
    }
    case DumpState::Footer: {
      char payload[128];
      std::snprintf(payload, sizeof(payload), "{\"boot\":\"%08X\",\"complete\":true}",
                    static_cast<unsigned>(this->boot_id_));
      std::snprintf(topic, sizeof(topic), "%s/telemetry/dump/footer", TOPIC_ROOT);
      this->publish_raw_(topic, payload, false);
      this->dump_state_ = DumpState::Idle;
      return;
    }
    default:
      this->dump_state_ = DumpState::Idle;
      return;
  }
}

void FireplaceECM::emit_summary_() {
  char payload[640];
  std::snprintf(payload, sizeof(payload),
                "{\"boot\":\"%08X\",\"uptime_ms\":%u,\"frames_parsed\":%u,"
                // Two separate faults, reported separately. The `Crc Errors`
                // entity sums them (it always has); a rising total alone cannot
                // tell you which one you are looking at.
                "\"frames_parse_errors\":%u,\"framing_overflows\":%u,\"framing_resyncs\":%u,"
                "\"frames_unknown\":%u,\"frames_replied\":%u,\"reply_latency_max_ms\":%u,"
                "\"ring_count\":%u,\"ring_capacity\":%u,\"records_evicted\":%u,"
                "\"records_pushed\":%u,\"records_published\":%u,\"publish_skipped\":%u,"
                "\"seq_resets\":%u,"
                "\"census_keys\":%u,\"census_slot_ops\":%u,\"census_overflow\":%u}",
                static_cast<unsigned>(this->boot_id_), static_cast<unsigned>(millis()),
                static_cast<unsigned>(this->ecm_frames_), static_cast<unsigned>(this->parse_errors()),
                static_cast<unsigned>(this->framing_overflows()),
                static_cast<unsigned>(this->assembler_.resyncs()),
                static_cast<unsigned>(this->unknown_cmds_),
                static_cast<unsigned>(this->frames_replied_),
                static_cast<unsigned>(this->reply_latency_ms_),
                static_cast<unsigned>(this->ring_.count()),
                static_cast<unsigned>(this->ring_.capacity()),
                static_cast<unsigned>(this->ring_.evicted()),
                static_cast<unsigned>(this->records_pushed_),
                static_cast<unsigned>(this->records_published_),
                static_cast<unsigned>(this->publish_skipped_),
                static_cast<unsigned>(this->ecm_seq_resets_),
                static_cast<unsigned>(this->inventory_.key_count()),
                static_cast<unsigned>(this->inventory_.slot_op_count()),
                static_cast<unsigned>(this->inventory_.overflowed()));
  char topic[80];
  std::snprintf(topic, sizeof(topic), "%s/telemetry/summary", TOPIC_ROOT);
  this->publish_raw_(topic, payload, true);
}

void FireplaceECM::emit_inventory_() {
  // ONE key per tick, not the whole census at once. A full census runs to tens
  // of kilobytes — nearly all of it per-index value sets — which is far past
  // what one MQTT payload or one stack buffer should carry. Walking a single
  // key per tick also spreads the refresh over many loop iterations instead of
  // stalling one of them.
  //
  // Each key is published RETAINED to its own topic, so a consumer that
  // subscribes later still sees the current census without waiting a full
  // refresh. The count of keys lives in the summary topic.
  constexpr size_t TOTAL = ecm::telemetry::MAX_KEYS + ecm::telemetry::MAX_SLOT_OPS;
  for (size_t tries = 0; tries < TOTAL; ++tries) {
    const size_t i = this->inventory_cursor_ % TOTAL;
    this->inventory_cursor_ = (i + 1) % TOTAL;

    static char one[2048];
    char slug[40];
    char topic[96];

    if (i < ecm::telemetry::MAX_KEYS) {
      const ecm::telemetry::KeyStats &k = this->inventory_.key(i);
      if (!k.used) {
        continue;
      }
      if (!ecm::telemetry::key_to_json(k, one, sizeof(one))) {
        continue;  // would not fit; skip rather than emit a partial object
      }
      std::snprintf(slug, sizeof(slug), "%s_0x%02X_0x%02X", k.channel(), k.dev, k.cmd);
    } else {
      const ecm::telemetry::SlotOpStats &s = this->inventory_.slot_op(i - ecm::telemetry::MAX_KEYS);
      if (!s.used) {
        continue;
      }
      const char *d = s.dir == ecm::telemetry::DIR_D1 ? "D1" : "D0";
      std::snprintf(one, sizeof(one),
                    "{\"key\":\"slot/%s/0x%02X/0x%02X\",\"namespace\":\"slot_op\","
                    "\"occurrences\":%u,\"known\":%s,\"first_ms\":%u,\"last_ms\":%u}",
                    d, s.dev, s.op, static_cast<unsigned>(s.occurrences),
                    ecm::telemetry::is_known_slot_op(s.op) ? "true" : "false",
                    static_cast<unsigned>(s.first_ms), static_cast<unsigned>(s.last_ms));
      std::snprintf(slug, sizeof(slug), "slot_%s_0x%02X_0x%02X", d, s.dev, s.op);
    }
    std::snprintf(topic, sizeof(topic), "%s/inventory/%s", TOPIC_ROOT, slug);
    this->publish_raw_(topic, one, true);
    return;
  }
}

void FireplaceECM::set_outcome_(const std::string &text) {
  if (this->last_outcome_ != text) {
    this->last_outcome_ = text;
  }
}

// ---------------------------------------------------------------------------
// Intent
// ---------------------------------------------------------------------------

void FireplaceECM::request_flame(bool on) {
  // Records intent only. Clamping, the drop of any superseded flame command,
  // and every other rule live in the reconciler's request_flame().
  this->reconciler_.request_flame(on);
  this->apply_outcome_();
}

void FireplaceECM::request_pilot(bool on) {
  // Records intent only, exactly like request_flame(). NOTE: this does not gate
  // the flame in either direction — see the reconciler's header. A flame-ON
  // aimed at a pilot the ECM reports off still goes out, and reports the
  // distinct "flame requested but pilot reports off" outcome.
  this->reconciler_.request_pilot(on);
  this->apply_outcome_();
}

void FireplaceECM::request_height_app(int app_height) {
  this->reconciler_.request_height_app(app_height);
  this->apply_outcome_();
}

void FireplaceECM::clear_fault() {
  this->reconciler_.clear_fault();
  this->set_outcome_("fault cleared");
}

// ---------------------------------------------------------------------------
// Read-backs
// ---------------------------------------------------------------------------

float FireplaceECM::height_app() const {
  if (this->last_height_ < 0) {
    return NAN;  // no status frame yet — unknown, not zero
  }
  return static_cast<float>(this->last_height_ + 1);  // internal -> app 1-5
}

bool FireplaceECM::link_alive() const {
  if (!this->have_link_) {
    return false;
  }
  return (millis() - this->last_valid_frame_ms_) < LINK_TIMEOUT_MS;
}

bool FireplaceECM::write_enabled() {
#ifdef USE_ESP32
  if (this->write_enable_pin_ < 0 || this->write_enable_pin_ >= 32) {
    return false;  // unwired or misconfigured: fail safe to DISABLED
  }
  // Inverted by design: the sense line is pulled UP, and DPST pole B pulls it
  // to ground only when the switch is closed (pole A closed = TX connected).
  // So LOW = enabled, and every failure mode (open switch, cut wire, failed-open
  // switch) reads HIGH = DISABLED.
  return gpio_get_level(static_cast<gpio_num_t>(this->write_enable_pin_)) == 0;
#else
  return false;  // unsupported target: fail safe to DISABLED
#endif
}

}  // namespace fireplace_ecm
}  // namespace esphome

#!/usr/bin/env python3
"""Durable host-side collector for the ESPHome ECM telemetry stream.

READ-ONLY OBSERVATION. This tool never transmits on the bus, never opens a
serial port, and imports nothing that could. It subscribes to an MQTT broker
and writes what the ESPHome device publishes. The device is the only thing on
this link that touches the appliance.

WHY THIS EXISTS
---------------
The ESP32 standing in for the Wi-Fi module publishes every CRC-validated bus
frame on <prefix>/frame. MQTT is a live, lossy transport: a broker restart, a
dropped subscriber, or the retained-message window all lose frames silently,
and none of that is visible afterwards. The analysis tools (ecm_diff,
ecm_session) need a durable per-boot record on disk, in the layout the harness
already writes, so those tools read it unchanged.

Layout written, one directory per boot_id:

    <out>/<run_id>/
      frames_D0.jsonl         every frame the device published, verbatim
      frames_D1.jsonl
      inventory.jsonl         one line per census key publish received
      unknown_commands.jsonl  {t, key, hex, slot_op?} per newly seen opcode
      alerts.jsonl            device alerts (new key / new slot op / bad length)
      manifest.jsonl          {boot_id, started_wall, elapsed_s, gaps, reboots}
      run.json                path-compatible with ecm_harness.py

plus <out>/sweep-<stamp>-index.jsonl, readable by ecm_diff.load_sweep_index.

WHAT THE DEVICE PUBLISHES
-------------------------
The topic root is FIXED at `fireplace/ecm` (TOPIC_ROOT in
firmware/components/fireplace_ecm/fireplace_ecm.cpp) and deliberately does NOT
follow ESPHome's mqtt.topic_prefix: the telemetry feed has its own consumer and
must not move when the device is renamed.

    <root>/frame                 one Frame.to_json() line + `boot`
    <root>/status                retained "online"/"offline" (MQTT LWT)
    <root>/telemetry/summary     retained counters, incl. census_keys /
                                 census_slot_ops / census_overflow
    <root>/inventory/<slug>      RETAINED, ONE KEY PER PUBLISH
    <root>/telemetry/dump/...    header / chunk/<n> / footer
    <root>/telemetry/alert       new key, new slot op, or unexpected length

There is NO <root>/boot topic. Each frame line carries `"boot"` (8 uppercase
hex chars), and that field alone is what keys a run — see Collector.handle_frame.

The inventory is per key and not a single array: one key's value sets can run to
~18 KB, so the device walks one key per refresh tick and publishes it retained
to its own topic. A complete census therefore ACCUMULATES over several ticks,
and the summary's `census_keys` + `census_slot_ops` are the authority on how
many entries to expect. The first snapshot received is not a complete census.
Slot opcodes are a SEPARATE namespace (`"namespace": "slot_op"`, with `known`)
and are never folded into the frame key table. `values_truncated: true` means
`values_observed` was capped at 16 values per byte index and is NOT the complete
set.

A NEW RUN DIRECTORY PER boot_id, never frames merged across a boot boundary.
ecm_session.py's Timeline.per_segment already records what merging costs it:
integrity evaluated across a discontinuity reports every boundary as a fault.
The device is the same story — a boot restarts its seq counter and its clock,
so a merged file would carry two frames of reference in one `t` column. The
tool also starts a fresh run on its own restart, and records the gap it left.

TWO CENSUS CHECKS, NOT ONE. ecm_session.py flags a frame whose `cmd` is
outside KNOWN_CMDS. That cannot see a new WRITE OPCODE: an operator command
travels in the 0xBC reply's payload slot, never in a frame's cmd byte, so a
novel write opcode is invisible to a cmd-only census. So this tool also checks
payload[2] of every 0xBC frame against the known slot opcodes (0x00 idle,
0x64 flame, 0x65 pilot, 0x67 height) and flags anything else.

Usage:
    ./ecm_collector.py --broker 192.0.2.10          # root fireplace/ecm
    ./ecm_collector.py --broker localhost --out captures/live
    ./ecm_collector.py --replay canned.jsonl --out /tmp/replay  # no broker
    ./ecm_collector.py --selftest                              # no broker, no paho

The replay file is JSONL. Each line is either a bare frame record (the
device's own payload shape) or {"topic": "...", "payload": ...} to drive a
non-frame topic. --selftest builds one in a temp dir and asserts the output.
"""
from __future__ import annotations

import argparse
import json
import os
import signal
import sys
import time
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
#: Reuse the session tool's opcode sets rather than redefining them. KNOWN_CMDS
#: is the command-byte census; WRITE_OPS is the set of opcodes the module puts
#: in the 0xBC reply slot, which is the check that actually catches a new operator
#: command. Both live in one place so the two tools cannot drift apart.
from ecm_session import KNOWN_CMDS, WRITE_OPS  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(HERE, "captures")
#: The device's fixed topic root (TOPIC_ROOT in fireplace_ecm.cpp). It is NOT
#: ESPHome's mqtt.topic_prefix — this feed must not move when the node is
#: renamed. --prefix only exists so a second device or a bridge can be read.
DEFAULT_PREFIX = "fireplace/ecm"
DEFAULT_BROKER = "localhost"
DEFAULT_PORT = 1883

#: payload[2] of a 0xBC frame, plus the two known names from ecm_session.py.
#: 0x00 is the idle slot the module sends when it has nothing to write.
KNOWN_SLOT_OPS: dict[int, str] = {0x00: "idle", **WRITE_OPS}

#: Inter-frame silence worth recording in the manifest. The idle bus is not
#: quiet for anywhere near this long (ecm_session uses the same 2 s figure for
#: "module silence"), so a gap this size means something actually stopped.
DEFAULT_GAP_S = 2.0

#: Bound on the per-run frame-identity set used to deduplicate dump backfill.
#: Past this the run is long enough that the memory matters more than the
#: dedupe, and the tool says so rather than growing without limit.
SEEN_CAP = 1_000_000

#: Cap on retained side-topic messages held before the first frame arrives.
#: The census alone is one message per key, so this is generous; past it the
#: oldest are dropped and counted.
PENDING_CAP = 20_000

CHANNELS = ("D0", "D1")

#: A boot id we do not know yet. A frame can beat its retained boot topic; if
#: that happens the run adopts the real id rather than splitting in two.
UNKNOWN_BOOT = "unknown"


def iso_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def stamp_now() -> str:
    return datetime.now().strftime("%Y%m%d-%H%M%S")


def _slug(s: str) -> str:
    """A boot id goes into a directory name, so keep it to safe characters."""
    out = "".join(c if c.isalnum() or c in "-_" else "-" for c in str(s or ""))
    return out.strip("-")[:40] or UNKNOWN_BOOT


class SeqTracker:
    """Incremental seq-discontinuity counts for one channel of one run.

    Mirrors ecm_session.seq_events, and exists for the same reason: the counter
    wraps at 256 and the ECM PARKS it at 0 after a reply timeout, so a naive
    "seq went down" count conflates three different events. Splitting them here
    means a stall is reported as one park entry rather than as a run of
    anomalies, which is the error that inflated the first live capture's count.

        wrap    0xFF -> 0x00   the counter rolling over. A normal step.
        park    X    -> 0x00   the ECM giving up on a reply (a stall starts)
        bounce  0x01 -> 0x00   a second reset while recovering from a stall
        held    delta 0        the counter parked; the frames it sits at 0
        skipped anything else  a real discontinuity - the only anomaly
    """

    def __init__(self) -> None:
        self.prev: int | None = None
        self.steps = 0
        self.held = 0
        self.park = 0
        self.bounce = 0
        self.skipped = 0
        self.saw_ff = False
        self.saw_00 = False

    def add(self, seq: int) -> None:
        if seq == 0xFF:
            self.saw_ff = True
        if seq == 0x00:
            self.saw_00 = True
        prev = self.prev
        self.prev = seq
        if prev is None:
            return
        self.steps += 1
        delta = (seq - prev) & 0xFF
        if delta == 0:
            self.held += 1
        elif seq == 0x00:
            # Lands on zero: a park entry, or a bounce if it was already there.
            if prev == 0x01:
                self.bounce += 1
            else:
                self.park += 1
        elif delta != 1:
            self.skipped += 1

    def to_json(self, prefix: str) -> dict:
        return {
            f"{prefix}_steps": self.steps,
            f"{prefix}_held": self.held,
            f"{prefix}_park_entries": self.park,
            f"{prefix}_skipped": self.skipped,
            "wrap_observed": self.saw_ff and self.saw_00,
        }


class RunWriter:
    """One run directory: one boot_id's frames, plus the per-run side files.

    Every file is opened append-only and line-buffered, so a line on disk is a
    line that survives a hard kill. run.json is written at run start as well as
    at close, so a run dir is loadable even if the tool never got to shut down.
    """

    def __init__(self, out_root: str, run_id: str, boot_id: str,
                 reset_reason: str | None = None, fw: str | None = None,
                 gap_s: float = DEFAULT_GAP_S) -> None:
        self.run_id = run_id
        self.boot_id = boot_id
        self.dir = os.path.join(out_root, run_id)
        os.makedirs(self.dir, exist_ok=True)
        self.started_wall = iso_now()
        self.started_mono = time.monotonic()
        self.reset_reason = reset_reason
        self.fw = fw
        self.gap_s = gap_s

        self.counts: dict[str, int] = {ch: 0 for ch in CHANNELS}
        self.seq = {ch: SeqTracker() for ch in CHANNELS}
        self.gaps: list[dict] = []
        self.reboots: list[dict] = []
        self.notes: list[str] = []
        self.census: dict[str, int] = {}
        self.device_summary: dict | None = None
        self.first_t: float | None = None
        self.last_t: float | None = None
        self.last_frame_mono: float | None = None
        self.seen: set = set()
        self.seen_capped = False
        self.dump_records = 0
        self.valid = False
        #: Set by the Collector when this run follows a tool restart.
        self.restart_gap_s: float | None = None
        #: Latest census entry per slug. The census is walked one key per tick,
        #: so this is an ACCUMULATION, not a snapshot — completeness is checked
        #: against the summary's counts at close, never assumed.
        self.inventory: dict[str, dict] = {}
        self.inventory_truncated = 0
        self.expected_census: int | None = None
        self.census_overflow = False

        self._frames = {
            ch: open(os.path.join(self.dir, f"frames_{ch}.jsonl"), "a",
                     encoding="utf-8", buffering=1)
            for ch in CHANNELS
        }
        self._inventory = open(os.path.join(self.dir, "inventory.jsonl"), "a",
                               encoding="utf-8", buffering=1)
        self._unknown = open(os.path.join(self.dir, "unknown_commands.jsonl"),
                             "a", encoding="utf-8", buffering=1)
        self._alerts = open(os.path.join(self.dir, "alerts.jsonl"), "a",
                            encoding="utf-8", buffering=1)
        self._manifest = open(os.path.join(self.dir, "manifest.jsonl"), "a",
                              encoding="utf-8", buffering=1)
        self._write_run_json()
        self._write_manifest(reason=None, final=False)

    # -- frame path --------------------------------------------------------

    def write_frame(self, line: str, rec: dict) -> None:
        """Append one frame payload to its channel file, VERBATIM.

        The device already emits Frame.to_json() plus its additive keys, so any
        rewriting here would be a second, divergent definition of the record
        shape. `line` is written exactly as it arrived. The identity is also
        remembered, so a later dump backfill can tell which records it already
        has.
        """
        ch = rec.get("channel", "D0")
        if ch not in CHANNELS:
            self.notes.append(f"frame with unknown channel {ch!r} kept on D0")
            ch = "D0"
        self._frames[ch].write(line if line.endswith("\n") else line + "\n")
        self.counts[ch] += 1
        self.valid = True

        t = rec.get("t")
        if isinstance(t, (int, float)):
            if self.first_t is None:
                self.first_t = float(t)
            self.last_t = float(t)
        seq = rec.get("seq")
        if isinstance(seq, int):
            self.seq[ch].add(seq & 0xFF)

        key = rec.get("key") or self._key_of(rec)
        self.census[key] = self.census.get(key, 0) + 1

        now = time.monotonic()
        if self.last_frame_mono is not None:
            gap = now - self.last_frame_mono
            if gap >= self.gap_s:
                self.gaps.append({
                    "t": round(t, 3) if isinstance(t, (int, float)) else None,
                    "gap_s": round(gap, 2)})
        self.last_frame_mono = now
        self._remember(rec)

    def identity(self, rec: dict) -> tuple:
        """A boot-stable identity for a record, for dump deduplication.

        Keyed on fields the device cannot vary for the same wire observation.
        `index` and `t` are deliberately excluded: a re-published record may
        legitimately carry a different publish index.
        """
        return (rec.get("channel"), rec.get("boot"), rec.get("dev"),
                rec.get("seq"), rec.get("cmd"),
                rec.get("hex") or tuple(rec.get("raw") or ()))

    def _remember(self, rec: dict) -> None:
        if self.seen_capped:
            return
        if len(self.seen) >= SEEN_CAP:
            self.seen_capped = True
            self.notes.append(
                "frame-identity set hit its cap; dump dedupe is now best-effort")
            return
        self.seen.add(self.identity(rec))

    def already_seen(self, rec: dict) -> bool:
        if self.seen_capped:
            return False
        if self.identity(rec) in self.seen:
            return True
        self._remember(rec)
        return False

    def write_frame_from_dump(self, line: str, rec: dict) -> bool:
        """Write a dump record unless an identical one is already on disk."""
        if self.already_seen(rec):
            return False
        self.dump_records += 1
        self.write_frame(line, rec)
        return True

    @staticmethod
    def _key_of(rec: dict) -> str:
        def _h(v) -> str:
            if isinstance(v, str):
                return v
            return f"0x{int(v):02X}" if v is not None else "0x??"
        return f"{rec.get('channel', 'D0')}/{_h(rec.get('dev'))}/{_h(rec.get('cmd'))}"

    # -- side files --------------------------------------------------------

    def write_inventory(self, slug: str, entry: dict) -> None:
        """One line per census key publish.

        `inventory.jsonl` is a log of what arrived, so it keeps every publish
        even though `self.inventory` keeps only the latest per slug: a value set
        that shrinks or a count that moves is visible in the log and invisible
        in the aggregate.
        """
        self.inventory[slug] = entry
        if entry.get("values_truncated"):
            self.inventory_truncated += 1
        self._inventory.write(json.dumps({
            "t": self.elapsed(), "wall": iso_now(),
            "boot_id": self.boot_id, "slug": slug, "entry": entry}) + "\n")

    def write_unknown(self, key: str, hexs: str = "", slot_op: int | None = None,
                      seq: int | None = None, source: str = "frame") -> None:
        # Shapes documented in the module docstring: {t, key, hex, slot_op?}.
        # `source` is additive: a finding recovered from the retained census has
        # no frame bytes behind it, so `hex` is empty and says where it came from.
        row = {"t": self.elapsed(), "key": key, "hex": hexs}
        if slot_op is not None:
            row["slot_op"] = f"0x{slot_op:02X}"
        if isinstance(seq, int):
            row["seq"] = f"0x{seq:02X}"
        if source != "frame":
            row["source"] = source
        self._unknown.write(json.dumps(row) + "\n")

    def write_alert(self, alert) -> None:
        self._alerts.write(json.dumps({
            "t": self.elapsed(), "wall": iso_now(),
            "boot_id": self.boot_id, "alert": alert}) + "\n")

    # -- close -------------------------------------------------------------

    def elapsed(self) -> float:
        return round(time.monotonic() - self.started_mono, 3)

    def _integrity(self) -> dict | None:
        out = {}
        for ch in CHANNELS:
            if not self.counts[ch]:
                continue
            out[ch] = {"frames": self.counts[ch],
                       "seq_advance": self.seq[ch].to_json(ch.lower())}
        return out or None

    def run_json(self) -> dict:
        """The run.json body, in ecm_harness.RunResult.to_json() shape.

        `marks` is always empty: this tool is passive observation and there is
        no write path, so there is nothing for a mark to describe. A mark-less
        run is what ecm_diff treats as an idle CONTROL, which is the honest
        classification for a passive capture.
        """
        notes = list(self.notes)
        if self.first_t is not None and abs(self.first_t) > 5.0:
            # ecm_diff.load_run documents `t` as seconds relative to the run's
            # capture start. We write the device's `t` through unchanged, so if
            # the device did not rebase at boot this is the place that says so.
            notes.append(
                f"first frame t={self.first_t:.3f}s is not near zero — the "
                f"device did not rebase `t` to this run's start, and "
                f"ecm_diff.load_run assumes it did")
        return {
            "run_id": self.run_id,
            "started_wall": self.started_wall,
            "duration_s": self.elapsed(),
            "marks": [],
            "valid": self.valid,
            "verified": True,
            "invalid_reason": None if self.valid else "NO_FRAMES",
            "integrity": self._integrity(),
            "frame_counts": {ch: n for ch, n in self.counts.items() if n},
            "reboots": self.reboots,
            "errors": [],
            "run_dir": os.path.abspath(self.dir),
            "notes": notes,
            "discard": False,
            # additive: not part of the harness shape, ignored by its loaders
            "collector": {
                "boot_id": self.boot_id,
                "fw": self.fw,
                "reset_reason": self.reset_reason,
                "device_summary": self.device_summary,
                "dump_records_written": self.dump_records,
                "census_keys_received": len(self.inventory),
                "census_expected": self.expected_census,
                "census_complete": (self.expected_census is not None and
                                    len(self.inventory) >= self.expected_census),
                "census_overflow": self.census_overflow,
                "values_truncated_keys": self.inventory_truncated,
            },
        }

    def _write_run_json(self) -> None:
        with open(os.path.join(self.dir, "run.json"), "w",
                  encoding="utf-8") as fh:
            json.dump(self.run_json(), fh, indent=2)

    def _write_manifest(self, reason: str | None, final: bool) -> None:
        """Append the run's manifest row.

        Written once at start (so the run is visible while it is live) and once
        at close, which is the row a consumer should read: the start row has a
        zero elapsed_s and empty gaps/reboots.
        """
        row = {
            "run_id": self.run_id,
            "boot_id": self.boot_id,
            "started_wall": self.started_wall,
            "ended_wall": iso_now() if final else None,
            "elapsed_s": self.elapsed(),
            "frames": dict(self.counts),
            "gaps": list(self.gaps),
            "reboots": list(self.reboots),
            "final": final,
            "reason": reason,
            "notes": list(self.notes),
        }
        self._manifest.write(json.dumps(row) + "\n")

    def close(self, reason: str) -> dict:
        """Flush, write run.json + the manifest close row, and return the
        record that goes into the sweep index.

        A tool restart is a hole in coverage exactly like a boot is, and it is
        invisible in the frames alone, so it is recorded as the first entry in
        `gaps` rather than left as an unexplained absence.
        """
        if self.restart_gap_s is not None:
            self.gaps.insert(0, {"t": None, "gap_s": round(self.restart_gap_s, 2),
                                 "kind": "tool_restart"})
            self.notes.append(
                f"tool restart: {self.restart_gap_s:.1f}s between the previous "
                f"run's end and this one's start — no frames were collected in "
                f"that window")
        # The census is walked one key per tick, so a short run legitimately
        # ends with fewer keys than the device holds. Say so, rather than
        # leaving a consumer to read a partial census as the whole one.
        if self.expected_census is not None and len(self.inventory) < self.expected_census:
            self.notes.append(
                f"census incomplete: {len(self.inventory)} of "
                f"{self.expected_census} keys received — the device walks one "
                f"key per refresh tick, so a short run never sees them all")
        if self.inventory_truncated:
            self.notes.append(
                f"{self.inventory_truncated} census key(s) report "
                f"values_truncated=true: their values_observed lists are capped "
                f"at 16 values per byte index and are NOT the complete set")
        if self.census_overflow:
            self.notes.append(
                "device reports census_overflow — its key table is full, so the "
                "census is incomplete at the source and new keys are not tracked")
        for fh in list(self._frames.values()) + [self._inventory,
                                                 self._unknown, self._alerts]:
            try:
                fh.flush()
            except Exception:
                pass
        rec = self.run_json()
        self._write_run_json()
        self._write_manifest(reason=reason, final=True)
        for fh in list(self._frames.values()) + [self._inventory, self._unknown,
                                                 self._alerts, self._manifest]:
            try:
                fh.close()
            except Exception:
                pass
        return rec


class Collector:
    """Routes the device's MQTT topics into run directories on disk.

    Every topic in one place, so the routing is auditable against the device's
    published layout rather than spread through callbacks.
    """

    def __init__(self, out_root: str, prefix: str = DEFAULT_PREFIX,
                 quiet: bool = False, verbose: bool = False,
                 gap_s: float = DEFAULT_GAP_S) -> None:
        self.out_root = os.path.abspath(out_root)
        os.makedirs(self.out_root, exist_ok=True)
        self.prefix = prefix.rstrip("/")
        self.quiet = quiet
        self.verbose = verbose
        self.gap_s = gap_s
        self.stamp = stamp_now()
        self.sweep_path = os.path.join(self.out_root,
                                       f"sweep-{self.stamp}-index.jsonl")
        # Created empty at start so the index exists even if no run closes.
        open(self.sweep_path, "a", encoding="utf-8").close()

        self.run: RunWriter | None = None
        self.index_records: list[dict] = []
        self.census: dict[str, int] = {}
        self.unknown_keys: set[str] = set()
        self.unknown_slot_ops: set[int] = set()
        self.topic_counts: dict[str, int] = {}
        self.last_summary: dict | None = None
        self.device_online: bool | None = None
        self.dump: dict | None = None
        self.dump_truncated = 0
        self.dump_completed = 0
        self.dump_foreign = 0
        self.malformed = 0
        #: Run-scoped messages that arrived before any frame had established a
        #: run. Retained topics are delivered the instant we subscribe, which
        #: is always before the next bus exchange, so without this buffer the
        #: whole census would be dropped on every connect.
        self.pending: list[tuple[str, str]] = []
        self.pending_dropped = 0
        self._pending_reset_reason: str | None = None
        #: The gap this tool left between the previous process and this one.
        #: Carried onto the first run's manifest so a hole in coverage is never
        #: just an absence.
        self.restart_gap_s = self._previous_gap_s()

    # -- logging -----------------------------------------------------------

    def log(self, msg: str) -> None:
        if not self.quiet:
            print(msg, flush=True)

    def log_debug(self, msg: str) -> None:
        if self.verbose and not self.quiet:
            print(msg, flush=True)

    # -- run lifecycle -----------------------------------------------------

    def _previous_gap_s(self) -> float | None:
        """Seconds since the newest previous run in this output root ended.

        A tool restart is a hole in the record exactly like a boot is, and it
        is invisible in the frames alone, so it is measured here and carried
        into the new run's manifest.
        """
        newest: float | None = None
        try:
            entries = os.listdir(self.out_root)
        except OSError:
            return None
        for name in entries:
            rj = os.path.join(self.out_root, name, "run.json")
            if not os.path.isfile(rj):
                continue
            try:
                with open(rj, encoding="utf-8") as fh:
                    rec = json.load(fh)
                start = datetime.fromisoformat(rec["started_wall"]).timestamp()
                end = start + float(rec.get("duration_s") or 0.0)
            except Exception:
                continue
            if newest is None or end > newest:
                newest = end
        if newest is None:
            return None
        return max(0.0, time.time() - newest)

    def _new_run_id(self, boot_id: str) -> str:
        base = f"{self.stamp}-{_slug(boot_id)}"
        candidate, n = base, 1
        while os.path.exists(os.path.join(self.out_root, candidate)):
            n += 1
            candidate = f"{base}-{n}"
        return candidate

    def start_run(self, boot_id: str, reason: str,
                  reset_reason: str | None = None,
                  fw: str | None = None) -> None:
        if self.run is not None:
            self.close_run(reason)
        run_id = self._new_run_id(boot_id)
        self.run = RunWriter(self.out_root, run_id, boot_id,
                             reset_reason=reset_reason, fw=fw, gap_s=self.gap_s)
        # Carried onto the run so close() can record it in the manifest's gaps.
        self.run.restart_gap_s = self.restart_gap_s
        self.restart_gap_s = None
        self.log(f"run {run_id}: boot_id={boot_id} ({reason})")

        # Now that a run exists, apply what arrived before it did. Retained
        # topics are delivered at subscribe time, which is invariably before
        # the next bus exchange, so this is the normal path and not a corner.
        if self.pending:
            pending, self.pending = self.pending, []
            for suf, pl in pending:
                self._dispatch(suf, pl)
            self.log(f"replayed {len(pending)} retained message(s) that "
                     f"arrived before the first frame")
        if self.pending_dropped:
            self.run.notes.append(
                f"{self.pending_dropped} retained message(s) were dropped from "
                f"the pre-run buffer (cap {PENDING_CAP})")

    def adopt_boot(self, boot_id: str, reset_reason: str | None = None,
                   fw: str | None = None) -> None:
        """Give a still-unnamed run its real boot id, without splitting it.

        A frame can beat its retained boot topic by a few milliseconds. Starting
        a second run for the same boot would manufacture exactly the false
        boundary this tool exists to avoid, so the placeholder id is replaced
        instead. The directory name keeps the placeholder; run.json is
        authoritative for the id.
        """
        if self.run is None:
            return
        self.log(f"run {self.run.run_id}: adopted boot_id={boot_id} "
                 f"(placeholder was {self.run.boot_id!r})")
        self.run.boot_id = boot_id
        self.run.reset_reason = reset_reason or self.run.reset_reason
        self.run.fw = fw or self.run.fw

    def close_run(self, reason: str,
                  next_boot: str | None = None) -> dict | None:
        if self.run is None:
            return None
        run = self.run
        if next_boot:
            run.reboots.append({
                "at_s": run.elapsed(),
                "from_boot_id": run.boot_id,
                "to_boot_id": next_boot,
                "reset_reason": self._pending_reset_reason,
            })
        # A dump that never got its footer is NOT treated as a complete
        # backfill: the footer is the only statement that every chunk arrived,
        # and a partial ring-buffer dump that silently looked complete would
        # understate what is missing. It is recorded, not written.
        if self.dump is not None:
            self.dump_truncated += 1
            run.notes.append(
                f"dump of boot {self.dump.get('boot_id')} truncated: "
                f"{len(self.dump.get('chunks', {}))} of "
                f"{self.dump.get('chunk_count')} chunks arrived with no footer "
                f"— its records were NOT written")
            self.dump = None
        # Before the run is sealed, so the rows land in this run's file.
        self._flag_census_findings(run)
        rec = run.close(reason)
        self.index_records.append(rec)
        with open(self.sweep_path, "a", encoding="utf-8") as fh:
            fh.write(json.dumps(rec) + "\n")
        self.log(f"closed {run.run_id} after {rec['duration_s']:.1f}s "
                 f"({reason}); frames={rec['frame_counts'] or 'none'}")
        self.run = None
        return rec

    # -- topic routing -----------------------------------------------------

    #: Topics whose content belongs to a run, and so cannot be acted on until a
    #: run exists. Everything else (frames, status, dump framing) is either
    #: self-establishing or collector-scoped.
    RUN_SCOPED = ("telemetry/summary", "telemetry/alert")

    @classmethod
    def _run_scoped(cls, suffix: str) -> bool:
        return suffix.startswith("inventory/") or suffix in cls.RUN_SCOPED

    def handle(self, topic: str, payload: str) -> None:
        suffix = self._suffix(topic)
        self.topic_counts[suffix] = self.topic_counts.get(suffix, 0) + 1
        # Retained side-topic messages arrive the moment we subscribe, which is
        # before any frame has established a run. Buffer them and let start_run
        # replay them, rather than discard the census on every connect.
        if self.run is None and self._run_scoped(suffix):
            if len(self.pending) >= PENDING_CAP:
                self.pending.pop(0)
                self.pending_dropped += 1
            self.pending.append((suffix, payload))
            self.log_debug(f"{suffix}: buffered (no run yet)")
            return
        self._dispatch(suffix, payload)

    def _dispatch(self, suffix: str, payload: str) -> None:
        try:
            if suffix == "frame":
                self.handle_frame(payload)
            elif suffix == "boot":
                # The device publishes no boot topic today; each frame carries
                # its own `boot` field and that is what keys a run. This stays
                # wired in case a boot topic appears later.
                self.handle_boot(payload)
            elif suffix == "status":
                self.handle_status(payload)
            elif suffix == "telemetry/summary":
                self.handle_summary(payload)
            elif suffix.startswith("inventory/"):
                self.handle_inventory_key(suffix[len("inventory/"):], payload)
            elif suffix == "inventory":
                self.log("*** <root>/inventory received, but the device "
                         "publishes the census per key under inventory/<slug> "
                         "— this is not one of its topics ***")
            elif suffix == "telemetry/alert":
                self.handle_alert(payload)
            elif suffix == "telemetry/dump/header":
                self.handle_dump_header(payload)
            elif suffix.startswith("telemetry/dump/chunk/"):
                self.handle_dump_chunk(suffix.rsplit("/", 1)[-1], payload)
            elif suffix == "telemetry/dump/footer":
                self.handle_dump_footer(payload)
            else:
                self.log_debug(f"unrouted topic {topic!r}")
        except Exception as exc:                      # never die on one message
            self.malformed += 1
            self.log(f"*** {suffix}: {type(exc).__name__}: {exc} ***")

    def _suffix(self, topic: str) -> str:
        if topic == self.prefix:
            return ""
        if topic.startswith(self.prefix + "/"):
            return topic[len(self.prefix) + 1:]
        # A replay line may name the suffix directly; live topics never do.
        return topic

    # -- messages ----------------------------------------------------------

    def handle_frame(self, payload: str) -> None:
        line = payload.strip("\n").strip()
        if not line:
            return
        rec = json.loads(line)
        if not isinstance(rec, dict):
            raise ValueError("frame payload is not a JSON object")
        boot = rec.get("boot")

        if self.run is None:
            self.start_run(boot or UNKNOWN_BOOT, reason="first frame")
        elif boot and boot != self.run.boot_id:
            if self.run.boot_id == UNKNOWN_BOOT:
                self.adopt_boot(boot)
            else:
                # The boot changed under us: a new run, never a merge. This is
                # the boundary the device itself declares, and the
                # reset_reason on the boot topic is what makes it legible
                # rather than a bare gap.
                self.close_run("boot_change", next_boot=boot)
                self.start_run(boot, reason="boot_change",
                               reset_reason=self._pending_reset_reason)

        self.run.write_frame(line, rec)
        self._census_check(rec)

    def _census_check(self, rec: dict) -> None:
        key = rec.get("key") or RunWriter._key_of(rec)
        self.census[key] = self.census.get(key, 0) + 1
        t = rec.get("t")
        seq = rec.get("seq")
        cmd = rec.get("cmd")
        cmd_i = int(cmd, 16) if isinstance(cmd, str) else cmd
        seq_s = f" seq=0x{seq:02X}" if isinstance(seq, int) else ""

        if cmd_i is not None and cmd_i not in KNOWN_CMDS:
            if key not in self.unknown_keys:
                self.unknown_keys.add(key)
                self.run.write_unknown(key, rec.get("hex", ""), seq=seq)
                self.log(f"*** FINDING *** new command byte {key} at t={t}"
                         f"{seq_s}  {rec.get('hex', '')}")

        # The check ecm_session.py does NOT do: a write opcode lives in the
        # 0xBC reply's payload slot, so a novel one never appears as a cmd
        # byte. Gate on D1 because that is the module's write direction and its
        # slot is the one that carries a command (ecm_session.write_slot).
        if cmd_i == 0xBC and rec.get("channel") == "D1":
            pl = rec.get("payload") or []
            if len(pl) >= 3:
                op = pl[2]
                if isinstance(op, int) and op not in KNOWN_SLOT_OPS:
                    if op not in self.unknown_slot_ops:
                        self.unknown_slot_ops.add(op)
                        self.run.write_unknown(key, rec.get("hex", ""),
                                               slot_op=op, seq=seq)
                        self.log(f"*** FINDING *** new SLOT opcode 0x{op:02X} in "
                                 f"{key} at t={t}{seq_s}  {rec.get('hex', '')}")

    def handle_boot(self, payload: str) -> None:
        """A boot announcement, IF one is ever published.

        The device publishes no such topic today — `boot` rides on every frame
        line and handle_frame is what actually keys a run. This path is kept
        because it is the only place a `reset_reason` could come from, and it
        costs nothing while unused.
        """
        msg = json.loads(payload)
        boot_id = msg.get("boot_id") or UNKNOWN_BOOT
        self._pending_reset_reason = msg.get("reset_reason")
        if self.run is None:
            self.start_run(boot_id, reason="boot topic",
                           reset_reason=msg.get("reset_reason"), fw=msg.get("fw"))
        elif boot_id != self.run.boot_id:
            if self.run.boot_id == UNKNOWN_BOOT:
                self.adopt_boot(boot_id, reset_reason=msg.get("reset_reason"),
                                fw=msg.get("fw"))
            else:
                self.close_run("boot_change", next_boot=boot_id)
                self.start_run(boot_id, reason="boot_change",
                               reset_reason=msg.get("reset_reason"),
                               fw=msg.get("fw"))
        else:
            self.run.fw = msg.get("fw") or self.run.fw
        self.log(f"boot: id={boot_id} reason={msg.get('reset_reason')} "
                 f"fw={msg.get('fw')}")

    def handle_status(self, payload: str) -> None:
        status = payload.strip().strip('"')
        self.device_online = (status == "online")
        # LWT is the broker telling us the device died; that is a real coverage
        # gap, so it is noted on the run rather than merely logged.
        if self.run is not None and status != "online":
            self.run.notes.append(
                f"device status {status!r} at t={self.run.elapsed()}s")
        self.log(f"status: {status}")

    def handle_summary(self, payload: str) -> None:
        msg = json.loads(payload)
        self.last_summary = msg
        # The summary is retained, so a connect that lands just after a device
        # reboot can be handed the PREVIOUS boot's counters. They would set a
        # wrong expected census size and a wrong loss count, so they are
        # refused rather than applied to a boot they do not describe.
        sboot = msg.get("boot")
        if self.run is not None and sboot and \
                self.run.boot_id not in (UNKNOWN_BOOT, sboot):
            self.run.notes.append(
                f"retained summary for boot {sboot} ignored: this run is boot "
                f"{self.run.boot_id}")
            self.log(f"summary for boot {sboot} ignored (run is "
                     f"{self.run.boot_id})")
            return
        if self.run is not None:
            self.run.device_summary = msg
            # Evicted/skipped counts are the device's own admission of loss;
            # they bound what this run can possibly contain.
            ev = msg.get("records_evicted") or 0
            sk = msg.get("publish_skipped") or 0
            if ev or sk:
                self.run.notes.append(
                    f"device reports records_evicted={ev} "
                    f"publish_skipped={sk} — frames exist that this run never "
                    f"saw; a dump backfill is the only way to recover them")
            # The summary is the AUTHORITY on how many census keys exist: the
            # device walks one key per tick, so no single inventory message can
            # say whether the census is complete.
            ck = msg.get("census_keys")
            cs = msg.get("census_slot_ops")
            if isinstance(ck, int) or isinstance(cs, int):
                self.run.expected_census = (ck or 0) + (cs or 0)
            if msg.get("census_overflow"):
                self.run.census_overflow = True
        self.log(f"summary: uptime={msg.get('uptime_ms')}ms "
                 f"parsed={msg.get('frames_parsed')} "
                 f"unknown={msg.get('frames_unknown')} "
                 f"evicted={msg.get('records_evicted')} "
                 f"census={msg.get('census_keys')}k+"
                 f"{msg.get('census_slot_ops')}slot")

    def handle_inventory_key(self, slug: str, payload: str) -> None:
        """One census key, published retained to its own topic.

        The census accumulates one key per refresh tick, so this is an upsert
        and never a complete snapshot. The retained messages mean a subscriber
        that connects late still receives the current census for every key the
        device holds — which is also how a finding can be recovered after the
        frame that produced it scrolled past, so the entries are checked for
        findings here too, not only on the live frame path.
        """
        entry = json.loads(payload)
        if not isinstance(entry, dict):
            raise ValueError("inventory entry is not a JSON object")
        if self.run is None:
            self.log_debug(f"inventory {slug} with no live run — not written")
            return
        self.run.write_inventory(slug, entry)
        # Findings are NOT raised here. A census entry has no frame bytes, and
        # an opcode the bus is still carrying will supply them a moment later;
        # flagging now would produce a hex-less row and then have to either
        # duplicate it or lose the bytes. They are raised at run close instead,
        # for whatever the frames never supplied. See _flag_census_findings.
        note = entry.get("namespace") == "slot_op" and not entry.get("known", True)
        if note or entry.get("role") == "unknown":
            self.log_debug(f"inventory {slug}: unknown opcode noted, will be "
                           f"flagged at close if no frame supplies it")
        self.log_debug(f"inventory {slug}: updated "
                       f"({len(self.run.inventory)} key(s) so far)")

    def _flag_census_findings(self, run: RunWriter) -> None:
        """Flag census opcodes that no live frame ever supplied.

        Deferred to close on purpose. A frame carries the raw bytes, so if the
        opcode is still on the bus the frame will supply them; only at close is
        it certain the census is all we will ever have. That keeps one row per
        opcode, always carrying the best evidence available, and it is how an
        opcode that occurred BEFORE this tool connected is still recovered —
        the retained census is the only record of it.
        """
        for slug, entry in run.inventory.items():
            if entry.get("namespace") == "slot_op":
                op = self._slot_op_of(entry, slug)
                if op is None or entry.get("known", True) \
                        or op in self.unknown_slot_ops:
                    continue
                self.unknown_slot_ops.add(op)
                run.write_unknown(entry.get("key", slug), slot_op=op,
                                  source="inventory")
                self.log(f"*** FINDING *** new SLOT opcode 0x{op:02X} "
                         f"(census only, {entry.get('occurrences')} "
                         f"occurrence(s)) — no frame bytes for it in this run")
            else:
                key = entry.get("key")
                if entry.get("role") != "unknown" or key in self.unknown_keys:
                    continue
                self.unknown_keys.add(key)
                run.write_unknown(key, source="inventory")
                self.log(f"*** FINDING *** new command byte {key} "
                         f"(census only, {entry.get('occurrences')} "
                         f"occurrence(s)) — no frame bytes for it in this run")

    @staticmethod
    def _slot_op_of(entry: dict, slug: str) -> int | None:
        """The write opcode from a slot_op census entry.

        The entry's `key` is `slot/<DIR>/0x<DEV>/0x<OP>`; the slug is the same
        with underscores. Both are parsed, because either could be missing.
        """
        for candidate in (entry.get("key"), slug):
            if not isinstance(candidate, str):
                continue
            parts = candidate.replace("_", "/").split("/")
            if len(parts) >= 4 and parts[-1].lower().startswith("0x"):
                try:
                    return int(parts[-1], 16)
                except ValueError:
                    continue
        return None

    def handle_alert(self, payload: str) -> None:
        try:
            alert = json.loads(payload)
        except json.JSONDecodeError:
            alert = payload
        if self.run is not None:
            self.run.write_alert(alert)
        self.log(f"*** ALERT *** {alert}")

    # -- dump backfill -----------------------------------------------------

    def handle_dump_header(self, payload: str) -> None:
        msg = json.loads(payload)
        self.dump = {"boot_id": msg.get("boot_id"),
                     "chunk_count": msg.get("chunk_count"), "chunks": {}}
        self.log(f"dump begins: boot={msg.get('boot_id')} "
                 f"chunks={msg.get('chunk_count')}")

    def handle_dump_chunk(self, n: str, payload: str) -> None:
        if self.dump is None:
            self.log(f"dump chunk {n} with no header — ignored")
            return
        records = json.loads(payload)
        if isinstance(records, (list, dict)):
            self.dump["chunks"][n] = records
        self.log_debug(f"dump chunk {n}: "
                       f"{len(records) if hasattr(records, '__len__') else '?'} "
                       f"records")

    def handle_dump_footer(self, payload: str) -> None:
        """Only a footer makes a dump complete. Records are written only when
        the dump matches the current run's boot: a dump from another boot would
        merge two frames of reference into one run directory, which is the one
        thing the run boundary exists to prevent."""
        if self.dump is None:
            self.log("dump footer with no header — ignored")
            return
        dump, self.dump = self.dump, None
        self.dump_completed += 1
        if self.run is None:
            self.log(f"dump for boot {dump.get('boot_id')} with no live run "
                     f"— not written")
            return
        if dump.get("boot_id") not in (None, self.run.boot_id):
            self.dump_foreign += 1
            self.run.notes.append(
                f"dump for foreign boot {dump.get('boot_id')} discarded: it "
                f"does not belong to this run's boot {self.run.boot_id}")
            self.log(f"dump for foreign boot {dump.get('boot_id')} discarded")
            return

        added = 0
        for n in sorted(dump["chunks"], key=lambda x: (len(x), x)):
            records = dump["chunks"][n]
            if not isinstance(records, list):
                continue
            for rec in records:
                if not isinstance(rec, dict):
                    continue
                if self.run.write_frame_from_dump(json.dumps(rec), rec):
                    added += 1
        self.log(f"dump complete: {added} record(s) backfilled, "
                 f"duplicates skipped")

    # -- summary -----------------------------------------------------------

    def census_frames(self, ch: str) -> int:
        return sum(r["frame_counts"].get(ch, 0) for r in self.index_records)

    def finish(self, reason: str) -> None:
        self.close_run(reason)
        if self.quiet:
            return
        print("\n--- collector summary ---------------------------------")
        print(f"sweep index : {self.sweep_path}")
        print(f"runs        : {len(self.index_records)}")
        print(f"frames      : D0={self.census_frames('D0')} "
              f"D1={self.census_frames('D1')}")
        if self.census:
            print("census:")
            for k, v in sorted(self.census.items(), key=lambda kv: -kv[1]):
                print(f"  {v:>6}  {k}")
        if self.unknown_keys:
            print(f"*** {len(self.unknown_keys)} new command key(s): "
                  f"{sorted(self.unknown_keys)}")
        if self.unknown_slot_ops:
            print(f"*** {len(self.unknown_slot_ops)} new slot opcode(s): "
                  f"{[f'0x{o:02X}' for o in sorted(self.unknown_slot_ops)]}")
        print(f"dumps       : completed={self.dump_completed} "
              f"truncated={self.dump_truncated} foreign={self.dump_foreign}")
        if self.malformed:
            print(f"malformed   : {self.malformed} message(s)")


# --------------------------------------------------------------------------
# offline replay
# --------------------------------------------------------------------------

def replay_file(collector: Collector, path: str) -> None:
    """Drive the collector from a canned JSONL file. No broker, no paho.

    A line that names a `topic` is routed like a live message; a bare line is
    the device's own frame record and is routed to <prefix>/frame, VERBATIM —
    the raw text is handed through, not re-serialized.
    """
    with open(path, encoding="utf-8") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.rstrip("\n")
            if not line.strip():
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                collector.log(f"replay:{lineno}: not JSON, ignored")
                continue
            if isinstance(obj, dict) and "topic" in obj:
                payload = obj.get("payload")
                text = payload if isinstance(payload, str) else json.dumps(payload)
                collector.handle(str(obj["topic"]), text)
            else:
                collector.handle(f"{collector.prefix}/frame", line)


# --------------------------------------------------------------------------
# live MQTT
# --------------------------------------------------------------------------

def _import_mqtt():
    """Import paho lazily so every offline path runs without it installed."""
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        raise SystemExit(
            "paho-mqtt is required to connect to a broker but is not "
            "installed.\n"
            "  install:  python3 -m pip install paho-mqtt\n"
            "  offline:  ./ecm_collector.py --replay <file.jsonl> --out <dir>\n"
            "            ./ecm_collector.py --selftest")
    return mqtt


def run_live(collector: Collector, broker: str, port: int,
             username: str | None, password: str | None,
             client_id: str | None) -> None:
    mqtt = _import_mqtt()

    def on_connect(client, userdata, flags, rc, properties=None) -> None:
        if rc != 0:
            print(f"broker refused the connection (rc={rc})", file=sys.stderr)
            return
        # Subscribing to the root catches frame, status, telemetry/# and
        # inventory/+ (one retained topic per census key) in one go, including
        # topics added on the device later. The retained census and summary are
        # delivered immediately, before the first frame — the collector buffers
        # them until a run exists rather than dropping them.
        client.subscribe(f"{collector.prefix}/#", qos=1)
        print(f"subscribed {collector.prefix}/#  ->  {collector.out_root}",
              flush=True)

    def on_message(client, userdata, msg) -> None:
        try:
            payload = msg.payload.decode("utf-8", "replace")
        except Exception as exc:
            collector.log(f"*** undecodable payload on {msg.topic}: {exc} ***")
            return
        collector.handle(msg.topic, payload)

    client = mqtt.Client(client_id=client_id or f"ecm-collector-{stamp_now()}")
    if username:
        client.username_pw_set(username, password)
    client.on_connect = on_connect
    client.on_message = on_message

    while True:
        try:
            client.connect(broker, port, keepalive=30)
            break
        except Exception as exc:
            print(f"broker {broker}:{port} not reachable ({exc}); retrying in "
                  f"5s", file=sys.stderr, flush=True)
            time.sleep(5)

    try:
        client.loop_forever()
    finally:
        try:
            client.disconnect()
        except Exception:
            pass


# --------------------------------------------------------------------------
# selftest
# --------------------------------------------------------------------------

class _Results:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0
        self.failures: list[str] = []

    def check(self, ok: bool, label: str, detail: str = "") -> bool:
        if ok:
            self.passed += 1
            print(f"  PASS  {label}")
        else:
            self.failed += 1
            self.failures.append(label)
            print(f"  FAIL  {label}" + (f"\n          {detail}" if detail else ""))
        return ok

    def summary(self) -> int:
        print(f"\n{'=' * 62}")
        if self.failed:
            print(f"RESULT: {self.failed} FAILED, {self.passed} passed")
            for f in self.failures:
                print(f"  - {f}")
        else:
            print(f"RESULT: all {self.passed} checks passed")
        print("=" * 62)
        return 1 if self.failed else 0


#: Boot ids are 8 uppercase hex chars on the wire (snprintf("%08X", ...)); the
#: fixtures use realistic ones so the run-directory naming is exercised too.
BOOT_A = "3F2A1B0C"
BOOT_B = "9E8D7C6B"


def make_frame(ch: str, dev: int, seq: int, cmd: int, payload: list[int],
               t: float, boot: str | None = None) -> dict:
    """One frame record in the device's exact shape: Frame.to_json() plus
    `boot`. No CRC: the device validated the frame before publishing, and
    nothing downstream re-checks it from this line."""
    from ecm_frames import Frame

    d = Frame(channel=ch, t=t, dev=dev, seq=seq, cmd=cmd,
              payload=list(payload), raw=[0x4B, dev, seq, cmd] + list(payload),
              crc=0, index=0).to_json()
    if boot is not None:
        d["boot"] = boot
    return d


def fixture_frames() -> list[dict]:
    """A short canned exchange on two boots: an idle rotation, an unknown cmd
    byte, and a novel slot opcode. Built from the real Frame.to_json() shape."""
    def fr(ch, dev, seq, cmd, payload, t, boot, **extra):
        d = make_frame(ch, dev, seq, cmd, payload, t, boot)
        d.update(extra)
        return d

    return [
        # boot A: idle rotation, then a 0xBC reply carrying a NOVEL slot op
        fr("D0", 0x02, 0x10, 0xBC, [0x02, 0x04], 0.0, BOOT_A),
        fr("D1", 0x04, 0x10, 0xBC, [0x02, 0x04, 0x00, 0x00, 0, 0], 0.01, BOOT_A,
           latency_ms=4),
        fr("D0", 0x01, 0x11, 0xDC, [0x00, 0x17] + [0] * 23, 0.40, BOOT_A),
        fr("D1", 0x03, 0x11, 0xDC, [0x00, 0x17], 0.41, BOOT_A),
        fr("D0", 0x02, 0x12, 0xBC, [0x02, 0x04], 0.80, BOOT_A),
        fr("D1", 0x04, 0x12, 0xBC, [0x02, 0x04, 0x66, 0x01, 0, 0], 0.81, BOOT_A),
        fr("D0", 0x01, 0x13, 0x7A, [0x00], 1.20, BOOT_A),
        # boot B: the boundary. New boot id, counter restarts at 0x00.
        fr("D0", 0x02, 0x00, 0xBC, [0x02, 0x04], 0.0, BOOT_B),
        fr("D1", 0x04, 0x00, 0xBC, [0x02, 0x04, 0x00, 0x00, 0, 0], 0.01, BOOT_B),
        fr("D0", 0x01, 0x01, 0xDC, [0x00, 0x17] + [0] * 23, 0.40, BOOT_B),
    ]


def fixture_stream(prefix: str) -> list[str]:
    """The canned JSONL --selftest replays, in the order a live session sees it.

    Note there is NO boot topic: the device publishes none, so the frames' own
    `boot` field is what must key the runs. The census is per key, published
    under inventory/<slug>, and deliberately carries two findings the live
    frames never show (D0/0x01/0x55 and slot op 0x77) so the retained-census
    recovery path is exercised rather than assumed.
    """
    lines: list[str] = []

    def env(suffix: str, payload) -> None:
        lines.append(json.dumps({"topic": f"{prefix}/{suffix}",
                                 "payload": payload}))

    frames = fixture_frames()
    a = [f for f in frames if f["boot"] == BOOT_A]
    b = [f for f in frames if f["boot"] == BOOT_B]

    def key_entry(key, **kw):
        e = {"key": key, "occurrences": 1, "frame_len": 8, "lengths_seen": [8],
             "lengths_varied": False, "distinct_payloads": 1,
             "payload_set_truncated": False, "role": "observed", "new": False,
             "values_observed": {"6": ["0x04"]}, "values_truncated": False,
             "first_ms": 0, "last_ms": 0}
        e.update(kw)
        return e

    def slot_entry(key, known, **kw):
        e = {"key": key, "namespace": "slot_op", "occurrences": 1,
             "known": known, "first_ms": 0, "last_ms": 0}
        e.update(kw)
        return e

    env("status", "online")
    # The summary is the AUTHORITY on the census size. It declares 4 keys and 3
    # slot ops (7), but only 6 are ever published — so the run must end
    # reporting the census INCOMPLETE rather than treating 6 as the whole.
    env("telemetry/summary",
        {"boot": BOOT_A, "uptime_ms": 1000, "frames_parsed": 7,
         "frames_crc_bad": 0, "frames_unknown": 1, "frames_replied": 4,
         "reply_latency_max_ms": 5, "ring_count": 7, "ring_capacity": 64,
         "records_evicted": 0, "records_pushed": 7, "records_published": 7,
         "publish_skipped": 0, "census_keys": 4, "census_slot_ops": 3,
         "census_overflow": 0})
    env("inventory/D0_0x02_0xBC", key_entry("D0/0x02/0xBC", occurrences=3))
    env("inventory/D0_0x01_0x7A", key_entry("D0/0x01/0x7A", role="unknown"))
    # a key the live frames never carried — must still be recovered
    env("inventory/D0_0x01_0x55", key_entry("D0/0x01/0x55", role="unknown"))
    env("inventory/slot_D1_0x04_0x64", slot_entry("slot/D1/0x04/0x64", True))
    env("inventory/slot_D1_0x04_0x66", slot_entry("slot/D1/0x04/0x66", False))
    # ...and so must this slot op, which no frame in this stream shows
    env("inventory/slot_D1_0x04_0x77", slot_entry("slot/D1/0x04/0x77", False))
    env("telemetry/alert", {"kind": "new_key", "key": "D0/0x01/0x7A"})
    for rec in a:
        lines.append(json.dumps(rec))
    # a complete dump for boot A — both records were already streamed, so the
    # backfill must add nothing (it is a duplicate check, not an append)
    env("telemetry/dump/header", {"boot_id": BOOT_A, "chunk_count": 1})
    env("telemetry/dump/chunk/0", [a[0], a[1]])
    env("telemetry/dump/footer", {"boot_id": BOOT_A, "records": 2})

    for rec in b:
        lines.append(json.dumps(rec))
    # boot B's census has a capped value set, which must be reported as such
    env("inventory/D0_0x01_0xDC",
        key_entry("D0/0x01/0xDC", values_truncated=True,
                  values_observed={"6": ["0x%02X" % v for v in range(16)]}))
    # a truncated dump for boot B: header + chunk, NO footer
    env("telemetry/dump/header", {"boot_id": BOOT_B, "chunk_count": 2})
    env("telemetry/dump/chunk/0", [b[0]])
    env("status", "offline")
    return lines


def fixture_stream_boot_topic(prefix: str) -> list[str]:
    """A stream that DOES publish a boot topic, to prove the vestigial path
    still works: a first frame that carries no `boot` field, then a boot topic
    naming the same boot. The run must adopt the id, not split in two."""
    return [
        json.dumps(make_frame("D0", 0x02, 0x30, 0xBC, [0x02, 0x04], 0.0)),
        json.dumps({"topic": f"{prefix}/boot",
                    "payload": {"boot_id": "CAFEF00D",
                                "reset_reason": "power_on", "fw": "0.3.12"}}),
        json.dumps(make_frame("D0", 0x02, 0x31, 0xBC, [0x02, 0x04], 0.5,
                              "CAFEF00D")),
    ]


def selftest() -> int:
    import shutil
    import tempfile

    from ecm_diff import load_run, load_sweep_index

    tmp = tempfile.mkdtemp(prefix="ecm-collector-selftest-")
    tin = os.path.join(tmp, "canned.jsonl")
    out = os.path.join(tmp, "out")
    prefix = DEFAULT_PREFIX

    with open(tin, "w", encoding="utf-8") as fh:
        for line in fixture_stream(prefix):
            fh.write(line + "\n")

    collector = Collector(out, prefix=prefix, quiet=True)
    try:
        replay_file(collector, tin)
    finally:
        collector.finish("selftest")

    r = _Results()
    runs = sorted(d for d in os.listdir(out)
                  if os.path.isdir(os.path.join(out, d)))
    r.check(len(runs) == 2, "one run directory per boot_id (2)", f"got {runs}")

    # ---- verbatim --------------------------------------------------------
    first_run = next((d for d in runs if d.endswith(f"-{BOOT_A}")), runs[0])
    d0_path = os.path.join(out, first_run, "frames_D0.jsonl")
    d0_lines = [ln.rstrip("\n") for ln in open(d0_path) if ln.strip()]
    want = [json.dumps(x) for x in fixture_frames()
            if x["channel"] == "D0" and x["boot"] == BOOT_A]
    r.check(d0_lines == want,
            "frames_D0.jsonl holds the device payloads verbatim, in order",
            f"want {len(want)} lines, got {len(d0_lines)}\n"
            f"  want[0]={want[0] if want else None}\n"
            f"  got [0]={d0_lines[0] if d0_lines else None}")
    r.check(len(d0_lines) == 4,
            "the duplicate dump backfill added no frames",
            f"got {len(d0_lines)} lines")

    # ---- findings --------------------------------------------------------
    unk = [json.loads(ln) for ln in
           open(os.path.join(out, first_run, "unknown_commands.jsonl"))
           if ln.strip()]
    r.check(any(u["key"] == "D0/0x01/0x7A" and u.get("hex") for u in unk),
            "the unknown cmd byte is recorded from the LIVE frame (with its hex)",
            f"got {unk}")
    r.check(any(u.get("slot_op") == "0x66" for u in unk),
            "the new SLOT opcode 0x66 is recorded (the check ecm_session lacks)",
            f"got {unk}")
    r.check(any(u["key"] == "D0/0x01/0x55" and u.get("source") == "inventory"
                for u in unk),
            "an unknown cmd the frames never carried is recovered from the "
            "retained census", f"got {unk}")
    r.check(any(u.get("slot_op") == "0x77" and u.get("source") == "inventory"
                for u in unk),
            "an unknown SLOT op the frames never carried is recovered too",
            f"got {unk}")
    r.check(not any(u.get("slot_op") == "0x64" for u in unk),
            "a KNOWN slot op (0x64) from the census is not flagged", f"got {unk}")

    inv_lines = [json.loads(ln) for ln in
                 open(os.path.join(out, first_run, "inventory.jsonl"))
                 if ln.strip()]
    r.check(len(inv_lines) == 6 and
            {"t", "wall", "boot_id", "slug", "entry"} <= set(inv_lines[0]),
            "inventory.jsonl logs one line per per-key publish", f"got {inv_lines}")
    alerts = os.path.join(out, first_run, "alerts.jsonl")
    r.check(os.path.exists(alerts) and sum(1 for _ in open(alerts)) == 1,
            "the alert is written")

    # ---- run.json / manifest --------------------------------------------
    rj = json.load(open(os.path.join(out, first_run, "run.json")))
    r.check(os.path.isabs(rj["run_dir"]) and rj["marks"] == [] and rj["valid"],
            "run.json is harness-shaped: absolute run_dir, marks=[], valid",
            f"got { {k: rj[k] for k in ('run_dir', 'marks', 'valid')} }")
    r.check(rj["frame_counts"].get("D0") == len(want),
            "run.json frame_counts matches what was written")
    col = rj["collector"]
    r.check(col["census_keys_received"] == 6 and col["census_expected"] == 7
            and col["census_complete"] is False,
            "the census is counted against the summary's declared size, and a "
            "partial one is NOT reported complete", f"got {col}")

    man = [json.loads(ln) for ln in
           open(os.path.join(out, first_run, "manifest.jsonl")) if ln.strip()]
    final = [m for m in man if m.get("final")]
    r.check(len(final) == 1 and
            {"boot_id", "started_wall", "elapsed_s", "gaps", "reboots"} <=
            set(final[0]),
            "manifest.jsonl carries {boot_id, started_wall, elapsed_s, gaps, "
            "reboots}", f"got {final}")
    r.check(any("census incomplete" in n for n in final[0]["notes"]),
            "the manifest says the census was incomplete rather than implying "
            "6 keys is the whole census", f"got {final[0]['notes']}")

    # ---- the boot boundary ----------------------------------------------
    b2 = next((d for d in runs if d.endswith(f"-{BOOT_B}")), None)
    r.check(b2 is not None, "the second boot got its own run directory")
    if b2 is not None:
        b2_d0 = [json.loads(ln) for ln in
                 open(os.path.join(out, b2, "frames_D0.jsonl")) if ln.strip()]
        r.check(all(x["boot"] == BOOT_B for x in b2_d0) and len(b2_d0) == 2,
                "no frames were merged across the boot boundary",
                f"got {[(x['boot'], x['seq']) for x in b2_d0]}")
        r.check(bool(final[0]["reboots"]) and
                final[0]["reboots"][0].get("to_boot_id") == BOOT_B,
                "the boot change is recorded as a reboot in the manifest",
                f"got {final[0]['reboots']}")
        b2_rj = json.load(open(os.path.join(out, b2, "run.json")))
        r.check(any("truncated" in n for n in b2_rj["notes"]) and
                b2_rj["collector"]["dump_records_written"] == 0,
                "a footer-less dump is recorded as truncated, NOT written",
                f"notes={b2_rj['notes']}")
        r.check(any("values_truncated" in n for n in b2_rj["notes"]),
                "a capped value set is reported, not passed off as complete",
                f"notes={b2_rj['notes']}")

    # ---- no boot topic is the normal case --------------------------------
    # The device publishes none, so every run must be keyed off the frames'
    # own `boot` field. This stream also carries one frame with no `boot` and a
    # later boot topic, to prove the vestigial path adopts rather than splits.
    out2 = os.path.join(tmp, "out2")
    tin2 = os.path.join(tmp, "canned2.jsonl")
    with open(tin2, "w", encoding="utf-8") as fh:
        for line in fixture_stream_boot_topic(prefix):
            fh.write(line + "\n")
    c2 = Collector(out2, prefix=prefix, quiet=True)
    try:
        replay_file(c2, tin2)
    finally:
        c2.finish("selftest")
    runs2 = sorted(d for d in os.listdir(out2)
                   if os.path.isdir(os.path.join(out2, d)))
    r.check(len(runs2) == 1,
            "frames alone key the run, with no boot topic ever arriving",
            f"got {runs2}")
    if runs2:
        rj2 = json.load(open(os.path.join(out2, runs2[0], "run.json")))
        r.check(rj2["collector"]["boot_id"] == "CAFEF00D" and
                rj2["frame_counts"]["D0"] == 2,
                "a run that saw an unnamed frame ADOPTS the real boot id "
                "instead of splitting in two", f"got {rj2['collector']}")

    # ---- loaders ---------------------------------------------------------
    by_ch = load_run(os.path.join(out, first_run))
    r.check(len(by_ch.get("D0", [])) == len(want),
            "ecm_diff.load_run reads the run directory",
            f"got {len(by_ch.get('D0', []))} D0 frames")

    sweeps = sorted(f for f in os.listdir(out)
                    if f.startswith("sweep-") and f.endswith("-index.jsonl"))
    r.check(len(sweeps) == 1, "one sweep index written", f"got {sweeps}")
    if sweeps:
        load = load_sweep_index(os.path.join(out, sweeps[0]))
        total = len(load.runs) + len(load.controls)
        r.check(total == len(runs) and not load.excluded,
                "ecm_diff.load_sweep_index consumes the sweep index",
                f"runs={len(load.runs)} controls={len(load.controls)} "
                f"excluded={load.excluded}")

    code = r.summary()
    if code == 0:
        shutil.rmtree(tmp, ignore_errors=True)
    else:
        print(f"\nselftest artifacts kept at: {tmp}")
    return code


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--prefix", default=DEFAULT_PREFIX,
                    help="the device's fixed topic root, NOT an ESPHome "
                         "mqtt.topic_prefix (default: %(default)s)")
    ap.add_argument("--broker", default=DEFAULT_BROKER)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--username")
    ap.add_argument("--password")
    ap.add_argument("--client-id")
    ap.add_argument("--out", default=DEFAULT_OUT,
                    help="root the run directories are written under "
                         "(default: %(default)s)")
    ap.add_argument("--gap-s", type=float, default=DEFAULT_GAP_S,
                    help="inter-frame silence that counts as a manifest gap")
    ap.add_argument("--replay", metavar="FILE",
                    help="read a canned JSONL capture instead of a broker")
    ap.add_argument("--selftest", action="store_true",
                    help="replay a built-in fixture and assert the output "
                         "(no broker, no paho)")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--verbose", action="store_true",
                    help="also log unrouted topics and per-chunk dump progress")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    collector = Collector(args.out, prefix=args.prefix, quiet=args.quiet,
                          verbose=args.verbose, gap_s=args.gap_s)

    # A killed tool is a coverage hole like any other; close the run so its
    # run.json and manifest are on disk before the process goes away.
    def _stop(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)

    reason = "replay_end" if args.replay else "stopped"
    try:
        if args.replay:
            replay_file(collector, args.replay)
        else:
            run_live(collector, args.broker, args.port,
                     args.username, args.password, args.client_id)
    except KeyboardInterrupt:
        reason = "interrupted"
        print("\ninterrupted — closing the current run", file=sys.stderr)
    finally:
        collector.finish(reason)
    return 0


if __name__ == "__main__":
    sys.exit(main())

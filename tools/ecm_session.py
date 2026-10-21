#!/usr/bin/env python3
"""Post-session analysis for an ecm_monitor.py session.

READ-ONLY: opens no hardware, defines no transmit path. Reads the .sr segments
the monitor wrote, decodes them with the verified parser, merges them onto one
wall-clock timeline, and answers questions about what happened on the bus:

    reboots observed      uptime drops in polls.jsonl
    reboot duration       the module's TX silence spanning the uptime reset
    ECM during a reboot   does it keep polling? does seq park at 0?
    any 0x20 at a reboot  none expected - 0x20 marks an ECM start, not a module one
    frame loss            SKIPPED must be 0
    new command bytes     census vs the known set
    operator commands     write-slot writes: what was asked for, and when

Usage:
    ./ecm_session.py captures/run-manual
    ./ecm_session.py <session> --json out.json

WHY THIS SEPARATE FROM ecm_monitor.py
-------------------------------------
The monitor's own summary only knows what its exit path saw; it prints at the
END and it cannot see inside the segments. Reboot detection needs the .sr files
decoded and cross-referenced against /poll, which is a job for after the run.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ecm_frames import decode_channel, PairTable  # noqa: E402

#: Announced changes, and what the module's own /poll calls the same thing.
ANNOUNCEMENTS = {0xE4: "flame", 0xE1: "height", 0xDF: "pilot"}
#: Write opcodes the module puts in the 0xBC reply slot.
WRITE_OPS = {0x64: "flame", 0x65: "pilot", 0x67: "height"}
#: Commands we already know about. Anything outside this set is a finding.
KNOWN_CMDS = {0xBC, 0xDC, 0x64, 0x6E, 0x20, 0xE4, 0xE1, 0xDF}
#: A gap on a channel longer than this is a "silence" worth explaining.
SILENCE_S = 1.0


def iso_to_epoch(s: str) -> float:
    return datetime.fromisoformat(s).timestamp()


def load_manifest(session: str) -> list[dict]:
    p = os.path.join(session, "manifest.jsonl")
    if not os.path.exists(p):
        return []
    out = []
    with open(p) as fh:
        for line in fh:
            try:
                out.append(json.loads(line))
            except Exception:
                pass
    return out


def load_polls(session: str) -> list[dict]:
    p = os.path.join(session, "polls.jsonl")
    if not os.path.exists(p):
        return []
    out = []
    with open(p) as fh:
        for line in fh:
            try:
                out.append(json.loads(line))
            except Exception:
                pass
    return out


class Timeline:
    """Every frame from every segment, on one epoch timeline.

    Each segment's decoder starts its own t at 0, so segment-local times are
    useless until they are anchored. manifest.jsonl records the segment's END
    wall time, so start = wall - elapsed. That is good to about a second (the
    gap between sigrok opening the device and its first sample).
    """

    def __init__(self) -> None:
        self.frames: list[tuple[float, object]] = []
        self.epoch0: float | None = None
        self.segment_spans: list[dict] = []
        self.counts: dict[str, int] = {"D0": 0, "D1": 0}
        #: Per-segment frames, kept separate on purpose. Integrity (park /
        #: SKIPPED) is only meaningful WITHIN a continuous capture: each segment
        #: ends mid-stream and the next opens on a different seq, so evaluating
        #: it across the merged timeline reports every segment boundary as a
        #: discontinuity. That produced a false SKIPPED=1745 on the first run.
        self.per_segment: list[dict] = []

    def add_segment(self, path: str, wall_end: str, elapsed: float) -> dict:
        seg_start = iso_to_epoch(wall_end) - elapsed
        if self.epoch0 is None:
            self.epoch0 = seg_start
        got = {}
        local: dict[str, list] = {"D0": [], "D1": []}
        for ch in ("D0", "D1"):
            try:
                fr = decode_channel(path, ch)
            except Exception:
                fr = []
            got[ch] = len(fr)
            self.counts[ch] += len(fr)
            local[ch] = fr
            for f in fr:
                if f.t is not None:
                    self.frames.append((seg_start + f.t, f))
        self.frames.sort(key=lambda x: x[0])
        #: The decoded span is the AUTHORITATIVE measure of how much of the
        #: segment was actually captured. Do not trust manifest's `short` flag:
        #: ecm_monitor.py decides it by scanning the last 3 lines of the WHOLE
        #: capture.log, so one segment's "Device only sent N samples." keeps
        #: marking every later segment short. That produced a false short=True
        #: on a segment that in fact ran its full 300 s.
        spans = {}
        for ch in ("D0", "D1"):
            ts = [f.t for f in local[ch] if f.t is not None]
            spans[ch] = (max(ts) - min(ts)) if ts else 0.0
        self.segment_spans.append(
            {"file": os.path.basename(path), "start": seg_start, "frames": got,
             "span_s": round(spans["D0"], 1)})
        self.per_segment.append({"file": os.path.basename(path), "local": local,
                                 "span_s": spans["D0"]})
        return self.segment_spans[-1]

    def rel(self, epoch: float) -> float:
        return epoch - (self.epoch0 or epoch)

    def channel(self, ch: str) -> list[tuple[float, object]]:
        return [x for x in self.frames if x[1].channel == ch]

    def silences(self, ch: str, minimum: float = SILENCE_S) -> list[dict]:
        """Gaps between consecutive frames on one channel — the module going
        quiet, in the case of D1, is what a reboot looks like on the wire."""
        seq = self.channel(ch)
        out = []
        for (t0, f0), (t1, f1) in zip(seq, seq[1:]):
            gap = t1 - t0
            if gap >= minimum:
                out.append({"t": round(self.rel(t1), 1), "gap_s": round(gap, 2),
                            "after_seq": f0.seq, "before_seq": f1.seq,
                            "after_cmd": f0.cmd, "before_cmd": f1.cmd})
        return out


def _slope(pts: list[tuple[float, int]]) -> float | None:
    n = len(pts)
    if n < 3:
        return None
    mt = sum(t for t, _ in pts) / n
    mu = sum(u for _, u in pts) / n
    den = sum((t - mt) ** 2 for t, _ in pts)
    if den == 0:
        return None
    return sum((t - mt) * (u - mu) for t, u in pts) / den


def clock_rate(polls: list[dict]) -> dict | None:
    """How fast the module's own `uptime` runs versus the poller's clock.

    Anchor-free least-squares slope of uptime against the poller's t, so it
    needs no wall-clock reference. It matters because reboot intervals are
    naturally read off `uptime` deltas: if the module's second is not a second,
    every interval quoted in module-seconds is biased by exactly this factor.

    ⚠️ MUST be fitted PER UPTIME EPISODE. `uptime` resets at a reboot, so a
    single fit across a reset drags the slope toward zero — on this session's
    own data that produced a confident, meaningless -69.9%. Episodes are split
    wherever uptime decreases; a reboot is the only thing that does that.
    """
    pts = [(r["t"], r["poll"]["uptime"]) for r in polls
           if r.get("poll", {}).get("uptime") is not None]
    if len(pts) < 3:
        return None
    episodes: list[list[tuple[float, int]]] = [[]]
    for t, u in pts:
        if episodes[-1] and u < episodes[-1][-1][1]:
            episodes.append([])
        episodes[-1].append((t, u))
    rates = []
    for ep in episodes:
        s = _slope(ep)
        if s is not None:
            rates.append({"samples": len(ep), "span_s": round(ep[-1][0] - ep[0][0], 1),
                          "rate": round(s, 5)})
    if not rates:
        return None
    rates.sort(key=lambda r: -r["samples"])
    return {"episodes": rates, "primary_rate": rates[0]["rate"],
            "episode_count": len(episodes)}


def poll_gaps(polls: list[dict], minimum: float = 10.0) -> list[dict]:
    """Windows where /poll produced no sample.

    These BOUND what /poll can corroborate. `uptime` is monotonic, so reboot
    DETECTION survives a gap (sample before, sample after, compare) — but
    temporal CORRELATION does not: an event inside a 25 s gap can only be
    bracketed to 25 s, not pinned. Report this rather than implying that /poll
    and the wire agree to the second.
    """
    out = []
    for a, b in zip(polls, polls[1:]):
        gap = b.get("t", 0) - a.get("t", 0)
        if gap >= minimum:
            out.append({"t": round(b.get("t", 0), 1), "gap_s": round(gap, 1),
                        "from_t": round(a.get("t", 0), 1)})
    return out


def seq_events(frames: list) -> dict:
    """Classify every DECREASE in a channel's seq counter.

    The counter wraps at 256 and the ECM parks it at 0 after a reply timeout,
    so a naive "seq went down = discontinuity" count conflates three very
    different things:

        wrap      0xFF -> 0x00   the counter rolling over. A normal STEP.
        park      X    -> 0x00   the ECM giving up on a reply (a stall starts)
        bounce    0x01 -> 0x00   a second reset while recovering from a stall

    So resets-to-zero ≈ 2 per stall, and quoting that raw number as "reply
    timeouts" doubles the true figure. Stalls are counted by park entries.
    """
    seqs = [f.seq for f in frames]
    ev = {"wrap": 0, "park": 0, "bounce": 0, "other": 0, "events": []}
    for prev, s in zip(seqs, seqs[1:]):
        if s >= prev:
            continue                      # normal advance or hold
        if prev == 0xFF and s == 0x00:
            ev["wrap"] += 1
        elif s != 0x00:
            ev["other"] += 1              # unexplained - would be a finding
        elif prev == 0x01:
            ev["bounce"] += 1
        else:
            ev["park"] += 1
    return ev


def find_reboots(polls: list[dict]) -> list[dict]:
    """A reboot is `uptime` going DOWN. Nothing else on this link can do that."""
    out, prev = [], None
    for r in polls:
        d, t = r.get("poll", {}), r.get("t", 0)
        u = d.get("uptime")
        if u is None:
            continue
        if prev is not None and u < prev[1]:
            out.append({"t": round(t, 1), "from_s": prev[1], "to_s": u,
                        "down_s": prev[1] - u, "prev_t": prev[0]})
        prev = (t, u)
    return out


def poll_state(d: dict) -> dict:
    return {k: d.get(k) for k in
            ("power", "pilot", "hot", "height", "light", "fanspeed", "errors")}


def state_transitions(polls: list[dict]) -> list[dict]:
    out, prev = [], {}
    for r in polls:
        d, t = r.get("poll", {}), r.get("t", 0)
        for k in ("power", "pilot", "hot", "height", "light", "fanspeed", "errors"):
            if k in d and k in prev and d[k] != prev[k]:
                out.append({"t": round(t, 1), "field": k, "from": prev[k],
                            "to": d[k]})
            prev[k] = d.get(k)
    return out


def write_slot(f) -> list[int] | None:
    """The 4-byte slot in a 0xBC frame. Only D1's is the module's write."""
    if f.cmd != 0xBC or f.channel != "D1" or len(f.payload) < 6:
        return None
    return list(f.payload[2:6])


def analyse(session: str) -> dict:
    tl = Timeline()
    for m in load_manifest(session):
        if not m.get("file") or not os.path.exists(m["file"]):
            continue
        tl.add_segment(m["file"], m["wall"], m.get("elapsed_s", 0.0))

    polls = load_polls(session)
    reboots = find_reboots(polls)
    rate = clock_rate(polls)

    # ---- per-segment capture health, from the DATA not the log -----------
    # manifest's `short` flag is known to over-report (see Timeline.add_segment),
    # so truncation is decided here on the decoded span.
    segment_health = []
    for m in load_manifest(session):
        if not m.get("file"):
            continue
        base = os.path.basename(m["file"])
        span = next((s["span_s"] for s in tl.segment_spans if s["file"] == base),
                    None)
        req = m.get("requested_s") or 0
        segment_health.append({
            "file": base,
            "requested_s": req,
            "elapsed_s": m.get("elapsed_s"),
            "captured_span_s": span,
            "truncated": (span is not None and req and span < req * 0.95),
            "manifest_short_flag": m.get("short"),
            "manifest_note": m.get("note", ""),
        })

    # ---- frame census ----------------------------------------------------
    census: dict[str, int] = {}
    unknown: list[dict] = []
    announcements: list[dict] = []
    writes: list[dict] = []
    for t, f in tl.frames:
        census[f.key_s] = census.get(f.key_s, 0) + 1
        if f.cmd not in KNOWN_CMDS:
            unknown.append({"t": round(tl.rel(t), 1), "key": f.key_s, "hex": f.hex})
        if f.cmd in ANNOUNCEMENTS:
            # Only D0 (the ECM) carries a value. The module answers the same
            # cmd with a SHORT payload - `00 01` for E4/E1/DF - because it is a
            # polled slave echoing the exchange, not restating the value. Taking
            # payload[-1] from a D1 reply reports the echo as if it were the
            # value, which is wrong (D0 says height 0 where D1's echo ends 01).
            announcements.append({
                "t": round(tl.rel(t), 1), "what": ANNOUNCEMENTS[f.cmd],
                "channel": f.channel, "seq": f.seq, "payload": f.payload,
                "value": (f.payload[-1] if f.payload else None)
                         if f.channel == "D0" else None,
                "hex": f.hex})
        slot = write_slot(f)
        if slot and any(b for b in slot):
            writes.append({"t": round(tl.rel(t), 1), "seq": f.seq, "slot": slot,
                           "op": WRITE_OPS.get(slot[0], f"0x{slot[0]:02X}"),
                           "value": slot[2], "hex": f.hex})

    # ---- integrity: PER SEGMENT, then summed -----------------------------
    # Never across the merged timeline - see Timeline.per_segment.
    integrity = {}
    per_segment_integrity = []
    for seg in tl.per_segment:
        row = {"file": seg["file"]}
        for ch in ("D0", "D1"):
            fr = seg["local"].get(ch) or []
            if fr:
                row[ch] = PairTable(fr).integrity()
        per_segment_integrity.append(row)
    for ch in ("D0", "D1"):
        pfx = ch.lower()                      # integrity keys are d0_/d1_
        tot = {"frames": 0, "wrap_observed": False,
               "seq_advance": {f"{pfx}_{k}": 0 for k in
                               ("steps", "held", "park_entries", "skipped")}}
        for row in per_segment_integrity:
            v = row.get(ch)
            if not v:
                continue
            tot["frames"] += v.get("frames", 0)
            tot["wrap_observed"] = tot["wrap_observed"] or \
                v["seq_advance"]["wrap_observed"]
            for k in ("steps", "held", "park_entries", "skipped"):
                tot["seq_advance"][f"{pfx}_{k}"] += v["seq_advance"][f"{pfx}_{k}"]
        if tot["frames"]:
            integrity[ch] = tot

    # ---- reboots, against the wire --------------------------------------
    d1_sil = tl.silences("D1")
    d0_sil = tl.silences("D0")
    reboot_rows = []
    for rb in reboots:
        # /poll and the segments share the session start to ~a second.
        lo, hi = rb["t"] - 60, rb["t"] + 60
        win = [(t, f) for t, f in tl.frames if lo <= tl.rel(t) <= hi]
        wd0 = [f for _, f in win if f.channel == "D0"]
        wd1 = [f for _, f in win if f.channel == "D1"]
        local = [s for s in d1_sil if lo <= s["t"] <= hi]
        local0 = [s for s in d0_sil if lo <= s["t"] <= hi]
        biggest = max(local, key=lambda s: s["gap_s"]) if local else None
        before = next((p["poll"] for p in polls if p["t"] >= rb["prev_t"]), {})
        after = next((p["poll"] for p in polls if p["t"] >= rb["t"]), {})
        reboot_rows.append({
            "t": rb["t"], "uptime_from": rb["from_s"], "uptime_to": rb["to_s"],
            "module_silence_s": biggest["gap_s"] if biggest else None,
            "module_silence_t": biggest["t"] if biggest else None,
            "ecm_silence_s": max((s["gap_s"] for s in local0), default=0.0),
            "ecm_frames_in_window": len(wd0), "module_frames_in_window": len(wd1),
            "seq_at_reboot": biggest["before_seq"] if biggest else None,
            "seq_after": None,
            "boot_frame_0x20": any(f.cmd == 0x20 for _, f in win),
            "state_before": poll_state(before), "state_after": poll_state(after),
        })
        # seq at the segment after the silence: is the counter parked at 0?
        if biggest:
            after_frames = [(t, f) for t, f in tl.channel("D1")
                            if tl.rel(t) > biggest["t"]]
            if after_frames:
                reboot_rows[-1]["seq_after"] = after_frames[0][1].seq

    # parked / skipped totals - SKIPPED must be 0 or it is a real finding
    sk = sum(v["seq_advance"][f"{ch.lower()}_skipped"]
             for ch, v in integrity.items())
    resets = sum(v["seq_advance"][f"{ch.lower()}_park_entries"]
                 for ch, v in integrity.items())

    # ---- seq discontinuities, classified per segment ---------------------
    seq_ev = {"wrap": 0, "park": 0, "bounce": 0, "other": 0}
    for seg in tl.per_segment:
        for ch in ("D0", "D1"):
            fr = seg["local"].get(ch) or []
            if not fr:
                continue
            e = seq_events(fr)
            for k in seq_ev:
                seq_ev[k] += e[k]

    return {
        "session": os.path.abspath(session),
        "segments_decoded": len(tl.segment_spans),
        "segments": [s["file"] for s in tl.segment_spans],
        "frames": dict(tl.counts),
        "polls": len(polls),
        "reboots": reboot_rows,
        "reboot_count": len(reboots),
        "segment_health": segment_health,
        "segments_truncated": sum(1 for s in segment_health if s["truncated"]),
        "manifest_short_overcount": sum(
            1 for s in segment_health
            if s.get("manifest_short_flag") and not s["truncated"]),
        "census": dict(sorted(census.items(), key=lambda kv: -kv[1])),
        "announcements": announcements,
        "operator_commands": writes,
        "unknown_commands": unknown,
        "state_transitions": state_transitions(polls),
        "module_silences": [s for s in d1_sil if s["gap_s"] >= 2.0],
        "integrity": integrity,
        "per_segment_integrity": per_segment_integrity,
        "total_skipped": sk,
        "total_seq_resets": resets,
        "seq_events": seq_ev,
        "poll_gaps": poll_gaps(polls),
        "clock_rate": rate,
    }


def report(a: dict) -> str:
    L = []
    w = L.append
    w(f"session          : {a['session']}")
    w(f"segments decoded : {a['segments_decoded']}")
    w(f"frames           : {a['frames']}   /poll samples: {a['polls']}")
    w("")
    if a.get("clock_rate"):
        cr = a["clock_rate"]
        r = cr["primary_rate"]
        w(f"module clock rate  : {r:.5f} s uptime per poller second "
          f"({(r - 1) * 100:+.2f}%)   [fitted per uptime episode]")
        for ep in cr["episodes"][:4]:
            w(f"    episode: {ep['samples']:>3} samples over {ep['span_s']:>7.1f}s "
              f"-> {ep['rate']:.5f}")
        w(f"                     -> reboot intervals read off `uptime` are biased by")
        w(f"                        this factor; multiply by {r:.4f} for true seconds")
    w("")
    if a.get("segment_health"):
        w("--- capture health (from decoded data, NOT the manifest flag) --------")
        for s in a["segment_health"]:
            flag = "TRUNCATED" if s["truncated"] else "full"
            w(f"  {s['file']:<14} req={s['requested_s']:.0f}s "
              f"captured={s['captured_span_s']}s  {flag}")
        if a.get("manifest_short_overcount"):
            w(f"  ! manifest.jsonl's `short` flag over-reports by "
              f"{a['manifest_short_overcount']} segment(s) — known bug in")
            w( "    ecm_monitor.py (scans the whole capture.log tail, not this")
            w( "    segment's own output). Trust `captured_span_s` instead.")
        w("")
    w("--- expected vs observed -------------------------------------------")
    w(f"  reboots observed   : {a['reboot_count']}  (expected one per 11-26 min)")
    w(f"  frame loss SKIPPED : {a['total_skipped']}  {'OK' if a['total_skipped']==0 else '*** REAL FINDING ***'}")
    e = a.get("seq_events") or {}
    w(f"  reply stalls       : {e.get('park', 0)}  (seq parked at 0 = ECM gave up"
      f" on a reply)")
    w(f"  seq resets to 0    : {a['total_seq_resets']}  = {e.get('park', 0)} park "
      f"entries + {e.get('bounce', 0)} recovery bounces")
    w( "                       -> so the raw reset count is ~2x the stall count."
      " Do NOT quote it as \"N reply timeouts\".")
    w(f"  counter wraps      : {e.get('wrap', 0)}  (0xFF->0x00; a normal step)")
    if e.get("other"):
        w(f"  *** {e['other']} UNEXPLAINED seq decrease(s) — real finding ***")
    if a.get("poll_gaps"):
        w(f"  /poll coverage gaps: {len(a['poll_gaps'])} window(s) with no sample")
        for g in a["poll_gaps"][:8]:
            w(f"                       t={g['from_t']} -> {g['t']}  ({g['gap_s']}s)")
        w( "                       -> reboot DETECTION survives these (uptime is")
        w( "                          monotonic, so before/after comparison holds),")
        w( "                          but temporal CORRELATION with the wire does")
        w( "                          not. Do not quote /poll to sub-second accuracy.")
    unk = a["unknown_commands"]
    w(f"  new command bytes  : {len(unk)}  {'none - as expected' if not unk else '*** FINDING ***'}")
    w("")
    if a["reboots"]:
        w("--- reboots ---------------------------------------------------------")
        for r in a["reboots"]:
            w(f"  t={r['t']:>7.1f}s  uptime {r['uptime_from']}s -> {r['uptime_to']}s")
            w(f"      module TX silence : {r['module_silence_s']}s"
              f"   (this is the reboot window)")
            w(f"      ECM frames within +/-60s of it: {r['ecm_frames_in_window']}"
              f"  (NOT the window count — see below)")
            w(f"      seq at silence    : {r['seq_at_reboot']} -> after: {r['seq_after']}")
            w(f"      boot frame 0x20   : {r['boot_frame_0x20']} (none expected)")
            b, f_ = r["state_before"], r["state_after"]
            w(f"      /poll before      : power={b.get('power')} pilot={b.get('pilot')}"
              f" hot={b.get('hot')} height={b.get('height')}")
            w(f"      /poll after       : power={f_.get('power')} pilot={f_.get('pilot')}"
              f" hot={f_.get('hot')} height={f_.get('height')}")
    else:
        w("--- reboots ---------------------------------------------------------")
        w("  NONE detected. Check /poll coverage before concluding - a gap in")
        w("  polls.jsonl would hide one.")
    w("")
    if a["operator_commands"]:
        w("--- commands written into the 0xBC slot (operator/app actions) --------")
        for c in a["operator_commands"]:
            w(f"  t={c['t']:>7.1f}s  seq={c['seq']:#04x}  {c['op']} = {c['value']}"
              f"   slot={[f'{b:02X}' for b in c['slot']]}")
        w("")
    if a["announcements"]:
        w("--- ECM announcements ------------------------------------------------")
        for c in a["announcements"]:
            w(f"  t={c['t']:>7.1f}s  {c['channel']} {c['what']:<7} seq={c['seq']:#04x}"
              f"  value={c['value']}   {c['hex']}")
        w("")
    if a["state_transitions"]:
        w("--- /poll state transitions ------------------------------------------")
        for s in a["state_transitions"]:
            w(f"  t={s['t']:>7.1f}s  {s['field']}: {s['from']} -> {s['to']}")
        w("")
    w("--- module silence windows on the wire (>= 2s) -----------------------")
    if a["module_silences"]:
        for s in a["module_silences"]:
            w(f"  t={s['t']:>7.1f}s  {s['gap_s']}s  seq {s['after_seq']}->{s['before_seq']}")
    else:
        w("  (none)")
    w("")
    w("--- command census ---------------------------------------------------")
    for k, v in a["census"].items():
        w(f"  {v:>6}  {k}")
    return "\n".join(L)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("session")
    ap.add_argument("--json", help="also write the full analysis here")
    args = ap.parse_args()

    a = analyse(args.session)
    print(report(a))
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(a, fh, indent=2, default=str)
        print(f"\njson: {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

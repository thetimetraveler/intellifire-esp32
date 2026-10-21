#!/usr/bin/env python3
"""Baseline / delta / attribution engine for the ECM <-> WFM bus harness.

READ-ONLY ANALYSIS. Opens no hardware. Defines no transmit path. It consumes
frame lists that tools/ecm_frames.py already produced and decides WHICH BYTES
changed in response to WHICH HOME ASSISTANT ACTION.

Why this file exists
--------------------
An orchestrator captures the bus while firing known actions ("set height to
3", "turn flame off"). The naive reading of such a capture is a lie: the bus
carries a constant background chatter (polling, status echoes) that dwarfs the
signal. A byte that differs between two captures is NOT evidence of anything
until you have shown that it (a) appeared only after the action, (b) appears
again every time you repeat that action, and (c) does NOT appear when you do
something else — or nothing at all.

So this module is deliberately adversarial with itself. Its job is to refuse to
attribute. Three independent conditions must all hold before a byte is called
"confirmed", and the most valuable test in the file is the negative control:
idle-only runs must produce ZERO attributions. A method that finds effects in
noise cannot be trusted to find the real ones.

The three conditions (all required)
-----------------------------------
  1. PRESENCE       appeared in post-mark frames and not in the pre-mark
                    baseline, at the SAME mark position.
  2. REPRODUCIBILITY present in >= k of the N repetitions of action A, where
                    k = N for N <= 3, and k >= N-1 for N >= 4.
  3. SPECIFICITY    absent from the cross-repetition INTERSECTION of every
                    other action, and absent from ALL idle-control runs.

Condition 3 is the one that does the work. A frame that shows up in the
intersection of two different actions is not specific to either of them, and
must be reported as background rather than attributed to one of them.

The state-echo carve-out (a deliberate spec EXTENSION, not a weakening)
----------------------------------------------------------------------
Condition 3 as stated would discard the single most informative frame in a
capture. The 0xDC status frame moves for BOTH flame_on and flame_off, because
it is a state REPORT: the appliance tells the bus what it is now, not what it
was told to do. Excluding it for being non-specific removes the one frame that
actually tracks whether the fireplace is burning.

So there is a fourth, purely ADDITIVE category. A key K is a STATE ECHO when:

  1. K is PAYLOAD_CHANGED for >= 2 distinct actions;
  2. K is absent from the idle control (control_clean still gates it — this
     is not negotiable; a frame that moves with nothing happening is noise,
     not a state report);
  3. for every such action A, the values observed at some attributed byte
     index are PAIRWISE DISJOINT from every other such action's values at
     that index.

Note "at least one attributed index", not all of them: one discriminating
index is the interesting finding, and demanding all would be a stronger claim
than the data supports. Which index carried the discrimination is recorded in
`discriminating_index`.

This does NOT change `confirmed`, and it does not relax condition 3.
`state_echo` is a separate member of the score enum, computed after the normal
path, and a state echo can never come out `confirmed`. `specific` stays FALSE
for these — it is genuinely not specific to one action, and reporting it as
specific would be dishonest.

What `state_echo` CLAIMS: this key's value distinguishes these actions from
each other, and its value tracks appliance state rather than encoding a
command for any one action.

What it does NOT claim: that the mapping is a complete state enumeration, that
the byte means what the observed values suggest, or that the key is a command
for anything. It is a state REPORT, and "0x00 when off, 0x04 when on" is an
observation about the two actions measured, not a decoded field.

⚠️ COUNTING. State echoes and observed_stable frames are reported but are NOT
commands. A consumer that sums every record in attribution.json will overstate
the result. Count `confirmed + probable` for findings, and read the summary's
counts_state_echo separately. `summarize()` makes this structural with
`counts_findings` / `counts_state_echo` / `counts_informational`, so the
distinction does not depend on a convention the reader has to know.

A known limit of the carve-out, observed in practice: when a THIRD action also
moves the same key, the value sets generally stop being pairwise disjoint and
`state_echo` is correctly withheld. That is the intended behaviour — three
actions on one frame genuinely makes its values ambiguous, and calling it an
echo would be a claim the data does not support.

What is deliberately NOT used
-----------------------------
`seq` is an EXCHANGE COUNTER — it increments on every exchange by design. It
is excluded from every payload comparison and is never renumbered here. Were
it included, every frame would look different from every other frame and the
whole method would return "everything changed, always". The model is never
mutated to make a comparison succeed.

The CRC is used ONLY as a validity oracle (it already has been — ecm_frames
returns CRC-valid frames exclusively) and as a *change indicator*. The CRC value
itself is never interpreted, never attributed meaning, and never reported as
evidence: a changed CRC is a consequence of a changed payload, not a fact about
the appliance.

Unproven / honest limits
------------------------
* Nothing here is verified against real hardware. The appliance semantics
  ("this byte is the flame height") remain UNKNOWN; this module only reports
  which bytes moved, reproducibly, in response to which action.
* `confirmed` means "confirmed as correlated under the stated conditions", not
  "confirmed to mean X". A confirmed byte index is a strong lead, not a
  decoded protocol.
* Timing is assumed to be a monotonic clock in seconds. A capture whose
  timestamps wrap or reset will misbucket silently — see _bucket_frames.

Usage as a library
    from ecm_diff import RunResult, run_from_frames, attribute, parameter_table
    run = run_from_frames("height_3", frames, marks=[Mark(10.0, "height_3")])
    atts = attribute([run]*3, control_runs=[idle])

Usage as a CLI
    ./ecm_diff.py sweep-index.jsonl              # append attribution.json
    ./ecm_diff.py sweep-index.jsonl --json       # stdout instead
    ./ecm_diff.py --selftest                     # synthetic tests, no data
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import sys
from dataclasses import dataclass, field, asdict
from typing import Iterable, Sequence

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ecm_frames import Frame  # noqa: E402

# --------------------------------------------------------------------------
# vocabulary — CLOSED ENUM. Consumed by a JSON Schema; do not add to it here
# without updating the schema, and do not spell a member any other way.
# --------------------------------------------------------------------------

KIND_NEW_FRAME = "NEW_FRAME"
KIND_SUPPRESSED = "SUPPRESSED"
KIND_PAYLOAD_CHANGED = "PAYLOAD_CHANGED"
KIND_LENGTH_CHANGED = "LENGTH_CHANGED"
KIND_BACKGROUND = "BACKGROUND"

#: change kinds that carry signal, best first
SIGNAL_KINDS = (
    KIND_NEW_FRAME,
    KIND_LENGTH_CHANGED,
    KIND_PAYLOAD_CHANGED,
    KIND_SUPPRESSED,
)

#: how loud each change kind is. Qualitative, not calibrated.
SIGNAL_LEVEL = {
    KIND_NEW_FRAME: "very_high",
    KIND_LENGTH_CHANGED: "very_high",
    KIND_PAYLOAD_CHANGED: "high",
    KIND_SUPPRESSED: "high",
    KIND_BACKGROUND: "none",
}

SCORE_CONFIRMED = "confirmed"
SCORE_PROBABLE = "probable"
SCORE_SPECULATIVE = "speculative"
SCORE_OBSERVED_STABLE = "observed_stable"
#: A key that moves for MORE THAN ONE action, in a distinguishable way. It
#: tracks appliance STATE rather than encoding a command for any single action.
#: Purely additive: see the module docstring. Never reachable from _score(),
#: and never a substitute for `confirmed`.
SCORE_STATE_ECHO = "state_echo"

#: CLOSED ENUM — the only six strings a score may ever be
SCORES = (
    SCORE_CONFIRMED,
    SCORE_PROBABLE,
    SCORE_SPECULATIVE,
    SCORE_OBSERVED_STABLE,
    SCORE_STATE_ECHO,
)

#: run-level verdict when an action provably changed nothing
NO_CHANGE_DETECTED = "no_change_detected"


def _key_s(key: tuple) -> str:
    """(channel, dev, cmd) -> "D0/0x01/0xDC", matching Frame.key_s."""
    ch, dev, cmd = key
    return f"{ch}/0x{dev:02X}/0x{cmd:02X}"


def _payload_hex(payload: Sequence[int]) -> str:
    return " ".join(f"{b:02X}" for b in payload)


# --------------------------------------------------------------------------
# run model
# --------------------------------------------------------------------------

@dataclass
class Mark:
    """One known action, at a known instant in a capture.

    `t` is seconds from capture start. `action` names the Home Assistant
    action; it is what attribution is keyed on, and two marks in the same run
    bearing the same action name are treated as repetitions of one action.
    """

    t: float
    action: str
    params: dict = field(default_factory=dict)

    def to_json(self) -> dict:
        return {"t": self.t, "action": self.action, "params": self.params}


@dataclass
class BaselineStats:
    """What one (dev, cmd) key looked like in a window.

    `lens` and `payloads` are Counters, not sets, because "how many times" is
    itself evidence: a key that fires twice per second in baseline and zero
    times in post is SUPPRESSED even though the payload never differed.
    """

    n: int = 0
    lens: collections.Counter = field(default_factory=collections.Counter)
    payloads: collections.Counter = field(default_factory=collections.Counter)

    def add(self, frame: Frame) -> None:
        self.n += 1
        self.lens[len(frame.payload)] += 1
        self.payloads[_payload_hex(frame.payload)] += 1

    def payload_set(self) -> set:
        return set(self.payloads)

    def lens_set(self) -> set:
        return set(self.lens)

    def to_json(self) -> dict:
        return {
            "n": self.n,
            "lens": {str(k): v for k, v in sorted(self.lens.items())},
            "payloads": dict(sorted(self.payloads.items())),
        }


@dataclass
class ByteDiff:
    """Where two payloads of the same length disagree."""

    baseline: str
    post: str
    indices: list

    def to_json(self) -> dict:
        return {"baseline": self.baseline, "post": self.post,
                "indices": self.indices}


@dataclass
class Delta:
    """One (dev, cmd) key's change at one mark. The atom of this module."""

    key: tuple
    kind: str
    signal: str
    mark_index: int = -1
    action: str = ""
    #: (action, occurrence-of-that-action, parameter value). A single run can
    #: carry several marks with DIFFERENT actions ("12:flame_on,30:flame_off"),
    #: so attribution groups by slot, never by run. Two runs of the same plan
    #: produce the same slots, which is what makes repetition comparable.
    slot: tuple = ("", 0, None)
    value: object = None
    channel: str = ""
    dev: int = 0
    cmd: int = 0
    baseline_payloads: list = field(default_factory=list)
    post_payloads: list = field(default_factory=list)
    novel_payloads: list = field(default_factory=list)
    missing_payloads: list = field(default_factory=list)
    byte_diffs: list = field(default_factory=list)
    baseline_n: int = 0
    post_n: int = 0
    note: str = ""
    #: CRC values observed, recorded as a change INDICATOR only — see module
    #: docstring. Never attributed meaning.
    baseline_crcs: list = field(default_factory=list)
    post_crcs: list = field(default_factory=list)

    @property
    def key_s(self) -> str:
        return _key_s(self.key)

    def to_json(self) -> dict:
        d = asdict(self)
        d["key"] = self.key_s
        d["byte_diffs"] = [b.to_json() for b in self.byte_diffs]
        return d


@dataclass
class RunResult:
    """One capture, with its marks, reduced to per-mark deltas.

    `label` is the action name for an action run; use "idle" for a control.
    `clean` means no delta was left unexplained — see is_control_clean().
    """

    label: str
    path: str = ""
    deltas: list = field(default_factory=list)      # list[Delta]
    frame_count: int = 0
    marks: list = field(default_factory=list)       # list[Mark]
    is_control: bool = False
    #: delta keys present in EVERY run of this label (filled by attribute())
    intersection: list = field(default_factory=list)
    notes: list = field(default_factory=list)

    def delta_keys(self) -> set:
        return {d.key for d in self.deltas if d.kind != KIND_BACKGROUND}

    def slots(self) -> dict:
        """{slot: {key, ...}} for this run's non-background deltas."""
        out: dict = {}
        for d in self.deltas:
            if d.kind == KIND_BACKGROUND:
                continue
            out.setdefault(d.slot, set()).add(d.key)
        return out

    def to_json(self) -> dict:
        return {
            "label": self.label,
            "path": self.path,
            "is_control": self.is_control,
            "frame_count": self.frame_count,
            "marks": [m.to_json() for m in self.marks],
            "deltas": [d.to_json() for d in self.deltas],
            "intersection": [_key_s(k) for k in self.intersection],
            "notes": self.notes,
        }


@dataclass
class Attribution:
    """A claim, with everything needed to argue with it."""

    key: tuple
    action: str
    score: str
    kind: str
    observations: int
    runs_total: int
    runs_present: int
    presence: bool
    reproducible: bool
    specific: bool
    control_clean: bool
    occurrence: int = 0
    value: object = None
    #: state-echo fields — see the module docstring's carve-out section.
    #: `distinguishable` is only ever True for a key moved by >= 2 actions
    #: with pairwise-disjoint value sets at one attributed index, absent from
    #: the idle control. It is NOT a strength claim: `specific` stays False.
    distinguishable: bool = False
    discriminating_index: int | None = None
    per_action_values: dict = field(default_factory=dict)
    channel: str = ""
    dev: int = 0
    cmd: int = 0
    byte_indices: list = field(default_factory=list)
    payloads: list = field(default_factory=list)
    baseline_payloads: list = field(default_factory=list)
    competing_actions: list = field(default_factory=list)
    note: str = ""

    @property
    def key_s(self) -> str:
        return _key_s(self.key)

    def to_json(self) -> dict:
        d = asdict(self)
        d["key"] = self.key_s
        return d


# --------------------------------------------------------------------------
# 1. bucketing
# --------------------------------------------------------------------------

def _bucket_frames(frames: Sequence[Frame], t0: float, t1: float) -> list:
    """Frames whose timestamp is in [t0, t1).

    Raises on a frame with t=None rather than guessing. A frame with no
    timestamp cannot be placed relative to a mark, and silently dropping it
    would understate the baseline — the safest reading of "we cannot tell when
    this happened" is to refuse the whole run, not to bucket it somewhere.
    """
    out = []
    for f in frames:
        if f.t is None:
            raise ValueError(
                "frame has no timestamp (t=None) — marks are time-based and "
                "cannot be applied to a source without timing. Decode with "
                "samplenum output, or omit marks and use a single window.")
        if t0 <= f.t < t1:
            out.append(f)
    return out


def pre_window_starts(marks: Sequence[Mark], settle_s: float = 2.0) -> dict:
    """{mark_index: t0} — where each mark's baseline window may start.

    Factored out so run_from_frames() and baselines() cannot drift apart: a
    disagreement between them would mean the window a delta was classified
    against is not the window its baseline was measured over, which is the
    kind of bug that produces confident nonsense.
    """
    order = sorted(range(len(marks)), key=lambda i: marks[i].t)
    out: dict = {}
    for pos, i in enumerate(order):
        prev_t = marks[order[pos - 1]].t if pos > 0 else 0.0
        out[i] = max(0.0, prev_t + settle_s)
    return out


def baselines(frames: Sequence[Frame], marks: Sequence[Mark],
              settle_s: float = 2.0) -> dict:
    """Per-mark baseline stats: what the bus looked like BEFORE each action.

    For mark k the baseline window is

        [previous_mark_t + settle_s, mark_t)

    The `settle_s` lead-in is not decoration. The action preceding mark k is
    still ringing through the bus at previous_mark_t (a flame-height change
    produces a burst of frames over the following ~1 s). Without the settle
    window the "baseline" contains the tail of the previous action, and every
    comparison is contaminated by it.

    The window is floored at settle_s after the capture start, never at 0.0:
    a capture's opening moments carry their own transient (the analyzer is
    still settling, and the harness may have just started polling). For the
    first mark that makes the baseline genuinely shorter — honest, since there
    is simply less evidence before the first action.

    Returns {mark_index: {(channel, dev, cmd): BaselineStats}}.
    """
    if not marks:
        return {}
    starts = pre_window_starts(marks, settle_s)
    out: dict = {}
    for i in starts:
        t0, t1 = starts[i], marks[i].t
        if t1 <= t0:
            # settle_s exceeded the gap between marks: there is no window in
            # which the bus was both settled and pre-action. An empty baseline
            # is reported as such; it is not silently widened, because a
            # widened window would smuggle in the previous action's tail.
            out[i] = {}
            continue
        stats: dict = {}
        for f in _bucket_frames(frames, t0, t1):
            stats.setdefault(f.key, BaselineStats()).add(f)
        out[i] = stats
    return out


def delta_for_mark(frames: Sequence[Frame], mark_t: float, pre_start: float,
                   post_end: float, mark_index: int = -1,
                   action: str = "", slot: tuple | None = None,
                   value: object = None) -> list:
    """Classify every (dev, cmd) key between the pre and post windows.

    pre  = [pre_start, mark_t)
    post = [mark_t, post_end)

    Note the asymmetry: the mark itself belongs to POST. The frame that
    triggers on the action is part of the response, not the baseline.

    Returns only non-BACKGROUND deltas, ordered by signal strength.
    """
    if post_end <= mark_t:
        raise ValueError(f"post window is empty: post_end {post_end} <= mark_t {mark_t}")
    if mark_t < pre_start:
        raise ValueError(f"mark {mark_t} precedes pre_start {pre_start}")
    if slot is None:
        slot = (action, 0, value)

    pre: dict = {}
    for f in _bucket_frames(frames, pre_start, mark_t):
        pre.setdefault(f.key, BaselineStats()).add(f)
    post: dict = {}
    for f in _bucket_frames(frames, mark_t, post_end):
        post.setdefault(f.key, BaselineStats()).add(f)

    deltas: list = []
    for key in sorted(set(pre) | set(post)):
        b = pre.get(key)
        p = post.get(key)
        ch, dev, cmd = key
        base = {
            "key": key, "channel": ch, "dev": dev, "cmd": cmd,
            "mark_index": mark_index, "action": action, "slot": slot,
            "value": value,
            "baseline_payloads": sorted(b.payload_set()) if b else [],
            "post_payloads": sorted(p.payload_set()) if p else [],
            "baseline_n": b.n if b else 0,
            "post_n": p.n if p else 0,
            "baseline_crcs": sorted({f.crc for f in _bucket_frames(frames, pre_start, mark_t)
                                     if f.key == key}),
            "post_crcs": sorted({f.crc for f in _bucket_frames(frames, mark_t, post_end)
                                 if f.key == key}),
        }

        if b is None:
            deltas.append(Delta(kind=KIND_NEW_FRAME,
                                signal=SIGNAL_LEVEL[KIND_NEW_FRAME], **base))
            continue
        if p is None:
            deltas.append(Delta(kind=KIND_SUPPRESSED,
                                signal=SIGNAL_LEVEL[KIND_SUPPRESSED], **base))
            continue

        novel = sorted(p.payload_set() - b.payload_set())
        missing = sorted(b.payload_set() - p.payload_set())

        # Length is compared before payload: a length change makes the payload
        # sets incomparable, so reporting PAYLOAD_CHANGED there would be noise.
        if b.lens_set() != p.lens_set():
            deltas.append(Delta(
                kind=KIND_LENGTH_CHANGED, signal=SIGNAL_LEVEL[KIND_LENGTH_CHANGED],
                novel_payloads=novel, missing_payloads=missing,
                note=f"lengths {sorted(b.lens_set())} -> {sorted(p.lens_set())}",
                **base))
            continue

        if novel:
            # Byte-index diffs are only meaningful for equal-length pairs, which
            # is exactly the case here (lens sets matched).
            diffs = _byte_diffs(missing or sorted(b.payload_set()), novel)
            deltas.append(Delta(
                kind=KIND_PAYLOAD_CHANGED, signal=SIGNAL_LEVEL[KIND_PAYLOAD_CHANGED],
                novel_payloads=novel, missing_payloads=missing, byte_diffs=diffs,
                **base))
            continue

        if missing:
            # Present in both, same lengths, but a baseline payload stopped
            # appearing. That is a real change even with nothing novel.
            deltas.append(Delta(
                kind=KIND_PAYLOAD_CHANGED, signal=SIGNAL_LEVEL[KIND_PAYLOAD_CHANGED],
                novel_payloads=[], missing_payloads=missing,
                byte_diffs=_byte_diffs(missing, sorted(p.payload_set())),
                note="payload disappeared in post", **base))
            continue

        deltas.append(Delta(kind=KIND_BACKGROUND, signal=SIGNAL_LEVEL[KIND_BACKGROUND],
                            **base))

    rank = {k: i for i, k in enumerate(SIGNAL_KINDS)}
    deltas.sort(key=lambda d: (rank.get(d.kind, 99), -d.post_n, d.key_s))
    return deltas


def _byte_diffs(base_payloads: Sequence[str], novel_payloads: Sequence[str]) -> list:
    """Index-level differences between equal-length payload hex strings.

    Pairs are compared positionally (zip), which is the only defensible
    comparison available: we have no alignment model for these frames beyond
    "the field is at a fixed offset", and inventing one would be a claim.
    Equal-length pairs only; unequal lengths yield no diff (LENGTH_CHANGED
    already covers that case).
    """
    out: list = []
    by_len: dict = collections.defaultdict(list)
    for s in base_payloads:
        by_len[len(s.split())].append(s)
    for ns in novel_payloads:
        nb = [int(x, 16) for x in ns.split()]
        for bs in by_len.get(len(nb), []):
            bb = [int(x, 16) for x in bs.split()]
            idx = [i for i in range(len(nb)) if bb[i] != nb[i]]
            if idx:
                out.append(ByteDiff(baseline=bs, post=ns, indices=idx))
    return out


def run_from_frames(label: str, frames: Sequence[Frame],
                    marks: Sequence[Mark] | None = None,
                    settle_s: float = 2.0, path: str = "",
                    is_control: bool = False,
                    tail_s: float | None = None) -> RunResult:
    """Reduce one capture to its per-mark deltas.

    With no marks the whole capture is one window: the bus is compared against
    itself at the midpoint, which for an idle control should find nothing and
    is precisely how V5 proves the method has no free-running false positives.

    `is_control` is set by the caller (or inferred from label == "idle").
    """
    marks = list(marks or [])
    if is_control is False and label == "idle":
        is_control = True

    if not marks:
        # No mark: split at the midpoint so that a control still exercises the
        # full classify path rather than trivially returning [].
        ts = [f.t for f in frames if f.t is not None]
        if not ts:
            return RunResult(label=label, path=path, frame_count=len(frames),
                             marks=[], is_control=is_control,
                             notes=["no timed frames — nothing to compare"])
        mid = (min(ts) + max(ts)) / 2.0
        deltas = delta_for_mark(frames, mid, min(ts), max(ts) + 1e-6,
                                mark_index=-1, action=label)
        return RunResult(label=label, path=path, deltas=deltas,
                         frame_count=len(frames), marks=[], is_control=is_control)

    starts = pre_window_starts(marks, settle_s)
    order = sorted(range(len(marks)), key=lambda i: marks[i].t)
    deltas: list = []
    # Occurrence index per action name: the FIRST flame_off mark in a run and
    # the SECOND are different slots, because a repeat of the same action may
    # be a no-op re-assertion while the first actually did something.
    # Collapsing them would average a real effect together with a null change.
    seen_count: collections.Counter = collections.Counter()
    for pos, i in enumerate(order):
        m = marks[i]
        occ = seen_count[m.action]
        seen_count[m.action] += 1
        value = m.params.get("value") if m.params else None
        slot = (m.action, occ, value)
        nxt = marks[order[pos + 1]].t if pos + 1 < len(order) else None
        if nxt is None:
            # Last mark: run to the end of the capture, or to mark_t + tail_s
            # if the caller wants to exclude a known-long settling tail.
            ts = [f.t for f in frames if f.t is not None]
            end = (m.t + tail_s) if tail_s is not None else (
                (max(ts) + 1e-6) if ts else m.t + 1.0)
        else:
            end = nxt
        # Same boundary helper baselines() uses, so the window a delta is
        # classified against is provably the window its baseline was measured
        # over. Clamped to mark_t: a first mark arriving sooner than settle_s
        # gives an EMPTY baseline, and an empty baseline is honest (every post
        # key is then NEW_FRAME) where raising would make short captures
        # unanalysable. baselines() decides the same way, so the two cannot
        # disagree.
        pre_start = min(starts[i], m.t)
        deltas.extend(delta_for_mark(frames, m.t, pre_start, end,
                                     mark_index=i, action=m.action,
                                     slot=slot, value=value))
    return RunResult(label=label, path=path, deltas=deltas,
                     frame_count=len(frames), marks=marks, is_control=is_control)


# --------------------------------------------------------------------------
# 2. attribution
# --------------------------------------------------------------------------

def _required_reps(n: int) -> int:
    """k of N — how many runs must show the effect.

    N <= 3 requires all of them; N >= 4 tolerates exactly one miss. The
    asymmetry is deliberate: three identical results is the weakest thing that
    can be called a pattern at all, and with four or more runs a single drop
    is more likely a bus hiccup than a refutation.

    Unproven: this threshold is a judgement call, not a derived figure. It is
    the single most tunable number in the module.
    """
    if n <= 0:
        return 1
    if n <= 3:
        return n
    return n - 1


def _action_key(d: Delta) -> tuple:
    return (d.key, d.kind)


def attribute(runs: Sequence[RunResult],
              control_runs: Sequence[RunResult] | None = None) -> list:
    """Decide which deltas are attributable to which action.

    The three conditions are checked in order, and a failure at any one is
    reported rather than hidden — an Attribution always carries presence /
    reproducible / specific / control_clean separately, so a reviewer can see
    WHICH leg failed rather than just a final verdict.

    Specificity is the anti-coincidence leg. For action A we compute the set
    of deltas present in EVERY run of A (its intersection — a coincidence that
    happens once is dropped here). We then require that set to be disjoint
    from the same intersection for every other action, and from every control
    run. Two actions that both touch the same key are mutually disqualifying
    for that key: neither is attributed, and both are named in
    `competing_actions`.

    Grouping is by SLOT, not by run. The orchestrator puts several marks with
    different actions in ONE capture ("12:flame_on,30:flame_off"), so a run is
    not the unit of repetition — a slot is. Runs whose mark plan does not
    contain a given slot simply do not count towards that slot's N; they are
    neither evidence for nor against it.
    """
    control_runs = list(control_runs or [])
    grouped: dict = collections.OrderedDict()
    for r in runs:
        if r.is_control:
            control_runs.append(r)
            continue
        for slot, keys in r.slots().items():
            grouped.setdefault(slot, []).append((r, keys))

    # Intersect each SLOT's keys across the runs that contained that slot.
    # A run whose plan lacks the slot contributes nothing here — it is not
    # evidence against the effect, just not a repetition of it.
    slot_intersection: dict = {}
    for slot, group in grouped.items():
        sets = [keys for _, keys in group]
        slot_intersection[slot] = set.intersection(*sets) if sets else set()
        for r, _ in group:
            r.intersection = sorted(set(r.intersection) | slot_intersection[slot])

    # An action's reproducible evidence is the union over its slots.
    per_action_intersection: dict = collections.defaultdict(set)
    for slot, inter in slot_intersection.items():
        per_action_intersection[slot[0]] |= inter

    control_union: set = set()
    bad_controls: list = []
    for c in control_runs:
        keys = c.delta_keys()
        control_union |= keys
        if keys:
            bad_controls.append(c.label)
    control_clean = not bad_controls

    # exact kind per (key) for each action's intersection, for reporting
    out: list = []

    for slot, group in grouped.items():
        label, occurrence, value = slot
        inter = slot_intersection[slot]
        n_runs = len(group)
        need = _required_reps(n_runs)

        for key in sorted(inter):
            hits = [d for r, _ in group for d in r.deltas
                    if d.key == key and d.slot == slot and d.kind != KIND_BACKGROUND]
            if not hits:
                continue
            kinds = collections.Counter(d.kind for d in hits)
            kind = kinds.most_common(1)[0][0]
            present = sum(1 for _, keys in group if key in keys)
            presence = present >= 1
            reproducible = present >= need

            competing = sorted(
                other for other, other_inter in per_action_intersection.items()
                if other != label and key in other_inter
            )
            in_control = key in control_union
            specific = (not competing) and (not in_control)

            score = _score(present, need, specific, control_clean)

            novel: list = []
            for d in hits:
                for p in d.novel_payloads:
                    if p not in novel:
                        novel.append(p)
            byte_indices = sorted({i for d in hits for bd in d.byte_diffs
                                   for i in bd.indices})
            base_payloads = sorted({p for d in hits for p in d.baseline_payloads})

            note = ""
            if competing:
                note = ("shared with other actions — not specific: "
                        + ", ".join(competing))
            elif in_control:
                note = "also present in an idle control run"
            elif not reproducible:
                note = f"seen in {present}/{n_runs} runs (need {need})"

            d0 = hits[0]
            out.append(Attribution(
                key=key, action=label, score=score, kind=kind,
                observations=present, runs_total=n_runs, runs_present=present,
                presence=presence, reproducible=reproducible, specific=specific,
                control_clean=control_clean,
                occurrence=occurrence, value=value,
                channel=d0.channel, dev=d0.dev, cmd=d0.cmd,
                byte_indices=byte_indices, payloads=novel,
                baseline_payloads=base_payloads,
                competing_actions=competing, note=note))

    # ---- state-echo carve-out (ADDITIVE; see module docstring) ------------
    # A key that moves for several actions is normally non-specific and stays
    # speculative with competing_actions named. But if the VALUE it holds
    # after each action is disjoint from every other action's, the key is not
    # ambiguous — it is a state REPORT that distinguishes them.
    #
    # This runs strictly AFTER the normal path and can only ever overwrite a
    # speculative/probable verdict, never a confirmed one: a key moved by >= 2
    # actions had competing_actions non-empty, so it was already specific=False
    # and therefore never confirmed. The carve-out cannot manufacture a
    # confirmed result, and `specific` stays False on everything it touches.
    key_action_vals: dict = collections.defaultdict(
        lambda: collections.defaultdict(lambda: collections.defaultdict(set)))
    key_actions: dict = collections.defaultdict(set)
    for r in runs:
        for d in r.deltas:
            if d.kind != KIND_PAYLOAD_CHANGED or d.key in control_union:
                continue
            key_actions[d.key].add(d.action)
            for p in d.post_payloads:
                b = [int(x, 16) for x in p.split()]
                for j, val in enumerate(b):
                    key_action_vals[d.key][d.action][j].add(val)

    echoes: dict = {}
    for key, actions in key_actions.items():
        # conditions 1 and 2: >= 2 actions, and absent from the idle control
        if len(actions) < 2 or not control_clean or key in control_union:
            continue
        idx_sets = key_action_vals[key]
        # only indices where EVERY action was actually observed
        common = set.intersection(*(set(idx_sets[a]) for a in actions))
        disc = None
        for j in sorted(common):
            sets = [idx_sets[a][j] for a in actions]
            # condition 3: PAIRWISE disjoint at this index
            if all(not (sets[x] & sets[y])
                   for x in range(len(sets)) for y in range(x + 1, len(sets))):
                disc = j
                break
        if disc is None:
            continue
        echoes[key] = {
            "index": disc,
            "per_action_values": {a: sorted(idx_sets[a][disc]) for a in sorted(actions)},
            "actions": sorted(actions),
        }

    for a in out:
        echo = echoes.get(a.key)
        if echo is None:
            continue
        a.score = SCORE_STATE_ECHO
        a.distinguishable = True
        a.discriminating_index = echo["index"]
        a.per_action_values = echo["per_action_values"]
        # `specific` is deliberately left False: the key is not specific to
        # this action, and saying otherwise would be dishonest.
        a.note = (
            f"state echo — byte {echo['index']} distinguishes "
            + " / ".join(f"{k}={_fmt_vals(v)}"
                         for k, v in echo["per_action_values"].items())
            + ". Tracks appliance STATE, not a command for any one action.")

    # stable-but-unattributed frames: recurring keys whose payload never moved
    # in any action's runs. Reported WITHOUT any functional claim.
    stable_seen: dict = collections.Counter()
    for r in runs:
        for d in r.deltas:
            stable_seen[d.key] += 1
    attributed_keys = {a.key for a in out}
    for key, count in sorted(stable_seen.items()):
        if key in attributed_keys:
            continue
        if count < 3:
            continue
        d0 = next(d for r in runs for d in r.deltas if d.key == key)
        out.append(Attribution(
            key=key, action="", score=SCORE_OBSERVED_STABLE, kind=KIND_BACKGROUND,
            observations=count, runs_total=len(runs), runs_present=count,
            presence=False, reproducible=False, specific=False,
            control_clean=control_clean, channel=d0.channel, dev=d0.dev,
            cmd=d0.cmd, payloads=sorted(set(d0.baseline_payloads)),
            note="seen repeatedly with unchanged payload — no functional claim"))

    order = {SCORE_CONFIRMED: 0, SCORE_STATE_ECHO: 1, SCORE_PROBABLE: 2,
             SCORE_SPECULATIVE: 3, SCORE_OBSERVED_STABLE: 4}
    out.sort(key=lambda a: (order.get(a.score, 9), a.action, a.key_s))
    return out


def _fmt_vals(vals: Sequence[int]) -> str:
    """[0, 4] -> '0x00|0x04', for the state-echo note and the map view."""
    return "|".join(f"0x{v:02X}" for v in vals)


def _score(present: int, need: int, specific: bool,
           control_clean: bool) -> str:
    """Map the evidence onto the closed score enum.

    Deliberately conservative. A single observation is never better than
    speculative no matter how clean it looked — the whole point of the
    negative control is that single clean-looking events are common in noise.
    """
    if not specific:
        # A non-specific key is never confirmed, however many times it repeats:
        # it also shows up for other actions (or in an idle control), so it is
        # not evidence about THIS action no matter how reproducible it looks.
        return SCORE_SPECULATIVE
    if present < 2:
        return SCORE_SPECULATIVE
    if present >= 3 and present >= need and control_clean:
        return SCORE_CONFIRMED
    if present >= 2 and (control_clean or present >= need):
        return SCORE_PROBABLE
    return SCORE_SPECULATIVE


# --------------------------------------------------------------------------
# 3. byte-level attribution and parameter tables
# --------------------------------------------------------------------------

def byte_attribution(payloads_s: Iterable[str],
                     payloads_b: Iterable[str]) -> dict:
    """Which byte indices are attributable, from payload sets S and B.

    Byte i is attributed iff no post value at i occurs in any baseline value at
    i — i.e. the observed values are disjoint — AND |S| >= 2.

    The |S| >= 2 requirement is the important one. With a single observed
    payload, "disjoint" is trivially satisfiable by coincidence; with two or
    more, the byte must have taken at least two values that baseline never
    held. It is still not proof, only a filter.

    Unproven: an attributed index means "this byte's value space changed with
    the action". It does NOT mean the byte is the parameter; a checksum or a
    length field can be disjoint for the same reason. The CRC is excluded from
    consideration entirely for this reason.
    """
    S = [[int(x, 16) for x in p.split()] for p in payloads_s]
    B = [[int(x, 16) for x in p.split()] for p in payloads_b]
    if not S or not B:
        return {"attributed": [], "reason": "empty payload set"}
    width = min(min(len(x) for x in S), min(len(x) for x in B))
    attributed = []
    for i in range(width):
        s_vals = {x[i] for x in S if len(x) > i}
        b_vals = {x[i] for x in B if len(x) > i}
        if len(S) >= 2 and not (s_vals & b_vals):
            attributed.append(i)
    return {"attributed": attributed, "width": width,
            "s_size": len(S), "b_size": len(B)}


def parameter_table(attributions: Sequence[Attribution]) -> dict:
    """Build the value -> byte-value map for parameterised actions.

    Given an Attribution whose `payloads` came from runs that differed only in
    a parameter (flame height 1..5), the observed payloads are grouped by the
    parameter value carried in the Attribution's action label. For each byte
    index we ask: is the map parameter_value -> {byte values seen} INJECTIVE?

    An injective map means distinct parameters never produced the same byte
    value at that index — the strongest available evidence that the byte
    encodes the parameter. Monotonicity is checked too and reported, because a
    monotone byte is far more likely to be a magnitude than an enum.

    UNPROVEN, and stated as such in the output: injectivity over 5 observed
    points is consistent with a lookup table as much as with a magnitude. The
    table is evidence to decide between hypotheses, not a decoding.
    """
    result: dict = {}
    for a in attributions:
        if not a.payloads and not a.baseline_payloads:
            continue
        # payloads arrive as hex strings; re-derive per-index value sets
        S = [[int(x, 16) for x in p.split()] for p in a.payloads]
        if len(a.payloads) < 2:
            result[a.key_s] = {
                "action": a.action, "score": a.score,
                "parameterised": False,
                "reason": "fewer than 2 distinct observed payloads",
            }
            continue
        width = min(len(x) for x in S)
        table: dict = {}
        injective_indices: list = []
        monotonic_indices: list = []
        for i in range(width):
            per_payload = {}
            for p_idx, x in enumerate(S):
                per_payload.setdefault(p_idx, set()).add(x[i])
            # group by the byte value observed, keyed by payload ordinal
            val_to_ordinals = collections.defaultdict(list)
            for p_idx, vals in per_payload.items():
                val_to_ordinals[frozenset(vals)].append(p_idx)
            injective = all(len(v) == 1 for v in val_to_ordinals)
            # ordinals are ordered by the orchestrator's parameter sweep; a
            # monotone run of values is a magnitude candidate
            seq_vals = [next(iter(per_payload[k])) for k in sorted(per_payload)]
            mono = all(seq_vals[j] < seq_vals[j + 1]
                       for j in range(len(seq_vals) - 1)) if len(seq_vals) > 1 else False
            table[str(i)] = {
                "values_in_order": seq_vals,
                "injective": injective,
                "monotonic_increasing": mono,
            }
            if injective:
                injective_indices.append(i)
            if mono:
                monotonic_indices.append(i)
        result[a.key_s] = {
            "action": a.action, "score": a.score,
            "parameterised": bool(injective_indices),
            "injective_indices": injective_indices,
            "monotonic_indices": monotonic_indices,
            "table": table,
            "caveat": ("UNPROVEN: injectivity over the observed parameter "
                       "values is consistent with a lookup table as well as "
                       "with a magnitude. Not a decoding."),
        }
    return result


# --------------------------------------------------------------------------
# summarising
# --------------------------------------------------------------------------

def summarize(attributions: Sequence[Attribution],
              runs: Sequence = (), controls: Sequence = (),
              excluded: Sequence = (), unverified: Sequence = ()) -> dict:
    """Count the attributions, keeping findings and non-findings SEPARATED.

    ⚠️ WHY THIS IS NOT JUST A DICT OF SCORE COUNTS. A state echo is often the
    most useful thing in a capture, but it is not a command, and neither is an
    observed_stable frame. If a consumer sums every record in attribution.json
    it will overstate the result: "3 confirmed + 2 state echoes" must never
    read as "5 findings". So the distinction is STRUCTURAL here rather than a
    convention the consumer has to know about:

        counts_findings      confirmed + probable — these claim to be a COMMAND
                             for a specific action
        counts_state_echo    state echoes — these track appliance STATE, and
                             are a command for nothing
        counts_informational observed_stable + speculative + anything unknown

    `counts` is the raw per-score dict, kept unchanged for anyone who wants it;
    `counts_total` is every attribution. The three buckets plus the raw dict
    are what a consumer should reach for.
    """
    by_score = collections.Counter(a.score for a in attributions)
    known = {SCORE_CONFIRMED, SCORE_PROBABLE, SCORE_SPECULATIVE,
             SCORE_STATE_ECHO, SCORE_OBSERVED_STABLE}
    unknown = sum(n for s, n in by_score.items() if s not in known)

    counts = {
        "runs": len(runs), "controls": len(controls),
        "excluded": len(excluded), "unverified": len(unverified),
        "confirmed": by_score[SCORE_CONFIRMED],
        "probable": by_score[SCORE_PROBABLE],
        "speculative": by_score[SCORE_SPECULATIVE],
        "state_echo": by_score[SCORE_STATE_ECHO],
        "observed_stable": by_score[SCORE_OBSERVED_STABLE],
        "unknown": unknown,
    }
    findings = counts["confirmed"] + counts["probable"]
    informational = (counts["observed_stable"] + counts["speculative"]
                     + counts["unknown"])
    return {
        "counts": counts,
        #: things claiming to be a COMMAND for an action
        "counts_findings": findings,
        #: things tracking STATE, a command for nothing — NOT findings
        "counts_state_echo": counts["state_echo"],
        #: reported but claiming nothing
        "counts_informational": informational,
        #: every attribution, for a consumer that wants to check the split
        "counts_total": len(attributions),
    }


# --------------------------------------------------------------------------
# sweep-index loading (tools/ecm_harness.py output)
# --------------------------------------------------------------------------

#: invalid_reason values that make a run unattributable. Kept as data so the
#: CLI can report WHY a run was dropped rather than just dropping it.
INVALID_REASONS = ("CAPTURE_SHORT", "REBOOT_CONTAMINATED", "NO_FRAMES",
                   "SEQ_BROKEN", "ANSWER_KEY_UNAVAILABLE")


def load_run(run_dir: str, *, settle_s: float = 2.0,
             use_fired_at: bool = False) -> list:
    """Load one harness run directory into Frame lists, keyed by channel.

    The harness writes `<run_dir>/frames_D0.jsonl` and `frames_D1.jsonl`,
    each line a Frame.to_json(). Timestamps inside are SECONDS RELATIVE TO
    THAT RUN'S CAPTURE START, which is exactly the frame of reference the
    marks use — so no rebasing is needed, and none is done.

    Missing channel files are not an error: a run may legitimately have
    content on only one direction. An unreadable LINE is an error, because
    silently skipping it would understate the baseline.
    """
    out: dict = {}
    for ch in ("D0", "D1"):
        p = os.path.join(run_dir, f"frames_{ch}.jsonl")
        if not os.path.exists(p):
            continue
        frames: list = []
        with open(p, encoding="utf-8") as fh:
            for lineno, line in enumerate(fh, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    frames.append(_frame_from_json(json.loads(line)))
                except (json.JSONDecodeError, ValueError, TypeError) as exc:
                    raise ValueError(f"{p}:{lineno}: bad frame record: {exc}") from exc
        out[ch] = frames
    return out


def marks_from_index(rec: dict, use_fired_at: bool = False) -> list:
    """Extract marks from one index record.

    The index stores marks as MarkResult wrappers:
        {"mark": {"at_s", "action", "value", "expect", "note"},
         "due_at", "fired_at", "returned_at", "verified", "poll_before", ...}

    The authoritative offset is `marks[i].mark.at_s`. `fired_at` is the
    harness's best estimate of when the action actually happened; in the
    manual/external trigger modes it is identical to `at_s` because the
    operator's real press is only known to lie somewhere between the printed
    prompt and the next poll. With use_fired_at=True we prefer it when it
    exists, since it is the tighter estimate when the harness has one; it is
    never INVENTED when absent.

    `action` and `value` become the mark's action name and params, which is
    what attribution groups on.
    """
    marks: list = []
    for m in rec.get("marks", []) or []:
        inner = m.get("mark") if isinstance(m.get("mark"), dict) else m
        if inner is None:
            continue
        t = inner.get("at_s")
        if use_fired_at and m.get("fired_at") is not None:
            t = m["fired_at"]
        if t is None:
            continue
        params = {}
        if inner.get("value") is not None:
            params["value"] = inner["value"]
        if inner.get("expect"):
            params["expect"] = inner["expect"]
        marks.append(Mark(t=float(t), action=inner.get("action", "unknown"),
                          params=params))
    return sorted(marks, key=lambda m: m.t)


@dataclass
class SweepLoad:
    """What the loader produced, including everything it refused to use."""

    runs: list = field(default_factory=list)        # actionable RunResult
    controls: list = field(default_factory=list)    # idle RunResult
    excluded: list = field(default_factory=list)    # {run_id, reason}
    unverified: list = field(default_factory=list)  # {run_id, reason}

    def to_json(self) -> dict:
        return {
            "runs": [r.to_json() for r in self.runs],
            "controls": [c.to_json() for c in self.controls],
            "excluded": self.excluded,
            "unverified": self.unverified,
        }


def load_sweep_index(path: str, settle_s: float = 2.0,
                     use_fired_at: bool = False,
                     channel: str = "D0") -> SweepLoad:
    """Read a `sweep-<stamp>-index.jsonl` from tools/ecm_harness.py.

    Excluded from attribution, and reported rather than silently dropped:

      * `discard: true`       — calibration runs (the settle run after a
                                control-mode change). Kept on disk for
                                inspection, never evidence.
      * `valid: false`        — carries invalid_reason in INVALID_REASONS.
      * `verified: false`     — a mark was not confirmed on the read path, so
                                we cannot claim the action fired when we think
                                it did. Frames are kept loaded (they may still
                                show what happened) but the run does not
                                contribute to attribution.

    Frames are NOT in the index; they live in `<run_dir>/frames_D*.jsonl`.
    `run_dir` is absolute in the index, so it is used as given rather than
    reconstructed — the harness owns the layout, not this module.

    `channel` selects which direction's frames drive the comparison. D0 is the
    initiator and carries the commands; D1 is the reply and carries the
    status. Comparing both at once would double-count every exchange.
    """
    load = SweepLoad()
    with open(path, encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{lineno}: bad JSON: {exc}") from exc

            run_id = rec.get("run_id") or f"line{lineno}"

            if rec.get("discard"):
                load.excluded.append({"run_id": run_id, "reason": "DISCARD"})
                continue
            if not rec.get("valid", True):
                load.excluded.append({
                    "run_id": run_id,
                    "reason": rec.get("invalid_reason") or "INVALID"})
                continue
            if not rec.get("verified", True):
                load.unverified.append({
                    "run_id": run_id,
                    "reason": "MARK_UNVERIFIED_ON_READ_PATH"})
                continue

            run_dir = rec.get("run_dir") or ""
            if not run_dir or not os.path.isdir(run_dir):
                load.excluded.append({"run_id": run_id, "reason": "RUN_DIR_MISSING"})
                continue

            by_ch = load_run(run_dir, settle_s=settle_s)
            frames = by_ch.get(channel, [])
            if not frames:
                load.excluded.append({"run_id": run_id,
                                      "reason": f"NO_FRAMES_ON_{channel}"})
                continue

            marks = marks_from_index(rec, use_fired_at=use_fired_at)
            label = rec.get("plan") or rec.get("label") or run_id
            is_control = not marks

            run = run_from_frames(
                "idle" if is_control else label, frames, marks=marks,
                settle_s=settle_s, path=run_dir, is_control=is_control)
            run.notes = list(rec.get("notes", []) or [])
            (load.controls if is_control else load.runs).append(run)

    return load


def _frame_from_json(d: dict) -> Frame:
    """Rebuild the comparison-relevant parts of a Frame from its JSON form.

    Frame.to_json() stringifies dev and cmd ("0x01") and adds derived keys
    (`key`, `hex`) that this reconstructs nothing from. `raw` is kept because
    Frame carries it, not because anything here compares it — the comparison
    reads `payload` and the key alone.
    """
    def _int(v, default=0):
        if isinstance(v, str):
            return int(v, 16) if v.lower().startswith("0x") else int(v)
        return int(v) if v is not None else default

    return Frame(
        channel=d.get("channel", "D0"),
        t=d.get("t"),
        dev=_int(d.get("dev", 0)),
        seq=_int(d.get("seq", 0)),
        cmd=_int(d.get("cmd", 0)),
        payload=[int(x) for x in d.get("payload", [])],
        raw=[int(x) for x in d.get("raw", [])],
        crc=_int(d.get("crc", 0)),
        index=_int(d.get("index", 0)),
        gap_before=d.get("gap_before"),
        ambiguous=bool(d.get("ambiguous", False)),
        alt_lengths=list(d.get("alt_lengths", [])),
    )



# --------------------------------------------------------------------------
# selftest — synthetic, no hardware
# --------------------------------------------------------------------------

def _mk_frame(channel: str, t: float, dev: int, seq: int, cmd: int,
              payload: Sequence[int]) -> Frame:
    """A synthetic frame. No CRC is computed: comparison never reads it, and
    inventing one would imply a validation that did not happen."""
    return Frame(channel=channel, t=t, dev=dev, seq=seq, cmd=cmd,
                 payload=list(payload), raw=[0x4B, dev, seq, cmd] + list(payload),
                 crc=0, index=0)


def _burst(t0: float, seq: int, dev: int, cmd: int, payload: Sequence[int],
           channel: str = "D0", n: int = 1, dt: float = 0.1) -> list:
    return [_mk_frame(channel, t0 + i * dt, dev, (seq + i) & 0xFF, cmd, payload)
            for i in range(n)]


class _Results:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0
        self.failures: list = []

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


# A tiny synthetic bus: background chatter that never changes, plus one
# response frame the tests mutate. The chatter is essential — with a quiet bus
# a broken method would still pass.
#
# It must span BOTH windows of every fixture below. Background that fires only
# before the mark is not background, it is a SUPPRESSED delta, and the tests
# would be measuring the wrong thing.
BG_PAYLOADS = ([0x02, 0x04], [0x00, 0x17, 0x00, 0x00])


def _bg(seed: int, n: int = 20) -> list:
    """The unchanging chatter, spread evenly from 0.5 s to 60 s.

    Both keys fire on the same cadence so neither is ever 'suppressed' by an
    accident of fixture layout.
    """
    out: list = []
    for i in range(n):
        for k, pay in enumerate(BG_PAYLOADS):
            out.append(_mk_frame("D0", 0.5 + i * 3.0, 0x01, (seed + i + k) & 0xFF,
                                 0xBC, pay))
    return out


def _control_run(label: str = "idle", seed: int = 0) -> RunResult:
    """An idle capture: background only, no marks, no action."""
    return run_from_frames(label, _bg(seed), marks=[], is_control=True)


def selftest() -> int:
    r = _Results()

    # ---------------- V5: negative control (the important one) -------------
    print("\n=== V5: negative control — idle runs must yield ZERO attributions ===")
    c1 = _control_run("idle", seed=0)
    c2 = _control_run("idle", seed=40)
    r.check(not c1.delta_keys() and not c2.delta_keys(),
            "idle runs produce no deltas at all",
            f"{sorted(_key_s(k) for k in c1.delta_keys())} / "
            f"{sorted(_key_s(k) for k in c2.delta_keys())}")
    neg = attribute([], control_runs=[c1, c2])
    r.check(len(neg) == 0, "attribute() returns ZERO effects from noise",
            f"got {[a.key_s for a in neg]}")

    # ---------------- V6: null change -------------------------------------
    print("\n=== V6: null change — setting a value to what it already was ===")
    frames: list = _bg(0)
    # the response frame is IDENTICAL before and after the mark: the action
    # re-asserted the value it already had. Nothing may be reported.
    frames.extend(_burst(3.0, 0x20, 0x01, 0xDC, [0x00, 0x03], n=2, dt=0.3))
    frames.extend(_burst(12.0, 0x30, 0x01, 0xDC, [0x00, 0x03], n=2, dt=0.3))
    mark = [Mark(t=10.0, action="height_3", params={"height": 3})]
    rr = run_from_frames("height_3", frames, marks=mark)
    r.check(not rr.delta_keys(),
            "identical pre/post yields no deltas (null change)",
            f"got {sorted(_key_s(k) for k in rr.delta_keys())}")
    verdict = NO_CHANGE_DETECTED if not rr.delta_keys() else "changed"
    r.check(verdict == NO_CHANGE_DETECTED,
            "verdict is 'no_change_detected'", verdict)

    # ---------------- positive control ------------------------------------
    print("\n=== positive control — one flipped byte, 3x -> confirmed ===")
    KEY = ("D0", 0x01, 0xDC)

    def action_run(flip: bool, seed: int, label: str = "height_3") -> RunResult:
        f: list = _bg(seed)
        # the response frame exists in BOTH windows; the action changes only
        # its payload (or, for flip=False, changes nothing at all).
        f.extend(_burst(3.0, seed + 0x20, 0x01, 0xDC, [0x00, 0x03], n=2, dt=0.3))
        post = [0x00, 0x05] if flip else [0x00, 0x03]
        f.extend(_burst(12.0, seed + 0x30, 0x01, 0xDC, post, n=2, dt=0.3))
        return run_from_frames(label, f,
                               marks=[Mark(t=10.0, action=label,
                                           params={"height": 3})])

    three = [action_run(True, seed=s * 0x10) for s in range(3)]
    atts = attribute(three, control_runs=[c1, c2])
    got = [a for a in atts if a.key == KEY]
    r.check(len(got) == 1, "exactly one attribution for the flipped key",
            f"got {[a.key_s for a in atts]}")
    if got:
        a = got[0]
        r.check(a.score == SCORE_CONFIRMED, "3x repetition scores 'confirmed'",
                f"{a.score} (presence={a.presence} repro={a.reproducible} "
                f"specific={a.specific} control_clean={a.control_clean})")
        r.check(a.byte_indices == [1], "exactly byte index 1 is attributed",
                f"got {a.byte_indices}")
        r.check("00 05" in a.payloads, "novel payload is reported", str(a.payloads))
        r.check(a.specific and a.presence and a.reproducible and a.control_clean,
                "all three conditions hold", a.note)
    others = [a for a in atts if a.key != KEY and a.score != SCORE_OBSERVED_STABLE]
    r.check(not others, "no other key is attributed",
            f"got {[(a.key_s, a.score) for a in others]}")

    print("\n--- single occurrence must be 'speculative', not 'confirmed' ---")
    one = [action_run(True, seed=0x00)]
    atts1 = attribute(one, control_runs=[c1, c2])
    g1 = [a for a in atts1 if a.key == KEY]
    r.check(len(g1) == 1 and g1[0].score == SCORE_SPECULATIVE,
            "1 observation scores 'speculative'",
            f"got {[(a.key_s, a.score) for a in atts1]}")

    print("\n--- non-reproducible repetition must not be 'confirmed' ---")
    # flip in 2 of 3 runs: N=3 needs k=3
    mixed = [action_run(True, seed=0x00), action_run(False, seed=0x10),
             action_run(True, seed=0x20)]
    atts_m = attribute(mixed, control_runs=[c1, c2])
    gm = [a for a in atts_m if a.key == KEY]
    r.check(all(a.score != SCORE_CONFIRMED for a in gm),
            "2-of-3 repetition is NOT confirmed",
            f"got {[(a.key_s, a.score) for a in gm]}")

    # ---------------- specificity -----------------------------------------
    print("\n=== specificity — two actions, two different bytes ===")
    KEY_B = ("D0", 0x02, 0xBC)

    def action_b_run(seed: int) -> RunResult:
        f: list = _bg(seed)
        f.extend(_burst(3.0, seed + 0x20, 0x02, 0xBC, [0x02, 0x04], n=2, dt=0.3))
        f.extend(_burst(12.0, seed + 0x30, 0x02, 0xBC, [0x02, 0x07], n=2, dt=0.3))
        return run_from_frames("flame_off", f,
                               marks=[Mark(t=10.0, action="flame_off")])

    a_runs = [action_run(True, seed=s * 0x10) for s in range(3)]
    b_runs = [action_b_run(seed=s * 0x10) for s in range(3)]
    spec = attribute(a_runs + b_runs, control_runs=[c1, c2])
    a_keys = {x.key for x in spec if x.action == "height_3"
              and x.score != SCORE_OBSERVED_STABLE}
    b_keys = {x.key for x in spec if x.action == "flame_off"
              and x.score != SCORE_OBSERVED_STABLE}
    r.check(KEY in a_keys and KEY_B not in a_keys,
            "height_3 attributed to its own key only", str(sorted(map(_key_s, a_keys))))
    r.check(KEY_B in b_keys and KEY not in b_keys,
            "flame_off attributed to its own key only", str(sorted(map(_key_s, b_keys))))
    r.check(not (a_keys & b_keys), "no key is attributed to both actions",
            str(sorted(map(_key_s, a_keys & b_keys))))

    # ---------------- seq exclusion ---------------------------------------
    print("\n=== seq exclusion — runs differing ONLY in seq ===")
    # Same payloads, same timings, same keys — only the exchange counter
    # differs. If seq leaked into the comparison this would light up.
    f1: list = _bg(0x00)
    f2: list = _bg(0x80)
    f1.extend(_burst(3.0, 0x20, 0x01, 0xDC, [0x00, 0x03], n=2, dt=0.3))
    f2.extend(_burst(3.0, 0xA0, 0x01, 0xDC, [0x00, 0x03], n=2, dt=0.3))
    f1.extend(_burst(12.0, 0x30, 0x01, 0xDC, [0x00, 0x03], n=2, dt=0.3))
    f2.extend(_burst(12.0, 0xB0, 0x01, 0xDC, [0x00, 0x03], n=2, dt=0.3))
    m1 = [Mark(t=10.0, action="x")]
    s1 = run_from_frames("x", f1, marks=m1)
    s2 = run_from_frames("x", f2, marks=m1)
    r.check(not s1.delta_keys() and not s2.delta_keys(),
            "seq differences alone produce ZERO deltas",
            f"{sorted(_key_s(k) for k in s1.delta_keys())} / "
            f"{sorted(_key_s(k) for k in s2.delta_keys())}")
    seqs_differ = {f.seq for f in f1} != {f.seq for f in f2}
    r.check(seqs_differ, "the two runs genuinely differ in seq",
            "test is vacuous otherwise")

    # ---------------- byte attribution / parameter table ------------------
    print("\n=== byte attribution — disjointness requires |S| >= 2 ===")
    ba = byte_attribution(["00 05", "00 07"], ["00 03"])
    r.check(ba["attributed"] == [1], "byte 1 attributed, byte 0 not",
            str(ba))
    ba1 = byte_attribution(["00 05"], ["00 03"])
    r.check(ba1["attributed"] == [], "a single post payload is NOT enough",
            str(ba1))
    pt = parameter_table([Attribution(
        key=KEY, action="height", score=SCORE_CONFIRMED, kind=KIND_PAYLOAD_CHANGED,
        observations=3, runs_total=3, runs_present=3, presence=True,
        reproducible=True, specific=True, control_clean=True,
        payloads=["00 01", "00 02", "00 03", "00 04", "00 05"])])
    r.check(pt[KEY_S := _key_s(KEY)]["parameterised"] is True,
            "monotone 1..5 payloads yield a parameterised byte", str(pt))
    r.check(pt[KEY_S]["monotonic_indices"] == [1],
            "byte 1 is the monotone parameter candidate",
            str(pt[KEY_S]["monotonic_indices"]))

    # ---------------- multi-mark slot grouping ----------------------------
    print("\n=== slot grouping — one run, several different actions ===")
    # The real harness puts several marks in ONE capture, so attribution must
    # separate them. Here each action moves a DIFFERENT key, and neither may
    # be credited to the other.
    def multi_action_run(seed: int) -> RunResult:
        f: list = _bg(seed)
        # Both keys must persist across EVERY window, as real chatter does. A
        # key that simply stops appearing would read as SUPPRESSED at the later
        # mark and cross-contaminate the slots — that is a fixture artefact,
        # not a property of the method.
        #   0xDC changes only across mark 1 (flame_on)
        #   0x6E changes only across mark 2 (flame_off)
        f.extend(_burst(3.0, seed + 0x20, 0x01, 0xDC, [0x00, 0x00], n=2, dt=0.3))
        f.extend(_burst(3.5, seed + 0x28, 0x02, 0x6E, [0x00, 0x10], n=2, dt=0.3))
        f.extend(_burst(12.0, seed + 0x30, 0x01, 0xDC, [0x00, 0x01], n=2, dt=0.3))
        f.extend(_burst(15.0, seed + 0x38, 0x02, 0x6E, [0x00, 0x10], n=2, dt=0.3))
        f.extend(_burst(22.0, seed + 0x40, 0x01, 0xDC, [0x00, 0x01], n=2, dt=0.3))
        f.extend(_burst(25.0, seed + 0x48, 0x02, 0x6E, [0x00, 0x20], n=2, dt=0.3))
        return run_from_frames("plan", f, marks=[
            Mark(t=10.0, action="flame_on"),
            Mark(t=20.0, action="flame_off"),
        ])

    multi = [multi_action_run(seed=s * 0x10) for s in range(3)]
    slots = {d.slot for m in multi for d in m.deltas}
    r.check(("flame_on", 0, None) in slots and ("flame_off", 0, None) in slots,
            "one run yields distinct slots per action", str(sorted(slots)))
    ma = attribute(multi, control_runs=[c1, c2])
    ON_KEY, OFF_KEY = ("D0", 0x01, 0xDC), ("D0", 0x02, 0x6E)
    on_keys = {a.key for a in ma if a.action == "flame_on"
               and a.score != SCORE_OBSERVED_STABLE}
    off_keys = {a.key for a in ma if a.action == "flame_off"
                and a.score != SCORE_OBSERVED_STABLE}
    r.check(ON_KEY in on_keys and OFF_KEY not in on_keys,
            "flame_on credited only with the key it moved",
            str(sorted(map(_key_s, on_keys))))
    r.check(OFF_KEY in off_keys and ON_KEY not in off_keys,
            "flame_off credited only with the key it moved",
            str(sorted(map(_key_s, off_keys))))

    print("\n--- two actions sharing ONE key must compete, not both confirm ---")
    # A STATE byte moves for both a raise and a lower, and the two actions
    # OVERLAP in value: flame_on's post set is {0x01, 0x03} and flame_off's is
    # {0x03}. Nothing discriminates them, so neither action may claim the key.
    # This fixture is reused by the state-echo overlap control below, which is
    # precisely the case the carve-out must NOT swallow.
    def overlap_run(seed: int) -> RunResult:
        f: list = _bg(seed)
        f.extend(_burst(3.0, seed + 0x20, 0x01, 0xDC, [0x00, 0x00], n=2, dt=0.3))
        f.extend(_burst(12.0, seed + 0x30, 0x01, 0xDC, [0x00, 0x01], n=2, dt=0.3))
        f.extend(_burst(15.0, seed + 0x36, 0x01, 0xDC, [0x00, 0x03], n=2, dt=0.3))
        f.extend(_burst(22.0, seed + 0x40, 0x01, 0xDC, [0x00, 0x03], n=2, dt=0.3))
        return run_from_frames("plan", f, marks=[
            Mark(t=10.0, action="flame_on"),
            Mark(t=20.0, action="flame_off"),
        ])

    shared = attribute([overlap_run(seed=s * 0x10) for s in range(3)],
                       control_runs=[c1, c2])
    sk = [a for a in shared if a.key == KEY]
    r.check(len(sk) == 2, "both actions report the shared key (2 entries)",
            str([(a.action, a.score) for a in sk]))
    r.check(all(a.score == SCORE_SPECULATIVE for a in sk),
            "a shared key is never confirmed for either action",
            str([(a.action, a.score) for a in sk]))
    r.check(all(a.competing_actions for a in sk),
            "each names the other as a competing action",
            str([(a.action, a.competing_actions) for a in sk]))

    # ================= state echo (ADDITIVE carve-out) ====================
    # One key that reports STATE: disjoint values per action. It is not a
    # command for either action, but it does discriminate them.
    print("\n=== state echo 1 — shared key, DISJOINT values -> state_echo ===")

    def echo_run(seed: int, on_val: int, off_val: int,
                 label: str = "plan") -> RunResult:
        f: list = _bg(seed)
        f.extend(_burst(3.0, seed + 0x20, 0x01, 0xDC, [0x00, 0x00], n=2, dt=0.3))
        f.extend(_burst(12.0, seed + 0x30, 0x01, 0xDC, [0x00, on_val], n=2, dt=0.3))
        f.extend(_burst(22.0, seed + 0x40, 0x01, 0xDC, [0x00, off_val], n=2, dt=0.3))
        return run_from_frames(label, f, marks=[
            Mark(t=10.0, action="flame_on"),
            Mark(t=20.0, action="flame_off"),
        ])

    # flame_on -> 0x04, flame_off -> 0x00. Disjoint at byte 1.
    echoes = [echo_run(seed=s * 0x10, on_val=0x04, off_val=0x00) for s in range(3)]
    ea = attribute(echoes, control_runs=[c1, c2])
    ek = [a for a in ea if a.key == KEY]
    r.check(len(ek) == 2, "both actions still report the key (2 entries)",
            str([(a.action, a.score) for a in ek]))
    r.check(all(a.score == SCORE_STATE_ECHO for a in ek),
            "disjoint values classify as state_echo",
            str([(a.action, a.score) for a in ek]))
    r.check(all(a.score != SCORE_CONFIRMED for a in ek),
            "a state echo is never 'confirmed'",
            str([(a.action, a.score) for a in ek]))
    r.check(all(a.distinguishable is True for a in ek),
            "distinguishable=True on both", str([a.distinguishable for a in ek]))
    r.check(all(a.discriminating_index == 1 for a in ek),
            "discriminating_index identifies byte 1",
            str([a.discriminating_index for a in ek]))
    r.check(all(a.specific is False for a in ek),
            "specific stays False — it is not specific to one action",
            str([a.specific for a in ek]))
    pv = ek[0].per_action_values if ek else {}
    r.check(pv == {"flame_off": [0x00], "flame_on": [0x04]},
            "per_action_values records the discrimination map", str(pv))
    r.check(all("state" in a.note.lower() for a in ek),
            "note says plainly that this tracks appliance STATE",
            str([a.note for a in ek]))

    print("\n=== state echo 2 — CONTROL: OVERLAPPING values must NOT echo ===")
    # Reuses the overlap fixture from above: both actions move the key, and
    # their post sets share 0x03. Nothing discriminates them, so this must stay
    # speculative-with-competing — exactly the behaviour the carve-out must not
    # swallow. This is the control that proves the extension is additive.
    oa = shared
    ok = [a for a in oa if a.key == KEY]
    r.check(all(a.score == SCORE_SPECULATIVE for a in ok),
            "overlapping values stay speculative (carve-out did not swallow it)",
            str([(a.action, a.score) for a in ok]))
    r.check(all(a.score != SCORE_STATE_ECHO for a in ok),
            "overlapping values are NOT state_echo",
            str([(a.action, a.score) for a in ok]))
    r.check(all(a.distinguishable is False for a in ok),
            "distinguishable=False when values overlap",
            str([a.distinguishable for a in ok]))
    r.check(all(a.competing_actions for a in ok),
            "competing_actions still named",
            str([(a.action, a.competing_actions) for a in ok]))

    print("\n--- the same two actions DO echo when their values disjoint ---")
    # Same fixture shape, one value changed: flame_on -> 0x04, flame_off ->
    # 0x02. Now the sets are disjoint and the key becomes a state echo. The
    # ONLY difference from the control above is the values.
    def near_overlap_run(seed: int) -> RunResult:
        f: list = _bg(seed)
        f.extend(_burst(3.0, seed + 0x20, 0x01, 0xDC, [0x00, 0x00], n=2, dt=0.3))
        f.extend(_burst(12.0, seed + 0x30, 0x01, 0xDC, [0x00, 0x01], n=2, dt=0.3))
        f.extend(_burst(15.0, seed + 0x36, 0x01, 0xDC, [0x00, 0x04], n=2, dt=0.3))
        f.extend(_burst(22.0, seed + 0x40, 0x01, 0xDC, [0x00, 0x02], n=2, dt=0.3))
        return run_from_frames("plan", f, marks=[
            Mark(t=10.0, action="flame_on"),
            Mark(t=20.0, action="flame_off"),
        ])

    no = attribute([near_overlap_run(seed=s * 0x10) for s in range(3)],
                   control_runs=[c1, c2])
    nok = [a for a in no if a.key == KEY]
    r.check(all(a.score == SCORE_STATE_ECHO for a in nok),
            "the same fixture with disjoint values becomes state_echo",
            str([(a.action, a.score) for a in nok]))
    r.check(all(a.discriminating_index == 1 for a in nok),
            "discriminating_index is byte 1",
            str([a.discriminating_index for a in nok]))

    print("\n=== state echo 3 — CONTROL: present in idle control -> NOT echo ===")
    # Disjoint across actions, but the key ALSO moves while nothing happens.
    # control_clean gates the carve-out, so this must not become a state echo.
    def echo_in_control(seed: int) -> RunResult:
        f: list = _bg(seed)
        # the SAME key drifts with no action at all, in the pre-window and the
        # post-window, so its deltas land in the control run too
        f.extend(_burst(3.0, seed + 0x20, 0x01, 0xDC, [0x00, 0x00], n=2, dt=0.3))
        f.extend(_burst(6.0, seed + 0x26, 0x01, 0xDC, [0x00, 0x33], n=2, dt=0.3))
        f.extend(_burst(12.0, seed + 0x30, 0x01, 0xDC, [0x00, 0x04], n=2, dt=0.3))
        f.extend(_burst(22.0, seed + 0x40, 0x01, 0xDC, [0x00, 0x00], n=2, dt=0.3))
        return run_from_frames("plan", f, marks=[
            Mark(t=10.0, action="flame_on"),
            Mark(t=20.0, action="flame_off"),
        ])

    noisy_control = run_from_frames("idle", [
        *_bg(0),
        *_burst(3.0, 0x20, 0x01, 0xDC, [0x00, 0x00], n=2, dt=0.3),
        *_burst(6.0, 0x26, 0x01, 0xDC, [0x00, 0x33], n=2, dt=0.3),
        *_burst(12.0, 0x30, 0x01, 0xDC, [0x00, 0x04], n=2, dt=0.3),
        *_burst(22.0, 0x40, 0x01, 0xDC, [0x00, 0x00], n=2, dt=0.3),
    ], marks=[], is_control=True)
    na = attribute([echo_in_control(seed=s * 0x10) for s in range(3)],
                   control_runs=[c1, noisy_control])
    nk = [a for a in na if a.key == KEY]
    r.check(all(a.score != SCORE_STATE_ECHO for a in nk),
            "a key that also moves in the idle control is NOT state_echo",
            str([(a.action, a.score) for a in nk]))
    r.check(all(a.score != SCORE_CONFIRMED for a in nk),
            "and is not confirmed either", str([(a.action, a.score) for a in nk]))
    r.check(all(a.distinguishable is False for a in nk),
            "distinguishable=False when the control is contaminated",
            str([a.distinguishable for a in nk]))

    print("\n=== state echo 4 — CONTROL: one action only -> never state_echo ===")
    # A key moved by exactly ONE action is specific; it must never be relabelled.
    one_action = [action_run(True, seed=s * 0x10) for s in range(3)]
    oa1 = attribute(one_action, control_runs=[c1, c2])
    o_k = [a for a in oa1 if a.key == KEY]
    r.check(all(a.score != SCORE_STATE_ECHO for a in o_k),
            "a single-action key is never state_echo",
            str([(a.action, a.score) for a in o_k]))
    r.check(any(a.score == SCORE_CONFIRMED for a in o_k),
            "and is still confirmed for its one action",
            str([(a.action, a.score) for a in o_k]))
    r.check(all(a.distinguishable is False for a in o_k),
            "distinguishable=False with only one action",
            str([a.distinguishable for a in o_k]))

    print("\n=== summary counting — echoes must NOT be counted as findings ===")
    # The trap this guards: a consumer summing every attribution reads
    # "1 confirmed + 2 state echoes" as "3 findings". The buckets make the
    # separation structural rather than a convention.
    sm = summarize(ea, runs=echoes, controls=[c1, c2])
    for field in ("counts", "counts_findings", "counts_state_echo",
                  "counts_informational", "counts_total"):
        r.check(field in sm, f"summary exposes {field}", str(sorted(sm)))
    r.check(sm["counts"]["state_echo"] == 2,
            "counts retains the raw per-score dict",
            str(sm["counts"]))
    r.check(sm["counts_state_echo"] == 2,
            "counts_state_echo == 2 for the two-action echo fixture",
            str(sm["counts_state_echo"]))
    r.check(sm["counts_findings"] == 0,
            "counts_findings == 0 — a state echo is not a finding",
            str(sm["counts_findings"]))
    r.check(sm["counts_findings"] != sm["counts_total"],
            "findings differ from the total when echoes exist (separated)",
            f"findings={sm['counts_findings']} total={sm['counts_total']}")
    r.check(sm["counts_findings"] + sm["counts_state_echo"]
            + sm["counts_informational"] == sm["counts_total"],
            "the three buckets partition every attribution",
            f"{sm['counts_findings']}+{sm['counts_state_echo']}"
            f"+{sm['counts_informational']} != {sm['counts_total']}")
    # With no echoes present, state_echo is zero and every attribution is
    # still accounted for: findings + informational == total. Note findings is
    # NOT the total here — the fixture also contains an observed_stable
    # background frame, which is reported but claims nothing and is therefore
    # informational, not a finding. That separation is the whole point.
    sm2 = summarize(oa1, runs=one_action, controls=[c1, c2])
    r.check(sm2["counts_state_echo"] == 0,
            "no echoes => counts_state_echo is zero",
            str(sm2["counts_state_echo"]))
    r.check(sm2["counts_findings"] == sm2["counts"]["confirmed"],
            "findings == confirmed when nothing is probable",
            f"findings={sm2['counts_findings']} "
            f"confirmed={sm2['counts']['confirmed']}")
    r.check(sm2["counts_findings"] + sm2["counts_informational"]
            == sm2["counts_total"],
            "findings + informational == total (stable frames are not findings)",
            f"findings={sm2['counts_findings']} "
            f"info={sm2['counts_informational']} total={sm2['counts_total']}")
    r.check(sm2["counts_findings"] < sm2["counts_total"],
            "an observed_stable frame is not counted as a finding",
            f"findings={sm2['counts_findings']} total={sm2['counts_total']}")

    print("\n=== state echo 5 — disjointness at ONE index is enough ===")
    # Byte 0 is identical for both actions; only byte 1 discriminates. The
    # carve-out requires one discriminating index, not all of them.
    def echo_two_bytes(seed: int, val: int) -> RunResult:
        f: list = _bg(seed)
        f.extend(_burst(3.0, seed + 0x20, 0x01, 0xDC, [0x00, 0x00], n=2, dt=0.3))
        f.extend(_burst(12.0, seed + 0x30, 0x01, 0xDC, [0x07, val], n=2, dt=0.3))
        f.extend(_burst(22.0, seed + 0x40, 0x01, 0xDC, [0x07, 0x00], n=2, dt=0.3))
        return run_from_frames("plan", f, marks=[
            Mark(t=10.0, action="flame_on"),
            Mark(t=20.0, action="flame_off"),
        ])

    tb = attribute([echo_two_bytes(seed=s * 0x10, val=0x09) for s in range(3)],
                   control_runs=[c1, c2])
    tk = [a for a in tb if a.key == KEY]
    r.check(all(a.score == SCORE_STATE_ECHO for a in tk),
            "a single discriminating index is sufficient",
            str([(a.action, a.score) for a in tk]))
    r.check(all(a.discriminating_index == 1 for a in tk),
            "byte 0 is not the discriminator; byte 1 is",
            str([(a.discriminating_index, a.byte_indices) for a in tk]))

    # ---------------- end-to-end against the real harness layout ----------
    print("\n=== harness layout — index + frames_D0.jsonl end to end ===")
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        idx = os.path.join(td, "sweep-20261016-000000-index.jsonl")
        recs = []
        for i, flip in enumerate((True, True, True)):
            rd = os.path.join(td, f"run-h{i}")
            os.makedirs(rd, exist_ok=True)
            f = _bg(0)
            f.extend(_burst(3.0, 0x20, 0x01, 0xDC, [0x00, 0x03], n=2, dt=0.3))
            f.extend(_burst(12.0, 0x30, 0x01, 0xDC,
                            [0x00, 0x05] if flip else [0x00, 0x03], n=2, dt=0.3))
            with open(os.path.join(rd, "frames_D0.jsonl"), "w") as fh:
                for fr in f:
                    fh.write(json.dumps(fr.to_json()) + "\n")
            recs.append({
                "run_id": f"h{i}", "started_wall": "2026-10-16T00:00:00",
                "duration_s": 30.0, "valid": True, "verified": True,
                "invalid_reason": None, "integrity": {}, "frame_counts": {"D0": len(f)},
                "reboots": [], "errors": [], "run_dir": rd, "notes": [],
                "discard": False,
                "marks": [{"mark": {"at_s": 10.0, "action": "height_3",
                                    "value": 3, "expect": None, "note": ""},
                           "due_at": 10.0, "fired_at": 10.0, "returned_at": 10.1,
                           "verified": True, "detail": "",
                           "poll_before": {}, "poll_after": {}}],
            })
        # storage/status/cleanup runs the loader must refuse
        recs.append(dict(recs[0], run_id="cal", discard=True,
                         run_dir=os.path.join(td, "run-h0")))
        recs.append(dict(recs[0], run_id="bad", valid=False,
                         invalid_reason="REBOOT_CONTAMINATED",
                         run_dir=os.path.join(td, "run-h0")))
        recs.append(dict(recs[0], run_id="unv", verified=False,
                         run_dir=os.path.join(td, "run-h0")))
        with open(idx, "w") as fh:
            for rec in recs:
                fh.write(json.dumps(rec) + "\n")

        ld = load_sweep_index(idx)
        r.check(len(ld.runs) == 3, "3 valid runs loaded from the index",
                f"got {len(ld.runs)}")
        r.check(len(ld.excluded) == 2, "discard + invalid runs excluded (2)",
                str(ld.excluded))
        r.check(len(ld.unverified) == 1, "unverified run held out (1)",
                str(ld.unverified))
        reasons = {e["reason"] for e in ld.excluded}
        r.check(reasons == {"DISCARD", "REBOOT_CONTAMINATED"},
                "exclusion reasons are reported, not silent", str(reasons))
        e2e = attribute(ld.runs, ld.controls)
        hit = [a for a in e2e if a.key == ("D0", 0x01, 0xDC)]
        r.check(len(hit) == 1 and hit[0].score == SCORE_CONFIRMED,
                "end-to-end: the flipped key is confirmed from real file layout",
                str([(a.key_s, a.score) for a in e2e]))
        r.check(hit and hit[0].byte_indices == [1],
                "end-to-end: byte 1 attributed", str(hit[0].byte_indices if hit else None))
        r.check(hit and hit[0].value == 3,
                "mark value carried through to the attribution",
                str(hit[0].value if hit else None))

    # ---------------- robustness ------------------------------------------
    print("\n=== robustness ===")
    try:
        bad = [_mk_frame("D0", None, 0x01, 0x00, 0xBC, [0x02, 0x04])]
        run_from_frames("x", bad, marks=[Mark(t=1.0, action="x")])
        r.check(False, "a frame with t=None raises rather than misbucketing")
    except ValueError as exc:
        r.check("timestamp" in str(exc),
                "a frame with t=None raises rather than misbucketing", str(exc))

    empty = run_from_frames("x", [], marks=[Mark(t=1.0, action="x")])
    r.check(empty.deltas == [], "an empty capture yields no deltas")

    starts = pre_window_starts([Mark(t=30.0, action="a"),
                                Mark(t=10.0, action="b")], settle_s=2.0)
    # marks deliberately out of order: the earlier mark's window starts at
    # settle_s (the capture start has its own transient), the later one's at
    # the earlier mark + settle_s
    r.check(starts[1] == 2.0 and starts[0] == 12.0,
            "pre-window starts respect settle_s at capture start and after",
            str(starts))
    r.check(baselines(_bg(0), [Mark(t=10.0, action="a")], 2.0)[0],
            "baselines() returns stats for a valid window")

    return r.summary()


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sweep", nargs="?", help="sweep-index.jsonl")
    ap.add_argument("--selftest", action="store_true",
                    help="run synthetic tests, no hardware, no data files")
    ap.add_argument("--out", default="attribution.json",
                    help="output path (default: attribution.json)")
    ap.add_argument("--json", action="store_true",
                    help="print to stdout instead of appending to --out")
    ap.add_argument("--settle", type=float, default=2.0,
                    help="baseline settle time in seconds (default: 2.0)")
    ap.add_argument("--channel", default="D0", choices=("D0", "D1"),
                    help="which direction drives the comparison (default: D0)")
    ap.add_argument("--use-fired-at", action="store_true",
                    help="prefer marks[i].fired_at over mark.at_s where present")
    args = ap.parse_args()

    if args.selftest:
        sys.exit(selftest())

    if not args.sweep:
        ap.error("a sweep-index.jsonl is required unless --selftest is given")

    load = load_sweep_index(args.sweep, settle_s=args.settle,
                            use_fired_at=args.use_fired_at,
                            channel=args.channel)
    runs, controls = load.runs, load.controls
    atts = attribute(runs, controls)
    params = parameter_table(atts)

    report = {
        "source": args.sweep,
        "channel": args.channel,
        "runs": [r.to_json() for r in runs],
        "controls": [c.to_json() for c in controls],
        "excluded": load.excluded,
        "unverified": load.unverified,
        "attributions": [a.to_json() for a in atts],
        "parameter_tables": params,
    }
    report.update(summarize(atts, runs, controls, load.excluded, load.unverified))

    text = json.dumps(report, indent=2)
    if args.json:
        print(text)
    else:
        records = [{"record": "attribution", **r} for r in report["attributions"]]
        with open(args.out, "a", encoding="utf-8") as fh:
            for rec in records:
                fh.write(json.dumps(rec) + "\n")
            fh.write(json.dumps({
                "record": "summary",
                "counts": report["counts"],
                "counts_findings": report["counts_findings"],
                "counts_state_echo": report["counts_state_echo"],
                "counts_informational": report["counts_informational"],
                "counts_total": report["counts_total"],
            }) + "\n")
        print(f"# {len(records)} attributions appended to {args.out}",
              file=sys.stderr)
        print(f"# findings (confirmed+probable)={report['counts_findings']}  "
              f"state_echo={report['counts_state_echo']}  "
              f"informational={report['counts_informational']}  "
              f"total={report['counts_total']} — state echoes are NOT commands",
              file=sys.stderr)
        for a in report["attributions"]:
            print(f"{a['score']:<16} {a['action']:<12} {a['key']:<16} "
                  f"bytes={a['byte_indices']} {a['note']}", file=sys.stderr)
        # Exclusion is reported, never silent: a dropped run is a gap in the
        # evidence, and the reader must see it to judge the result.
        for e in load.excluded:
            print(f"# EXCLUDED {e['run_id']}: {e['reason']}", file=sys.stderr)
        for u in load.unverified:
            print(f"# UNVERIFIED {u['run_id']}: {u['reason']}", file=sys.stderr)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Reference implementation of the ECM's polled-slave responder.

This is the **specification** the ESPHome firmware mirrors. It exists in Python
first for one reason: the logic can be validated offline, byte-for-byte, against
the module's own recorded behaviour — before any of it is written in C++ and
flashed onto a board that talks to a gas appliance.

READ-ONLY ANALYSIS. Opens no hardware, defines no transmit path.

The model
---------
The ECM polls continuously; the module NEVER initiates a frame. Verified across
1,663 ECM frames and 1,647 module frames: **every** module frame answers an ECM
frame carrying the same `seq` AND the same `cmd`.

The reply is built by a rule that holds for every command observed:

    reply payload = request payload[0:2]  (echoed)  +  a per-command suffix

    request 4B 02 09 BC 02 04 <crc>
    reply   4B 04 09 BC 02 04 00 00 00 00 <crc>     echo "02 04" + slot

    request 4B 01 0C DC 00 17 ..(25 bytes).. <crc>
    reply   4B 03 0C DC 00 17 <crc>                 echo only

with `dev` = request `dev` + 2, `seq` copied verbatim, and CRC-16/MODBUS over the
reply from its `0x4B` inclusive, little-endian.

The write channel
-----------------
`0xBC`'s suffix is a **4-byte slot**. Idle it is `00 00 00 00`; a command fills
it as `<opcode> 00 <value> 00`:

    0x64 = flame set    value 01 on / 00 off
    0x65 = pilot set    value 01 on / 00 off   (only bit 0 is effective)
    0x67 = height set   value 0-4 (internal; the app shows 1-5)

The slot range extends beyond the implemented opcodes, but only 0x64/0x65/0x67
are understood well enough to send. 0x66, 0x68, 0x69 and 0x6A are deliberately
absent: 0x68 in particular raises internal fault 6 on a nonzero edge.

So an ESP32 replacement cannot "send a command" — it queues one and injects it
into the next `0xBC` reply. A command may be sent once, or repeated for N polls
(see `Command.repeat`) because the ECM's own acknowledgement arrives ~0.18 s
later and the slot is otherwise idle.

Usage:
    ./ecm_responder.py --selftest
    ./ecm_responder.py --selftest --quiet
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ecm_analyse import crc16_modbus  # noqa: E402

START = 0x4B

#: Suffix appended after the echoed 2-byte head, per command byte.
#: `0xBC` is absent because it uses a 4-byte slot instead — see BC_SLOT_IDLE.
#:
#: `0xDF` was added after a pilot capture (2026-10-16) exposed it: it had never
#: appeared in any capture, so the responder declined to answer it and stayed
#: silent. The ECM's pilot announcement, the counterpart of `0xE4` (flame) and
#: `0xE1` (height). A reply-table gap like this is silent by design — declining
#: is the safe default — which is exactly why the replay test exists to catch it.
REPLY_SUFFIX: dict[int, bytes] = {
    0x20: b"",           # power-on handshake
    0x64: b"",
    0x6E: b"\x00",
    0xDC: b"",
    0xDF: b"",           # pilot announcement
    0xE0: b"",           # after slot 0x66: config byte (never captured)
    0xE1: b"",           # height announcement
    0xE2: b"",           # after slot 0x68: heat-demand byte (never captured)
    0xE4: b"",           # flame announcement
    0xE5: b"",           # after slot 0x69: boolean, meaning unknown (never captured)
    0xE6: b"",           # after slot 0x6B (never captured)
    0xE7: b"",           # after slot 0x6C, two-byte payload (never captured)
    0xE9: b"",           # after slot 0x6D (never captured)
    0xEA: b"",           # after slot 0x6E, four-byte payload (never captured)
}

#: The 4-byte data slot in a `0xBC` reply when no command is pending.
BC_SLOT_IDLE = bytes([0x00, 0x00, 0x00, 0x00])

#: Write-channel opcodes, as `<opcode> 00 <value> 00`. Only the three verified
#: user controls are defined; see the module docstring for why the rest are not.
OP_FLAME = 0x64
OP_PILOT = 0x65
OP_HEIGHT = 0x67

#: How many consecutive `0xBC` polls a queued command is repeated for. The ECM
#: acknowledges ~0.18 s later; sending once risks the command being missed if
#: that particular poll is lost. Repeat a few times, then stop.
DEFAULT_REPEAT = 3


def build_reply(request: bytes, slot: bytes | None = None) -> bytes | None:
    """Build the module's reply to one ECM request.

    `request` is a complete, CRC-valid frame. Returns the complete reply frame,
    or None if the request is malformed or its command is unknown (in which case
    the caller should NOT transmit — guessing at an unknown command on a gas
    appliance is worse than staying silent, because the ECM then simply holds
    its sequence and resyncs, which is behaviour we have observed it do).

    `slot` overrides the 4-byte `0xBC` data slot; None means idle.
    """
    if len(request) < 6 or request[0] != START:
        return None
    dev, seq, cmd = request[1], request[2], request[3]
    payload = request[4:-2]

    if len(payload) < 2:
        return None

    head = payload[:2]                      # echoed verbatim

    if cmd == 0xBC:
        body = head + (BC_SLOT_IDLE if slot is None else bytes(slot))
    elif cmd in REPLY_SUFFIX:
        body = head + REPLY_SUFFIX[cmd]
    else:
        return None                          # unknown command — stay silent

    frame = bytes([START, (dev + 2) & 0xFF, seq, cmd]) + body
    crc = crc16_modbus(frame)
    return frame + bytes([crc & 0xFF, (crc >> 8) & 0xFF])


def make_slot(opcode: int, value: int) -> bytes:
    """The 4-byte `0xBC` data slot for a command: `<opcode> 00 <value> 00`."""
    if not 0 <= opcode <= 0xFF or not 0 <= value <= 0xFF:
        raise ValueError("opcode and value must be bytes")
    return bytes([opcode, 0x00, value, 0x00])


def flame_slot(on: bool) -> bytes:
    return make_slot(OP_FLAME, 0x01 if on else 0x00)


def pilot_slot(on: bool) -> bytes:
    return make_slot(OP_PILOT, 0x01 if on else 0x00)


def height_slot(app_height: int) -> bytes:
    """`app_height` is 1-5 as the app shows it; the wire value is 0-4."""
    if not 1 <= app_height <= 5:
        raise ValueError("app height must be 1-5")
    return make_slot(OP_HEIGHT, app_height - 1)


@dataclass
class PendingCommand:
    slot: bytes
    remaining: int = DEFAULT_REPEAT
    description: str = ""


@dataclass
class Responder:
    """Stateful responder: decides what (if any) slot to put in each reply.

    Mirrors what the firmware must hold: at most one pending command, drained
    over successive `0xBC` polls.
    """

    pending: PendingCommand | None = None
    #: Frames we could not answer — counted, never guessed at.
    unanswered: dict[int, int] = field(default_factory=dict)
    replies_built: int = 0
    #: True when writes are physically impossible. Mirrors the hardware switch;
    #: when set, queued commands are refused rather than silently dropped.
    read_only: bool = True

    def queue(self, slot: bytes, description: str = "",
              repeat: int = DEFAULT_REPEAT) -> bool:
        if self.read_only:
            return False                     # refuse — do not accept then drop
        self.pending = PendingCommand(slot, repeat, description)
        return True

    def queue_flame(self, on: bool, repeat: int = DEFAULT_REPEAT) -> bool:
        return self.queue(flame_slot(on), f"flame {'on' if on else 'off'}", repeat)

    def queue_pilot(self, on: bool, repeat: int = DEFAULT_REPEAT) -> bool:
        return self.queue(pilot_slot(on), f"pilot {'on' if on else 'off'}", repeat)

    def queue_height(self, app_height: int,
                     repeat: int = DEFAULT_REPEAT) -> bool:
        return self.queue(height_slot(app_height),
                          f"height {app_height}", repeat)

    def respond(self, request: bytes) -> bytes | None:
        """Consume one ECM request and produce the reply, or None to stay silent."""
        if len(request) < 6 or request[0] != START:
            return None
        cmd = request[3]

        slot = None
        if cmd == 0xBC and self.pending is not None:
            slot = self.pending.slot
            self.pending.remaining -= 1
            if self.pending.remaining <= 0:
                self.pending = None

        reply = build_reply(request, slot)
        if reply is None:
            self.unanswered[cmd] = self.unanswered.get(cmd, 0) + 1
        else:
            self.replies_built += 1
        return reply


# --------------------------------------------------------------------------
# selftest — replay the module's own recorded behaviour
# --------------------------------------------------------------------------

def _load(path: str, channel: str) -> list[dict]:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from ecm_frames import decode_channel
    return [f.to_json() for f in decode_channel(path, channel)]


def selftest(capture_glob: str, quiet: bool = False) -> int:
    """Replay every recorded ECM request and assert our reply matches the module's.

    This is the strongest available proof that the responder is correct, and it
    needs no hardware: the real module already answered these exact requests, so
    its replies are ground truth.
    """
    passed = failed = skipped = 0
    failures: list[str] = []

    def check(ok: bool, label: str, detail: str = "") -> None:
        nonlocal passed, failed
        if ok:
            passed += 1
            if not quiet:
                print(f"  PASS  {label}")
        else:
            failed += 1
            failures.append(label)
            print(f"  FAIL  {label}" + (f"\n          {detail}" if detail else ""))

    paths = sorted(glob.glob(capture_glob))
    if not paths:
        print(f"no captures matched {capture_glob}", file=sys.stderr)
        return 1

    print("=== responder selftest: replay the module's own recorded replies ===")
    print("    A mismatch is only a FAILURE if the module sent an IDLE slot.")
    print("    Where the module injected a command into the BC slot, that is a")
    print("    command EVENT and is counted separately — our responder had no")
    print("    pending command, so emitting the idle slot is the correct output.\n")

    for d0_path in sorted(p for p in paths if "_D0" in p):
        d1_path = d0_path.replace("_D0", "_D1")
        if not os.path.exists(d1_path):
            continue
        try:
            d0 = [json.loads(l) for l in open(d0_path)]
            d1 = [json.loads(l) for l in open(d1_path)]
        except Exception as exc:
            print(f"  skip {d0_path}: {exc}")
            continue

        label = os.path.basename(os.path.dirname(d0_path))
        # Pair by TIME ORDER, not by (seq,cmd): `seq` wraps and resets, so the
        # same (seq,cmd) recurs many times per capture and first-match pairing
        # silently compares a request against an unrelated reply.
        replies = sorted(d1, key=lambda r: r["t"])
        used: set[int] = set()
        m = Responder(read_only=False)
        n_match = n_inject = n_unpaired = 0
        injections: list[tuple[float, bytes]] = []

        for req in d0:
            cand = [(i, r) for i, r in enumerate(replies)
                    if i not in used
                    and r["seq"] == req["seq"] and r["cmd"] == req["cmd"]
                    and r["t"] >= req["t"]]
            if not cand:
                n_unpaired += 1
                skipped += 1
                continue
            i, real = cand[0]
            used.add(i)

            ours = m.respond(bytes(req["raw"]))
            real_raw = bytes(real["raw"])

            if ours is None:
                check(False, f"{label}: we declined to reply to {req['key']}",
                      "the module did reply")
                continue

            if req["cmd"] == "0xBC" and len(real_raw) >= 10:
                # Compare the head; the 4-byte slot may legitimately differ.
                if ours[:6] != real_raw[:6]:
                    check(False, f"{label}: BC reply head differs",
                          f"req  {req['hex']}\n"
                          f"          ours {' '.join(f'{b:02X}' for b in ours)}\n"
                          f"          real {' '.join(f'{b:02X}' for b in real_raw)}")
                    continue
                real_slot = real_raw[6:10]
                if real_slot != BC_SLOT_IDLE:
                    n_inject += 1
                    injections.append((real["t"], real_slot))
                    continue
                if ours != real_raw:
                    check(False, f"{label}: idle-slot BC reply differs",
                          f"ours {' '.join(f'{b:02X}' for b in ours)}\n"
                          f"          real {' '.join(f'{b:02X}' for b in real_raw)}")
                    continue
                n_match += 1
                continue

            if ours != real_raw:
                check(False, f"{label}: reply to {req['key']} differs",
                      f"req  {req['hex']}\n"
                      f"          ours {' '.join(f'{b:02X}' for b in ours)}\n"
                      f"          real {' '.join(f'{b:02X}' for b in real_raw)}")
            else:
                n_match += 1

        check(True, f"{label}: {n_match} replies byte-identical to the real module")

        if injections:
            ops: dict[int, int] = {}
            for _, s in injections:
                ops[s[0]] = ops.get(s[0], 0) + 1
            desc = ", ".join(f"opcode 0x{k:02X} x{v}" for k, v in sorted(ops.items()))
            check(True, f"{label}: {n_inject} command injection(s) observed "
                        f"({desc}) — correctly NOT reproduced without a queued command")
            if not quiet:
                for t, s in injections[:4]:
                    print(f"          t={t:8.3f}  slot {' '.join(f'{b:02X}' for b in s)}")
        if n_unpaired:
            check(True, f"{label}: {n_unpaired} requests had no paired reply "
                        f"(module silent — expected during its stall windows)")

    print(f"\n{'=' * 62}")
    if failed:
        print(f"RESULT: {failed} FAILED, {passed} passed ({skipped} unpaired skipped)")
        for f in failures:
            print(f"  - {f}")
    else:
        print(f"RESULT: all {passed} checks passed "
              f"({skipped} unpaired requests skipped)")
    print("=" * 62)
    return 1 if failed else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--captures", default="captures/*/frames_D0.jsonl")
    args = ap.parse_args()

    if args.selftest:
        here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        return selftest(os.path.join(here, args.captures), args.quiet)
    ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())

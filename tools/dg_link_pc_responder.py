#!/usr/bin/env python3
"""PC as the *responder*: the handheld initiates, the PC answers.

    python tools/dg_link_pc_responder.py --port COM5 --slot 3 --effort 0

Frame order (docs/LINK-BATTLE-FEASIBILITY.md §1):
    A1 handheld card -> B1 our card -> A2 handheld verdict -> B2 our verdict
The verdict is computed by the *initiating* ROM, so only this direction can
increment `bouts`.  The answerer merely echoes the complement.
"""
import argparse
import re
import sys
import time

sys.path.insert(0, "tools")
from dg_link_battle import Dev          # noqa: E402
from dg_link_pair import to_battle      # noqa: E402


def bitrev8(b):
    b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4)
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2)
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1)
    return b & 0xFF


def wire_frame(d0, d1):
    return (bitrev8(d0) & 0xFF) | ((bitrev8(d1) & 0xFF) << 8)


def collect(d, want, timeout):
    """Read until `want` frames have been seen (or timeout); return list of hex."""
    end = time.time() + timeout
    buf = d.buf
    out = []
    while time.time() < end and len(out) < want:
        chunk = d.ser.read(512)
        if chunk:
            buf += chunk.decode("utf-8", "replace")
        while True:
            m = re.search(r"LX (\d+) ([0-9A-F]{4})", buf)
            if not m:
                break
            out.append(m.group(2))
            print("  <- LX %s %s" % (m.group(1), m.group(2)), flush=True)
            buf = buf[m.end():]
        if not chunk:
            time.sleep(0.02)
    d.buf = buf
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--slot", type=int, default=3)
    ap.add_argument("--effort", type=int, default=0)
    ap.add_argument("--watch", type=float, default=40.0)
    args = ap.parse_args()

    d = Dev(args.port)
    print("boot ready:", d.wait_boot())
    d.send("LINKLOG 1", wait=0.3)
    d.send("RAMW 33 0", wait=0.05)
    d.send("RAMW 8D 0", wait=0.05)
    d.send("RAMW 8C 4", wait=0.1)
    print("to_battle:", to_battle(d, "peer"))
    d.send("RAMW 7A 0", wait=0.2)

    st = d.send("STATE", wait=0.4)
    print("peer:", st.strip()[:170])

    # Third B: this is the handheld's own "send request".
    d.ser.write(b"RAMW 05 0\nRAMW 8C 4\nKEY 2 600\n")
    d.ser.flush()
    print("handheld triggered (third B)")

    got = collect(d, 1, 6.0)
    if not got:
        print("no card from handheld -> LINKSTAT:", d.send("LINKSTAT", wait=0.6).strip())
        d.close()
        return 3

    card = ((args.effort & 0xF) << 4) | (args.slot & 0xF)
    f = wire_frame(card, (~card) & 0xFF)
    print("-> our card %04X (slot %d effort %d)" % (f, args.slot, args.effort))
    d.send("LINKFRAME %04X" % f, wait=0.3)

    got += collect(d, 1, 6.0)
    if len(got) < 2:
        print("no verdict from handheld -> LINKSTAT:", d.send("LINKSTAT", wait=0.6).strip())
        d.close()
        return 4

    d0 = bitrev8(int(got[1], 16) & 0xFF)
    mine = 2 if (d0 & 0xF) == 1 else 1
    vf = wire_frame(mine, (~mine) & 0xFF)
    print("handheld verdict data %02X -> our answer %04X (verdict %d)" % (d0, vf, mine))
    d.send("LINKFRAME %04X" % vf, wait=0.3)

    extra = collect(d, 1, 5.0)
    if extra:
        print("  handheld sent one more:", extra)

    for i in range(int(args.watch // 4)):
        time.sleep(4.0)
        st = d.send("STATE", wait=0.35)
        m = re.search(r"mode=(\w+) sub=(\w+).*win=(\d+) bouts=(\d+)", st)
        print("  t+%.0fs %s" % ((i + 1) * 4.0, m.groups() if m else "?"))
    d.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

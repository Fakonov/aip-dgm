#!/usr/bin/env python3
"""PC as the *initiator* against one real Passport (the handheld answers).

    python tools/dg_link_pc_battle.py --port COM5

This is the same frame order the studio page's「③ 我当发起方」button uses:
  PC card  ->  handheld answers with its card (LX 1)
  PC verdict -> handheld answers with its verdict (LX 2) -> M(0x7A)=0xF -> sub=3

If this passes, the browser button is protocol-correct; only Web Serial's
permission prompt is left as a manual step.
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


def read_frame(d, timeout):
    end = time.time() + timeout
    buf = ""
    while time.time() < end:
        chunk = d.ser.read(256)
        if chunk:
            buf += chunk.decode("utf-8", "replace")
        m = re.search(r"LX (\d+) ([0-9A-F]{4})", buf)
        if m:
            return int(m.group(1)), m.group(2)
    return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--slot", type=int, default=3)
    ap.add_argument("--effort", type=int, default=0)
    args = ap.parse_args()

    d = Dev(args.port)
    print("boot ready:", d.wait_boot())
    d.send("LINKLOG 1", wait=0.3)

    # Sleep / starvation gates before the battle screen
    d.send("RAMW 33 0", wait=0.05)
    d.send("RAMW 8D 0", wait=0.05)
    d.send("RAMW 8C 4", wait=0.1)
    print("to_battle:", to_battle(d, "peer"))

    # M(0x7A) is the four-bit handshake progress (bit0 peer card, bit1 peer
    # verdict, bit2 we sent card, bit3 we sent verdict).  The ROM does NOT clear
    # it when it leaves the battle screen, so a finished/rubbish handshake from
    # the previous fight makes the next one answer nothing at all.
    d.send("RAMW 7A 0", wait=0.15)
    print("handshake reset: RAM 7A ->", d.send("RAM", wait=0.5).strip()[-0:])

    st = d.send("STATE", wait=0.4)
    print("peer STATE:", st.strip()[:190])
    if "mode=5 sub=2" not in st and not re.search(r"mode=5 sub=2", st):
        print("NOT at mode=5 sub=2 ->", st.strip()[:160])
        d.close()
        return 2

    card = ((args.effort & 0xF) << 4) | (args.slot & 0xF)
    f = wire_frame(card, (~card) & 0xFF)
    print("PC card   %04X (slot %d effort %d)" % (f, args.slot, args.effort))
    # The ROM rewrites M(0x05) every frame, so the injury bit has to be cleared
    # in the same burst as the frame itself, not in an earlier command.
    d.ser.write(("RAMW 05 0\nRAMW 8C 4\nLINKFRAME %04X\n" % f).encode())
    d.ser.flush()

    n, hx = read_frame(d, 4.0)
    print("handheld frame:", n, hx)
    if n is None:
        print("no answer -> LINKSTAT:", d.send("LINKSTAT", wait=0.5).strip())
        d.send("STATE", wait=0.4)
        d.close()
        return 3

    d0 = bitrev8(int(hx, 16) & 0xFF)
    mine = 2 if (d0 & 0xF) == 1 else 1
    vf = wire_frame(mine, (~mine) & 0xFF)
    print("peer card data %02X (slot %d effort %d) -> PC verdict %04X"
          % (d0, d0 & 0xF, (d0 >> 4) & 0xF, vf))
    d.send("LINKFRAME %04X" % vf, wait=0.3)

    n2, hx2 = read_frame(d, 5.0)
    print("handheld second frame:", n2, hx2)

    for i in range(9):
        time.sleep(4.0)
        st = d.send("STATE", wait=0.35)
        m = re.search(r"mode=(\w+) sub=(\w+).*win=(\d+) bouts=(\d+)", st)
        print("  t+%.0fs %s" % ((i + 1) * 4.0, m.groups() if m else "?"))

    d.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

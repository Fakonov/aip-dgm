#!/usr/bin/env python3
"""Offline battle-card self-test over one USB cable (G2 / M3).

Writes a card, starts it with CARDGO and watches the firmware do the rest: the
auto-walk walks the ROM into the battle screen and the card answers both of the
ROM's frames on its own -- no PC relay, no second machine.

    python tools/dg_card_selftest.py --port COM5
    python tools/dg_card_selftest.py --port COM5 --slot A --effort 3 --ver 1

PASS = the ROM sent two frames, the card answered two, and `bouts` moved.
"""
import argparse
import re
import sys
import time

from dg_link_battle import Dev

CARD_INDEX = 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--slot", default="A", help="species slot 3..E (hex)")
    ap.add_argument("--effort", default="3", help="0..4")
    ap.add_argument("--ver", default="1", help="0=V1 1=V2 2=V3 3=V4")
    ap.add_argument("--name", default="CARDTEST")
    ap.add_argument("--seconds", type=int, default=90)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    dev = Dev(args.port, verbose=args.verbose)
    print("== M3 offline card self-test on %s ==" % args.port)
    if not dev.wait_boot():
        print("!! the unit never answered STATE (is the console busy?)")
        return 2

    print(dev.send("CARDW %d %s %s %s %s" % (CARD_INDEX, args.slot, args.effort,
                                             args.ver, args.name), wait=0.4).strip())
    print(dev.send("CARDL", wait=0.4).strip())

    st = dev.send("STATE", wait=0.4)
    m = re.search(r"bouts=(\w+)", st)
    bouts0 = int(m.group(1), 16) if m else None
    print("-- bouts before: %s" % (bouts0 if bouts0 is not None else "?"))

    print("-- CARDGO: the firmware arms the card and walks itself into the battle")
    dev.send("CARDGO %d" % CARD_INDEX, wait=0.5)

    frames = []
    card_state = None
    t0 = time.time()
    while time.time() - t0 < args.seconds:
        text = dev.send("CARDST", wait=0.6)
        m = re.search(r"state=(\w+) frames=(\d+) replies=(\d+)", text)
        if m:
            card_state = m.group(1)
            frames = (int(m.group(2)), int(m.group(3)))
        if "LX " in text:
            for hit in re.finditer(r"LX (\d+) ([0-9A-F]{4})", text):
                print("   [rom] frame #%s %s" % (hit.group(1), hit.group(2)))
        if frames and frames[1] >= 2:
            print("   card answered both frames -> waiting for the result")
            break
        time.sleep(0.4)

    # the ROM plays a ~26 s animation before it writes the result
    print("-- waiting for the battle animation to finish")
    deadline = time.time() + args.seconds
    bouts1 = bouts0
    while time.time() < deadline:
        st = dev.send("STATE", wait=0.6)
        m = re.search(r"bouts=(\w+)", st)
        if m:
            bouts1 = int(m.group(1), 16)
            if bouts0 is not None and bouts1 != bouts0:
                break
        cst = dev.send("CARDST", wait=0.4)
        if "state=IDLE" in cst and frames and frames[1] >= 2:
            dev.send("STATE", wait=0.4)
            m = re.search(r"bouts=(\w+)", dev.buf)
            if m:
                bouts1 = int(m.group(1), 16)
                break
        time.sleep(1.0)

    print(dev.send("CARDST", wait=0.4).strip())
    print(dev.send("LINKSTAT", wait=0.4).strip())
    st = dev.send("STATE", wait=0.6)
    print(st.strip())

    ok = bool(frames) and frames[0] >= 2 and frames[1] >= 2 and bouts0 != bouts1
    print("\n  card state : %s" % card_state)
    print("  frames     : %s" % (frames,))
    print("  bouts      : %s -> %s" % (bouts0, bouts1))
    if ok:
        print("\nM3 OFFLINE CARD: PASS (ROM played a battle against the stored card)")
    else:
        print("\nM3 OFFLINE CARD: FAIL")
        print("  check: sleep (M(0x33)), injured (M(0x05) bit0), energy (M(0x8C))")
    dev.close()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

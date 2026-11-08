#!/usr/bin/env python3
"""Two real Passports, one D-Link battle, PC as the wire.

Each device drives P2.0 itself; the PC only moves 16-bit frames between the two
USB consoles (there is no physical D-Link socket -- every GPIO is taken).

    python tools/dg_link_pair.py --a COM5 --b COM4 [--initiator a|b|both]

Steps per device: walk to the battle icon, press B once (sub=2), clear M(0x33)
(the ROM parks in sub=2 without entering RX while sleep=3), then relay frames.
The initiator is triggered with the `M(0x02)` event code that the ROM treats as
"press B to start" (3 or 5; it is not perfectly repeatable, so we also fall back
to a second B press).
"""
import argparse
import re
import sys
import time

sys.path.insert(0, "tools")
from dg_link_battle import Dev, wire_frame, decode, bitrev8  # noqa: E402


def to_battle(d, label):
    """Walk one device to mode=5 sub=2.  Retries: a device that just finished a
    battle sits on sub=7 (result screen) and swallows the first C press, which
    silently produced zero exchanges when two fights were chained."""
    groups = ("?", "?")
    for attempt in range(3):
        d.send("LINKLOG 1", wait=0.3)
        # A hungry / weak pet refuses to enter the battle screen (HANDOFF 坑 40/41);
        # topping the two nibbles up buys a few seconds before the ROM rewrites them.
        d.send("RAMW 3E 4", wait=0.05)
        d.send("RAMW 3D 4", wait=0.05)
        d.key(1, 600)
        for _ in range(10):
            st = d.send("STATE", wait=0.25)
            m = re.search(r"mode=(\w+)", st)
            if m and m.group(1) == "5":
                break
            d.key(4, 600, gap=0.9)
        d.key(2, 600)
        st = d.send("STATE", wait=0.3)
        m = re.search(r"mode=(\w+) sub=(\w+)", st)
        groups = m.groups() if m else ("?", "?")
        if groups[1] != "2":
            # A wounded pet (M(0x05) bit0 == injured) is refused at 0x0B5F's
            # sibling check 0x037F-0x0381, but the ROM rewrites that nibble
            # every frame -- so clear it and press B inside the same write.
            d.ser.write(b"RAMW 05 0\nKEY 2 600\n")
            d.ser.flush()
            time.sleep(1.0)
            st = d.send("STATE", wait=0.3)
            m = re.search(r"mode=(\w+) sub=(\w+)", st)
            groups = m.groups() if m else ("?", "?")
        if groups[1] == "1":          # icon page opened but battle screen not entered
            d.key(2, 600)
            st = d.send("STATE", wait=0.3)
            m = re.search(r"mode=(\w+) sub=(\w+)", st)
            groups = m.groups() if m else ("?", "?")
        if groups[1] == "2":
            break
        print("  %s not in sub=2 (mode=%s sub=%s), retrying"
              % (label, groups[0], groups[1]))
        time.sleep(2.0)
    print("  %s at mode=%s sub=%s" % (label, groups[0], groups[1]))
    d.send("RAMW 33 0", wait=0.2)
    return groups


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", default="COM5")
    ap.add_argument("--b", default="COM4")
    ap.add_argument("--initiator", choices=("a", "b", "both", "none", "user"), default="a")
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--passive", action="store_true",
                    help="do not touch the buttons at all: just relay frames. Put both units in the battle screen yourself first.")
    ap.add_argument("--rewrite", action="store_true",
                    help="rewrite the answerer's frame-2 into ~verdict (old hack). "
                         "Left OFF by default since round 84: the answering ROM sends "
                         "its own verdict value, and the peer accepts 1/2 as a verdict "
                         "frame. See docs/M2-DEVICE-LINK.md 8.7")
    args = ap.parse_args()

    A = Dev(args.a)
    B = Dev(args.b)
    # Opening a port is harmless, but the *previous* tool's close reset both
    # units.  Wait until each ROM answers STATE before pressing anything.
    for d, tag in ((A, "A"), (B, "B")):
        print("  %s boot ready: %s" % (tag, d.wait_boot()))
    # Wired mode only: the ESP-NOW radio layer was removed from the firmware in
    # round 91, so there is no radio to silence any more.
    print("== two-device D-Link battle: %s <-> %s ==" % (args.a, args.b))

    if args.passive:
        print("-- passive: relay only, leave the buttons alone")
        for d in (A, B):
            d.send("LINKLOG 1", wait=0.2)
    else:
        print("-- walking both devices into the battle screen (in parallel)")
        import threading
        res = {}
        def walk(d, tag):
            res[tag] = to_battle(d, tag)
        tA = threading.Thread(target=walk, args=(A, "A/" + args.a))
        tB = threading.Thread(target=walk, args=(B, "B/" + args.b))
        tA.start(); tB.start(); tA.join(); tB.join()

    # M(0x08) bit1 is the ROM's own "send request" flag (main loop 0x0B06 checks
    # mode==5 && sub==2 && that bit && P2.0 high).  Poking it makes the initiator
    # start a frame without touching the keypad.
    targets = []
    if args.initiator not in ("none", "user"):
        targets = [A, B] if args.initiator == "both" else ([A] if args.initiator == "a" else [B])
    for d in targets:
        # Load the card explicitly: poking only M(0x08) bit1 sends whatever
        # M(0x10..0x13) held from the previous battle (a stale verdict), which
        # makes the very first frame a "verdict" instead of a card.
        d.send("RAMW 10 B", wait=0.06)
        d.send("RAMW 11 2", wait=0.06)
        d.send("RAMW 08 2", wait=0.08)
    print("-- initiator: %s" % args.initiator)

    t0 = time.time()
    log = []
    last_state = 0.0

    def drain(src, dst, tag):
        """Read everything the device has said, parse complete LX lines from the
        accumulated buffer, and forward each frame to the other side."""
        src.buf += src.ser.read(src.ser.in_waiting or 1).decode("utf-8", "replace")
        while True:
            m = re.search(r"LX (\d+) ([0-9A-F]{4})\r?\n", src.buf)
            if not m:
                break
            src.buf = src.buf[m.end():]
            val = int(m.group(2), 16)
            log.append((tag, m.group(1), val))
            print("   [%s] #%s %04X data %02X %02X" % (tag, m.group(1), val, *decode(val)))
            # The answering ROM echoes its own verdict in frame 2; the initiator
            # only accepts the BITWISE COMPLEMENT (0x02 -> 0x0D).  Fix it up on
            # the wire, same as a compliant peer would send.
            out = val
            if args.rewrite and tag == "B->A" and len(log) >= 3 and (len(log) % 2) == 0:
                d0 = bitrev8(val & 0xFF)
                if (d0 & 0xF0) == 0:
                    v = (~d0) & 0x0F
                    out = wire_frame(v, (~v) & 0xFF)
                    print("      (rewrote %04X -> %04X, verdict ~%X)" % (val, out, v))
            dst.buf = dst.send("LINKFRAME %04X" % out, wait=0.9)

    while time.time() - t0 < args.seconds:
        drain(A, B, "A->B")
        drain(B, A, "B->A")
        if time.time() - t0 - last_state > 3.0:
            last_state = time.time() - t0
            for d, tag in ((A, "A"), (B, "B")):
                d.buf = ""
                st = d.send("STATE", wait=0.2)
                m = re.search(r"mode=(\w+) sub=(\w+).*win=(\d+) bouts=(\d+)", st)
                d.buf = ""
                ram = d.send("RAM", wait=0.45)
                mr = re.search(r"RAM 70 ([0-9A-F]{16})", ram)
                s7a = mr.group(1)[10] if mr else "?"
                print("   t=%.0fs %s %s 0x7A=%s" % (last_state, tag,
                                                    m.groups() if m else "?", s7a))

    print("\n== summary ==")
    for tag, n, val in log:
        print("  %-5s #%s %04X" % (tag, n, val))
    print("  frames relayed: %d" % len(log))
    for d, tag in ((A, "A"), (B, "B")):
        d.buf = ""
        st = d.send("STATE", wait=0.5)
        m = re.search(r"mode=(\w+) sub=(\w+).*win=(\d+) bouts=(\d+)", st)
        print("  %s final: %s" % (tag, m.groups() if m else "?"))
    A.close()
    B.close()
    print("\n%s" % ("PAIR BATTLE: exchange happened" if len(log) >= 3 else "PAIR BATTLE: no exchange"))
    return 0 if len(log) >= 3 else 2


if __name__ == "__main__":
    sys.exit(main())

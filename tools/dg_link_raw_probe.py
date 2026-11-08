#!/usr/bin/env python3
"""Raw-stream probe: handheld initiates, PC answers, everything is logged.

Unlike dg_link_battle.Dev.send(), this reads for the whole window and never
throws a byte away, so we can see whether the handheld really emits its second
(verdict) frame.
"""
import re
import sys
import time

sys.path.insert(0, "tools")
from dg_link_battle import Dev          # noqa: E402
from dg_link_pair import to_battle      # noqa: E402

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"


def bitrev8(b):
    b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4)
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2)
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1)
    return b & 0xFF


def wire_frame(d0, d1):
    return (bitrev8(d0) & 0xFF) | ((bitrev8(d1) & 0xFF) << 8)


def main():
    d = Dev(PORT)
    print("boot ready:", d.wait_boot(), flush=True)
    d.send("LINKLOG 1", wait=0.3)
    d.send("RAMW 33 0", wait=0.05)
    d.send("RAMW 8D 0", wait=0.05)
    d.send("RAMW 8C 4", wait=0.1)
    print("to_battle:", to_battle(d, "peer"), flush=True)
    d.send("RAMW 7A 0", wait=0.2)
    d.send("LINKSTAT", wait=0.4)

    d.ser.write(b"RAMW 05 0\nRAMW 8C 4\nKEY 2 600\n")
    d.ser.flush()
    print("triggered", flush=True)

    raw = ""
    sent = False
    replied = False
    t0 = time.time()
    while time.time() - t0 < 34:
        chunk = d.ser.read(1024)
        if chunk:
            txt = chunk.decode("utf-8", "replace")
            raw += txt
            for line in txt.split("\n"):
                if "LX " in line:
                    print("  %5.1fs %s" % (time.time() - t0, line.strip()), flush=True)
        if not sent and re.search(r"LX \d+ ([0-9A-F]{4})", raw):
            m = re.search(r"LX \d+ ([0-9A-F]{4})", raw)
            sent = True
            print("  -> PC card 3FC0 in reply to %s" % m.group(1), flush=True)
            d.ser.write(b"LINKFRAME 3FC0\n")
            d.ser.flush()
        # Second frame from the handheld is its verdict; answer with the
        # complement (docs/LINK-BATTLE-FEASIBILITY.md §1: the receiving side just
        # echoes ~verdict).  BF40 -> verdict 2 -> we send 7F80.
        if sent and not replied:
            m2 = re.search(r"LX 2 ([0-9A-F]{4})", raw)
            if m2:
                replied = True
                hx = int(m2.group(1), 16)
                d0 = bitrev8(hx & 0xFF)
                mine = 2 if (d0 & 0xF) == 1 else 1
                vf = wire_frame(mine, (~mine) & 0xFF)
                print("  -> PC verdict %04X (peer said %02X -> verdict %d, we answer %d)"
                      % (vf, d0, d0 & 0xF, mine), flush=True)
                d.ser.write(("LINKFRAME %04X\n" % vf).encode())
                d.ser.flush()
        time.sleep(0.02)

    open("docs/probe_shots/responder_raw.log", "w", encoding="utf-8").write(raw)
    print(d.send("LINKSTAT", 0.5).strip(), flush=True)
    print(d.send("STATE", 0.5).strip()[:190], flush=True)
    d.send("RAMW 7A 0", wait=0.1)
    d.close()


if __name__ == "__main__":
    main()

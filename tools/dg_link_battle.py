#!/usr/bin/env python3
"""M2: real device <-> PC D-Link battle over the USB console.

The PC plays the *initiator*: it pushes frame 1 (its card), the ROM answers
with frame 1 of its own, the PC answers with the verdict, the ROM echoes it
back inverted.  Frames on the wire are bit-reversed per byte (CON_DGM shifts
MSB-first), so everything here uses the same `wire_frame()` helper the probe
uses.

    python tools/dg_link_battle.py --port COM5 --slot 3 --effort 0

Leaves the device in whatever mode the battle ends in; run with --verbose to
see the raw console chatter.
"""
import argparse
import re
import sys
import time

import serial


def bitrev8(b):
    b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4)
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2)
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1)
    return b & 0xFF


def wire_frame(d0, d1):
    return (bitrev8(d0) & 0xFF) | ((bitrev8(d1) & 0xFF) << 8)


def decode(frame):
    return bitrev8(frame & 0xFF), bitrev8((frame >> 8) & 0xFF)


class Dev:
    def __init__(self, port, baud=115200, verbose=False):
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.buf = ""
        self.verbose = verbose
        time.sleep(0.3)
        self.drain()

    def drain(self):
        while self.ser.in_waiting:
            self.buf += self.ser.read(self.ser.in_waiting).decode("utf-8", "replace")

    def send(self, cmd, wait=0.35, until=None, timeout=4.0):
        if self.verbose:
            print(">> %s" % cmd)
        self.ser.write((cmd + "\n").encode())
        self.ser.flush()
        end = time.time() + (timeout if until else wait)
        got = ""
        while time.time() < end:
            chunk = self.ser.read(1)
            if not chunk:
                continue
            text = chunk.decode("utf-8", "replace")
            got += text
            if self.verbose:
                sys.stdout.write(text)
            if until and re.search(until, got):
                break
        self.drain()
        # send() 的等待窗口里读到的字节进了 got，drain() 看不到它们。如果丢掉 got，
        # 窗口内到达的 LX 帧（对端回的卡/判定）就丢了 —— 之前"真机发了第二帧却收不到"
        # 就是这个原因。并回 buf 让上层还能解析。
        self.buf += got
        return got

    def wait_boot(self, timeout=30.0):
        """USB-Serial-JTAG resets the unit when the port is closed, so whichever
        tool ran last left it rebooting.  Wait until the emulator actually answers
        STATE before touching the keypad, otherwise the first few presses land in
        a ROM that is still starting up."""
        end = time.time() + timeout
        while time.time() < end:
            if "STATE slot=" in self.send("STATE", wait=0.6):
                return True
        return False

    def key(self, mask, ms=250, gap=0.9):
        self.send("KEY %d %d" % (mask, ms), wait=gap)
        return self

    def close(self):
        self.ser.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--slot", type=lambda x: int(x, 0), default=3)
    ap.add_argument("--effort", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--verdict", type=lambda x: int(x, 0), default=2,
                    help="what our side claims (1 or 2)")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--no-keys", action="store_true",
                    help="skip the button dance, assume the ROM is already waiting")
    args = ap.parse_args()

    dev = Dev(args.port, verbose=args.verbose)
    print("== M2 real-device battle ==")

    dev.send("LINKLOG 1")
    dev.send("LINKSTAT")
    print(dev.buf.strip().splitlines()[-1] if dev.buf.strip() else "")

    if not args.no_keys:
        print("-- walking to the battle icon")
        dev.key(1, 600)                  # C: back to the main screen
        # The ROM samples K0 at ~2 Hz, so a 250 ms tap can fall between two
        # scans and vanish.  Hold 600 ms and steer by STATE instead of counting.
        for attempt in range(10):
            st = dev.send("STATE", wait=0.3)
            m = re.search(r"mode=(\w+) sub=(\w+)", st)
            if not m:
                continue
            mode, sub = m.group(1), m.group(2)
            print("   mode=%s sub=%s" % (mode, sub))
            if mode == "5":
                break
            dev.key(4, 600, gap=0.9)     # A: next function icon
        st = dev.send("STATE", wait=0.4)
        print("   icon page: %s" % (" ".join(st.strip().split()[:3])))
        dev.key(2, 600)                  # B: enter the battle screen
        st = dev.send("STATE", wait=0.4)
        m = re.search(r"mode=(\w+) sub=(\w+)", st)
        print("   after B   : mode=%s sub=%s" % (m.group(1), m.group(2)) if m else st)

    # M(0x33)=3 (asleep) keeps the ROM parked in sub=2 without entering the
    # receive loop: it answers nothing.  Clearing it makes RX work immediately.
    dev.send("RAMW 33 0", wait=0.25)
    # M(0x10)/M(0x11) must hold *our card*: poking only the send bit re-sends
    # whatever the last battle left there (usually a verdict), which makes the
    # first frame a "verdict" and the handshake never completes.
    dev.send("RAMW 10 %X" % (args.slot & 0xF), wait=0.06)
    dev.send("RAMW 11 %X" % (args.effort & 0xF), wait=0.06)
    # M(0x08) bit1 is the ROM's own "send request" flag (main loop 0x0B06 checks
    # mode==5 && sub==2 && line high, then sets it).  Poking it makes the ROM
    # initiate, which is what a real peer's button press would do.
    dev.send("RAMW 08 2", wait=0.06)
    print("-- ROM initiating (M(0x08) bit1)")

    card = ((args.effort << 4) | args.slot) & 0xFF
    a1 = wire_frame(card, (~card) & 0xFF)
    print("-- PC card: slot=%d effort=%d -> frame1 %04X" % (args.slot, args.effort, a1))

    frames = []
    reply2_sent = False

    def feed(text):
        nonlocal reply2_sent
        for m in re.finditer(r"LX (\d+) ([0-9A-F]{4})", text):
            n = int(m.group(1))
            f = int(m.group(2), 16)
            d0, d1 = decode(f)
            frames.append((n, f, d0, d1))
            print("   [dev] frame #%d %04X  data %02X %02X" % (n, f, d0, d1))
            if n == 1 and not reply2_sent:
                # The peer's frame 2 carries its own verdict, 1 or 2, in the
                # first nibble; the receiver files 1..2 as a VERDICT frame and
                # sets M(0x7A) bit1.  Sending the complement (0x0D) instead
                # lands in 3..0xE and is treated as a *card* -- that was the
                # pre-round-84 confusion.  See docs/M2-DEVICE-LINK.md 8.7.
                v = args.verdict & 0x0F
                a2 = wire_frame(v, (~v) & 0xFF)
                print("   [pc ] verdict=%d -> frame2 %04X" % (args.verdict, a2))
                dev.buf = ""
                time.sleep(0.06)   # let the ROM finish its tail and arm the receiver
                r = dev.send("LINKFRAME %04X" % a2, wait=0.9)
                feed(r + dev.buf)
                dev.buf = ""
                reply2_sent = True

    time.sleep(2.0)   # let the ROM settle into its receive loop
    dev.buf = ""
    got = dev.send("LINKFRAME %04X" % a1, wait=0.9)
    feed(got + dev.buf)
    dev.buf = ""

    t0 = time.time()
    last_state = 0.0
    trace = []
    while time.time() - t0 < 7.0:
        chunk = dev.ser.read(1)
        if chunk:
            text = chunk.decode("utf-8", "replace")
            if args.verbose:
                sys.stdout.write(text)
            feed(text)
        if time.time() - t0 - last_state >= 0.7:
            last_state = time.time() - t0
            dev.buf = ""
            st = dev.send("STATE", wait=0.12)
            feed(st + dev.buf)
            m = re.search(r"mode=(\w+) sub=(\w+)", st)
            if m:
                trace.append((round(last_state, 1), m.group(1), m.group(2)))
                print("   t=%.1fs mode=%s sub=%s" % (last_state, m.group(1), m.group(2)))

    dev.drain()
    st = dev.send("STATE", wait=0.6)
    m = re.search(r"slot=(\w+) mode=(\w+) sub=(\w+).*win=(\d+) bouts=(\d+)", st)
    print("\n== result ==")
    print("  frames exchanged: %d" % len(frames))
    if m:
        print("  device: slot=%s mode=%s sub=%s win=%s bouts=%s" % m.groups())
    print("  console tail: %s" % st.strip().splitlines()[-1] if st.strip() else "")
    dev.send("LINKSTAT")
    dev.close()

    ok = len(frames) >= 2
    print("\n%s" % ("M2 REAL BATTLE: PASS" if ok else "M2 REAL BATTLE: INCOMPLETE"))
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Poll SND fast around a key press to see whether the ROM ever drives the buzzer.

    python tools/dg_snd_probe.py --port COM5 --key 4 --ms 600 --poll 0.05 --secs 3
"""
import argparse
import re
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--key", type=int, default=4, help="4=A 2=B 1=C, 0 = none")
    ap.add_argument("--ms", type=int, default=600)
    ap.add_argument("--poll", type=float, default=0.05)
    ap.add_argument("--secs", type=float, default=3.0)
    ap.add_argument("--pre", default="",
                    help="extra console lines to send in the same burst as KEY")
    ap.add_argument("--settle", type=float, default=0.3,
                    help="seconds to wait after opening the port; opening it resets "
                         "the unit over USB-Serial-JTAG, so give the ROM time to boot")
    args = ap.parse_args()

    ser = serial.Serial(args.port, 115200, timeout=0.05)
    time.sleep(args.settle)
    ser.reset_input_buffer()

    seen = []
    t0 = time.time()
    pressed = (args.key == 0)
    while time.time() - t0 < args.secs:
        if not pressed and time.time() - t0 > 0.25:
            burst = ""
            if args.pre:
                burst += args.pre.replace(";", "\n") + "\n"
            burst += "KEY %d %d\n" % (args.key, args.ms)
            ser.write(burst.encode())
            ser.flush()
            pressed = True
        ser.write(b"SND\n")
        ser.flush()
        time.sleep(args.poll)
        text = ser.read(ser.in_waiting or 1).decode("utf-8", "replace")
        for m in re.finditer(r"SND active=(\d+) freq=([\d.]+) amp=([\d.]+)", text):
            t = time.time() - t0
            row = (round(t, 2), int(m.group(1)), float(m.group(2)), float(m.group(3)))
            if not seen or seen[-1][1:] != row[1:]:
                seen.append(row)
                print("t=%.2f active=%d freq=%.1f amp=%.3f" % row)

    ser.write(b"SND\n")
    ser.flush()
    time.sleep(0.4)
    tail = ser.read(ser.in_waiting or 1).decode("utf-8", "replace")
    print("--- raw tail ---")
    print(tail[-600:])
    ser.close()
    if not seen:
        print("NO SND REPLY")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())

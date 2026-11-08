#!/usr/bin/env python3
"""Tiny USB-console driver for the AI Passport (USB-Serial-JTAG).

Usage:
  python tools/dg_console.py --listen 5
  python tools/dg_console.py --cmd STATE --cmd DUMP --cmd AGES
  python tools/dg_console.py --script "5" --cmd STATE     # wait N s, then send

Every reply is printed with a `<<` / `>>` marker so logs can be diffed.
"""
import argparse
import sys
import time

import serial


def read_for(ser, seconds, sink):
    end = time.time() + seconds
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            text = chunk.decode("utf-8", "replace")
            sink.append(text)
            sys.stdout.write(text)
            sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--listen", type=float, default=2.0,
                    help="seconds to listen before the first command")
    ap.add_argument("--gap", type=float, default=0.6,
                    help="seconds to listen after each command")
    ap.add_argument("--cmd", action="append", default=[])
    ap.add_argument("--raw", action="store_true", help="do not add \\n")
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.1)
    sink = []
    try:
        time.sleep(0.3)
        read_for(ser, args.listen, sink)
        for c in args.cmd:
            payload = c.encode() if args.raw else (c + "\n").encode()
            print("\n>> %s" % c)
            ser.write(payload)
            ser.flush()
            read_for(ser, args.gap, sink)
    finally:
        ser.close()
    print("\n-- %d bytes read --" % sum(len(s) for s in sink))


if __name__ == "__main__":
    main()

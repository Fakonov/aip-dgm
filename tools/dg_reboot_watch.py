#!/usr/bin/env python3
"""Watch one Passport for a spontaneous reset.

    python tools/dg_reboot_watch.py --port COM5 --secs 240 --wifi 1 --log docs/probe_shots/reboot_com5.log

Keeps the console open the whole time (so the boot banner is not lost), pokes
STATE every 10 s, and reports the moment `alive frame` goes backwards or a
ROM boot banner / rst: line shows up.
"""
import argparse
import re
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--secs", type=float, default=240.0)
    ap.add_argument("--log", default="docs/probe_shots/reboot_watch.log")
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.1)
    time.sleep(0.3)
    ser.reset_input_buffer()
    log = open(args.log, "w", encoding="utf-8")
    print("watching %s for %.0f s -> %s" % (args.port, args.secs, args.log))

    t0 = time.time()

    buf = ""
    last_alive = -1
    last_state = 0.0
    next_poll = time.time() + 5
    resets = 0
    while time.time() - t0 < args.secs:
        data = ser.read(4096)
        if data:
            txt = data.decode("utf-8", "replace")
            log.write(txt)
            log.flush()
            buf += txt
            for m in re.finditer(r"alive frame=(\d+)", buf):
                n = int(m.group(1))
                if last_alive >= 0 and n < last_alive:
                    resets += 1
                    print("  RESET t=%.1f  alive %d -> %d" % (time.time() - t0, last_alive, n))
                last_alive = n
            for pat, tag in ((r"rst:0x\w+ \([A-Z_]+\)", "RST"), (r"ESP-ROM:esp32\w*", "BOOT")):
                for m in re.finditer(pat, buf):
                    print("  %s t=%.1f  %s" % (tag, time.time() - t0, m.group(0)))
                    resets += 1
            buf = buf[-6000:]
        now = time.time() - t0
        if now - last_state > 10.0:
            last_state = now
            ser.write(b"STATE\n")
            ser.flush()
            print("  t=%.0f alive=%d" % (now, last_alive))
        time.sleep(0.05)

    log.close()
    ser.close()
    print("done: resets=%d last_alive=%d" % (resets, last_alive))
    return 0 if resets == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())

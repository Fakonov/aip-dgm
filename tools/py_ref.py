# -*- coding: utf-8 -*-
"""Dump a rich per-step trace from the Python reference (for diffing)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "ref"))
from cores.E0C6200 import E0C6200  # noqa: E402

ROM = r"<home>\Desktop\dsh sth\tama dg roms\digimon\digimon.bin"


class Bus:
    def register_pin_device(self, d):
        pass

    def emit_port(self, *a, **k):
        pass

    def emit_audio(self, *a, **k):
        pass


core = E0C6200({"rom_path": ROM, "port_pullup": {"K0": 15, "K1": 15},
                "p3_dedicated": 0}, 1060000, Bus())

n = int(sys.argv[1]) if len(sys.argv) > 1 else 500
out = open(os.path.join(HERE, "py_trace.txt"), "w", encoding="utf-8")
for i in range(1, n + 1):
    cyc = core.clock()
    f = core._IF << 3 | core._DF << 2 | core._ZF << 1 | core._CF
    out.write("T %d %d %d %d %d %d %d %d %d %d %d %d %d\n" % (
        i, core.pc(), core.istr_counter(), int(cyc) if not isinstance(cyc, float) else cyc,
        core._A, core._B, core._IX, core._IY, core._SP, core._PC, core._NPC, f,
        core._HALT))
out.close()
print("wrote %d steps" % n)

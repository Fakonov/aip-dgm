# -*- coding: utf-8 -*-
"""Assemble src/digimon_core.c from tools/core_body.c + tools/op_tbl.inc."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

body = open(os.path.join(HERE, "core_body.c"), "r", encoding="utf-8").read()
tbl = open(os.path.join(HERE, "op_tbl.inc"), "r", encoding="utf-8").read()

if "/*@OP_TABLE@*/" not in body:
    raise SystemExit("marker missing in core_body.c")

out = body.replace("/*@OP_TABLE@*/", tbl.rstrip("\n"))
dst = os.path.join(ROOT, "src", "digimon_core.c")
open(dst, "w", encoding="utf-8", newline="\n").write(out)
sys.stdout.write("wrote %s (%d lines)\n" % (dst, out.count("\n") + 1))

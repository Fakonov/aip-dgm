"""Minimal E0C6200 disassembler built from the BrickEmuPy dispatch table.

The reference core (ref/cores/E0C6200.py) builds a 4096-entry array of handler
references; the opcode is the array index.  We re-expand that table from the
source text so the mnemonics can never drift away from the emulation the
project is bit-exact against.

usage: python tools/disasm_v1.py ROM.bin START END
"""
import re
import sys

CORE = r".\ref\cores\E0C6200.py"

# handler name -> (mnemonic template, operand kind)
#   {i}  4-bit immediate, {l} 8-bit, {s} 8-bit step (page-local target),
#   {n}  4-bit memory nibble, {r}{q} register numbers, {x}/{y} 8-bit ptr
HANDLERS = {
    "_jp_s":            ("jp {s}", "s"),
    "_retd_l":          ("retd {l}", "l"),
    "_jp_c_s":          ("jp c,{s}", "s"),
    "_jp_nc_s":         ("jp nc,{s}", "s"),
    "_call_s":          ("call {s}", "s"),
    "_calz_s":          ("calz {l}", "l"),
    "_jp_z_s":          ("jp z,{s}", "s"),
    "_jp_nz_s":         ("jp nz,{s}", "s"),
    "_ld_y_y":          ("ld y,{yl}", "y"),
    "_lbpx_mx_l":       ("lbpx mx,{l}", "l"),
    "_adc_xh_i":        ("adc xh,{i}", "i"),
    "_adc_xl_i":        ("adc xl,{i}", "i"),
    "_adc_yh_i":        ("adc yh,{i}", "i"),
    "_adc_yl_i":        ("adc yl,{i}", "i"),
    "_cp_xh_i":         ("cp xh,{i}", "i"),
    "_cp_xl_i":         ("cp xl,{i}", "i"),
    "_cp_yh_i":         ("cp yh,{i}", "i"),
    "_cp_yl_i":         ("cp yl,{i}", "i"),
    "_add_r_q":         ("add r{q},r{r}", "rq"),
    "_adc_r_q":         ("adc r{q},r{r}", "rq"),
    "_sub_r_q":         ("sub r{q},r{r}", "rq"),
    "_sbc_r_q":         ("sbc r{q},r{r}", "rq"),
    "_and_r_q":         ("and r{q},r{r}", "rq"),
    "_or_r_q":          ("or r{q},r{r}", "rq"),
    "_xor_r_q":         ("xor r{q},r{r}", "rq"),
    "_rlc_r":           ("rlc r{r}", "r"),
    "_ld_x_x":          ("ld x,{x}", "x"),
    "_add_r_i":         ("add r{r},{i}", "ri"),
    "_adc_r_i":         ("adc r{r},{i}", "ri"),
    "_and_r_i":         ("and r{r},{i}", "ri"),
    "_or_r_i":          ("or r{r},{i}", "ri"),
    "_xor_r_i":         ("xor r{r},{i}", "ri"),
    "_sbc_r_i":         ("sbc r{r},{i}", "ri"),
}


def build_table():
    src = open(CORE, encoding="utf-8", errors="replace").read().splitlines()
    start = next(i for i, ln in enumerate(src) if "self._execute = (" in ln)
    end = next(i for i in range(start, len(src)) if re.match(r"^\s*\)\s*$", src[i]))
    names = []
    for ln in src[start + 1:end]:
        body = ln.split("#")[0]
        m = re.search(r"E0C6200\.(_\w+)", body)
        if not m:
            continue
        rep = re.search(r"\*\s*(\d+)", body)
        names.extend([m.group(1)] * (int(rep.group(1)) if rep else 1))
    if len(names) != 4096:
        raise SystemExit("dispatch table parsed as %d entries, expected 4096" % len(names))
    return names


def disasm(rom, start, end, table):
    out = []
    for a in range(start, end):
        op = (rom[2 * a] << 8 | rom[2 * a + 1]) & 0xFFF
        h = table[op]
        i = op & 0xF
        l = op & 0xFF
        s = op & 0xFF
        entry = HANDLERS.get(h)
        if entry:
            txt = entry[0].format(i=i, l="0x%02X" % l, s="0x%02X" % s,
                                  n=i, r=(op >> 2) & 3, q=op & 3,
                                  x="0x%02X" % l, y="0x%02X" % l, yl="0x%02X" % l)
        else:
            txt = "%s 0x%02X" % (h.lstrip("_"), l)
        out.append("%04X  %03X  %s" % (a, op, txt))
    return out


if __name__ == "__main__":
    rom_path = sys.argv[1]
    start = int(sys.argv[2], 16)
    end = int(sys.argv[3], 16)
    rom = open(rom_path, "rb").read()
    for line in disasm(rom, start, end, build_table()):
        print(line)

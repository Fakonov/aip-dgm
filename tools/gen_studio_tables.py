# -*- coding: utf-8 -*-
"""Generate the V2/V3 JS block (species + rules + base64 art) for the studio
page from the measured JSON and the cut cards.

    python tools/gen_studio_tables.py

Writes tools/_evo_tables_generated.js.  The block is then spliced into
tools/digimon_studio.html by hand (or by the one-off patch used in round 67);
the generated file is kept out of git -- regenerate it when the JSON or the
card PNGs change.
"""
import base64
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REF = ROOT / "tools" / "ref-disasm"

# Box index produced by tools/cut_evo_cards.py -> ROM species slot.
# V2 chart: box 0 Punimon, 1 Tsunomon, 2 Gabumon, 3 Elecmon, 4 Kabuterimon,
# 5 Angemon, 6 Birdramon, 7 Vegiemon, 8 Garurumon, 9 Yukidarumon, 10 Whamon,
# 11 SkullGreymon, 12 MetalMamemon, 13 Vademon.
MAP_V2 = {1: 0, 2: 1, 3: 2, 4: 3, 5: 4, 6: 5, 7: 6, 8: 8, 9: 9, 10: 10,
          11: 7, 12: 11, 13: 12, 14: 13}
# V3 chart: box 1 is the cartridge photo (dropped by --drop 1), then
# 0 Poyomon, 2 Tokomon, 3 Patamon, 4 Kunemon, 5 Unimon, 6 Ogremon,
# 7 Shellmon, 8 Scumon, 9 Centalmon, 10 Bakemon, 11 Drimogemon,
# 12 Andromon, 13 Giromon, 14 Etemon.
MAP_V3 = {1: 0, 2: 2, 3: 3, 4: 4, 5: 5, 6: 9, 7: 6, 8: 10, 9: 7, 10: 11,
          11: 8, 12: 12, 13: 13, 14: 14}
# V4 chart (page_img_05.jpg): box 1 is the cartridge photo (dropped by --drop 1),
# then 0 Yuramon, 2 Tanemon, 3 Piyomon, 4 Palmon, 5 Kokatorimon,
# 6 Leomon, 7 Coelamon, 8 Nanimon, 9 Cocatrimon, 10 Kuwagamon,
# 11 Mojyamon, 12 MegaDramon, 13 Picklemon, 14 DigiTamamon.
MAP_V4 = {1: 0, 2: 2, 3: 3, 4: 4, 5: 5, 6: 9, 7: 6, 8: 7, 9: 10, 10: 11,
          11: 8, 12: 12, 13: 13, 14: 14}


def load_rules(ver):
    return json.load(open(REF / ("evo_%s_rules.json" % ver), encoding="utf-8"))


def art(ver, mapping):
    out = {}
    for slot, box in mapping.items():
        p = ROOT / "docs" / ("evo_cards_%s" % ver) / ("box%02d.png" % box)
        out[slot] = "data:image/png;base64," + base64.b64encode(p.read_bytes()).decode("ascii")
    return out


def js_species(d):
    lines = ["  " + json.dumps(s, ensure_ascii=False) + "," for s in d["species"]]
    return "[\n" + "\n".join(lines) + "\n]"


def js_rules(d):
    lines = []
    for r in d["rules"]:
        lines.append("  { from: %d, to: %d, care: %s, train: %s, over: %s, sleep: %s }," % (
            r["from"], r["to"], json.dumps(r["care"]), json.dumps(r["train"]),
            json.dumps(r["over"]), json.dumps(r["sleep"])))
    return "[\n" + "\n".join(lines) + "\n]"


def js_art(a):
    lines = ["  %d: %s," % (slot, json.dumps(a[slot])) for slot in sorted(a)]
    return "{\n" + "\n".join(lines) + "\n}"


def main():
    v2, v3, v4 = load_rules("v2"), load_rules("v3"), load_rules("v4")
    out = [
        "/* ---- V2 / V3 / V4 tables (measured 2026-11-05) ----------------------",
        " * V2/V3/V4 share the 0x0D10 picker code word for word; the per-slot",
        " * species table differs, so the rule list is the same and the names/art",
        " * are not.  Cards cut from docs/pdf_assets/page_img_0{3,4,5}.jpg with the",
        " * same connected-component method as V1.",
        " */",
        "const SPECIES_V2 = " + js_species(v2) + ";",
        "const EVO_RULES_V2 = " + js_rules(v2) + ";",
        "const EVO_ART_V2 = " + js_art(art("v2", MAP_V2)) + ";",
        "",
        "const SPECIES_V3 = " + js_species(v3) + ";",
        "const EVO_RULES_V3 = " + js_rules(v3) + ";",
        "const EVO_ART_V3 = " + js_art(art("v3", MAP_V3)) + ";",
        "",
        "const SPECIES_V4 = " + js_species(v4) + ";",
        "const EVO_RULES_V4 = " + js_rules(v4) + ";",
        "const EVO_ART_V4 = " + js_art(art("v4", MAP_V4)) + ";",
    ]
    path = ROOT / "tools" / "_evo_tables_generated.js"
    path.write_text("\n".join(out) + "\n", encoding="utf-8")
    print("wrote", path, len("\n".join(out)), "chars")


if __name__ == "__main__":
    main()

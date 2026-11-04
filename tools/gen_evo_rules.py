# -*- coding: utf-8 -*-
"""Turn care_rules_v1.json (14112 measured rows) into an ordered rule list that
the PC tool can evaluate in plain JS, and prove the rules reproduce every row.

Rule order matters: the ROM walks its comparisons in a fixed order, so the
first matching rule wins.  Every rule is a conjunction of inclusive ranges.
"""
import json
import sys

SRC = r".\tools\ref-disasm\care_rules_v1.json"
OUT = r".\tools\ref-disasm\evo_v1_rules.json"

# (from_slot, to_slot, care, train, over, sleep) -- derived from the measured
# ranges, then verified exhaustively below.
RULES = [
    # Koromon (2): care mistakes decide the child
    (2, 3, (0, 3), (0, 63), (0, 6), (0, 12)),   # Agumon
    (2, 4, (4, 6), (0, 63), (0, 6), (0, 12)),   # Betamon

    # Agumon (3)
    (3, 5, (0, 3), (32, 63), (0, 6), (0, 12)),  # Greymon
    (3, 7, (0, 3), (0, 31), (0, 6), (0, 12)),   # Meramon
    (3, 6, (4, 6), (5, 15), (3, 6), (0, 4)),    # Tyranomon
    (3, 8, (4, 6), (16, 63), (3, 6), (0, 6)),   # Seadramon
    (3, 11, (4, 6), (0, 63), (0, 6), (0, 12)),  # Airdramon

    # Betamon (4)
    (4, 7, (0, 3), (48, 63), (0, 6), (0, 12)),  # Meramon
    (4, 8, (0, 3), (0, 47), (0, 6), (0, 12)),   # Seadramon
    (4, 9, (4, 6), (8, 31), (0, 3), (9, 12)),   # Numemon
    (4, 10, (4, 6), (8, 31), (4, 6), (0, 8)),   # Devimon
    (4, 11, (4, 6), (0, 63), (0, 6), (0, 12)),  # Airdramon
]

NAMES = {
    0:  ("EGG", "数码蛋"),
    1:  ("Botamon", "滚球兽"),
    2:  ("Koromon", "小鬼兽"),
    3:  ("Agumon", "亚古兽"),
    4:  ("Betamon", "比特兽"),
    5:  ("Greymon", "暴龙兽"),
    6:  ("Tyranomon", "巨龙兽"),
    7:  ("Meramon", "火焰兽"),
    8:  ("Seadramon", "海龙兽"),
    9:  ("Numemon", "鼻涕兽"),
    10: ("Devimon", "恶魔兽"),
    11: ("Airdramon", "飞龙兽"),
    12: ("MetalGreymon", "机械暴龙兽"),
    13: ("Mamemon", "豆豆兽"),
    14: ("Monzaemon", "熊仔兽"),
    15: ("DEAD", "死亡"),
}


def match(rule, care, train, over, sleep):
    _, _, c, t, o, s = rule
    return (c[0] <= care <= c[1] and t[0] <= train <= t[1]
            and o[0] <= over <= o[1] and s[0] <= sleep <= s[1])


def main():
    data = json.load(open(SRC, encoding="utf-8"))
    evo = data["evolution"]

    bad = []
    for key, want in evo.items():
        s, care, train, over, sleep = (int(x) for x in key.split(","))
        got = None
        for rule in RULES:
            if rule[0] == s and match(rule, care, train, over, sleep):
                got = rule[1]
                break
        if got != want:
            bad.append((key, want, got))

    if bad:
        print("MISMATCH %d rows, first 12:" % len(bad))
        for row in bad[:12]:
            print("   ", row)
        sys.exit(1)

    out = {
        "source": "care_rules_v1.json (ROM 0x0D10 swept over 14,112 combinations)",
        "species": [{"slot": k, "en": v[0], "zh": v[1]} for k, v in sorted(NAMES.items())],
        "rules": [
            {"from": f, "to": t,
             "care": list(c), "train": list(tr), "over": list(o), "sleep": list(s)}
            for (f, t, c, tr, o, s) in RULES
        ],
    }
    json.dump(out, open(OUT, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print("all %d rows reproduced by %d rules" % (len(evo), len(RULES)))
    print("wrote", OUT)


if __name__ == "__main__":
    main()

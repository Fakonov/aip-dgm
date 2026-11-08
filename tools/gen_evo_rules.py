# -*- coding: utf-8 -*-
"""Turn a measured care_rules JSON (14,112 boundary rows) into an ordered rule
list the PC tool can evaluate in plain JS, and prove the rules reproduce every
row.

Rule order matters: the ROM walks its comparisons in a fixed order, so the
first matching rule wins.  Every rule is a conjunction of inclusive ranges.

    python tools/gen_evo_rules.py --version v1
    python tools/gen_evo_rules.py --version v2
    python tools/gen_evo_rules.py --version v3
    python tools/gen_evo_rules.py --version all
"""
import argparse
import json
import sys

BASE = r"<home>\digimon-passport\tools\ref-disasm"

# (from_slot, to_slot, care, train, over, sleep)
# V1: decoded from 0x0D10 (the V1 branch constants), then verified exhaustively.
RULES_V1 = [
    # Koromon (2): care mistakes decide the child
    (2, 3, (0, 3), (0, 63), (0, 6), (0, 15)),   # Agumon
    (2, 4, (4, 6), (0, 63), (0, 6), (0, 15)),   # Betamon

    # Agumon (3)
    (3, 5, (0, 3), (32, 63), (0, 6), (0, 15)),  # Greymon
    (3, 7, (0, 3), (0, 31), (0, 6), (0, 15)),   # Meramon
    (3, 6, (4, 6), (5, 15), (3, 6), (0, 4)),    # Tyranomon
    (3, 8, (4, 6), (16, 63), (3, 6), (0, 6)),   # Seadramon
    (3, 11, (4, 6), (0, 63), (0, 6), (0, 15)),  # Airdramon

    # Betamon (4)
    (4, 7, (0, 3), (48, 63), (0, 6), (0, 15)),  # Meramon
    (4, 8, (0, 3), (0, 47), (0, 6), (0, 15)),   # Seadramon
    (4, 9, (4, 6), (8, 31), (0, 3), (9, 15)),   # Numemon
    (4, 10, (4, 6), (8, 31), (4, 6), (0, 8)),   # Devimon
    (4, 11, (4, 6), (0, 63), (0, 6), (0, 15)),  # Airdramon
]

# V2/V3: identical 0x0D10 code (the whole table at 0x0D00 matches word for
# word); only the per-slot species tables differ.  Verified against the full
# 150,528-row sweep of both ROMs.
RULES_V23 = [
    (2, 3, (0, 3), (0, 63), (0, 6), (0, 15)),   # child A
    (2, 4, (4, 6), (0, 63), (0, 6), (0, 15)),   # child B

    (3, 5, (0, 3), (48, 63), (0, 6), (0, 15)),  # high training
    (3, 7, (0, 3), (0, 47), (0, 6), (0, 15)),   # low training
    (3, 6, (4, 6), (5, 31), (3, 6), (0, 4)),
    (3, 8, (4, 6), (32, 63), (3, 6), (0, 6)),
    (3, 11, (4, 6), (0, 63), (0, 6), (0, 15)),  # fallback

    (4, 7, (0, 3), (48, 63), (0, 6), (0, 15)),
    (4, 9, (0, 3), (0, 47), (0, 6), (0, 15)),
    (4, 8, (4, 6), (8, 15), (0, 5), (5, 15)),
    (4, 10, (4, 6), (16, 63), (6, 6), (0, 4)),
    (4, 11, (4, 6), (0, 63), (0, 6), (0, 15)),  # fallback
]

NAMES_V1 = {
    0:  ("EGG", "数码蛋"),
    1:  ("Botamon", "滚球兽"),
    2:  ("Koromon", "小鬼兽"),
    3:  ("Agumon", "亚古兽"),
    4:  ("Betamon", "比特兽"),
    5:  ("Greymon", "暴龙兽"),
    6:  ("Tyranomon", "巨龙兽"),
    7:  ("Meramon", "火焰兽"),
    8:  ("Seadramon", "海龙兽"),
    9:  ("Airdramon", "飞龙兽"),
    10: ("Devimon", "恶魔兽"),
    11: ("Numemon", "鼻涕兽"),
    12: ("MetalGreymon", "机械暴龙兽"),
    13: ("Mamemon", "豆豆兽"),
    14: ("Monzaemon", "熊仔兽"),
    15: ("DEAD", "死亡"),
}

NAMES_V2 = {
    0:  ("EGG", "数码蛋"),
    1:  ("Punimon", "普尼兽"),
    2:  ("Tsunomon", "兹巴兽"),
    3:  ("Gabumon", "加布兽"),
    4:  ("Elecmon", "艾力兽"),
    5:  ("Kabuterimon", "甲虫兽"),
    6:  ("Angemon", "天使兽"),
    7:  ("Birdramon", "巴多拉兽"),
    8:  ("Garurumon", "加鲁鲁兽"),
    9:  ("Yukidarumon", "雪人兽"),
    10: ("Whamon", "巨鲸兽"),
    11: ("Vegiemon", "野菜兽"),
    12: ("SkullGreymon", "丧尸暴龙兽"),
    13: ("MetalMamemon", "金属豆豆兽"),
    14: ("Vademon", "宇宙脑魔"),
    15: ("DEAD", "死亡"),
}

NAMES_V3 = {
    0:  ("EGG", "数码蛋"),
    1:  ("Poyomon", "浮游兽"),
    2:  ("Tokomon", "迪哥兽"),
    3:  ("Patamon", "巴达兽"),
    4:  ("Kunemon", "库加兽"),
    5:  ("Unimon", "独角兽"),
    6:  ("Centalmon", "人马兽"),
    7:  ("Ogremon", "奥加兽"),
    8:  ("Bakemon", "猛鬼兽"),
    9:  ("Shellmon", "贝壳兽"),
    10: ("Drimogemon", "钻地兽"),
    11: ("Scumon", "烂泥兽"),
    12: ("Andromon", "安杜路兽"),
    13: ("Giromon", "守卫兽"),
    14: ("Etemon", "猩猩兽"),
    15: ("DEAD", "死亡"),
}

NAMES_V4 = {
    0:  ("EGG", "数码蛋"),
    1:  ("Yuramon", "尤拉兽"),
    2:  ("Tanemon", "种子兽"),
    3:  ("Piyomon", "比丘兽"),
    4:  ("Palmon", "巴鲁兽"),
    5:  ("Kokatorimon", "巨鸡兽"),
    6:  ("Cocatrimon", "鸡蛇兽"),
    7:  ("Leomon", "狮子兽"),
    8:  ("Coelamon", "腔棘鱼兽"),
    9:  ("Kuwagamon", "古加兽"),
    10: ("Mojyamon", "毛人兽"),
    11: ("Nanimon", "纳尼兽"),
    12: ("MegaDramon", "百万龙兽"),
    13: ("Picklemon", "泡菜兽"),
    14: ("DigiTamamon", "数码蛋兽"),
    15: ("DEAD", "死亡"),
}

VERSIONS = {
    "v1": ("care_rules_v1.json", "evo_v1_rules.json", RULES_V1, NAMES_V1,
           "care_rules_v1.json (V1 0x0D10 swept over 14,112 boundary rows)"),
    "v2": ("care_rules_v2.json", "evo_v2_rules.json", RULES_V23, NAMES_V2,
           "care_rules_v2.json (V2 0x0D10 swept over 14,112 boundary rows)"),
    "v3": ("care_rules_v3.json", "evo_v3_rules.json", RULES_V23, NAMES_V3,
           "care_rules_v3.json (V3 0x0D10 swept over 14,112 boundary rows)"),
    "v4": ("care_rules_v4.json", "evo_v4_rules.json", RULES_V23, NAMES_V4,
           "care_rules_v4.json (V4 0x0D10 swept over 14,112 boundary rows)"),
}


def match(rule, care, train, over, sleep):
    _, _, c, t, o, s = rule
    return (c[0] <= care <= c[1] and t[0] <= train <= t[1]
            and o[0] <= over <= o[1] and s[0] <= sleep <= s[1])


def one(ver):
    src_name, out_name, rules, names, source = VERSIONS[ver]
    data = json.load(open(f"{BASE}\\{src_name}", encoding="utf-8"))
    evo = data["evolution"]

    bad = []
    for key, want in evo.items():
        s, care, train, over, sleep = (int(x) for x in key.split(","))
        got = None
        for rule in rules:
            if rule[0] == s and match(rule, care, train, over, sleep):
                got = rule[1]
                break
        if got != want:
            bad.append((key, want, got))

    if bad:
        print("%s: MISMATCH %d rows, first 12:" % (ver, len(bad)))
        for row in bad[:12]:
            print("   ", row)
        return 1

    out = {
        "version": ver.upper(),
        "source": source,
        "species": [{"slot": k, "en": v[0], "zh": v[1]} for k, v in sorted(names.items())],
        "rules": [
            {"from": f, "to": t,
             "care": list(c), "train": list(tr), "over": list(o), "sleep": list(s)}
            for (f, t, c, tr, o, s) in rules
        ],
    }
    path = f"{BASE}\\{out_name}"
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(out, fh, ensure_ascii=False, indent=1)
    print("%s: all %d rows reproduced by %d rules -> %s" % (ver, len(evo), len(rules), out_name))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="all", choices=["v1", "v2", "v3", "v4", "all"])
    args = ap.parse_args()
    versions = ["v1", "v2", "v3", "v4"] if args.version == "all" else [args.version]
    rc = 0
    for v in versions:
        rc |= one(v)
    return rc


if __name__ == "__main__":
    sys.exit(main())

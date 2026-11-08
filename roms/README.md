# ROM 镜像（不随仓库分发）

`main/roms/*.bin` 是 Bandai《Digital Monster》1997 年原版固件的 16 KiB 镜像，
**版权不属于本项目**，因此不进这个仓库，也不在发布包里。

## 你需要自己准备

1. 从你自己的暴龙机读出 ROM，或使用你合法持有的镜像；
2. 改名成：

```
roms/digimon.bin      # V1（本项目用它对 50 万步黄金轨迹）
roms/digimonv2.bin    # V2
roms/digimonv3.bin    # V3
roms/digimonv4.bin    # V4（16 KiB，CRC32 27df4fd6；文件名随意，靠 CRC 认）
```

3. 跑主机回归：

```bash
cd src
gcc -O2 -I. -I../firmware/digimon-passport/main/core -o test_core test_core.c digimon_core.c
DG_ROM=../roms/digimon.bin ./test_core        # 期望 ALL CHECKPOINTS MATCH
```

`test_core` 的黄金轨迹（`pc trace sha256: f42d1802…`）是按 **V1** 的 16 KiB 镜像算出来的，
换别的镜像对不上是正常的。`test_core.c` 里的 ROM 路径是硬编码的，换机器要改。

4. 上机：刷完固件屏幕显示 `NO ROM`，用 PC 工具（`tools/digimon_studio.html`）连上设备，
   在「游戏卡带」页把 `roms/*.bin` 传到任意槽位即可（走 `ROMIN`，写 `roms` 数据分区）。

   `roms` 分区共 **8 槽 × 32 KB**（`0x210000`，6.1 MB）。槽位由 4 KB 头扇区 + 16 KiB 镜像组成，
   头最后写 —— 传到一半掉线不会顶掉这个槽原来的镜像。控制台等价命令：`ROMIN` / `ROMOUT` /
   `ROMINFO` / `ROMDEL` / `ROMBOOT`。

5. 想连 ROM 一起铺进整片镜像（救砖 / 仿真开局）：

```bash
python tools/mk_full_bin.py --out full-with-roms.bin \
    --rom 0=roms/digimon.bin:V1 --rom 1=roms/digimonv2.bin:V2
```

## 版本差异

V1 / V2 / V3 / V4 用的是同一个 E0C6200 核心，差异在进化规则表与物种表：
网页端按 ROM 名与 CRC 自动切表，固件侧不区分（它只跑 ROM）。
V4 已实测可直接跑（16 KiB / CRC `27df4fd6`，关键地址 `0x059D`、`0x0D00`、`0x0D10`
与 V2/V3 逐字相同）。

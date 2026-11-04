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
```

3. 跑主机回归：

```bash
cd src
gcc -O2 -I. -I../firmware/digimon-passport/main/core -o test_core test_core.c digimon_core.c
DG_ROM=../roms/digimon.bin ./test_core        # 期望 ALL CHECKPOINTS MATCH
```

`test_core` 的黄金轨迹（`pc trace sha256: f42d1802…`）是按 **V1** 的 16 KiB 镜像算出来的，
换别的镜像对不上是正常的。

4. 上机：刷完固件屏幕显示 `NO ROM`，用 PC 工具（`tools/digimon_studio.html`）连上设备，
   在 ROM/存档页把 `roms/*.bin` 传到槽 0 / 1 / 2 即可（走 `ROMIN`，写 `roms` 数据分区）。

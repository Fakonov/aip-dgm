# Digital Monster on AI Passport

把 Bandai《Digital Monster》（数码暴龙机，1997）的**原版 V1 V2 V3 固件**跑在
**FoloToy AI Passport**（ESP32-C3，240×320 屏，三个按键）上。

不是复刻玩法，是把原机的 4-bit MCU 完整模拟出来：CPU、定时器、中断、按键扫描、
1-bit 蜂鸣器全部按原机行为实现，并用 **50 万步逐位比对**锁定行为
（`pc trace sha256: f42d1802…`）。屏幕版式按真机玻璃实测还原：一块玻璃三条带 ——
上下各 4 枚功能图标（独立 LCD 段，由 ROM 自己点亮）、中间 32×16 点阵。

**v1.0** · MIT · 不含任何 Bandai 的固件或美术资源（ROM 需自备，见 `roms/README.md`）

## 特性

- **ROM 与固件分离**：ROM 走独立 `roms` 数据分区（8 槽 × 32 KB），网页端一键上传 / 切换 / 擦除，刷固件不动存档
- **存档**：每槽一份 NVS 存档（magic + ROM CRC + 状态 CRC），60 秒滚动保存，可导出成 `.hex` 备份
- **PC 控制台**：单文件 `tools/digimon_studio.html`，Web Serial 直连，三页 —— ROM/存档、养成、调试
- **配图进化树**：按 ROM `0x0D10` 跑遍 **14,112** 组养育条件得到的实测规则，画成说明书那种
  分栏卡片树，命中当前数据的分支高亮
- **真跑式跳过时间**：`SKIP TIME` 真执行游戏时间（C3 上约 1.26 s / 游戏分钟），不是改数值

## 快速开始

### 1. 构建

需要 ESP-IDF 5.5.x：

```bash
cd firmware/digimon-passport
idf.py set-target esp32c3
idf.py build
```

### 2. 刷机

```bash
# 只刷 app（存档 / 设置 / ROM 都保留）
python -m esptool --chip esp32c3 -p COM5 -b 921600 write_flash \
    --flash_mode dio --flash_freq 80m --flash_size 8MB \
    0x10000 build/FoloToy-AI-Passport.bin
```

从更老的、分区表不同的版本升上来时，先补一次 `0x8000 partition-table.bin`（4 KB，不动 NVS）。
**不要刷 `0x0`** —— 整片镜像会覆盖 NVS，存档全没。

### 3. 装 ROM

刷完屏上显示 `NO ROM` 是正常的。把自备的镜像改名成 `roms/digimon.bin`（V1）等，
用 PC 工具传到槽 0/1/2。详见 `roms/README.md`。

### 4. PC 控制台

用 Edge / Chrome 直接打开 `tools/digimon_studio.html`（`file://` 即可），点「连接设备」选串口。
无需安装、无需起服务器。说明书见 `docs/studio-tool.md`。

## 按键

三个键 1:1 对应原机 A / B / C。原机没有长按语义，长按只会被按键扫描当成反复按下；
固件把每次按下垫到 260 ms，避开原机约 120 ms 的按键扫描门槛。

| 按键 | 原机键 | 作用 |
|---|---|---|
| 上 / UP | A | 循环 7 个功能图标（状态 → 食物 → 训练 → 对战 → 便便 → 灯 → 药） |
| 中 / DOWN | B | 进入当前图标的功能页 |
| 下 / OK | C | 取消 / 返回 |

**长按 OK 3 秒**打开固件的设置菜单（原机没有）：`SKIP TIME` / `PLAY TIME` / `ROM`（二级槽位表）/
`SHELL`（14 种外壳配色）/ `BRIGHT` / `VOLUME` / `AUTO OFF`。
电源键是硬件闩锁、没接 MCU，固件读不到，不能用来做任何功能。

## 目录

```
firmware/digimon-passport/   固件工程（ESP-IDF 5.5 / esp32c3）
  main/digimon_app.c         渲染（点阵 + 8 个功能段）、按键、音频、主循环
  main/core/digimon_core.c   E0C6200 CPU 核心（C99 移植，无 malloc）
  main/core/digimon_segmap.h 32×16 段映射表（由参考 SVG 逆向生成）
  components/bsp/            屏幕 / 按键 / 音频 / 电池（来自 FoloToy 上游）
tools/                       PC 工具与生成器
  digimon_studio.html        单文件控制台（Web Serial）
  cut_evo_cards.py           从扫描件切进化卡片（需自备图片）
  gen_evo_rules.py           14112 组判定 → 12 条区间规则（含逐行回放校验）
src/                         主机侧开发与回归工具
  test_core.c                黄金轨迹回归（50 万步逐位比对）
  probe_causal.c             RAM→屏幕因果扫描（找出某个数字来自哪个 nibble）
docs/                        交接单、控制台说明书、屏幕版式
roms/                        ROM 放这里（不进仓库）
```

## 一些实测结论

- **进化判定是「四个 nibble 精确相等」**：`0x0596` 把阶段计时器 `M(48-4B)` 与目标时间
  `M(4C-4F)` 逐个 nibble 比较，全等才触发。跨过那一分钟就永久错过，ROM 会把目标写成 `0xFF`。
  另外 **小时是 8-bit 二进制，只有分钟是 BCD** —— 把它当 BCD 会让「早该进化却不进化」的假象骗你一整轮。
- **体重在 `M(56)`/`M(57)`，不是 `M(68)`**：`M(68-69)` 是种族基准体重（进化时写一次），
  屏幕上那个会涨的数在 `M(56)`（个位）/`M(57)`（十位），两位 BCD，上限 99 g。
  验证方法是把每个 nibble 逐个 +1 重画同一页，看哪一位能改变画面。
- **跳过时间只能真跑**：改 RAM 里的 stage 不会触发进化（ROM 只在自己的 tick 计数器跨过目标时判定），
  所以 2 小时档要跑约 150 秒 —— 这是硬件算力的硬成本。

更多坑与数据见 `docs/HANDOFF.md`。

## 许可

代码 MIT（见 `LICENSE`）。核心是 [BrickEmuPy](https://github.com/azya52/BrickEmuPy)（**CC0-1.0**）
的 C99 移植；BSP 来自 FoloToy（MIT）；MAME 仅作行为参考。
ROM 镜像、说明书扫描件、数码兽形象均不随本仓库分发，版权归各自权利人。
详见 `THIRD-PARTY.md`。

本项目与 Bandai、FoloToy 均无隶属关系。

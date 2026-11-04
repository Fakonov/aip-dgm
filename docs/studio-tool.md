# PC 端单文件工具 `tools/digimon_studio.html`

> **v3**：三页控制台（ROM / 存档 · 养成 · 调试）+ 常驻顶栏。浅色系、暴龙机 V1 外观元素。
> 存档只做"设备 ↔ 文件"直连，不经过浏览器缓存。
> 旧版留在 `tools/digimon_studio_v2_backup.html`。

## 是什么

一个 HTML 文件，双击用 **Edge / Chrome** 打开，`navigator.serial` 直接连设备的 USB-Serial-JTAG 控制台。

- 不装 Python、不装驱动（Windows 自带 CDC），不起本地服务器（`file://` 在 Chromium 里算安全上下文）
- 不联网、不引第三方 JS，整份文件 56 KB
- Firefox / Safari 没有 Web Serial，打开会直接提示

## 三个页面

### ① ROM / 存档

| 区域 | 内容 |
|---|---|
| 机内 ROM 槽 | 8 个槽，每张卡显示 **名字 / 字节数 / CRC32 / PLAY TIME**，`运行中` 标绿 |
| 槽操作 | **上传**（可一次多选，自动铺空槽）、**读出**（存 .bin 并本地核 CRC）、**启动**、**擦除** |
| 当前存档 | **导出到文件**（`SAVEOUT` → 直接下载 `.hex`） / **从文件导入**（选 `.hex`/`.bin` → 校验 → `SAVEIN` 写进设备） / **重置当前存档**（两段确认，走 `SAVEDEL`） |

PLAY TIME 来自 NVS 里跟存档配套的 `a0..a7`，由固件 `AGES` 命令一次给全 8 个槽。

### ② 养成

- **当前数码兽**：中英名（亚古兽 / Agumon）、slot、PLAY TIME、距下次进化的分钟数与目标时间
- **SKIP TIME**：5 / 30 / 60 / 120 分钟，走 `SKIP <分钟>`，真跑游戏时间
- **进化路线**：照说明书那张分解图的版式做的**配图树状图** —— 三栏（现在 → 下一步 → 再下一步），
  每只数码兽一张从 PDF 上裁下来的彩色卡片（按阶段红/黄/绿/青），卡片之间用曲线连接。
  命中当前数据的分支用橙色粗线点亮、卡片描红边；其余分支压暗但仍然画出来。
  卡片下方是该分支的阈值（失误 / 训练 / 过量 / 睡眠），顶部一句话给出「按当前数据 →
  哪一只」的结论。完全体不是养育分支（15 场对战 + 掷骰），只摆卡片连线不画
- **养育数据**：**6 项**（体重 / 养育时长 / 照顾失误 / 训练次数 / 过量喂食 / 睡眠打断），
  英文变量名 + 中文俗称，带满值进度条；其余 11 项收进「其它数据」折叠区，默认隐藏
  （Hunger 饱食度 / Strength 体力 / Base Weight 基准体重 / Protein 蛋白质 / Overdoses 用药过量 /
  Poop 便便 / Injuries 受伤次数 / Injury Hours 受伤时长 / Battles 对战次数 / Win Score 胜点 / Energy 能量）
- **Ver.1 全进化表**：幼年期 → 完全体，同一套卡片语言纯网格排布，当前形态高亮
- **官方路线图**：从 FileIsland《V-Pet Version 1-5》PDF 抽出的 Ver.1 彩色进化图（内嵌为 data URI）

> 体重取的是**真机状态页显示的那个数**（RAM `M(56)` 个位 / `M(57)` 十位，两位 BCD，上限 99 g）；
> `M(68-69)` 是种族基准体重（Betamon 20 g，只在进化时写一次），单独显示为「基准体重」。

### ③ 调试

- **RAM**：`0x00–0xFF` 的 16×16 nibble 表，非零格标黄，点格子看地址与十进制值
- **连接自检**：一键依次发 `ROMINFO / STATE / AGES / RAM / DUMP / PET`，逐条打 `OK n 行` 或 `NO REPLY`
  —— 排查"按钮没反应"先点它
- **工具**：原始命令输入、CRC32 计算（hex 或纯文本）、完整命令速查

底部常驻 **CONSOLE**：串口所有输出按级别上色，可过滤、清屏、存 `.txt`，保留最近 4000 行。

> **CONSOLE 现在在右侧栏**（宽 400 px，`?log=1` 可直接展开），顶栏「日志」按钮开合，
> 连接设备时自动展开。日志行按显示宽度自己换行（中文算 2、英文算 1，每行 44 单位），
> 不再被窄栏裁掉半截。以前它是底部横条，一展开就把页面内容顶上去，越堆越高。

## 进化条件的来源

不是抄攻略，是**从 ROM 里跑出来的**。`article` 那套反汇编把 `0x0D10` 的判定函数按
`物种 × 失误 × 训练 × 过量 × 睡眠打断` 跑遍 14112 种组合，结果在
`data/care_rules_v1.json`。

`tools/gen_evo_rules.py` 把这 14112 行压成 **12 条区间规则**，并逐行回放校验
（必须 100% 复现），通过后才写进页面的 `EVO_RULES`。判定顺序就是 ROM 的比较顺序，
先命中先得：

```
Koromon  失误 0–3 → Agumon        失误 4–6 → Betamon
Agumon   失误 0–3 & 训练 32+ → Greymon      失误 0–3 & 训练 0–31 → Meramon
         失误 4–6 & 训练 5–15  & 过量 3–6 & 睡眠 0–4  → Tyranomon
         失误 4–6 & 训练 16–63 & 过量 3–6 & 睡眠 0–6  → Seadramon
         失误 4–6（其余）→ Airdramon
Betamon  失误 0–3 & 训练 48+ → Meramon      失误 0–3 & 训练 0–47 → Seadramon
         失误 4–6 & 训练 8–31 & 过量 0–3 & 睡眠 9+ → Numemon
         失误 4–6 & 训练 8–31 & 过量 4–6 & 睡眠 0–8 → Devimon
         失误 4–6（其余）→ Airdramon
```

完全体（MetalGreymon / Mamemon / Monzaemon）不是养育分支，是 15 场对战后的掷骰，
页面只列名字不编条件。

## 协议速查

```
ROMINFO                          → ROMSLOTS 8 / ROMSLOT <i> <name|-> <len> <crc32> <ACTIVE|-> / ROMCUR <i> <name>
ROMIN <slot> <len> <crc32> [名]  → ROMREADY <slot> <len> …hex… ROMEND → ROMOK | ROMBAD
ROMOUT <slot>                    → ROMBEGIN … hex … ROMEND
ROMBOOT <slot> / ROMDEL <slot>   → ROMBOOT <slot> <name> / ROMDELETED <slot>
SAVEOUT / SAVEIN                 → SAVEBEGIN … SAVEEND / IMPORTREADY … IMPORTOK | IMPORTBAD
SAVEDEL <slot>                   → SAVEDEL ok <slot>（擦存档 blob + PLAY TIME，重置当前槽会回蛋）
STATE                            → STATE slot=3 care=1 train=3 … weight=91 basew=20 stage=18:42 next=78:13 left=3571 age=105995
AGES                             → AGES 0=378000 1=45200 …
RAM                              → RAM 00 <16 hex> × 16 行 → RAMEND
DUMP / PET / SKIP <分钟>         诊断 / 摘要 / 跳过时间
```

`crc32` 是 zlib/IEEE 多项式，8 位十六进制（带不带 `0x` 都行）。`name` 最多 11 个可打印 ASCII。

## 存档格式

设备导出的是纯文本（1324 B 的档 → 2648 个 hex 字符）：

```
SAVEBEGIN V1 1324
444747311bacff08...
SAVEEND
```

前 20 字节头，小端：

| 偏移 | 字段 | 说明 |
|---|---|---|
| 0 | magic | `0x31474744` = `"DGG1"` |
| 4 | rom_crc | 该 ROM 的 CRC32，换 ROM / 改核心结构就作废 |
| 8 | state_size | 状态长度（1304） |
| 12 | state_crc | 状态部分 CRC32 |
| 16 | seq | 存档序号 |

工具在收档时就地校验 magic / 长度 / state_crc，不合格直接报错、不落盘。

> **没有了中间缓存**：导出就是"设备 → 下载 .hex"，导入就是"选文件 → 写设备"，
> 两下都是终态。要保底就自己把 `.hex` 存好。

## 排错

| 现象 | 原因 / 处理 |
|---|---|
| 「这个浏览器没有 Web Serial」 | 换 Edge / Chrome；`file://` 或 `https` 打开 |
| 连上但没日志 | 设备在跑别的模式，或者 USB 线是充电线 |
| 屏幕 `NO ROM` | `roms` 分区空：选一个槽「上传」 |
| 上传回 `ROMBAD ESP_ERR_INVALID_CRC` | 传输掉字节或文件不是 16 KB；槽保持原样，重传 |
| 等不到 `ROMREADY` | 固件太老，或串口被别的程序占着 |
| `IMPORTBAD` | 头里 rom_crc 与设备当前 ROM 不符，或 hex 被截断 |
| 点「重置当前存档」没反应 | 要先连设备且设备已加载 ROM；确认两段弹窗都要点「确定」 |
| 所有按钮都没反应 | 先看底部 CONSOLE 有没有红色「写串口失败」，再断开重连。新版在连接后 1.8 s 内收不到任何数据时会直接告警 |
| 跳时间弹了提示但时间没动 | 旧版发送失败也会弹成功 toast（已修）；现在只有字节真的写出去才提示 |
| 改了页面没变化 | 浏览器还在跑旧的 HTML：硬刷新（Ctrl+F5）。旧标签页占着串口时，新页面连不上 |
| 点「跳过时间」后设备像死机 | 跑 SKIP 时 CPU 一直在忙循环，屏幕只刷那张进度卡（`SKIP 2H 45% / C=STOP`）。5M≈6 s、30M≈38 s、2H≈150 s 是硬成本，期间串口命令也不响应 |
| 点「断开连接」页面卡住 | 旧版 `disconnect()` 顺序有 bug（先 cancel 再清 port，读循环会重新挂上 reader，`port.close()` 永久等待）。新版先断状态再收尾，并且 `connect()` 会自动先断开上一次 |
| PLAY TIME 全是 0 | 该槽还没养过（NVS `a0..a7` 为空），或者固件不是本轮之后的版本 |

## 刷机（仍未接）

同一个串口既要跑日志又要跑 esptool 握手，所以刷机得单独一个模式：断日志 → `new ESPLoader({transport})` 接管 → 刷完挂回来。

```js
import { ESPLoader, Transport } from 'https://unpkg.com/esptool-js/bundle.js';
const transport = new Transport(port, true);
const loader = new ESPLoader({ transport, baudrate: 921600, terminal });
await loader.main();
await loader.writeFlash({
  fileArray: [{ data, address: 0x10000 }],   // 只写 app，NVS 不动
  flashMode: 'dio', flashFreq: '80m', flashSize: '8MB',
  eraseAll: false, compress: true,
});
```

**别写 0x0**：整片镜像会覆盖 `0x9000` 起的 NVS，存档和设置全没，只有救砖才这么干。

## 看排版

没设备也能看：`digimon_studio.html?demo=1`（填假数据），
加 `&tab=rom|care|debug` 直接落到某一页。

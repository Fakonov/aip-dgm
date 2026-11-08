# 联机对战可行性研究（D-Link on AI Passport）

> 日期：2026-11-05
> 结论先行：**可行，ROM 层已经在本机跑通一半（能发帧），真正的拦路虎是内存和接收路径，不是协议。**
> 本文所有"已核实"都有本机实测证据；推测与未验证单独标注。

---

## 0. 一句话结论

数码暴龙 V1/V2/V3 的"联机对战"在协议上极其轻：**一场只交换 4 个 16 bit 帧、有效数据 4 个 nibble**，
胜负还是发起方单方面判定的。所以"能不能做"不取决于协议，取决于两件事：

1. **链路引脚 P2.0 能不能在核里被驱动** —— 已定位，能观测到 ROM 的发送波形（见 §2.3）。
2. **这块板子还有没有资源** —— GPIO 已用光（插线方案直接死），WiFi/BLE 与现有 52 KB 空闲堆冲突（见 §3）。

---

## 1. 协议事实（外部资料，已交叉核对）

| 项 | 内容 | 出处 |
|---|---|---|
| 电气 | 2 端子：GND + 单根数据线；空闲 Hi-Z 上拉，发送时主动驱动高低 | [botamochi D-Link 解析](http://botamochi.github.io/articles/analysis/dlink.html) |
| 帧 | 16 bit，LSB first | 同上 |
| 起始 | Low 60 ms（长度不限，受方会一直等） | 同上 |
| 头脉冲 | High 2 ms + Low 1 ms（受方拿它当判决门限） | 同上 |
| 数据位 | 每位 4 ms：`1` = High 2.5 ms + Low 1.5 ms；`0` = High 1 ms + Low 3 ms | 同上 |
| 冗余 | 高 8 bit = 低 8 bit 的反码，不符即通信错误 | [初代通信解析](http://botamochi.github.io/articles/analysis/origin.html) |
| 帧内容 | 第 1 帧：`[effort 4b][slot 4b]`；第 2 帧：`[version 4b][胜败 4b]` | 同上 + [DMComm orig battle](https://dmcomm.github.io/orig_battle/) |
| 交换顺序 | A1 → B1 → A2 → B2（交替，不并发） | 同上 |
| 胜负 | **发起方 ROM 单方面算**，受方只把结果取反回传 | 同上 |
| 场次 | 原版一场 = 4 帧 ≈ 0.5 s | 按上表推算 |
| 数值 | slot 3–14（进化表位置）；effort 0–4（训练/增强）；version 0=V1、1=V2…；V1 当受方时会鹦鹉学舌回对方的 version | [DMComm orig battle](https://dmcomm.github.io/orig_battle/) |

关键推论：**对战不需要两台机器"同步跑同一段模拟"**。只要一台拿到另一台的 `slot + effort`，
它自己的 ROM 就能算出胜负并把结果写回 RAM。实时同步只影响"两手一拍即合"的观感，不影响正确性。

---

## 2. 本项目侧的事实（本机实测）

### 2.1 链路引脚就是 P2.0

BrickEmuPy 的 Digimon 设备配置（`brick.zip` 内 `assets/DigimonV1JA.brick`）：

```json
"peripherals": {
  "CON_DGM": { "port": "P2", "mask": 1 },
  "direct_input": {
    "btnTop":    { "port": "K0", "mask": 4, "level": 0 },
    "btnCenter": { "port": "K0", "mask": 2, "level": 0 },
    "btnBottom": { "port": "K0", "mask": 1, "level": 0 }
  }
}
```

`CON_DGM`（`brickemupy/peripherals/CON_DGM.py`）就是那套 D-Link 收发状态机：它挂在 **P2.0**
上，用 CPU 周期数测量 ROM 的高电平宽度来解码，再用同样的周期表把对方的帧"回放"成波形。
V1/V2 的 `.brick` 都是 `P2 / mask 1`。

### 2.2 在现有 C 核里的 I/O 事实

- `0xF62` = P2 数据口（`dg_io_get/0xF62` → `dg_port_read(PORT_P2)`）
- `0xF7D` = IOC，bit2 = P2 方向（写 4 → 输出，写 0 → 输入）
- 端口输入已有模型：`port_input[port][0/1]` + `dg_pin_handler()`（目前只对 K0 暴露了 `dg_set_button`）

### 2.3 ROM 真的会跑 D-Link（本机复现）

用 `src/battle_trace6.c`（带 I/O 事件记录的内核副本）实测 V1 ROM：

| 状态 | 现象 |
|---|---|
| slot 1（按 B 一次）/ slot 2（连按 B 五次），幼年期 | **P2 一次都没被访问**，只是切了个显示 |
| slot 3（成长期，Agumon） | `mode=5 sub=2`，2 s 内 `0xF62` 被访问 822 次，同时配 PTIMER（`0xF78/F26/F27/F79/F12`） |
| 再按一次 B | 出现 `F7D W 4`（设输出）+ `F62 W E/F`（拉低/拉高）→ **开始发帧** |

`src/battle_host.c`（主机侧"假对手"）把这段波形解出来：

```
[peer] RX #1 = FC03  slot=3 effort=0 ver=F out=3
```

`0xFC03`：低字节 `0x03` = `(effort 0)<<4 | slot 3`，高字节 `0xFC` = 低字节反码。
**与 §1 的规范逐项吻合**（起始 60 ms、头脉冲 66 tick=2 ms、位 33/90 tick=1 ms/2.75 ms、位周期 131 tick=4 ms、LSB first）。

### 2.4 还没打通的一环：接收

同一个 `battle_host.c` 把对手帧 `0xDC23`（slot 3 / effort 2）回放进去后，ROM **没有推进到第二帧**，
约 10 s 后自己退出对战回主屏。排查结果：

- 注入是有效的：ROM 读到 `0xE`（bit0 被拉低）而不是 `0xF`，说明 `port_input` 路径通；
- 但 ROM 对 P2 的采样周期是 **~150 tick ≈ 4.6 ms**（见 `battle_host.exe` 的 `read[i] t=...` 轨迹），
  **比 4 ms 的位周期还长**——轮询根本不可能区分 1 ms / 2.75 ms 的高电平；
- 核里 `ISIO` 中断标志从不被置位，`EIPT/PTIMER` 的时序模型也未经验证。

**判断（推测，需验证）**：ROM 的收帧不是轮询，而是靠某种边沿中断/定时器在测宽；
现有核（以及 BrickEmuPy 参考实现）可能在这条路径上与真机有偏差。这是本项目唯一需要继续逆向的点。

---

## 3. 这块板子的硬约束

| 约束 | 数值 | 影响 |
|---|---|---|
| GPIO | **已用光**：0–10、18–21；11–17 是 SPI flash；18/19 是 USB | **"插线对战"方案直接排除**，除非放弃 USB 控制台/刷机 |
| 内部 RAM | 稳态 `free=52,420 B / largest=20,480 B`（PROGRESS round 67） | Wi-Fi/BLE 协议栈需要几十 KB → 现在起不来 |
| 玻璃画布 | `224×203×2 = 90,944 B`（pitch 7） | 改 8 bpp 索引色可省 **45 KB**，这是最直接的腾挪空间 |
| Flash | app 分区 2 MB，实际 880,768 B；`roms` 分区 6.1 MB 只用了 256 KB | 空间富余，不是瓶颈 |
| 无线 | ESP32-C3 自带 Wi-Fi 4 + BLE 5，BSP 已有 demo 证明可用（进页起、退页停） | 只差内存 |
| 时间基准 | ROM 的链路波形由它自己的时钟生成；解码也按仿真周期计 | **与真实帧率无关**，C3 跑快跑慢都不会误码 |

最后一条很重要：因为链路是"周期域解码 + 本地回放"，C3 只需要把 ROM 跑到"逻辑上正确"的节奏，
不需要真·实时。真机对战（路线 C）才需要 1× 实时。

---

## 4. 三条路线

### A. 卡片制（离线交换 2 字节）——推荐先做
- 内容：`slot + effort + version`（12 bit 够用），胜负由本地 ROM 算；
- 传输：PC 工具经现有 USB 控制台喂一条 `BATTLE <slot><effort>`（零新内存），对手卡存 NVS；
- 体验：战斗动画/胜负/统计全由 ROM 产生，和联机几乎一样，只是"对手"来自卡片；
- 工作量：核心+状态机 2 天，PC 侧半天；
- 不需要动画布、不需要无线。

### B. 实时无线（ESP-NOW 或 BLE）
- 每帧 2 字节，一场 4 帧 ≈ 8 字节，ESP-NOW 单播延迟 <5 ms；
- 前置条件：先把堆腾到 ~100 KB —— 玻璃画布改 8 bpp（省 45 KB），或砍 LVGL 缓冲；
- 工作量：A 的全部 + 内存改造 2–3 天 + 双机联调 2 天 ≈ 1–1.5 周；
- 风险：8 bpp 画布要动渲染管线（现在 blit 直接按 RGB565 做）。

### C. 和真·暴龙机对战（PC 中转）
- `passport ↔ USB ↔ PC ↔ D-Link 适配器 ↔ 真机`（dmcomm / botamochi 都有现成 Arduino 方案）；
- 需要 passport 侧 1× 实时（现有核在快时钟下约 2.3× 实时，够用）；
- 不需要无线、不需要改内存；
- 工作量 3–5 天，含电平/接线（真机 3 V，需电平适配）。

---

## 5. 建议的里程碑

| 里程碑 | 内容 | 验收 |
|---|---|---|
| M0 | 主机侧把接收打通（找到 ROM 的收帧机制：轮询/定时器/中断） | 喂一帧对手数据后 ROM 进入第二帧并出结果，`battle count / win count` 在 RAM 里 +1 |
| M1 | 核里加 `dg_link_set_input()` / `dg_link_get_latch()`，把 CON_DGM 状态机移植成 C | 主机上两台仿真机对战成功，且 `test_core` 仍 `ALL CHECKPOINTS MATCH` |
| M2 | 固件接 PC 控制台卡片（路线 A） | 真机：菜单进 battle → `BATTLE 3 2` → 打完出胜负 |
| M3 | 决定是否上无线（路线 B） | 两台真机对着打 |

**M0 才是真正的风险点**，其余都是工程量。

---

## 6. 未验证清单（不要当结论用）

1. ROM 的收帧机制（§2.4）——最大未知；
2. 两台同时按 B"抢发"时 ROM 的行为（真机上是线与，双方可能同时拉低）；
3. 对战超时是 ROM 自己定的还是外部（实测约 10 s 后自动回主屏）；
4. V2/V3/V4 的 P2/波形是否与 V1 一致（大概率一致，未实测）；
5. WiFi 起起来之后的实际空闲堆（需要真机构建实测）；
6. 8 bpp 玻璃画布对渲染/性能的影响。

---

## 7. 参考

- [D-Link 帧格式与波形](http://botamochi.github.io/articles/analysis/dlink.html)
- [初代数码暴龙通信解析（帧内容/胜负/version）](http://botamochi.github.io/articles/analysis/origin.html)
- [DMComm: Original battles（slot/effort/version 取值）](https://dmcomm.github.io/orig_battle/)
- BrickEmuPy：`peripherals/CON_DGM.py`、`assets/DigimonV1JA.brick`（本机 `<home>\brickemupy`）

## 8. 本次用到的探针（都在 `src/`）

- `battle_trace.c` / `battle_trace2.c` / `battle_trace3.c` / `battle_trace5.c` / `battle_trace6.c`
  —— 带 I/O 记录的内核副本 + 按阶段枚举按钮，用来定位"什么时候 P2 才动"；
- `battle_host.c` —— 主机侧 D-Link 应答方（解码 ROM 的帧 + 回放对手帧）；
- `trace_core_link.c` —— 仅探针使用的内核副本（加了 `g_link_p2_hook` / `g_link_p2_read_hook`），
  **不是固件的一部分**，改内核时不要漏掉它。

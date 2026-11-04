# 交接单（新会话先读这一页）

> 项目：`.`
> 目标机：FoloToy AI Passport（ESP32-C3，240×320 面板已按横屏使用）
> 详细历史：`PROGRESS.md`（round 1–64，含每次真机日志）；设计稿：`docs/icons/DESIGN.md`
> PC 工具：`tools/digimon_studio.html`（说明见 `docs/studio-tool.md`）
> **当前指纹**：固件 `0xD7080` = 880,768 B（2026-11-04 20:35，**已刷真机**，`Wrote 880768`
> 核对通过，刷后 `STATE` 回读 `weight=91 basew=20`）；PC 工具 242,289 B（内嵌 Ver.1 进化图
> + 14 张说明书画片，单文件无依赖）
> **交付包**：`<dist>`
> （固件 / 分区表 / 工具 / README.md / 说明.txt / `source-and-tools.zip` / `aip dgm.zip` /
> `镜像\` 三个官方 ROM + 校验）
> **里程碑快照**：
> `milestones/2026-11-02_vpet-glass-darkgrey/`（v1：玻璃版式 + 深灰外壳）
> `milestones/2026-11-02_menu-ux-v2/`（v2：菜单重做 / SHELL 死机修复 / 熄屏与长按语义）
> `milestones/2026-11-02_rom-partition/`（v3：ROM 与固件分离 + NO ROM + 网页端上传）
> `milestones/2026-11-03_evo-fixed-menu/`（v4：进化解码结案 + 菜单 7 行 / ROM 二级槽位）
> ⚠️ **v4 是 2026-11-03 23:1x 打的包**，之后 round 63/64 又改了固件（STATE/AGES/RAM/SAVEDEL、
> con_printf 缓冲区）和 PC 工具（存档直连、右侧日志栏、一堆修复）。
> **要最新交付物看 `test\20261104-0031\`，别从 v4 里拿**
> 上一轮会话日志（只用于考古，别当记忆用）：

---

## 现在是什么状态（可直接继续）

| 项 | 状态 |
|---|---|
| 核心 | BrickEmuPy 的 E0C6200 翻成 C99，50 万步逐位对齐（`test_core` 全绿，`pc_trace_sha256=f42d1802…`） |
| **ROM** | **不再编译进固件**。ROM 是独立文件，走 `roms` 数据分区（`0x210000`，6.1 MB，8 槽 × 32 KB）；槽 = 4 KB 头扇区 + 16 KB 镜像，头最后写 = 提交点。**没传 ROM 时玻璃显示 `NO ROM`**，模拟器根本不 step |
| ROM 上传 | USB 控制台协议 `ROMIN/ROMOUT/ROMINFO/ROMDEL/ROMBOOT`，网页工具 `tools/digimon_studio.html` 一键上传（支持一次多选批量铺槽，CRC 校验不过就 `ROMBAD`，旧镜像保持可用） |
| 屏幕 | **横屏 320×240**：`bsp_pins.h` 的 `BSP_LCD_ROTATED_90=1`，BSP 把 LVGL 逻辑尺寸设 320×240、`swap_xy+mirror_x`，旋转走 MADCTL（不花 CPU） |
| 版式 | **按真机玻璃实测**（`ref/vpet_v1v2v3/屏幕版式说明.md`）：一块玻璃三条带，全部 32 列宽；上/下带各 4 格，中间 32×16 点阵，带与点阵间留 2.4/2.5 格空白；整块 32 : 28.7 |
| 尺寸 | pitch **7**（整数倍）→ 玻璃 224×203，居中于 (48,18)；点阵 224×112；图标槽 56×28（8 列/枚，ink 占槽宽 60%） |
| 调色 | 玻璃三色：底 `#93A06B`、格间 `#A2AF7B`、点亮 `#1B2417`；`DG_LCD_TONE` 0/1/2 可切（`docs/icons/lcd_tone_compare.png`） |
| 图标 | 8 个槽位贴图（56×28，背景烤成玻璃色）**直接 blit 进玻璃画布**，不再是独立 LVGL 控件；灭=橄榄描边，亮=深色底板+白色原图 |
| 外壳配色 | 菜单 `SHELL` 行循环 14 种（V1 六色 / V2 四色 / V3 橙紫 / **深灰默认** / 工程米色），存 NVS；只换外壳/HUD 墨色，玻璃三种色调不动 |
| 边框 | 三层：塑料外框（1px 高光）+ 深色凹槽 + 玻璃；玻璃整体上抬 6 px（y 12..215）给 HUD 留横带 |
| HUD | **底部右侧一行**：`V1`（或 `--`）+ `82%`（14px），右边缘停在 x=304（再往右被 30 px 圆角遮罩切）；电池图标已删 |
| 段映射 | mode 2..8 → 状态/病毒/药/对战/便便/灯泡/创可贴；`(0,0)` = 呼叫，孵化后常亮 |
| 菜单 | **游戏里按住 OK 满 3 s 当场弹出**（不用等松手），半透明面板 300×200（`LV_OPA_90` + 阴影），黄底光标，**7 行**：**SKIP TIME**/**PLAY TIME**/ROM/SHELL/BRIGHT/VOLUME/AUTO OFF（没有 EXIT，C 直接关；SAVE OUT/IN 已挪到 PC 端） |
| 菜单 SKIP TIME | OFF/5M/30M/1H/2H，选好**关菜单那一刻生效**（一次性，用完自动复位 OFF）。实现是**真跑游戏时间**（见坑 19），玻璃上弹 `SKIP 2H  45% / C=STOP` 卡片，C 可中途取消 |
| 菜单 PLAY TIME | 原名 AGE：养育总时间（从孵化算起，含跳过的时间），`1H13M` 格式，跟着 ROM 槽存 NVS `a0..a7`；**按 B 两次清零**（第一下只显示 `RESET? B`，移动光标即取消） |
| 菜单 ROM 行 | 右侧 `V1 >`，**按 B 进二级槽位表**：8 个槽一行一个，左列 `序号 *名字`（`*` = 当前槽），右列该槽存档的 PLAY TIME（空槽压暗、`--`）。`A` 移动、`B` 启动、`C` 返回一级。进二级时一级面板要先 `HIDDEN`，否则半透明叠半透明全是残影 |
| 菜单重置存档 | 二级列表最后一行红字 `RESET SAVE <当前 ROM>`，`B` 三道门：`SURE?` → `REALLY?` → 擦 `s%d`+`a%d`；重置的就是在跑的那只时还会 `dg_reset()` 回蛋（ROM 自己清 RAM，等于冷启动），**ROM 镜像不动**。作用于**当前激活槽**而非光标槽——只有一个 `A` 键，跟随光标必然指到最下面那个槽 |
| 菜单按键 | **A=下一行　B=改值/进二级　C=关闭（二级里是返回）**；NVS 只在关菜单时提交一次 |
| 熄屏 | `AUTO OFF` 到点关背光；**任何键只负责唤醒**（按下+松开一起吞，`s_wake_mask`），不会顺带开菜单 |
| 存档 | 每 ROM 一个 NVS 槽 `s0..s7`（magic+ROM CRC+状态 CRC+序号），60 s 滚动存；PC 端只做「设备 ↔ 文件」直连，串口命令 `SAVEOUT`/`SAVEIN`/`SAVEDEL`/`PET` |
| PC 工具 | `tools/digimon_studio.html` **v3**（242 KB 单文件，内嵌 Ver.1 进化图 + 14 张说明书画片）：常驻顶栏 + 三页——①ROM/存档（8 槽卡带上 PLAY TIME、上传·读出·启动·擦除；存档**只做「导出到文件 / 从文件导入」两条直连，无浏览器缓存**；另有两段确认的重置）②养成（当前数码兽、SKIP 四档、**配图树状图**进化路线（三栏卡片 + 曲线，命中橙线点亮）、**6 项**养育数据 + 其余折叠、Ver.1 全表（同套卡片）+ 官方路线图）③调试（16×16 RAM、**连接自检**、原始命令、CRC）。**CONSOLE 在右侧栏**（400 px，顶栏「日志」开合，**不再随连接自动展开**，用 `?log=1` 或点按钮）。旧版留 `digimon_studio_v2_backup.html` |
| 控制台命令 | `ROMINFO/ROMIN/ROMOUT/ROMDEL/ROMBOOT` + `SAVEOUT/SAVEIN/SAVEDEL` + `STATE`（一行全养育数据，含 `weight` 当前体重与 `basew` 基准体重）`AGES`（8 槽 PLAY TIME）`RAM`（0x00–0xFF）`DUMP/PET/SKIP <分钟>` |

## 编译与刷机

```powershell
# 编译（本机 PowerShell 5.1 下 build.ps1 会被 export.ps1 的 stderr 打断，用这个包装脚本）
powershell -NoProfile -ExecutionPolicy Bypass -File .\build_local.ps1

# 合并镜像（idf.py build 不会重建 full.bin！模拟器要用它）
python tools\mk_full_bin.py
# 想预置 ROM（仿真直接开局 / 救砖带 ROM）：
python tools\mk_full_bin.py --out docs\ui-rom\full-with-v1.bin --rom "0=firmware\digimon-passport\main\roms\digimon.bin:V1"
```

**烧写分两种情况，别搞混：**

```powershell
$py = "<IDF_PYTHON>

# A. 老机器第一次升到 v3（分区表变了：factory 1MB -> 2MB，多了 roms 分区）
#    0x8000 那 4 KB 不碰 NVS，0x10000 只写 app —— 存档/设置/NVS 全保
& $py -m esptool --chip esp32c3 -p COM5 -b 921600 write_flash `
    --flash_mode dio --flash_freq 80m --flash_size 8MB `
    0x8000 build\partition_table\partition-table.bin `
    0x10000 build\FoloToy-AI-Passport.bin

# B. 之后日常升级：只刷 app
& $py -m esptool --chip esp32c3 -p COM5 -b 921600 write_flash `
    --flash_mode dio --flash_freq 80m --flash_size 8MB `
    0x10000 build\FoloToy-AI-Passport.bin

# 救砖 / 换机：整片 0x0（会清 NVS）
& $py -m esptool --chip esp32c3 -p COM5 -b 921600 write_flash `
    --flash_mode dio --flash_freq 80m --flash_size 8MB `
    0x0 tools\mk_full_bin.py 生成的 full.bin
```

ROM 不进 bin 了：刷完固件屏幕就是 `NO ROM`，用 `tools/digimon_studio.html` 连上、点 ROM 槽的「上传」选 16 KB 镜像。三次上传（V1/V2/V3）后菜单 `ROM` 行就能切。

`python` 要用 IDF 的 venv（`<IDF_PYTHON>`），
系统 py311 里没有 esptool。

刷机前可以先备份当前 app 分区（回滚用）：

```powershell
python -m esptool --chip esp32c3 -p COM5 -b 921600 read_flash 0x10000 0x100000 docs\backup\app_before_x.bin
```

模拟器（无头，可截图，不需要真机）：

```powershell
$node = "<node>
& $node tools\dg_sim_ui.mjs firmware\digimon-passport\FoloToy-AI-Passport-full.bin docs\ui-v2 360
& $node tools\dg_sim_rom.mjs docs\ui-rom\full-sim-norom.bin docs\ui-rom\transfer `
    --rom "firmware\digimon-passport\main\roms\digimon.bin:V1" `
    --rom "firmware\digimon-passport\main\roms\digimonv2.bin:V2"
```

`dg_sim_ui.mjs` 会**先过时钟设置**（不做这一步 ROM 永远停在时钟页），再开菜单、等孵化、按 A 点亮图标，
输出已经旋转成 320×240 的真机视角。
`dg_sim_rom.mjs` 是 ROM 分区的回归：空分区 → NO ROM、错 CRC 被拒、上传、ROMINFO、ROMOUT 回读比对、
ROMBOOT 切槽、ROMDEL 回到 NO ROM，最后打 `ALL ROM CHECKS PASSED`。
它需要**仿真专用构建**（console 走 UART0）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build_sim.ps1
python tools\mk_full_bin.py --build firmware\digimon-passport\build-sim --out docs\ui-rom\full-sim-norom.bin
```

`tools/dg_sim_test.mjs` / `dg_sim_menu2.mjs` / `dg_sim_wake.mjs` 是老的行为回归脚本（它们假设 ROM 已经在跑，
用 `mk_full_bin.py --rom` 生成带 ROM 的镜像再跑）。

## 必踩的坑（都踩过了，别重踩）

1. **BSP 按键事件**：除 PRESS/RELEASE 还会上报 CLICK/DOUBLE/**LONG(500 ms)**；把非 PRESS 当松手会让长按永远到不了 1.2 s
2. **ROM 按键扫描要 ≥120 ms** 才认，短按会被丢；固件把每次按下垫到 260 ms
3. **ADC 阶梯键盘**：三键共用一个 ADC 脚，无法识别同时按下；长按不稳（触点瞬断会串到别的键窗口）
4. **电源键是硬件闩锁，没接 MCU**，固件读不到——不能用它做任何功能
5. **30 px 圆角遮罩**：显示接入层对每次 flush 都套圆角，四角 30 px 内会被切（x>290 且 y<30 的区域才受影响）
6. **整片刷机会清 NVS**（镜像覆盖 0x9000–0xF000），日常升级只刷 0x10000；**分区表变了（v3）还要多刷一次 0x8000**
7. **图标亮态别用实心剪影**：仪表盘会变成一团黑，要用"深底+白色原图"
8. **`lv_color_hex()` 吃的是 RGB888，画布缓冲是 RGB565**：把 `0x6BCA` 这种 565 常量塞给样式，屏幕边框会变亮蓝色（round 56 踩过）
9. **模拟器里看不到 app 日志**：控制台走 USB-Serial-JTAG，仿真只转 UART0（只有 ROM bootloader 的 banner）。
   `DG_SIM_UART0_INPUT` 构建能把 stdio 绑到 UART0，但**只能读能用**——UART 驱动的中断发送路径没被模拟，
   `printf` 出不来；协议回复因此走 `con_printf()`，在 sim 构建里落到 `esp_rom_printf`（轮询直写 FIFO，仿真能看见）
10. 核心状态快照 `dg_state_save/load` 是 `memcpy(dg_cpu)`，ROM 指针要单独保留（已处理；现在是 mmap 指针）
11. **玻璃画布 118 KB 要不到**：pitch 8（256×231）需要 118,272 B，而 LVGL 初始化后最大连续块只有 112 KB，
    `heap_caps_malloc` 直接返回 NULL → 屏幕只剩默认底色（`f7f7f7`）。这就是最后用 pitch 7 的原因，
    别再"顺手调大一点"。想看真实日志可以用 `esp_rom_printf` 打 UART0：仿真只转 UART0，app 的 ESP_LOG 走
    USB-Serial-JTAG，仿真里看不到
12. **图标带不是点阵的第 0/17 行**：它是玻璃上的独立电极区，中间各有 2.4/2.5 格空白；把它当点阵行画会把整块屏算成 18 行
13. 图标 ink 只占槽宽 60%，两侧留白是对齐真机的，别拉满槽宽
14. **`CONFIG_PARTITION_TABLE_CUSTOM` 曾经被注释吞掉**：`sdkconfig.defaults` 里那行被拼在一段 `#` 注释的末尾，
    于是构建一直用 IDF 内置的 `partitions_singleapp.csv`（factory 1 MB、**没有 roms 分区**），
    项目里的 `partitions.csv` 形同虚设。现在 sdkconfig 是 `CUSTOM=y`，改分区表后要确认
    `build/partition_table/partition-table.bin` 里真有 `roms @0x210000 size 0x5f0000`
15. **`idf.py build` 不重建 `FoloToy-AI-Passport-full.bin`**：模拟器会悄悄跑上一次的旧镜像（表现：改了固件，
    仿真里画面一点没变）。合并镜像统一走 `tools/mk_full_bin.py`
16. **ROM 槽头是提交点**：`ROMIN` 先擦写 16 KB 数据、校验 CRC，最后才擦写头扇区。中途掉线 = 槽读成空，
    旧镜像不会被半截数据顶掉。所以**别把 header 提前写**
17. **从游戏回到 NO ROM 必须擦玻璃画布**：`render_lcd()` 一停，画布就冻在上一帧，`NO ROM` 标签会叠在一只死宠物上。
    `ui_rom_state_locked()` 里 `glass_blank()` 就是干这个的
18. **UART0 真机不能用**：`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` 是有原因的——UART0 TX 和背光抢 GPIO21。
    `DG_SIM_UART0_INPUT` 只能在仿真构建里开
19. **跳过时间只能"真跑"**：改 RAM 把 stage/clock 写快没用——ROM 只在**自己的 tick 计数器跨过**
    `next_evolution_at` 时判定进化，把 stage 从 0 直接写成 120 分钟，蛋永远不孵（host 探针 + 仿真双重验证）。
    所以 SKIP 的成本是硬性的：C3 约 337k step/s ≈ **1.26 s / 游戏分钟**，2H ≈ 2.5 分钟
20. **跳完必须 `save_slot()`**：跳过改的是 RAM，自动存档 60 s 才跑一次，而用户看到"卡住"的第一反应是
    拔电重启——不立刻落盘，跳过的两小时会静默丢失（真机上就是这么丢的）
21. **跳过循环的让出粒度**：每 4096 step 让 1 ms 会浪费 95% 的 CPU（2H 要 3 分钟以上），
    现在每 65536 step 让 1 ms 并顺带刷进度，实测 30M 档从 45 s 降到 38 s
22. **`save_ui_settings()` 别夹在关面板和弹 SKIP 卡之间**：NVS commit 有 ~100 ms 的 flash 停顿，
    放在那里会让面板"慢动作收起"；现在挪到跳过之后
23. **进化判定是"阶段计时器精确等于 next"，而且 hours 不是 BCD**（round 61 结案）：
    `0x0596 evolution_check` 把 `M(48-4B)` 与 `M(4C-4F)` 四个 nibble 逐个比，**全等**才进 mode 9；
    跨过那一分钟就永久丢失，ROM 还会把 next 写成 `0xFF`（无下一次的哨兵）。
    **`M(4A-4B)` 和 `M(4E-4F)` 是 8-bit 二进制，只有分钟是 BCD；next 必须读满 4E+4F。**
    老代码两处都错，把 26:23 / 45:09 显示成 20:23 / 13:09，于是"stage 早超 next 却不进化"
    的假象骗了一整轮（还差点去改 ROM 逻辑）。`DUMP` 现在直接给
    `stage=26:23 next=45:09 left=1126min`，`left` 变负数才是真的错过了。
24. **SKIP 的速度已经到顶了，别再试这四种**（全部实测过，30 分钟档都是 37–39 s）：
    `-Og → -O2`（只把单步开销从 8.7 s/5min 降到 6.3 s/5min，收益有限）、
    核心热路径 `IRAM_ATTR`（无变化，还带来一堆 section 冲突 warning）、
    ROM 从 flash mmap 改成拷进 RAM（无变化）、
    `dg_set_osc1_speed()` 放大 OSC1 分频（**反而更慢**：ROM 大半时间在快时钟分支，
    改 `osc1_div` 只会打乱 `osc1_counter` 的累积，16 倍速要跑 4647 万步 vs 原速 538 万步）。
    真正的瓶颈是 `dg_step` 每步约 295 个 C3 周期（函数指针分派 + 大结构体 + 每步 tick 结算），
    要再快只能重写解释器（computed goto / 专用寄存器分配），那是另立项目的事。
    现在的档位开销：5M≈6 s、30M≈38 s、1H≈75 s、2H≈150 s，菜单行里直接标出来了
25. **`con_printf()` 的 `char buf[192]` 会把 STATE 截断**（round 64）：STATE 整行 198 字符，
    正好切在 `age=` 的那个 `a` 上，页面上表现为"读到了但不完整"。DUMP 只有 ~120 字符所以从没暴露。
    已加宽到 384。**以后再加长的协议行，回头核对缓冲区**
26. **编译成功 ≠ 刷进去了**（round 64，最坑的一次）：编译出新固件 `0xD7030`，但刷机命令跑的是
    上一个 `0xD6B00`，于是"STATE/AGES/RAM 全部无响应"排查了一轮。**刷完必须核对
    `Wrote N bytes` 是否等于构建产物的字节数**，对不上就当没刷
27. **`.hex` 要按信封识别**：带 `SAVEBEGIN` 的备份，前 64 字节不是纯 hex
    （`S/V/B/E/G/I/N` 都在 `a-f` 之外），旧判定会当二进制 → 读头 4 字节得 `"SAVE"` → `magic 不对`。
    现在 `looksLikeSaveText()` 先认 `^\s*SAVEBEGIN` 信封，再退回纯 hex
28. **Web Serial 的断开顺序**：先 `reader.cancel()` 再清 `state.port`，读循环会重新
    `getReader()` 挂上，`port.close()` 永久等锁 → 页面失去响应（要关浏览器）。
    必须**先置 `state.connected=false; state.port=null`**，再 cancel（400 ms 兜底）→ releaseLock → close（800 ms 兜底）
29. **串口 `SKIP n` 原本不显示进度卡**：卡片只在菜单路径里 `remove_flag(HIDDEN)`，
    命令路径 `label=NULL` 只改一个看不见的标签 → 两小时档"死"两分半，看着像崩溃。
    现在显示/隐藏都搬进 `rom_skip_minutes()`，两个入口行为一致
30. **别用 PowerShell 处理这个仓库的 UTF-8 文本**：PS 5.1 的 `Get-Content -Raw` 按 ANSI 读，
    配 `-replace` + `WriteAllText` 回写会把整份中文变成乱码且**不可逆**（PC 工具被毁过一次，只能重写）。
    改文本一律用 `edit` 工具或 python（`utf-8` + `surrogateescape` 保字节）
31. **headless 截图的 DPI 会骗人**：同一份 CSS，`--window-size=1500` 截出来侧栏只有 187 px
    （应为 400），因为系统缩放影响了 viewport 的解释。截图只用来核对布局关系，
    **要量真实尺寸用 `--dump-dom` 读 `getBoundingClientRect()`**
32. **体重读错地址**（round 65，真机上"网页 20 / 机器 91"）：`M(68-69)` 是 **种族基准体重**
    （`0C17` 在进化时写一次，Betamon = 20 g），**不是屏幕上那个数**。真机状态页的体重是
    `M(56)` 个位 / `M(57)` 十位（两位 BCD，上限 99 g）。证据是两条独立的实测：
    ① 喂一次食 `M(56)` 1→2（91→92 g）、`M(3E)` 4→5；
    ② 全 256 个 nibble 逐个 +1 重画同一页（`src/probe_causal.c`），只有 `M(56)/M(57)` 和
    `M(5E)/M(5F)` 能改变画面，`M(68)/M(69)` 一次都不出现。
    页面上的第二个数字来自 `M(5E)/M(5F)`（当前 01），**语义未证实**，别当结论用
33. **存档 blob 是 32 位 ABI 布局**：固件 `dg_state_size()` 1304 B，同一份 `digimon_core.c`
    编到 x86_64 上是 1312 B（`rom` 指针 4→8 字节 + `dg_sound` 里 `double` 的 8 字节对齐）。
    主机要复现真机状态，`memcpy` 要按 **+4 字节**平移（`src/probe_page.c` 里是暴力搜出来的：
    拿 `M(0B)/M(1A)/M(56)/M(68)` 当指纹，偏移 0..12 逐个试）
34. **`forEach` 里写 `continue` 是语法错误，整段 `<script>` 直接不执行**：症状极具欺骗性 ——
    页面照常渲染、按钮全无反应、`?tab=` 不跳转、`?demo=1` 不载入，看着像"初始化失败"。
    改完 JS 永远先跑 `node --check`（`tools/` 里的提取脚本见下方"改 PC 工具三件套"）
35. **`ResizeObserver` 盯 `body` + 回调里改 SVG 尺寸 = 自激死循环**：主线程被锁死，
    表现为"卡在别的标签页"。观察目标要收窄到真正要量的那个容器（`#evoFlow`），
    并在回调里比较尺寸、没变就别重画

## 待办（按优先级）

1. **esptool-js 网页刷机**：同一个串口既要跑日志又要跑握手，得先断日志通道再交给 `ESPLoader`，
   刷完挂回来。计划代码在 `docs/studio-tool.md` 末尾
2. **V2 / V3 的进化表**：养成页现在只有 Ver.1（`care_rules_v1.json` 压成的 12 条规则，全部校验过
   14112 行）。`care_rules_v2.json` **还不存在**，得先用 `tools/gen_evo_rules.py` 对 V2/V3 的 ROM
   重跑 14112 组判定再压缩；配图树状图的卡片可以直接从 `docs/pdf_assets/page_img_03.jpg`（V2）、
   `page_img_06.jpg`（V5）里按同样的连通域方法裁（脚本见 `tools/cut_evo_cards.py`）
3. **点阵可读性**：pitch 7 → 点阵 224×112（比 pitch 8 小 22%）。嫌小只能改回 pitch 8，
   但要先把 LVGL 的缓冲让出来（`s_canvas_buf` 挪到 LVGL 初始化之前，或玻璃画布降 8-bit 索引色）
4. **ROM 上传提速**：现在是 hex 行（16 KB → 33 KB 文本），可以考虑 base64 分块或 XMODEM-1K，省一半时间
5. 可选：用 `ref/vpet_ui_assets/` 的 5 张边框（540×720 竖版、挖孔 400×200）横向重切成 224×203 孔的版本，
   做成真正的塑料外壳贴图；现在的三层边框是纯色块

## 工程约定

- 主分支模型：`main` 只与上游同步，功能改动走 `feature/*`（本项目是 fork，实际一直在主工作区改）
- 每次改动后跑 `test_core`（`src/` 里 `gcc -O2 -o test_core.exe test_core.c digimon_core.c`），必须 `ALL CHECKPOINTS MATCH`
  （本机 gcc 在 `gcc`）
- 碰 ROM 时间 / 进化 / 存档相关的代码，另外跑 `src/evo_probe.exe <rom.bin>`：A 组（停在 next−2）
  必须 `EVOLVED`，B 组（停在 next+5）必须 `NO EVOLUTION`。这是进化语义的回归，比看日志可靠
- **stage / next 的 hour 是 8-bit 二进制，只有分钟是 BCD**；`next` 占 4C–4F 四个 nibble。
  解码只允许走 `ram_u8()` / `ram_bcd()` 这两个入口，别再手搓
- 真机验证优先看串口日志：`alive frame=… instr=…`、`icons=XX mode=X`、`saved/restored V1 (seq N)`、`screen on/off`、
  `menu open/closed`、`rom store ready: N/8 slots`、`ROMIN slot N: V1, 16384 B, crc …`
- **刷机后核对字节数**：`Wrote N bytes` 必须等于 `(Get-Item bin).Length`，对不上就是刷了旧文件（坑 26）
- **改 PC 工具的三件套**：
  1. 语法：抽出 `<script>` 用 `node --check`（python 提取，别用 PowerShell 读中文）
  2. 结构：`$("id")` 引用的元素必须在 HTML 里存在；各页 `<div>` 开闭数量要配平
  3. 渲染：Edge headless 截图 `file:///…/digimon_studio.html?demo=1&tab=rom|care|debug`，
     加 `&log=1` 展开日志栏。**截图只核对布局关系，不量像素**（坑 31）
- 改文本（`.md` / `.c` / `.html`）用 `edit` 工具或 python；**PowerShell 的 `-replace` 会毁掉中文**（坑 30）
- 固件改动后固件三件套：`test_core`（必须 ALL CHECKPOINTS MATCH）→ 编译 → 刷机核对字节数 →
  串口 `STATE` + `DUMP` 各打一次，确认新命令真的在跑

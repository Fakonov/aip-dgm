# 交接单（新会话先读这一页）

> 项目：`<home>\digimon-passport`
> 目标机：FoloToy AI Passport（ESP32-C3，240×320 面板已按横屏使用）
> 详细历史：`PROGRESS.md`（round 1–65，含每次真机日志）；设计稿：`docs/icons/DESIGN.md`
> **复盘与方法论**：`docs/RETROSPECTIVE.md`（十条可复用方法、工具清单、坑的分类学、开工清单）
> **v2.0.1 全项目审计**：`docs/AUDIT-2.0.1.md`（flash/RAM/每帧分账、实测优化清单、可复现实验）；
> **可复用方法论**：`docs/METHODOLOGY.md`（性能审计七步法、度量门禁矩阵、反模式清单）
> PC 工具：`tools/digimon_studio.html`（说明见 `docs/studio-tool.md`）
> **当前指纹**：固件 `0xFC500` = 1,033,472 B，SHA256 `94d32e38…`
> （round 104：石墙边界横缝改 ±3 px 波浪 + 自动存档 300 s→**60 s**；
> 2026-11-09 00:3x 已刷 COM5，`Wrote 1033472` + `Hash of data verified`，
> 真机日志 `I (60694) saved digimon (1304 B)` = 60.7 s 准时落盘）；
> round 99 的 #20 A+C 提前收手（真机 held 46 ms）仍在其中。
> PC 工具 407,641 B（v3.3，文案傻瓜化 + 调试按钮收进高级模式）；v8 里程碑 `0x10FA60` = 1,112,672 B
> **已打里程碑 `milestones/2026-11-08_dual-theme/`，用户实机确认**；
> COM4 仍是 v6 的 `0xD84B0`，用户明确不用管。
> **round 114（2026-11-10 12:1x，已刷 COM5）**：审计后的首批优化落地 —— 固件 **`0xD84E0` = 885,984 B**
> （−147,552 B / −14.3 %）：砖墙 ARGB8888→A8 掩码、LVGL 裁 30 项（保留 THEME_DEFAULT）、
> 去两处 `sscanf`、`fill_audio` 定点、`SAVE_BUF_SIZE` 4 KB→1.4 KB、删 `dg_link_session.c`。
> 验证：`test_core` 逐位一致、`dg_sim_rom.mjs` ALL PASS、砖墙区 0 像素差异；
> 刷后 `age=122710` 存档完好、五个 ROM 槽全在。回滚：`tools/audit/pre-opt-0xFC540.bin`。
> **round 115（2026-11-11 00:1x，审计，未改固件未刷机）**：第二轮审计 → `docs/AUDIT-R2.md`。
> 三条先看：① `parse_num()` 有符号饱和 = **CRC ≥ 0x80000000 的 ROM 传不上去**（P0，1/2 概率）；
> ② LVGL 11 种像素格式只用 3 种，成组关 **−101,552 B**、仿真 0 差异（P0，改 Kconfig 即可）；
> ③ `sdkconfig.defaults` 的 8 MB **从未生效**，从零构建仍失败（脚本子串判重跳过），
> 且 `-O2`/LV 池 40 KB/`THEME_SIMPLE` 只在生成的 `sdkconfig` 里 —— 清空重建只有 839,456 B + `-Og`。
> 另有 `SAVEDEL` 被 `SAVE` 吃掉、`dg_op_tbl` 越界、`dg_state_load` 死循环三处待修；
> 时钟环批量化补丁（2.08×，bit-exact）在 `tools/audit/r2/clock-batch.patch`。
> **整片备份（2026-11-11 20:5x，COM5 读回）**：`docs/backup/flash-2026-11-11-round114-full8m.bin`
> （8,388,608 B，SHA256 `6C7DB698…`）。校验：app 段与 `build/FoloToy-AI-Passport.bin` 逐字节相同
> （sha256 `6b2dd044…`）→ 确认设备跑的就是 round 114 那版；NVS 16,905/24,576 B 有数据；
> ROM 槽 0–4 全在且 data CRC / 头 CRC 全对（V1 / V2 / V3 / **V1ES `0xBD56E590`** / V4J `0x27DF4FD6`）。
> 恢复：`esptool --chip esp32c3 -p COM5 -b 921600 write_flash 0x0 docs\backup\flash-…-full8m.bin`。
> **当前指纹（round 116，2026-11-11 21:0x，已刷 COM5）**：**v2.1.0**，固件 **`0xB6990` = 747,920 B**
> （−138,064 B / −15.6 % vs round 114；−27.6 % vs v2.0.1）。内容：P0 四修（`parse_unum`、
> 取指 `& 0x0FFF`、`SAVEDEL` 顺序、`dg_state_load` 夹取）+ 时钟环批量化 + 体积三项
> （关 8 种像素格式、LVGL 换 libc malloc、backdrop I8）+ `sdkconfig.defaults` 补全。
> 验证：`test_core` 逐位一致、仿真 11 张 0 差异、`dg_sim_rom` 全过（含坏 CRC 仍拒 + V1ES 高 CRC 通过）、
> 真机 ROMIN V1ES `ROMOK 5 16384 bd56e590`、`SAVEDEL 7 → ok`、SKIP 3 从 4.15 s 降到 2.93 s（1.42×）、
> 50.00 fps、存档 `age` 连续、5 个 ROM 槽全在。从零构建 747,840 B（差 80 B / 0.011 %）。
> **版本号**：`firmware/digimon-passport/version.txt` = `2.1.0`，`esp_app_desc.version` 已从 `"1"`
> 变成 `2.1.0`（sha256 `D7C87516…`）。⚠️ `version.txt` 不是 ninja 依赖——**改完必须 `idf.py reconfigure`
> 再 build**，否则 bin 逐字节不变、版本号根本没进去。changelog 见 `docs/community/changelog-{zh,en}.md`。
> **2026-11-11 22:0x：整片擦除后全新刷入 v2.1.0**（`erase_flash` + bootloader/分区表/app 三件），
> **按用户要求不保留存档与 ROM**。刷后实测：`ROMINFO` 8 槽全空 `ROMCUR -1`、`AGES` 全 0、
> `STATE` 全 0（age=0）、`RAM` 全 0；设备 flash 回读 `esp_app_desc.version="2.1.0"`（magic `0xABCD5432`）。
> 擦除前的一切（5 个 ROM + 34 h 存档 + 设置）**只剩** `docs/backup/flash-2026-11-11-round114-full8m.bin`；
> 要 ROM 回来：从 `桌面\dsh sth\tama dg roms\digimon roms\` 用工具重传，或整片回滚那份备份。
> **round 94/95/96 改动**（2026-11-08，已刷 COM5）：SKIP 加 12H/24H、背光默认 70 %、存档 60 s→5 min（**round 104 已按用户要求改回 60 s**）、
> 石墙 tile 192→128（密度按实机反馈三次调，定稿 `ROWS=3` / 每行一条缝 / 线宽 5 px；
> round 103 把 tile 边界那条横缝改成 ±3 px 波浪并用 wrap 画法补满 5 px，其余横缝/竖缝没动）、墨色 `#1B2417`→`#2E382C`、删 592 项死色表；
> **菜单一级 9 → 5 行**（TIME / ROM / BATTLE / APPEAR / POWER，后三组共用二级面板）；
> **时钟界面长按 A 1.2 s = 注入 A+C（进入调时间）**，round 99 起松手时机改成"ROM 的 `sub` 一变就放"、
> 3.2 s 只做兜底；静音兜底改为加载存档时清一次；网页端加「高级模式」。
> 详见 `PROGRESS.md` round 94/95/96 与 `docs/NEXT-SESSION.md` §4。
> 这一版 = v7（离线卡片对战）**加上双主题**：`APPEAR` 二级里的 `STYLE` 在
> **A CLASSIC**（里程碑原样：实色壳 + 橄榄绿点阵）与
> **C RETAIL**（石墙砖纹机身 + 192×183 全分辨率海景 + 黑点阵 + 压暗 6 % 网格）之间切换。
> 外壳色从 15 种精简到 **8 种**（BROWN/GREY/RED/GREEN/BLUE/YELLOW/WHITE/DARKGREY），
> `SHELL` 行选；控制台 `STYLE 0|1` / `SHELLSET <索引|名字>`。LVGL 池 32 → 40 KB。
> **对战功能已结案；双机无线（G3）目标冻结。**
> 旧指纹：固件 `0xD7080` = 880,768 B（2026-11-04 20:35，v1.0 发布，已刷真机）；PC 工具 368,561 B（内嵌
> V1/V2/V3/V4 四套进化表 + 56 张说明书画片，单文件无依赖；按 ROM 名/CRC 自动切表）
> **交付包**：`...\aip-dgm\release\aip-dgm-v2.0\`（v2.0 本地完整包，28 项 9.3 MB）
> （app `0x10000` / 分区表 / **整片镜像 `0x0` ×2**（空 ROM 区、预置 V1/V2/V3）/ 工具 / 文档 /
> `说明.txt` / `SHA256SUMS.txt`（27 项）/ `镜像\` 三个官方 ROM + 校验）
> 公开仓库树：`...\release\aip-dgm-v2.0-github\`（187 项 2.5 MB，不含 ROM 与扫描素材，
> 控制台为无图精简版 120,169 B，`SHA256SUMS.txt` 186 项）
> **✅ v2.0 已发布**：提交 `69a09e3` + tag `v2.0.0` 已推上
> <https://github.com/Fakonov/aip-dgm>，Release 带两个资产（app bin 与 source-and-tools.zip）；
> ROM 与带图控制台不进 Release。推之前 `rebase` 过远端那次网页提交 `fa38a54`（README 的 V1 → V1 V2 V3）。
> 本机 git 需要 `-c http.proxy=http://127.0.0.1:2000 -c http.version=HTTP/1.1` 才连得上
> **下一段会话从这里开始**：docs/NEXT-SESSION.md（待办清单 + 硬约束，v9 之后的工作）
> **里程碑快照**：
> `milestones/2026-11-02_vpet-glass-darkgrey/`（v1：玻璃版式 + 深灰外壳）
> `milestones/2026-11-02_menu-ux-v2/`（v2：菜单重做 / SHELL 死机修复 / 熄屏与长按语义）
> `milestones/2026-11-02_rom-partition/`（v3：ROM 与固件分离 + NO ROM + 网页端上传）
> `milestones/2026-11-03_evo-fixed-menu/`（v4：进化解码结案 + 菜单 7 行 / ROM 二级槽位）
> `milestones/2026-11-04_v1.0-release/`（v5：**v1.0 发布定稿**，与 `release/aip-dgm-v1.0/` 逐字节一致）
> `milestones/2026-11-07_wired-battle/`（v6：**有线对战定稿**，`0xD84B0` = 885,936 B。
> 删掉 ESP-NOW 无线层、网页第 4 页对战（真机发起/电脑发起 + 图选对手 + 打完提示存档）、
> 静音锁存兜底、`SAVE` 与 1.5 s 落盘、断开不复位）
> `milestones/2026-11-07_offline-card-battle/`（v7：**离线卡片对战定稿**，`0xDA3A0` = 893,856 B。
> 菜单第 8 行 `BATTLE` + 8 槽卡片 NVS + 自动走位；网页第 4 页拆「离线卡片 / 电脑中继」；
> 构建脚本不再依赖 `export.ps1`）
> `milestones/2026-11-08_dual-theme/`（v8：**双主题定稿**，`0x10FA60` = 1,112,672 B。
> 菜单 `STYLE` 在 A（实色+绿玻璃）/ C（石墙+背景）之间切；配色 15 → 8；
> LVGL 池 40 KB；`dg_backdrop.h`(70 KB) + `dg_brick.h`(147 KB) 两个生成资源。
> 2026-11-08 用户实机确认）
> `milestones/2026-11-09_brick-seam-save60/`（v9：**菜单 5 行 + A+C + 石墙缝 + 存档 1 min**，
> `0xFC500` = 1,033,472 B，SHA256 `94d32e38…`。菜单 9→5 行 + 三个二级面板；
> 时钟页长按 A 注入 A+C（`sub` 一变就松手，真机 46 ms）；石墙 tile 128×128、
> 边界横缝改 ±3 px 波浪；自动存档 300 s→60 s；#15 USB 开关整块删除。
> **这是当前线上版本，2026-11-09 已刷 COM5 并经用户实机确认**）
> ⚠️ **v4 是 2026-11-03 23:1x 打的包**，之后 round 63/64 又改了固件（STATE/AGES/RAM/SAVEDEL、
> con_printf 缓冲区）和 PC 工具（存档直连、右侧日志栏、一堆修复）。
> **要最新交付物看 `test\20261104-2035\`，别从 v4 里拿**
> 上一轮会话日志（只用于考古，别当记忆用）：
> 上一轮会话日志（内部文件，不随公开仓库分发）
> （zstd 分帧，逐帧 `zstdDecompressSync` 后拼起来就是完整 JSONL，2021 帧 / 7.3 MB / 约 4.0 M 字符）

---

## 现在是什么状态（可直接继续）

| 项 | 状态 |
|---|---|
| 核心 | BrickEmuPy 的 E0C6200 翻成 C99，50 万步逐位对齐（`test_core` 全绿，`pc_trace_sha256=f42d1802…`） |
| **ROM** | **不再编译进固件**。ROM 是独立文件，走 `roms` 数据分区（`0x210000`，6.1 MB，8 槽 × 32 KB）；槽 = 4 KB 头扇区 + 16 KB 镜像，头最后写 = 提交点。**没传 ROM 时玻璃显示 `NO ROM`**，模拟器根本不 step |
| ROM 上传 | USB 控制台协议 `ROMIN/ROMOUT/ROMINFO/ROMDEL/ROMBOOT`，网页工具 `tools/digimon_studio.html` 一键上传（支持一次多选批量铺槽，CRC 校验不过就 `ROMBAD`，旧镜像保持可用） |
| **V4** | 可直接跑（round 67）：`DigimonV4J.bin` 16 KB / CRC `27df4fd6`，0x059D 与 0x0D00 跟 V2/V3 逐字相同；仿真 8 张截图 + 真机 slot 4 已跑过 3 小时无异常。网页端 V4 规则表已补（round 68，0x0D10 与 V2/V3 逐字相同，物种表/卡片用 page_img_05）。里程碑 v5 发布包没有带 V4（ROM 不进版本库） |
| **联机对战** | **只剩有线（PC 中继）**。round 91 把 ESP-NOW 无线层整块移除：`dg_link_esp.c/h` 移到 `docs/archive/wireless-espnow/`，`CMakeLists.txt` 去掉 `dg_link_esp.c` 与 `esp_wifi/esp_netif/esp_event` 依赖，`LINKWIFI`/`LINKINFO` 命令删除。固件 **1,458,112 B → 885,936 B**（Wi-Fi 协议栈不再链接，省 572 KB）。链路保留 `dg_link.c` 波形层 + USB 控制台 `LX`/`LINKFRAME`，由电脑转发。无线那套见下方「已移除」一节（留作历史）。**⛔ 2026-11-07 用户明确：双机无线互联（G3）目标暂停，不要再往无线电方向做。** |
| **离线对战（G2）** | round 92 新增，**不接电脑也不接第二台机器**：菜单第 8 行 `BATTLE` → 二级面板选 8 张卡之一 → 固件自己按键走进对战界面，卡片应答 ROM 的两帧。卡存 NVS（`digicard` / `c0..c7`），控制台 `CARDW/CARDL/CARDDEL/CARDGO/CARDST/CARDOFF`。主机 `src/card_test.c` PASS；**真机 COM5 两条路径都 PASS**（控制台 `bouts 1→2`、面板按 B `bouts 2→3`）。`0xDA3A0` 已刷 COM5，COM4 还是 v6 |
| 屏幕 | **横屏 320×240**：`bsp_pins.h` 的 `BSP_LCD_ROTATED_90=1`，BSP 把 LVGL 逻辑尺寸设 320×240、`swap_xy+mirror_x`，旋转走 MADCTL（不花 CPU） |
| 版式 | **按真机玻璃实测**（`ref/vpet_v1v2v3/屏幕版式说明.md`）：一块玻璃三条带，全部 32 列宽；上/下带各 4 格，中间 32×16 点阵，带与点阵间留 2.4/2.5 格空白；整块 32 : 28.7 |
| 尺寸 | pitch **7**（整数倍）→ 玻璃 224×203，居中于 (48,18)；点阵 224×112；图标槽 56×28（8 列/枚，ink 占槽宽 60%） |
| 调色 | 玻璃三色：底 `#93A06B`、格间 `#A2AF7B`、点亮 `#1B2417`；`DG_LCD_TONE` 0/1/2 可切（`docs/icons/lcd_tone_compare.png`） |
| 图标 | 8 个槽位贴图（56×28，背景烤成玻璃色）**直接 blit 进玻璃画布**，不再是独立 LVGL 控件；灭=橄榄描边，亮=深色底板+白色原图 |
| 外壳配色 | 菜单 `SHELL` 行循环 **8 种**（BROWN/GREY/RED/GREEN/BLUE/YELLOW/WHITE/**DARKGREY 默认**），存 NVS；只换外壳/HUD 墨色，玻璃三种色调不动 |
| 边框 | 三层：塑料外框（1px 高光）+ 深色凹槽 + 玻璃；玻璃整体上抬 6 px（y 12..215）给 HUD 留横带 |
| HUD | **底部右侧一行**：`V1`（或 `--`）+ `82%`（14px），右边缘停在 x=304（再往右被 30 px 圆角遮罩切）；电池图标已删 |
| 段映射 | mode 2..8 → 状态/病毒/药/对战/便便/灯泡/创可贴；`(0,0)` = 呼叫，孵化后常亮 |
| 菜单 | **游戏里按住 OK 满 3 s 当场弹出**（不用等松手），半透明面板 300×200（`LV_OPA_90` + 阴影），黄底光标，**一级 5 行**：**TIME** / **ROM** / **BATTLE** / **APPEAR** / **POWER**（行高 24 / 间距 30 / 起点 36；没有 EXIT，C 直接关；SAVE OUT/IN 已挪到 PC 端）。**TIME / APPEAR / POWER 打开同一个二级面板**（最多 3 行 × 2 列）：`A` 移动、`B` 改值、`C` 返回一级 |
| 菜单二级 · TIME | `SKIP TIME`（OFF/5M/30M/1H/2H/12H/24H，**仍是一级面板关闭那一刻生效**）+ `PLAY TIME`（`1H13M`，**B 两次清零**，第一下只显示 `RESET? B`，移动光标即取消） |
| 菜单二级 · APPEAR | `STYLE`（A CLASSIC ⇄ C RETAIL）+ `SHELL`（8 色循环） |
| 菜单二级 · POWER | `BRIGHT`（20–100 %）+ `VOLUME`（0–100 %）+ `AUTO OFF`（NEVER/15S/30S/1M/2M/5M） |
| SKIP 执行 | 档位 OFF/5M/30M/1H/2H/12H/24H，选好**关一级菜单那一刻生效**（一次性，用完自动复位 OFF）。实现是**真跑游戏时间**（见坑 19），玻璃上弹 `SKIP 2H  45% / C=STOP` 卡片，C 可中途取消 |
| PLAY TIME 存档 | 原名 AGE：养育总时间（从孵化算起，含跳过的时间），`1H13M` 格式，跟着 ROM 槽存 NVS `a0..a7` |
| 菜单 ROM 行 | 右侧 `V1 >`，**按 B 进二级槽位表**：8 个槽一行一个，左列 `序号 *名字`（`*` = 当前槽），右列该槽存档的 PLAY TIME（空槽压暗、`--`）。`A` 移动、`B` 启动、`C` 返回一级。进二级时一级面板要先 `HIDDEN`，否则半透明叠半透明全是残影 |
| 菜单重置存档 | 二级列表最后一行红字 `RESET SAVE <当前 ROM>`，`B` 三道门：`SURE?` → `REALLY?` → 擦 `s%d`+`a%d`；重置的就是在跑的那只时还会 `dg_reset()` 回蛋（ROM 自己清 RAM，等于冷启动），**ROM 镜像不动**。作用于**当前激活槽**而非光标槽——只有一个 `A` 键，跟随光标必然指到最下面那个槽 |
| 菜单按键 | **A=下一行　B=改值/进二级　C=关闭（二级里是返回）**；NVS 只在关菜单时提交一次。**时钟界面（`mode==1`）长按 A 1.2 s** = 注入 A+C，ROM 的 `sub` 一变就松手（3.2 s 只是兜底，round 99），进入调时间（实机 A+C 的等价操作），见「声音」一节 |
| 熄屏 | `AUTO OFF` 到点关背光；**任何键只负责唤醒**（按下+松开一起吞，`s_wake_mask`），不会顺带开菜单 |
| 存档 | 每 ROM 一个 NVS 槽 `s0..s7`（magic+ROM CRC+状态 CRC+序号），**60 s** 滚动存（round 94 曾改 5 min，round 104 按用户要求改回 1 min）；PC 端只做「设备 ↔ 文件」直连，串口命令 `SAVEOUT`/`SAVEIN`/`SAVEDEL`/`PET` |
| PC 工具 | `tools/digimon_studio.html` **v3.3**（407,641 B 单文件，内嵌 **V1/V2/V3/V4 四套进化表 + 56 张说明书画片**）：常驻顶栏 + 四页——①ROM/存档（8 槽卡带上 PLAY TIME、上传·读出·启动·擦除；存档**只做「导出到文件 / 从文件导入」两条直连，无浏览器缓存**；另有两段确认的重置）②养成（当前数码兽、SKIP 四档 + 自定义分钟（1–10080）、**配图树状图**进化路线（三栏卡片 + 曲线，命中橙线点亮）、**6 项**养育数据 + 其余折叠、**按当前 ROM 自动切 V1/V2/V3/V4 全表** + 官方路线图）③调试（16×16 RAM、**连接自检**、原始命令、CRC）。**CONSOLE 在右侧栏**（400 px，顶栏「日志」开合，**不随连接自动展开**，用 `?log=1` 或点按钮）。**④对战 = 两种模式**（round 92）：**离线卡片**（默认，卡片架 8 格 + 图鉴选对手 + `CARDW`/`CARDDEL`/`CARDGO`，**拔线可打**，固件自己走位、网页只轮询 `CARDST`）与**电脑中继**（round 91 的老玩法：真机发起/电脑发起 + `LINKFRAME` 喂帧）。打完自动检测 `win`/`bouts` 变化并提示「立即存档」。深链 `#battle`/`#care`/`#debug`/`#rom`。v3.3 起**所有报错都是人话**（`ROMBAD` → 「这个文件不是 16 KB 的 ROM，或者已经损坏」等），「刷新 AGES / 读状态 / 删除这一格」这类调试按钮只在高级模式出现。旧版留 `digimon_studio_v2_backup.html` / `digimon_studio_v3_backup3.html` |
| 控制台命令 | `ROMINFO/ROMIN/ROMOUT/ROMDEL/ROMBOOT` + `SAVEOUT/SAVEIN/SAVEDEL` + `STATE`（一行全养育数据，含 `weight` 当前体重与 `basew` 基准体重）`AGES`（8 槽 PLAY TIME）`RAM`（0x00–0xFF）`DUMP/PET/SKIP <分钟>` + **联机**：`KEY <mask> <ms>`（4=A 2=B 1=C）/ `LINKHELLO` / `LINKLOG 0\|1` / `LINKFRAME <hex>` / `LINKSTAT` + **调试**：`RAMW <addr> <val>`（十六进制，急救用，见坑 41）/ `SND`（`active/freq/amp/m17`）/ `SNDON`（清 ROM 静音锁存 `M(0x17)` bit3，**固件现在每帧兜底，正常用不到**）/ `SAVE`（立刻落盘，网页断开前会调）+ **离线卡片**：`CARDL` / `CARDW <n> <slot:hex> <effort> <ver> [name]` / `CARDDEL <n>` / `CARDGO <n>` / `CARDST` / `CARDOFF`（详见「离线卡片对战」一节） |

## 离线卡片对战（round 92，G2 / 路线图 M3）

**不用电脑、不用第二台机器、不用无线电。** 对手是存在 NVS 里的"卡片"——就是 D-Link 那
12 bit（species slot + effort + version），固件替它应答 ROM 发出来的两帧。

### 怎么玩

1. 长按 OK 开设置 → `A` 走到第 4 行 **`BATTLE`**（值显示当前选中的卡，如 `1 CARDTEST`）
2. `B` 进二级面板 `BATTLE CARDS  B=GO`：8 行，左 `序号 名字`，右 `slot/effort V版本`
3. `A` 选卡 → `B` 开打：面板关闭，**固件自己按键**走到对战图标、进界面、按 B 发起
4. ROM 播 26 s 动画 → 结果页 → 回主屏，`bouts` +1（`win` 看胜负）

走位失败会打日志说原因（`no ROM loaded` / `injured (M(0x05) bit0)` / `out of steps`），
不会一直瞎按键。睡着（`M(0x33)`≠0）时固件自己清掉再发起。

**用网页端就不用记这些**：打开 `tools/digimon_studio.html` → 连接设备 → 对战页默认就是
「离线卡片」——点一格卡片位、点一只对手、改名字/effort、按「保存并开打」，剩下的全自动
（固件走位 + 回帧，网页只轮询 `CARDST`，一帧都不发）。拔掉数据线以后卡片仍在设备里，
用菜单第 4 行 `BATTLE` 也能打。

### 控制台命令（`tools/dg_card_selftest.py` 用的就是这些）

| 命令 | 作用 |
|---|---|
| `CARDL` | 列 8 张：`CARD n slot= effort= ver= name=` 或 `CARD n EMPTY`，以 `CARDEND` 结尾 |
| `CARDW n slot effort ver [name]` | 写第 n 张。**slot 是十六进制**（`A` = 10），effort 0–4，ver 0–3；名字最多 10 字符 |
| `CARDDEL n` | 擦掉第 n 张 |
| `CARDGO n` | 武装第 n 张 + 启动自动走位（等价于在面板里按 B） |
| `CARDST` | `CARD armed= state= frames= replies= walk= picker= cards=` |
| `CARDOFF` | 解除武装并停掉走位 |

`LINKSTAT` 末尾多了 `card=`（`IDLE`/`ARMED`/`CARD`/`VERDICT`/`DONE`）。

### 实现要点（改之前先读）

- `main/dg_cards.{c,h}`：NVS 卡存储 + 应答器。每张 18 B：`'DGC1'|slot|effort|version|name[10]`，
  namespace `digicard`，key `c0..c7`。槽位/档位越界的卡直接跳过，不会把脏数据当对手。
- **应答顺序照 `src/link_test.c`**（M1 验收过的那份）：帧 1 → 立刻回自己的卡；
  帧 2 → **延迟 50 ms** 回反判定（`mine = verdict==1 ? 2 : 1`，高 nibble 放自己的 version）。
  早了 ROM 会丢帧重发自己的 A2。
- **位序**：`bitrev8()` + `wire_frame(d0,d1) = bitrev(d0) | bitrev(d1)<<8`，因为 CON_DGM 是
  MSB-first 移位寄存器。`dglink_start_rx()` 内部还会再交换一次字节，别重复换。
- 卡片只在 armed 时发帧，`CARDOFF` / 打完自动 disarm → **PC 中继那条路完全不受影响**。
- 菜单第 8 行让面板从 7 行变 8 行：行高 20 / 间距 21，起点 y=20（最后一行到 187，仍在 200 内）。

### 已知限制

- **`bouts` 是 4 bit，15 就满**；满了以后只能靠 `win` 看出变化。
  （只有观测证据：ROM 侧位宽 / 饱和还是回绕 / 是否污染相邻字节都还没查，
  待办见 `docs/NEXT-SESSION.md` **#18**）
- 卡里的 `version` 决定回帧高 nibble，不影响"谁赢"——胜负永远是**发起方 ROM 单方面算**。
- `injured` / 能量见底照样挡住对战，这是 ROM 行为，不是卡片模式的 bug（见下面已知问题 1、4）。
- 网页端 `digimon_studio.html` **有卡片页了**（round 92 三轮）：第 4 页默认就是「离线卡片」，
  卡片架 / 图鉴 / `CARDW` / `CARDGO` 全在界面上，不用再敲命令。详细用法见
  `docs/studio-tool.md` 的 ④ 对战。

### 验证状态（2026-11-07）

| 项 | 结果 |
|---|---|
| 主机 `src/card_test.c` | **PASS**：`帧1 3FC0(03) → 回 A35C(3A=A/3)`、`帧2 BF40(02) → 回 7788(11=V2/verdict1)`、`bouts 0→1` |
| 固件 | `BUILD_EXIT=0`，`0xDA3A0` = 893,856 B |
| 仿真 | `CARDW/CARDL` 回读正确、菜单显示 `BATTLE 1 CARDTEST`、按 B 后日志 `card picker open (sel=0, 8 rows)` |
| 真机 COM5（控制台路径） | **PASS**：`CARDW` → `CARDGO` 自动走位 → `frames=2 replies=2 state=DONE` → `bouts 1→2`，不接电脑、无第二台机器 |
| 真机 COM5（**面板路径**） | **PASS**：长按 OK → A×3 → B → 面板显示 `CARDTEST A/3 V2` → 按 B 全自动打完 → **`bouts 2→3`** 并落盘。日志 `docs/probe_shots/card_panel_watch2.log`，全程无 `rst:`/`Guru`/`assert` |
| 打完的状态 | `CARD armed=0 state=IDLE`（自动 disarm）；`bouts` 已落盘 |

### 踩过的坑（血泪，先读再改面板）

**LVGL 池只有 29,232 B（`CONFIG_LV_MEM_SIZE_KILOBYTES=32`），池满时 `lv_obj_create()` /
`lv_label_create()` 返回 NULL 而不报错。** 第一版卡片面板每行套容器 + 两个 label（26 个控件），
开机建得出来、面板看着正常，**第一次刷新就 panic 重启**（真机按 B 当场重启、进度丢失；仿真里
表现为"按 B 后画面不刷新"）。现在：

- 每行只留 1 个 label，整面板 **10 个控件**；实测 `free=6,760 / biggest=5,772 / used=77%`
- 所有控件建完判空，池满打 `only N of 8 rows fit` 并降级
- `lvgl_pool_log()` 在 `build_ui` 后和每次开面板各打一行（`total/free/biggest/used/frag/max`）
- **进卡片面板前先 `save_slot()`**，任何崩溃都不再吞掉当前 60 s 存档窗口

⚠️ **再加控件之前先看池余量**（77% 已经很高）。Wi-Fi 已从固件删除、堆不紧张，
真要加就把 `CONFIG_LV_MEM_SIZE_KILOBYTES` 提到 40，别硬塞。

---

## 双机联机：现状与接手（round 90 交接，先看这一节）

> **⛔ 2026-11-07：双机无线互联（G3）目标暂停。** 本节及下面「走无线」「双机无线对战怎么玩」
> 两节都是历史资料，别照着往下做；有线（PC 中继）仍在用，离线卡片（上一节）是当前方向。

**固件** `0xD84B0` = 885,936 B（round 91 六轮，删掉 ESP-NOW 之后重编），**两台都已刷并回归通过**。
比上一版 `0x1637E0`（1,455,648 B）瘦了 **572 KB**：Wi-Fi 协议栈不再链接，分区余量 30% → 58%，
运行期也少掉那 ~32 KB 常驻堆。回归结果见下面「round 91 六轮」。
这一版已经包含此前那几项：`esp_reset_reason()` 首行、`SNDON`、`SND m17=`、`SAVE`、
静音锁存兜底（round 96 起改成「加载存档时清一次 + 长按下键手动切换」）、`bouts` 1.5 s 落盘。

### 走数据线（**已验证，推荐当主力**）

```powershell
cd <home>\digimon-passport
python tools\dg_link_pair.py --a COM5 --b COM4 --passive --initiator none --seconds 180
```

- `--passive` = 只听不按：脚本不碰按键，只把两台之间的 16 位帧互相搬运；
  启动时自动 `LINKWIFI 0` 把无线电**静默**（不是 deinit），避免线和空两条路径重复发帧
- 你自己按：**两台都进对战界面 → 一台再按一次 B**
- 脚本窗口打 `[A->B] #1 2BD4 …` / `frames relayed: 4` 就是通了
- 不加 `--passive` 时脚本会自己走按键 + poke，`--initiator a` 那套是自动化验收用的

**round 91 实测**（自动模式，`--seconds 45`）：`A->B 2BD4 / B->A 2BD4 / A->B BF40 / B->A 7F80`，
`frames relayed: 4`，`B win 8→9 bouts 14→15`，`PAIR BATTLE: exchange happened`。
**有线这条现在是主力**，无线先搁置。

> **手动按键前先看两条**（round 91）：
> 1. **关闭串口 = 设备复位**（USB-Serial-JTAG 认 DTR/RTS）。所以脚本一启动，两台其实是在重启，
>    `dg_link_battle.Dev.wait_boot()` 会先等 `STATE` 能应答再动手 —— 别把这段等待当卡死。
> 2. **宠物在睡觉就进不了对战**：`STATE` 的 `sleep` 非 0（凌晨游戏时间里很常见），
>    物理按一下任意键唤醒，或脚本里 `RAMW 33 0`。睡着的宠按 B 只会被当成唤醒，
>    不会进 `sub=2`，看起来就是"停在二级界面按不动"。
>    另加 `energy` 见底 / `injured=1` 同样会把门（见下面已知问题 1、4）。

**round 90 实测**（自动模式）：`A->B 2BD4 / B->A 2BD4 / A->B BF40 / B->A 7F80`，
`A bouts 13→14`、`B bouts 12→13`，`PAIR BATTLE: exchange happened`。

### 走无线 —— ⛔ **round 91 已从固件移除，本节仅存档**

> **ESP-NOW 无线层已经整块拆掉**：`dg_link_esp.c/h` 移到 `docs/archive/wireless-espnow/`，
> `LINKWIFI` / `LINKINFO` 命令删除，CMake 也不再有 `esp_wifi` 依赖。
> `tools/dg_link_wifi.py`、`dg_link_wifi_test.py`、`dg_reboot_battle.py` 一并归档到同一目录。
> 要恢复的话：把两个 .c/.h 移回 `main/`、CMake 加回 `dg_link_esp.c` 和那三个 IDF 组件、
> 把 `digimon_app.c` 里的 `link_radio_task` / `LINKWIFI` / `LINKINFO` / 主循环发帧段补回去。
>
> 下面是移除前的状态，留作参考：

ESP-NOW 旧实现，`M4B BUTTONS-ONLY: PASS` 过了多次。
反复进出图标页会触发二次 `esp_wifi_init()` 崩溃（round 90 修过：无线电整场只 init 一次、
永不 close、`LINKWIFI 0` 只静默不 deinit），但只测了 3 轮进出，没做长时间验证。

### 已知问题（按优先级，round 91 复核）

1. **`energy` 见底时 ROM 不发起对战**。实测 `energy=1` 时两台都进了对战界面、按了 B，
   `LINKINFO wtx=0` —— 一帧都没发。**先喂饱再看别的**，别把它当链路故障。
2. ~~COM5 声音没了~~ **→ round 91 结案：ROM 自带静音锁存 `M(0x17)` bit3**
   （详见下面「声音：`M(0x17)` bit3 是 ROM 的静音锁存」）。`dg_state_save` 是整块
   `memcpy(&g_cpu)`，锁存跟着存档走，所以一个旧存档能让机器**重启后继续哑**。
3. **对战打完约 40 s 自重启**：round 91 **复现不出来，而且先排掉了一个大坑** ——

   > ⚠️ **关串口会复位 —— 已经修掉，网页端实测通过（round 91）。**
   > ESP32-C3 的 USB-Serial-JTAG 把 **DTR/RTS 的某些组合**当成复位序列（esptool 就靠它进下载模式）。
   > 实测四组电平（pyserial，关端口后看 `alive frame` 是否连续）：
   >
   > | DTR | RTS | close 之后 |
   > |---|---|---|
   > | 0 | 0 | 复位 |
   > | 1 | 1 | 复位 |
   > | 0 | 1 | 复位 |
   > | **1** | **0** | **不复位**（esptool 的 idle 组合）|
   >
   > 网页 `disconnect()` 现在先发 `SAVE`，再 `setSignals({dataTerminalReady:true, requestToSend:false})`，
   > 最后才 `port.close()`。
   > **用户实测确认**：网页断开 → 重连，顶栏「运行」胶囊的数字接着往上走，设备没有重启。
   > **命令行脚本仍是"关就复位"**（它们没设这一组电平），所以观测自重启必须全程保持同一条连接
   > （见 `tools/dg_reboot_watch.py`）。
   > 参考项目 TamaGo 的 `close()` 和我们一模一样（cancel → releaseLock → close），它不复位纯粹是因为
   > 电平口径不同 —— **别再去网页里找原因**。

   在串口一直连着的前提下：200 s 完整无线对战（`bouts` 各 +1、两机 `saved 1304 B` 都成功）
   + 之后 170 s，`alive frame` 连续；再补 3 轮重进对战页 + **8 次图标页进出**（round 90
   那个二次 `esp_wifi_init()` 场景），`resets: A=0 B=0`。
   **下一轮如果还复发**，刷 `main/main.c` 里新加的 `esp_reset_reason()` 探针固件
   （`0x163C80` = 1,457,280 B，已编译未刷），开机第一行会打
   `reset reason: N (BROWNOUT|TASK_WDT|PANIC|...)`，一次就能定性。
   另外 `dg_esp_start()` 现在**每开机只允许尝试一次**（`s_tried`），失败后 UI 不再
   每帧重建 `link_radio_task`，从根上掐掉"失败 → 重新 `esp_wifi_init` → 复位"这条路。
4. **受伤进不了对战界面**：`0x037F fan M(0x05),0x3` 把门。物理按**创可贴图标（第 8 个）
   多次**才能治好，别等它自愈。`tools/dg_link_pair.py` 里有个 `RAMW 05 0\nKEY 2 600`
   同批写的绕过法，只用于脚本。

### 声音：`M(0x17)` bit3 是 ROM 的静音锁存（round 91 结案）

现象：COM5 完全不出声，`SND` 常年 `active=0 freq=0.0 amp=1.0000`；COM4 按 A 会出
`active=1 freq=4096.0`。

定位过程（可复现）：

1. `tools/dg_snd_probe.py` 按 A 的同时高频采样 `SND`：COM4 有 `4096.0`，COM5 全程 0
   —— 是 ROM 没驱动蜂鸣器，不是音频驱动/喇叭坏。
2. `SAVEOUT` 导两台存档 → `src/snd_probe.c`（新写的宿主探针）在 PC 上重放：
   COM4 存档按 A 在 ROM `0x09FC` 出声，COM5 存档一声不响 → **纯 ROM 状态问题**。
3. `snd_probe <rom> <save> brute <other-save>` 逐个 nibble 把 COM5 的 RAM 换成 COM4 的值，
   唯一命中：**`M(0x017)` 8 → 0**。清掉立刻响。
4. 真机验证：`RAMW 17 0` 之后按 A，`SND` 立刻 `active=1 freq=4096.0`。
5. ROM 侧证据：`0x0ED6` 起 `LD X,3 / FAN MX,1 / JP NZ` → `LD X,0x17 / XOR MX,8`
   （`0x0EDA`），就是切换这个锁存；`0x09F4`–`0x09FB` 是发声子程序（写 `0xF54`/`0xF74`）。

**恢复方法**：`SNDON`（新命令，清 `M(0x17)` bit3）或直接 `RAMW 17 0`，然后等 60 s
自动存档（或 `SAVEOUT`）把修好的状态落盘。**光清不清存档，下次重启又哑。**

**为什么会被置位**：未证实。ROM 里是 `XOR`（切换）而不是 `SET`，说明它是个开关，
但 V1 没有可见的静音 UI。怀疑与 SKIP TIME / 长时间跳过后的某条事件路径有关（推测）。

`SND` 现在会一起打出 `m17=`，下次一看就知道是不是这个锁存。

**round 96 起：兜底只在加载存档后清一次（不再每帧清）。**
主循环里 `if (s_have_rom && !s_mute_checked) { 清一次 bit3；打 "cleared stale ROM mute latch" }`，
`load_slot()` 会把 `s_mute_checked` 重新置 false，所以换 ROM / 导入存档 / 冷启动都会再检查一次。
**不再每帧清**：`RAMW 17 8` 现在会一直保持（实测 2 s 后 `m17=8`），ROM 自己切换的静音也不会被踩掉。
旧存档里的哑巴状态仍会自动修（重启时打出 `cleared stale ROM mute latch (#N)` 并恢复声音）。
**长按【下】键静音已在 round 96 第三轮按用户要求删除**；要静音仍可用 `SNDON` 或 `RAMW 17 8`。

> **关于「实机 A+C」**（round 96 三轮实测，结论已修正两次）：
> - **A+C 是真的**，但 ROM 要按住约 **3 秒**（700 ms 完全没反应，这是第一轮误判的原因）
> - 行为随状态分流：主屏且**时钟已确认** → 静音切换；主屏但时钟未确认 → `M(0x02)=2` 进时钟设置；
>   **时钟界面 `mode=1` → `sub` 0→1 进调时间**（与 Wikimon 的 Sound / Pause 两条一致）
> - **落地**：`mode==1` 时**长按 A 1.2 s** → 释放 A → 注入 A+C（`AC_HOLD_US`）。
>   **round 99 起不再死按 3.2 s**：注入时记下 `M(0x0A)`，ROM 的 `sub` 一变立刻松手，
>   `AC_INJECT_US`（3.2 s）降级成兜底上限。**真机实测 46 ms**（仿真 42 ms），
>   日志 `A+C released (sub changed, sub 0->1, held 46 ms)`。
> - **触发条件必须是 `mode==1 && sub==0`**（round 99 第五轮踩过）：只判 `mode` 的话，
>   进调时间页（`sub=1`）后长按 A 想快速调小时，1.2 s 会再注入一次、把 A 掐断，
>   而 sub 已不再变化只能等 3.2 s 兜底 —— 体感就是"小时涨 3 个后卡 2 秒"。`sub>=1` 时
>   长按 A 归 ROM 自己管（原生快速递增），固件不要插手
> - 已经在调时间页（`sub=1`）再长按会走兜底：`A+C released (deadline, sub 1->1, held 3212 ms)` —— 正常。
>   其他界面 A 保持普通翻页键，不触发
> - 上面"3 秒"那一条只对**主屏的静音切换**成立；时钟页这一档 ROM 吃得很快。
>   两处别混（round 99 的对照实验才发现 `sub` 在进时钟页的过程里会自己从 3 漂到 0，
>   拿它当"注入生效"的判据时必须先等它稳定）
> - 副作用：长按开始时 A 已经转发给 ROM，时钟页的小时/分钟可能先 +1 一次
> - 控制台仍可用 `KEY 5 <ms>` 直接试（ms ≥ 3200）
> - 调试坑：串口注入按键前先 `KEY 4 400` 唤醒（60 s 自动熄屏时按键只做唤醒）；
>   被弄进时钟设置页时，**`C`×2 再按 `B`** 才回主屏

---

## 已放弃：#15 USB 控制台开关（2026-11-08 评估后整体移除）

> **结论：功能已从固件里删干净，`0xFC310` = 1,032,976 B，与 round 96 的字节数逐字节同尺寸。
> 别再重做**，除非下面三条前提有变化。

当初想做的是"设置里加个 USB 开关，关掉外设省电、给 #4 浅睡排障"。三轮真机实测之后放弃，
原因按重要性排：

1. **省电收益太小**：关得掉的只有 PHY pad + USJ 中断（推算 1–2 mA，占整机 2–4%）；
   APB 时钟关不掉 —— IDF 的 `usb_serial_jtag_connection_monitor.c` 注册的 FreeRTOS tick hook
   每 tick 读 `USB_SERIAL_JTAG.int_raw` 清 SOF，而 `esp_register_freertos_tick_hook` 没有反注册接口。
   对比：背光 100%→70% 就是 6–12 mA，是它的 5–10 倍。
2. **单向门，恢复代价远大于收益**：pad 关掉后
   `phy_enable_pad(true)`、`usb_serial_jtag_ll_reset_register()+phy_set_defaults()`、
   甚至 `esp_restart()`（软件复位）都救不回来；只有**按电源键断电**能让 PHY 回到上电默认。
   详见下面「评估记录」。
3. **对 #4 的价值也没想象中大**：浅睡本来就会让 USB 断开，真要浅睡就得接受"串口掉线"，
   而这件事不需要一个用户可见的开关来做。

**给 #4 的正确做法**（下一轮照这个走）：不要做用户开关，直接在进浅睡前把
`usb_serial_jtag_ll_disable_intr_mask(~0)` + `phy_enable_pad(false)` 做掉，接受"浅睡就断 USB"，
醒了以后不复原（反正也只有断电/复位能复原）；或者干脆评估"浅睡 + USB 全哑"是否可接受。

### 评估记录（三轮真机，都是 COM5）

| 轮 | 关断 | 尝试恢复 | 结果 |
|---|---|---|---|
| 1 | `USB 0` → `saved digimon (1304 B)` → `USB console OFF in 300 ms` → 彻底安静 | 只观察 | 25 s 后重开 COM5 → `PermissionError(13)`；按电源键断电后全部正常 |
| 2 | 关 20 s 后自动走重开路径 | `PERIPH_RCC_ATOMIC + reset_register()` → `phy_set_defaults()` → 恢复 int_ena → 恢复 vprintf | 旧句柄 30 s 零字节、`write timeout`；PnP InstanceId 与关断前**完全相同**（`6&30E20CB2`）；esptool 连不上 |
| 3 | 关 15 s 后自动 `esp_restart()`（照 TamaGo 换模式的手法） | 纯软件复位 | 40 s 内 COM5 一直打不开 → **软件复位也不行** |

**顺带两条会再踩的坑**：

- **拔 USB 线不是断电**：这台机器有电池，线一拔它照样跑，pad 还是关的，插回来也检测不到（`Get-PnpDevice` 里 COM5 变 `Unknown`）。真断电要**长按电源键**（硬件闩锁，坑 4：没接 MCU）。
- **上游 TamaGo 没有关外设**：`firmware/ai-passport-tamago-main` 的 `sdkconfig.defaults:17` 同样是
  `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`；它的"USB 文件传输模式"只是
  `usb_serial_jtag_driver_install()` + 起一个跑 `TG1` 协议的 task（`main/tamago_usb_transfer.c:223`），
  模式切换靠写 NVS + `esp_restart()`。**没有可抄的省电技巧**。

---

## 网页端对战页（round 91 二轮，按用户反馈修的四处）

| 反馈 | 改动 |
|---|---|
| ①「进入对战」按键多按一下，停到便便图标 | 原来固定 `KEY 4` ×5。图标是 `mode 2..8 = 状态/病毒/药/对战/便便/…`，从主屏只要 **4 下**到 `mode=5`。现在**每按一次读一次 `STATE`，命中 `mode=5` 就停**，不再数固定次数 |
| ③「我当发起方」进不去 | 改成**自包含**：先自动 `battleEnter()` 把机器带到对战界面，再检查 `sub`，必要时补一次 B。失败时把 `mode/sub` 和 `sleep/energy/injured` 三项指出来 |
| 打完立刻断开，战绩没保存 | **正是关串口复位吞掉了延迟落盘。** 网页 `disconnect()` 现在先发 `SAVE` 并等 NVS 写完再关；固件 `bouts` 落盘从 4 s 缩到 **1.5 s**；新增 `SAVE` 命令 |
| 对战几次后没声音 | 见上面「round 96 起：兜底只在加载存档后清一次」 |

**还加了一个「④ 补发判定」按钮**：关掉自动回帧时手动补第二帧（判定 = `[version 4B][胜负 4B]`，取上一帧的取反）。

**关于「③ 我当发起方」到底值不值得用**：值得知道的一点是 **胜负由发起方的 ROM 单方面算**
（`docs/LINK-BATTLE-FEASIBILITY.md` §1）。PC 没有 ROM，所以 ③ 能走完动画但战绩不动。

**round 91 二轮实测（新固件 `0x163DC0`）**：

```
② 真机发起 + PC 应答，四帧完整：
  0.2 s  LX 1 2BD4     真机的卡（slot B / effort 2）
         PC  -> 3FC0    我们的卡（slot 3 / effort 0）
  0.5 s  LX 2 7F80     真机的判定（verdict 1）
         PC  -> BF40    取反（verdict 2）
```

> ⚠️ **先更正一个错误结论**：一度写成"单机 `bouts` 不加"。**错。**
> `M(0x99)`（bouts）是 **4 bit，15 = 0xF 就是上限**，而两台机器的 bouts 早就满了，
> 打完当然"看不出变化"。把 COM4 的 `M(0x99)` 降到 14（`RAMW 99 E` + `SAVE`）再打一场：
>
> ```
> bouts 14 -> 15,  win 10 -> 11
> ```
>
> **单机陪练确实会计数。** 只要 bouts 没顶到 15，PC 应答也能让它 +1。
> `M(0x98)`（win）同理，实测每场都变。

**`bouts` 满值这件事本身也有用**：15 意味着"战斗次数"这个进化门槛**早就满足了**，
所以进化卡不卡和战绩无关，要看 care 数据（weight / care / train / hunger / strength / over）。

> **踩到一个隐蔽的坑**：`dg_link_battle.Dev.send()` 在等待窗口里读到的字节进了局部变量
> `got` 然后就丢了，`drain()` 看不到它们。表现是"真机明明发了第二帧（`LINKSTAT romtx=2`）
> 却收不到"。现在 `send()` 把 `got` 并回 `self.buf`。所有基于 `Dev` 的脚本受益。

### 关键改动一览（都在 `digimon_app.c` / `dg_link*.c`）

| 文件 | 改动 |
|---|---|
| `dg_link.c` | `dglink_start_rx()` 入口字节交换（判定帧不再被当卡帧，**这是双机打通的关键**） |
| `dg_link.c` | 回放期间丢弃 ROM 的 P2 边沿；REPLAY 开始/结束双向重置 TX 解码器 |
| `dg_link.c` | `tx_period != 0` 时把对端帧挂起，等 ROM 发完再回放（1 s 兜底） |
| `dg_link_esp.c` | ESP-NOW 广播，一条消息 = 一帧；`dg_esp_set_enabled()` 做静默开关 |
| `digimon_app.c` | 玻璃 **pitch 6**（192×183，画布 70,272 B）+ **LVGL 池 32 KB**，为无线电腾内存 |
| `digimon_app.c` | 无线电在**图标页**由 `link_radio_task` 后台初始化（主循环阻塞会触发看门狗） |
| `digimon_app.c` | `bouts` 变化 1.5 s 后落盘；`SND` / `SNDON` / `SAVE` / `LINKSTAT` 调试命令（`LINKWIFI`/`LINKINFO` 已随无线层删除） |
| `digimon_app.c` | round 91：`SND` 多打 `m17=`；新增 `SNDON` 清 ROM 静音锁存；`s_radio_failed` 让无线电启动失败后不再每帧重建任务 |
| `main.c` | round 91：开机第一行 `reset reason: N (NAME)`（`esp_reset_reason()`），用来抓自重启 |
| `dg_link_esp.c` | round 91：`s_tried` —— `esp_wifi_init()` 每开机只尝试一次，失败也不重试（二次 init 必复位） |
| `tools/dg_link_battle.py` | round 91：`Dev.wait_boot()`，开串口后等 `STATE` 能应答再按键（上一个工具关串口会把机器复位） |

### 别再踩的坑（本轮新踩的）

- **8bpp 调色板画布是死路**：`LV_COLOR_FORMAT_I8` + `lv_canvas_set_palette` 在 LVGL 9 里
  渲染成全黑；回滚时漏改 `heap_caps_malloc()` 大小导致每帧越界、整机卡死。
  改缓冲区类型**必须连分配大小一起改**。
- **`esp_wifi_init` 不能在 UI 主循环里调用**：阻塞 200 ms + 图标页动画 = 看门狗复位。
- **`esp_wifi_init` 只能成功调用一次**：`stop` 之后再 `init` 会复位。要么永不 close，要么别 stop。
- **刷机输出别用管道吞**：`Select-String` 过滤会把 `Wrote N bytes` 吃掉，
  看着像刷了其实没刷（本轮因此多排查一轮）。

---

## 双机无线对战怎么玩 —— ⛔ **round 91 已移除，本节仅存档**

> 无线层已经不在固件里了（见上面「走无线」的说明）。现在的联机只有一条路：
> **两台真机 + 电脑做中继**（`tools/dg_link_pair.py --passive`）。
> 下面这段是 round 88 的原始玩法，保留是为了万一要恢复时知道当初怎么验的。

**不需要电脑、不需要路由器、不需要配网。** 两台刷同一版固件就行。

1. 两台都开机，各自养一只（或已有存档）
2. 按 **A** 翻图标，翻到第 4 个（对战），按 **B** 进入对战界面
3. **再按一次 B** —— 这一下才是"发起"（第一下只是进界面）
4. 对方不用按任何键：它在接收循环里，会自动应答
5. 屏幕走 26 s 动画 → 结果页 → 回主屏，两边 `bouts` 各 +1

**无线电是自动开的**：主循环一看到 `mode==5 && sub==2` 就 `dg_esp_start()`，
日志会打 `link: ESP-NOW auto-start=0 free=26892 largest=18432`。

**宠物受伤了打不了**：`M(0x05)` 的 bit0 是 `injured`，ROM 在 `0x037F` 用
`fan M(0x05),0x3` 把门，非零就永远进不了 `sub=2`。**按治疗图标（创可贴，第 8 个）
多次点，直到 `STATE` 的 `injured` 变 0**，别指望它自己好。

**排查**：
- `LINKINFO` → `up=1 peer=1 wtx=2 wrx=2` 是打通的正常值（2 发 2 收 = 四帧）
- `up=0`：还没进过对战界面（无线电惰性启动）
- `peer=0`：没听到过对方的广播，检查是不是同一个信道/距离太远
- `wtx=0`：没有帧发出，通常是没按第二下 B，或者受伤被挡在 `sub=2` 外面

**为什么不走路由器**：ESP-NOW 是裸 802.11 广播，不建连接、不要 IP、延迟 <5 ms，
一台发一台收，正好是 D-Link 需要的半双工节奏。代码固定在信道 1。

**内存是怎么腾出来的（round 89 定案）**：
一度试过把玻璃画布改成 8bpp 调色板索引（`LV_COLOR_FORMAT_I8` + `lv_canvas_set_palette`），
省 45 KB，内存是够了 —— **但整块玻璃渲染成全黑**，LVGL 9 的软件渲染器不认索引色画布。
更糟的是回滚时漏了 `heap_caps_malloc()` 的大小，45 KB 的缓冲区按 RGB565 写 90 KB，
**每帧越界**，设备直接卡死（按键全失效、重启也回不来）。
**8bpp 这条路废了，别再试。**

现在的做法是**把玻璃缩到 pitch 6**：

| | 之前 | 现在 |
|---|---|---|
| 点阵单元 | 7 px | **6 px** |
| 玻璃 | 224×203 | **192×183** |
| 画布（RGB565） | 90,944 B | **70,272 B** |
| LVGL 池 | 64 KB | **32 KB** |
| 启动后 free | 15,520 | **68,748** |
| Wi-Fi 起来后 free | —— | **32,868** |

图标贴图还是烘焙的 56×28，`blit_slot()` 用最近邻缩到 48×28（这个尺寸看不出差别）。
代价是整块玻璃小约 14%，颜色和版式比例不变。

**已知问题（round 88 收尾时仍在）**：Wi-Fi 起来之后，**对战打完约 30–40 秒设备会自己重启**
（`LINKINFO` 回到 `up=0 mac=000000000000 free=62624`，就是冷启动后的样子）。
试过三版都没根治：
- 把 `bouts` 落盘推迟 4 s（避开收帧）—— 无效
- 离开对战界面 12 s 后 `dg_esp_stop()` —— 无效
- `dg_esp_stop()` 里不调 `esp_wifi_deinit()` —— 无效

没有 panic / WDT 日志打出来（`alive frame` 一路正常，然后下一行就是冷启动），
所以不是软件看门狗，更像是射频栈在某个状态下的硬复位。
**数据不会丢**：`bouts` 在重启前已经落盘（`battle counter 9 -> 10, saving shortly`
→ 4 s 后 `saved digimon (1304 B)`），重启后 `STATE` 里计数是对的。
**当前的规避**：打完一场按 C 回主屏，重启就重启了，宠物和战绩都在。
下一轮要继续查的话，优先怀疑 Wi-Fi 与 BSP 的 SPI/中断抢占，或者换 ESP-NOW 之外的
轻量传输（同一封装里换 `esp_now` → 裸 802.11 长帧 / ESP-MESH-LITE 都试一遍）。

**PC 那边还有一条老路**：`tools/dg_link_pair.py` 用 USB 串口转发帧，仍然可用，
调试时比无线好观察（每次转发都打一行）。无线这条是 `tools/dg_link_wifi.py`。

---

## 编译与刷机

```powershell
# 编译（本机 PowerShell 5.1 下 build.ps1 会被 export.ps1 的 stderr 打断，用这个包装脚本）
powershell -NoProfile -ExecutionPolicy Bypass -File <home>\digimon-passport\build_local.ps1

# 合并镜像（idf.py build 不会重建 full.bin！模拟器要用它）
python tools\mk_full_bin.py
# 想预置 ROM（仿真直接开局 / 救砖带 ROM）：
python tools\mk_full_bin.py --out docs\ui-rom\full-with-v1.bin --rom "0=firmware\digimon-passport\main\roms\digimon.bin:V1"
```

> ⚠️ **2026-11-07 起 `build_local.ps1` / `build_sim.ps1` 不再 source `export.ps1`。**
> 那天 IDF 的 export 脚本开始报
> `tool esp32ulp-elf / dfu-util has no installed versions` → `Activation script failed`
> （这两个工具 ESP32-C3 根本不用），而 PATH 上的 `python` 又是 3.8.2（IDF 5.5 要求 3.9+）。
> 现在两个脚本自己拼 PATH（riscv32-esp-elf / ninja / cmake / ccache / idf-exe / py311），
> 用 venv 的 python 直调 `idf.py`。**工具升级后要改脚本里的版本号目录**。
> 手工作业版（排查用）：
> ```powershell
> $IDF='<home>\idf-setup\esp-idf-5.5.3'; $T='<home>\.espressif'
> $PY="$T\python_env\idf5.5_py3.11_env\Scripts\python.exe"
> $env:IDF_PATH=$IDF; $env:IDF_TOOLS_PATH=$T
> $env:Path="$T\tools\riscv32-esp-elf\esp-14.2.0_20251107\riscv32-esp-elf\bin;$T\tools\ninja\1.12.1;$T\tools\cmake\3.30.2\bin;$T\tools\ccache\4.12.1;$T\tools\idf-exe\1.0.3;"+$env:Path
> & $PY "$IDF\tools\idf.py" -C firmware\digimon-passport build
> ```

**烧写分两种情况，别搞混：**

```powershell
$py = "<home>\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"

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
python tools\mk_full_bin.py
& $py -m esptool --chip esp32c3 -p COM5 -b 921600 write_flash `
    --flash_mode dio --flash_freq 80m --flash_size 8MB `
    0x0 build\FoloToy-AI-Passport-full.bin
```

ROM 不进 bin 了：刷完固件屏幕就是 `NO ROM`，用 `tools/digimon_studio.html` 连上、点 ROM 槽的「上传」选 16 KB 镜像。四次上传（V1/V2/V3/V4）后菜单 `ROM` 行就能切。

`python` 要用 IDF 的 venv（`<home>\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe`），
系统 py311 里没有 esptool。

刷机前可以先备份当前 app 分区（回滚用）：

```powershell
python -m esptool --chip esp32c3 -p COM5 -b 921600 read_flash 0x10000 0x100000 docs\backup\app_before_x.bin
```

模拟器（无头，可截图，不需要真机）：

```powershell
$node = "<home>\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\node\bin\node.exe"

# 一版通吃所有截图（todo #13 收敛后的唯一入口）
& $node tools\dg_sim.mjs docs\ui-rom\full-sim-ac20.bin docs\ui-sim --scenario=all
& $node tools\dg_sim.mjs <full.bin> docs\ui-sim --scenario=boot --hatch=360   # 启动 / 孵化 / 点亮图标
& $node tools\dg_sim.mjs <full.bin> docs\ui-sim --scenario=menu               # 五级菜单逐行
& $node tools\dg_sim.mjs <full.bin> docs\ui-sim --scenario=groups             # TIME / APPEAR / POWER
& $node tools\dg_sim.mjs <full.bin> docs\ui-sim --scenario=ac20 --verbose     # 长按 A 注入 A+C 的时长
& $node tools\dg_sim.mjs <full.bin> docs\ui-sim --scenario=rom                # ROM 槽列表 + ROMINFO
& $node tools\dg_sim.mjs <full.bin> docs\ui-sim --scenario=retail             # STYLE 1 切 C：砖墙主屏 + 菜单 + 切回 A

# ROM 分区协议回归（带断言，不是截图）
& $node tools\dg_sim_rom.mjs docs\ui-rom\full-sim-norom.bin docs\ui-rom\transfer `
    --rom "firmware\digimon-passport\main\roms\digimon.bin:V1" `
    --rom "firmware\digimon-passport\main\roms\digimonv2.bin:V2"
```

`tools/dg_sim.mjs` **先过时钟设置**（不做这一步 ROM 永远停在时钟页），截图已经旋转成 320×240 的真机视角；
`--scenario` 有 `boot / menu / groups / clock / ac20 / rom / retail / all`，加新检查就在这个文件里加一个场景，
**不要再复制一份脚本**（之前 dg_sim_ui.mjs 就是这么把"7 行菜单"抄进历史的）。
`dg_sim_rom.mjs` 是 ROM 分区的回归：空分区 → NO ROM、错 CRC 被拒、上传、ROMINFO、ROMOUT 回读比对、
ROMBOOT 切槽、ROMDEL 回到 NO ROM，最后打 `ALL ROM CHECKS PASSED`。
两者都需要**仿真专用构建**（console 走 UART0）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File <home>\digimon-passport\build_sim.ps1
python tools\mk_full_bin.py --build firmware\digimon-passport\build-sim --out docs\ui-rom\full-sim-norom.bin
```

`tools/dg_sim_test.mjs` / `dg_sim_menu2.mjs` / `dg_sim_wake.mjs` / `dg_sim_state.mjs` / `dg_sim_card.mjs`
是更早的行为回归（断言式，不是截图），仍然可用，用 `mk_full_bin.py --rom` 生成带 ROM 的镜像再跑。

## 验证门禁与仓库外依赖

**别照上游技能跑 `tools/validate.sh`**：它是 FoloToy 上游模板的门禁（bash 脚本），
而本机没有 bash —— MinGit 只带 cmd 版 git，也没有 WSL；而且它引用的
`main/demo_navigation.c`、`tests/test_demo_navigation.c` 在这个 fork 里早被
`main/digimon_app.c` 取代了。跑它必然失败，且失败与本项目的改动无关。
`passport-*` 系列技能里"run ./tools/validate.sh"那一条，在本 fork 上跳过。

本项目的门禁按改动面选：

| 改了什么 | 跑什么 | 通过标准 |
|---|---|---|
| 核心（`main/core/*`、`src/digimon_core.c`） | `src` 里 `gcc -O2 -o test_core.exe test_core.c digimon_core.c` 再运行 | `ALL CHECKPOINTS MATCH`，`pc trace sha256 = f42d1802…` |
| 固件 | `build_local.ps1` → `python tools\mk_full_bin.py` | `BUILD_EXIT=0`；`Wrote N bytes` 与产物字节数一致 |
| UI / 菜单 / 渲染 | `build_sim.ps1` + `dg_sim.mjs --scenario=all`（或 `rom` 回归 `dg_sim_rom.mjs`） | 截图对版式；ROM 分区回归打 `ALL ROM CHECKS PASSED` |
| 进化规则 / 网页表 | `python tools\gen_evo_rules.py --version all` → `node tools\check_evo_tables.mjs` | 三版各 14,112/14,112 行 0 mismatch |
| 发布包 | `python tools\refresh_sha256sums.py` → `python tools\check_release.py` | 自检全过 |

**仓库外的硬依赖（换机器要改脚本里的路径）**：

- ROM 样本 `<home>\Desktop\dsh sth\tama dg roms\digimon\digimon.bin`
  —— `src/test_core.c` 的 `k_rom_path` 是硬编码的，缺了它 test_core 直接 `cannot open ROM`
- 仿真器 `<home>\passport-sim\FoloToy-Passport-Simulator-main`
  —— `tools/dg_sim_*.mjs` 顶部 `const SIM = '…'` 硬编码，缺了 wasm 就 import 失败
- `firmware/digimon-passport/main/roms/` 下三个 16 KB 镜像是测试/仿真副本，**不入库**（见 `.gitignore`）
- V4 样本 `<home>\Desktop\dsh sth\tama dg roms\digimon roms\DigimonV4J.bin`
  —— 16 KB / SHA-256 `3b7eae59a7469e94…` / CRC32 `27df4fd6`；**不入库**，也不在 v5 发布包里。
  真机 slot 4 上另有一份同名镜像（CRC 相同），`ROMBOOT 4` 可切

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
    USB-Serial-JTAG，仿真里看不到。
    **补充（round 67，稳定性实测）**：pitch 7 不是稳定性妥协。仿真诊断构建实测
    `request=90,944 / 分配前 largest=114,688 (=112 KiB) / 分配后 free=69,828 largest=29,696`，
    UI 稳定后 `free≈52 KB / largest=20,480`；pitch 8 的 `request=118,272` 比 114,688 多 **3,584 B**，
    实测 pitch-8 构建启动时 `DIAG prealloc request=118272 free=164040 largest=114688`，随后
    所有控制台命令 NO REPLY（`digimon_app_start()` 返回 `ESP_ERR_NO_MEM`）。所以这是**内存预算**问题，
    与帧率/看门狗/堆碎片无关。画布只在点阵变化时重绘，实测 avg **1,895 µs** / max **1,912 µs**
    （50 fps 预算的 ~9.5%），不变帧直接 return。60 s 空跑 + 真机 `SKIP 120`
    （约 200 s，`skip done: SKIP 120M at 103,243,776 steps`）期间 `free/largest/minfree`
    全程不降、0 条 WDT/panic；真机同一版固件连续跑 **481,250 帧 / 10,869 s（约 3 h）** 无异常
    （`task entered, free heap=58424`、`first frame 416 µs`、`first render 3,527 µs`）。
    **结论：pitch 7 不影响稳定性，别再改 pitch。**
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
    **V2/V3/V4 的同一函数在 `0x059D`**（round 67 实测）：整段 `0x0596` 代码在 V2/V3/V4 里
    统一后移 7 字节，0x0D00 的 16 槽跳表也跟着后移；**RAM 地址完全不变**
    （48/49/4A/4B、4C/4D/4E/4F、0x31..0x35）。`src/evo_probe.c` 已扩成两级
    （1→2 与 2→3，care 0/6 各一遍），三个 ROM 的 A 组都 `EVOLVED`、B 组都
    `NO EVOLUTION`，`left` 变负。V2/V3 不需要第二套解码。
    **V1ES（西班牙语 V1，CRC `bd56e590`）也是同一套逻辑**：入口 `0x0594`（比 V1 再前移 2 字节），
    0x0D10 与 0x1200 跟 V1 逐字相同；`sweep_evo` 的 14,112 行与 V1 JSON 0 mismatch，
    evo_probe 的 A/B 也与 V1 一致。它只是文本/图形本地化，不是 V2/V3。
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
36. **`0x1200` 的槽位顺序不是图鉴顺序**（round 68）：V1 实际 **slot 9 = Airdramon、
    slot 11 = Numemon**，而图鉴顺序写的是 Numemon(9)/Airdramon(11)。网页曾按图鉴顺序
    硬编码，真机一只 slot 11 的 Numemon 被显示成 Airdramon。物种名/配图必须以 ROM
    的 `M(0x0B)` 槽位渲染（`src/slot_dump.c`）或 `0x1200` 表为准，不要照图鉴顺序抄。
    V2/V3/V4 的槽位也一样，见 `tools/gen_evo_rules.py` 的 `NAMES_V*`。
37. **D-Link 波形是"低→高→低→高"严格交替，第 i 段电平 = `i & 1`**（round 70）。
    `src/battle_host.c` 原来写的 `set_peer_line((g_tx_i & 1) ? 0 : 1)` 把整帧倒灌，
    于是"起始 60 ms 低"之后跟的是低、"头脉冲"成了低 —— ROM 收到一个倒相帧，静默 10 s 退出。
    症状极具有欺骗性：`dg_ram(0x33)` 照样 +1（ROM 认为"尝试过对战"），
    `[dir] P2 -> input` 也照常打印，看起来"注入有效但解码不对"，
    害得排查方向一路跑到 PTIMER / ISIO / 采样率上去（全是冤枉的）。
    **判据**：正确回放时抓头脉冲一定是**高**电平、宽度 66 tick；
    修好后 `battle_host` 会打出 `RX #2 = FE01`（A2），而不是只有 `RX #1`。
38. **短 SKIP 会触发看门狗复位**（round 72，真机实测）：`rom_skip_minutes()` 的循环
    原来每 8 个 batch（8×8192 = 64k 步）才 `vTaskDelay(1)` 一次。**5 分钟 SKIP 只有 6 个
    batch，一次都轮不到让出**，6.3 秒后任务看门狗复位，跳过的 5 分钟连同滚动存档一起丢
    （表现为 `SKIP 5` 之后设备重启、`age` 回退）。已改成 `(batch & 1)`：16k 步 ≈ 0.05 s 让
    1 ms，开销约 2%（2H 档从 150 s 变 153 s，可接受）。**判据**：串口要打出
    `skip done: SKIP 5M at N steps`，没有这行就是中途死了。
39. **`STATE` 里的 `sleep` 不是对战计数**（round 72）：`STATE` 的字段顺序是
    `care=0x31 train=0x34/35 over=0x32 sleep=0x33`，所以 `RAM 30..35 = 000100` 里的
    `0x33=1` 是 sleep 值，与"打了一场"无关。真正的胜负在 `win=0x98` / `bouts=0x99`。
    M0 报告里把它当对战计数写过一次，别沿用。
40. **宠物睡着/熄屏/饿着时，控制台注入的按键会被 ROM 忽略**（round 72–77，真机实测）：
    `KEY` 走 `dg_set_button()` 直连 ROM，**绕过了主循环的"按任意键唤醒"逻辑**
    （`s_screen_off` 分支只消费真实按键队列）。症状极具有欺骗性：`KEY 4 600 ok rom=1`、
    `age` 照常涨、`STATE` 正常回，**但 `mode` 不动**。
    **判据**：串口出现 `auto screen off` 日志，`STATE` 里 `sleep=3` 或 `hunger=0`。
    **round 77 结案**：用户物理喂食（`hunger=4 strength=4`）之后，**同一支 `KEY 4 600`
    立刻生效**（`mode 0→2→3`）。所以这条的根因就是"宠物饿着/睡着时不响应交互"，
    不是固件、不是核、也不是 `M(0x03)`/`M(0x02)` 那些 RAM 状态。
    **对策**：测按键前先看 `STATE` 的 `hunger/strength`，饿了就先喂（物理按键）。
41. **别为了"唤醒睡眠中的宠物"去 SKIP 几个小时**（round 72 收尾，真机血泪）：
    V1 槽 0 的 Numemon 睡着（`STATE sleep=3`），控制台注入的按键进不了 ROM，
    于是 `SKIP 120` 想跳到白天——结果 `hunger=0 strength=0`，宠物进入濒死。
    **`sleep` 清 0 后按键仍然无效**，说明按键不进 ROM 不是睡眠造成的，
    而是 ROM 在饥饿/虚弱状态下就不响应交互。`RAMW 3E 4` / `RAMW 3D 4` 把饥饿和体力
    写满只是**暂时的**：ROM 每帧按自己的计时器重写这两个 nibble，几秒内又归零。
    **结论**：控制台注入按键只在"宠物健康且醒着"时可用；救急只能靠物理按键。
    `RAMW` 命令留着当急救工具（`RAMW <addr> <val>`，十六进制），但别指望它能改被 ROM
    每帧刷新的字段。
42. **V2 的对战入口和 V1 不一样**（round 72）：V1 在图标页按一次 B 就是 `sub=2`（战斗界面）；
    V2 在 mode 5/6/7/8 按 B 全都只到 `sub=1`，`LINKSTAT edges=0`，没有发起也没有接收。
    拿 V2 测联机之前先摸清它的图标语义，别照抄 V1 的脚本。
43. **V1 按键链路的关键 RAM（round 72 逆向，`v1.lst`）**：追 `dg_set_button` → `dl_pin_handler`
    → `IK0` 中断（向量 6 → `0x1EDE` → `call 1AC6` → `jp 17CF`），最后落到：
    - `M(0x02)`：**按键事件**。非 0 才算"有新按键"。
    - `M(0x03)` bit3：**消费锁**。`0x1C7..0x1D3` 的逻辑是：事件非 0 且锁未置位 → 置锁、
      返回 `A = M(0x02)`（消费一次）；事件非 0 但锁已置位 → 清 `M(0x03)` 低 3 位后返回。
      **`RAMW 03 0` 清掉 bit3 会触发时钟设置页（mode=1）**，所以它不是"解锁按键"，
      是"要求重设时钟"。
    - `M(0x04)`：**K0 的读值缓存**，`0x1ACF` 读它。
    **实测结论**：`dg_set_button(4,1)` 确实把 K0 拉低（`KEY` 命令回 `k0=9`），
    `f=8`（IF 已开）、`ik0=0`（中断被消费）、`pc=016D`（主循环 halt 醒来处），
    **但 `M(0x02)` 不变**，所以 ROM 认为"没有新按键"。清 `M(0x04)` 也没用。
    **下次接着查**：`0x17CF` 开始的判定（它才是决定要不要写 `M(0x02)` 的地方），
    以及核的 `dg_interrupt()` 在 K0 场景下是否把 `PC/NPC` 恢复对了
    （时钟页的按键是有效的，说明中断返回路径本身没坏）。
44. **`M(0x02)` 是按键事件队列，可以直接伪造**（round 72 末尾，值得记住）：
    不必依赖 `KEY` 注入，`RAMW 03 0`（清消费锁）+ `RAMW 02 <n>` 就能把一次按键事件
    直接喂进 ROM。实测：
    - `RAMW 02 0` → 回主屏（`mode=0`）
    - `RAMW 02 2` → 进时钟设置（`mode=1 sub=3`）
    - `RAMW 02 8` / `RAMW 02 1` → `mode=1 sub=0`

    **注意**：`0x1EDE` 的 K0 中断处理链（`call 1AC6` → `jp 000F`）**根本不写 `M(0x02)`** ——
    `1AC6` 只是把 `M(0x04)`（K0 缓存）读进 A 就返回了。所以 ROM 认为"有没有新按键"
    完全是主循环自己判断的，这解释了为什么电气层/中断层都到位、`ik0=0`、`k0=9`，
    `mode` 却纹丝不动。
    **下一步**：把 `n` 与 A/B/C 的映射、以及从主屏走到 `sub=2` 的完整事件序列摸出来，
    就能完全不靠物理键驱动对战。这条路比修 `KEY` 注入更实在。
45. **`M(0x33)`（sleep）= 3 时，ROM 停在 `sub=2` 但不进接收循环**（round 78，真机）：
    同一支 `LINKFRAME 3FC0` 有时回 `LX 1`、有时一帧不动，根因就是它。
    `RAMW 33 0` 之后立刻 `romtx=1 peertx=1 edges=36`。
    **判据**：进 `sub=2` 后若 `STATE` 里 `sleep=3`，先 `RAMW 33 0` 再发帧。
    `tools/dg_link_battle.py` 已经内置这一步（`enter_battle` 之后、发帧之前）。
    **另一个坑**：`Dev.send()` 会把 `LX` 行吞进返回值，只读 `ser.read` 会误判成"没回帧"。
46. **`M(0x02)` 里疑似有"发起对战"的事件码**（round 79，已作废）：~~只成功过一次~~。
    **round 80 换了正确入口，见坑 47。** `M(0x02)` 是按键事件队列（`2` = B 进/出战斗界面），
    不是发起开关。
47. **发起对战 = `M(0x08)` bit1；应答帧 2 = 判定的按位取反**（round 80，**结案**）：
    - **发起**：`0x0B06` 的发送入口检查 `mode==5 && sub==2 && M(0x08)&2 && P2.0 为高`，
      四条同时满足才拉低起始脉冲。`RAMW 08 2` 就是"按一下发起"。发完 ROM 自己清 bit1。
    - **应答**：发起方发 `[version][verdict]`（实测 `0x02`），对面**必须回 `~verdict`**
      （`0x0D`）。回 `0x01` 或别的值只会让它不停重传自己的第二帧。扫过全部 16 个字节：
      只有 `0x0D` 能让 `sub` 从 2 跳进 3。
    - **完整流程**：`2BD4`(A1) → PC 回自己的卡 → `BF40`(A2) → PC 回 `~02` → `sub 3..8`
      动画约 26 s → `sub=0` 结果页 → `mode=0`，**`bouts` 0→1**。
    - **前置**：`M(0x33)`(sleep) 必须是 0，否则 ROM 停在 `sub=2` 不进接收循环（见坑 45）。
    `tools/dg_link_battle.py` 已按这套改好，打 `M2 REAL BATTLE: PASS`。
48. **`M(0x7A)` 是 D-Link 握手进度条，四条全齐（= `0xF`）才进动画**（round 83，
    `v1.lst` 静态分析 + BrickEmuPy 双机实证；这条解释了双机为什么一边结算一边不结算）：
    主循环 `0x0141 call 0B5B` 每轮进接收子程序，入口先过两道门：
    `0x00E7`（必须 `mode==5 && sub==2`）和 `0x0BEF`（`P2.0` 必须是**低**，线空闲就立刻返回）。
    收满 16 位（4 个 nibble，LSB first，存 `M(0x20..0x23)`）后校验
    `M(0x22)==~M(0x20)`、`M(0x23)==~M(0x21)`，然后**按收到的首 nibble `M(0x20)` 分三路**：

    | 首 nibble | 去处 | 含义 |
    |---|---|---|
    | `0` / `0xF` | `0x0E89` | 无效帧：**清 `M(0x7A)=0`**、`M(0x08)|=2` 重发 |
    | `1` / `2` | `0x0EAA` | **判定帧** |
    | `3`..`0xE` | `0x0E94` | **卡帧**（slot） |

    `M(0x7A)` 的四个位、四个写入点，**缺一个就永远停在 `sub=2` 等 10 s 超时**：

    | 位 | 置位点 | 事件 |
    |---|---|---|
    | bit0 `1` | `0x0E95` | 收到对手的**卡** |
    | bit1 `2` | `0x0EAD` | 收到对手的**判定** |
    | bit2 `4` | `0x0B4F` | 我**发出**了卡（发送收尾按 `M(0x10)` 的值分账：`3..0xE` → bit2） |
    | bit3 `8` | `0x0B51` | 我**发出**了判定（`1/2` → bit3；`0` 另走 `M(0x08)|=4`） |

    **两条进动画的口子**，都只认 `M(0x7A) == 0xF`（是 `cp …,0xF` 全等，不是 `>=`）：
    - 发送侧 `0x0B52` → `0x0B54 jp z` → `0x0EC0`
    - 接收侧 `0x0EBE` → `0x0EC0`（只在对端判定帧重复到达时走到）
    `0x0EC0` 干两件事：`M(0x0A)=3`（sub=3）+ `M(0x0C)=0xD`。**看到 `sub=3 0C=D` 就是它**。

    **所以"应答方发完 B2 会不会进动画"= 会**：它收到卡(bit0) → 发卡(bit2) → 收到判定(bit1)
    → 发判定(bit3) → `0x0B52` 见 `0xF` → `0x0EC0`。BrickEmuPy 双机实测就是这么走的
    （B 侧 `0x7A=F`、`sub 2→3→6`）。
    **发起方反而容易卡在 `0xD`**（缺 bit1）：它走 `0x0EA4 → 0x0D7A` 算判定，
    再收到对端帧时若首 nibble 不在 `1..2`，就永远补不上 bit1。
    另注：进 `sub=2` 那一刻 `0x0393` 会把 `M(0x7A)` 清 0，`0x0395 M(0x08)&=9`；
    `0x04F0..0x04FB` 才是"按 B 发起"的正规装载（`M(0x10)=slot`、`M(0x7B)=effort`、`M(0x08)|=2`）。
    `M(0x7B)` 由 `0x0FB9` 现算：`(min(P2值,4) + M(0x3D) - min(care,4)) >> 1`。
    **判读真机卡在哪一步**：对战中途读 `RAM` 的 `0x7A` 那一位——
    `0x5`=只收发过卡、`0xD`=自己两帧都发了但没认出对端判定、`0x7`=判定收到了但自己没发完。
49. **D-Link 帧的位编码：发送要逐 nibble 位反转，接收还原后是原样**（round 83，
    `brickemupy/m0a/probe_frame_map.py` 实测）：一帧 = 4 个 nibble，**发送顺序
    `M(0x10) M(0x11) M(0x12) M(0x13)`，每个 nibble 内 LSB first**，其中 `n2 = ~n0`、
    `n3 = ~n1`（`0x0B19/0x0B1B` 的 `xor A,0xF` 写的就是这两个）。波形层按 **rising 边沿**
    计周期，`period > 2` 才取位，`period == 10` 吐低字节、`period == 18` 吐高字节
    （`0x0B3F` 那句 `or M(X),0x1` 就是第 18 个 rising，别删）：

    ```
    word = ((rev4(n0)<<4)|rev4(n1)) | (((rev4(n2)<<4)|rev4(n3))<<8)
    ```

    接收侧 `serial_rx_handler` 从队列**尾部** `pop()`，于是 `_rx_word = (低字节<<8)|高字节`，
    回放又从 bit15 开始 —— 两次反转正好抵消，**ROM 存进 `M(0x20..0x23)` 的就是对端
    `M(0x10..0x13)` 的原样**，不做任何交换。实测两边 `0x10..0x13=30CF → word 3FC0 →
    0x20..0x23=30CF`。**帧类型只看第一个 nibble**：`0/0xF` 无效、`1/2` 判定、`3..0xE` 卡。
50. **回放期间 ROM 自己写 P2 会污染 TX 解码，把整帧打成垃圾**（round 83，仿真定位）：
    `g_link_hook` 只在 `dg_io_set(0xF62)`（ROM 写 P2）时触发、`dg_link_drive()` 走
    `dg_pin_handler()` 不触发 —— 这条是对的。但反过来：**ROM 在收帧回放
    （`rx_step != IDLE`）期间仍然会写 `0xF62`**（准备下一帧、拉高拉低），这些写被
    `dglink_p2_write()` 当成真边沿，`tx_word` 多移几位，**之后每一字节都偏一个 nibble**。
    实测：B 该发 `BF40`（判定 2），线上变成 `5FA0`（首 nibble 5 = 卡）；对端因此把它
    当卡帧，bit1 永远补不上，`0x7A` 卡在 `0xD`。
    **对照实验**：把回放期间的 P2 写全部丢弃（`brickemupy/m0a/probe_no_tx_during_rx.py`），
    B 立刻发回正确的 `BF40`，A 侧 `0x7A` 到 `0xF`、`bouts=1` —— **根因确认**。
    ⚠️ 但粗暴丢弃会让"恰好落在回放窗口里的合法发送"整体失帧（实测 A 丢 11 个边沿、
    它的判定帧没出去）。正确修法要成对做：**① 回放期间不计 TX 边沿 ② 发送前确认
    `rx_step == IDLE` 且线高**（`0x0B0C` 只查了线高，没查自己是否正在回放）。
    这条真机同样成立 —— 只是 pair 脚本帧少、时序碰巧错开，没暴露。

51. **真机 `dglink_start_rx` 少了 CON_DGM 的字节交换 —— 判定帧一直被当卡帧**（round 85，
    真机双机结案，**这是"B 不结算"的真因**）：BrickEmuPy 的 `serial_rx_handler` 从队列
    **尾部** `pop()`，所以回放寄存器是发送寄存器的字节交换 —— `tx_frame = lo | (hi<<8)`，
    而 `rx_word = (lo<<8) | hi`。C 版当年直接 `rx_word = word`，而 ROM 从 bit15 起取首 nibble，
    于是每一帧都偏一个字节：

    | 线上帧 | 交换前首 nibble | 判定 | 交换后首 nibble | 判定 |
    |---|---|---|---|---|
    | `2BD4` / `27D8`（卡） | `4` | 3..0xE，**碰巧还是卡** ✓ | `B` | 卡 ✓ |
    | `BF40`（判定 2） | `D` | 3..0xE，**被当成卡** ✗ | `2` | 判定 ✓ |
    | `7F80`（判定 1） | `E` | 3..0xE，**被当成卡** ✗ | `1` | 判定 ✓ |

    卡帧因为"4 也在 3..0xE"蒙混过关，所以过去看着像"交换通了、就是不结算"。
    `M(0x7A)` 于是永远缺 bit1，卡在 `0xD`，`sub` 不动。
    **修复**：`dglink_start_rx()` 入口加 `word = (word << 8) | (word >> 8);`。
    **同时上三件半双工保护**（坑 50 的完整修法）：
    ① 回放期间 `dglink_p2_write()` 直接丢弃 ROM 边沿（计 `tx_dropped`）；
    ② 回放开始/结束都重置 TX 解码器（`tx_period/tx_word/p2_prev`）；
    ③ `dglink_start_rx()` 遇到 `tx_period != 0`（ROM 正在发帧）就把对端帧挂起，
    由 `dglink_tick()` 在 `tx_period == 0` 时回放（1 s 超时兜底）。
    **PC 脚本配套**：`dg_link_battle.py` 应答帧不再取反（判定 1/2 原样发）；
    `dg_link_pair.py` 去掉 `~verdict` 改写，poke 前先写 `M(0x10)=slot`、`M(0x11)=effort`
    —— 否则第一帧发的是上一场残留的判定值。
    **真机实测（2026-11-06 21:5x）**：双机连打 3 场，两边 `M(0x7A)` 都到 `0xF`、
    `sub 2→3→4→5→6→7→0`、`bouts` 都 +1、胜负一致（A win=1/B win=0 与 A win=0/B win=1 各见一次）；
    单机 `dg_link_battle.py`（PC 当对手）也**首次进动画**。
    固件 `0xd7a70` = 883,312 B，两台 `Wrote 883312` 核对通过；
    门禁 `test_core` `ALL CHECKPOINTS MATCH`（`f42d1802…`）、`link_test` `M1 LINK WAVE: PASS`。
    `src/link_wave.{c,h}` 已同步（保留主机那边"帧尾释放不拉高"的差异）。

52. **对战计数要"一变就存"，60 s 滚动存档来不及**（round 86，真机验证）：
    `M(0x99)`（bouts）在打完那一刻变，而滚动存档 60 s 才跑一次；两台真机实测过
    "打完 `STATE` 是 `bouts=1`，过一会儿回主屏再读就是 0"。
    已在 `digimon_app.c` 主循环加看门狗：`s_bouts_seen` 与 `dg_ram(0x099)` 不一致就
    立刻 `save_slot()`（日志 `battle counter 0 -> 1, saving`）；`save_slot()` 成功时
    重新基线，`load_slot()` 之后置 `s_bouts_valid=false` 让它重新对齐。
    **实测**：A 打完 `bouts 1→2`，硬复位（`esptool … run`）后 `STATE` 仍 `bouts=2`，
    B 同理 `win=1 bouts=1`。**计数器不再丢。**
    **顺带的发现**：对战会真的让宠物受伤，而**受伤的宠物进不了对战界面**
    （`mode=5` 卡在 `sub=1`）。地址是 **`injured = M(0x05) bit0`**（`STATE` 的
    `injured` 字段就是 `dg_ram(0x005) & 1`），ROM 在 `0x037F-0x0381` 用
    `fan M(0x05),0x3` 把门：非零直接跳 `0x0364`，永远到不了 `0x038F` 的 `sub=2`。
    这个 nibble **每帧被 ROM 重写**，单独 `RAMW 05 0` 一秒就回去了（`RAMW 09/0A`
    强掰 mode/sub 同样保不住，下一帧就被主循环改回去）。
    **可复现的绕过**：把清零和按键写进**同一次串口写**——
    `RAMW 05 0\nKEY 2 600\n` 一把发出去，ROM 在处理按键那一刻正好读到 0，
    立刻进 `sub=2`。`tools/dg_link_pair.py` 的 `to_battle()` 已经内置这一步。
    正常玩法还是让宠物自己恢复（或吃药），别把它当常态。
    另外 `energy` 是 `ram_bcd(0x08C, 0x08D)`，不是受伤标志。

53. **`dg_osc1_ticks()` 是 32 位，**36.4 小时回绕一次；**时间比较一律用差值，禁止绝对目标**
    （round 112，真机复现）。跳过时间原来是：

    ```c
    uint32_t target = dg_osc1_ticks() + (uint32_t)minutes * 60u * OSC1_PER_SEC;
    while ((int32_t)(dg_osc1_ticks() - target) < 0) { ... }
    ```

    24 h 的 tick 是 2,831,155,200，只要开机超过 **约 12.4 小时**（`start > 2^32 − total`），
    加法回绕，`target` 落到一个"早已过去"的值，循环一次都不跑 —— 表现就是
    **玻璃闪一下就没了、时间没跳**。用户机器跑了 16.5 h 时必现。
    改法是**累加已跑 tick**，与起点无关：

    ```c
    uint64_t total = (uint64_t)minutes * 60u * OSC1_PER_SEC, acc = 0;
    uint32_t last = dg_osc1_ticks();
    while (acc < total) { ...; uint32_t now = dg_osc1_ticks();
                          acc += (uint32_t)(now - last); last = now; }
    ```

    已审计全仓其余 30 多处 `dg_osc1_ticks()`：都是小增量（≤3 s）或
    `(uint32_t)(now - last)` / `(int32_t)(now - target) < 0` 的差值写法，**36.4 小时回绕也安全**。
    所以**不需要为了计时去定期重启**；以后新增功能照差值写就行。

## 待办（按优先级）

0. **联机对战（D-Link）M0 已打通（2026-11-06）**——调研见 `docs/LINK-BATTLE-FEASIBILITY.md`，
   路线图见 `docs/ROADMAP-BATTLE.md`，M0 报告 `docs/M0A-BRICKEMUPY-LINK.md`。
   三个目标：网页端对战 / 离线卡片对战 / 双机联机；共用底座（核 P2.0 注入 + 波形层 + 传输抽象）。
   **A：参考实现能收帧**。BrickEmuPy 双实例（脚本 `<home>\brickemupy\m0a\`）
   在 0.605 s 内走完 `A1 3FC0 → B1 3FC0 → A2 BF40 → B2 7F80`（线上字节；ROM 看到 `03/03/02/01`），
   26 s 对战动画 + 6 s 结果页后回主屏，`RAM 0x33` 0→1；角色互换完全对称。
   **B：C 核的根因是探针自己的 bug**。`src/battle_host.c` 回放帧时极性写反了
   （`set_peer_line((g_tx_i & 1) ? 0 : 1)`，整帧倒灌）；改成 `? 1 : 0` 后同一支探针立刻
   解出 `RX#1 FC03 → TX DC23 → RX#2 FE01 → TX FD02`，`RAM 30..35 = 000100`。
   **时序常数一个都没改**（66/33/33/98/90/41/60ms/6ms 够用，脉宽 ±10% 不影响）。
   **M0 不需要降级方案，M1 可以直接开工。**
   三条 M1 要用的事实：①**应答方不用按键**，只要已在 `mode=5 sub=2` 接收循环里，
   收到起始低脉冲就自己回帧；②P2 外部上拉不是必需的；③ROM 确实开了 PTIMER 中断
   （`EIPT=1`、`PTC=3`、`PT` 在 0x18↔0x00 循环）。
   **波形层移植时最要命的一条**：起始低之后紧跟**高**电平，序列严格 `低→高→低…`，
   第 i 段电平 = `i & 1`（这条写反过一次，见坑 37）。
   已定位：链路引脚 **P2.0**（`0xF62` 数据 / `0xF7D` IOC bit2 方向）。
   探针在 `src/battle_*.c` + `src/trace_core_link.c`（后者是带 hook 的内核副本，不属于固件）。
   `src/battle_host2.c` 是 CON_DGM 常数版的对照探针（也通）。
   物理接线方案已排除：GPIO 0–10、18–21 全占。
   **M1 底座已完成（2026-11-06）**，新增三个模块 + 一个主机夹具：
   - `main/core/digimon_core.{c,h}`（与 `src/` 逐字节相同）：`dg_link_drive/latch/set_hook`，
     hook 在 `dg_io_set(0xF62)` 里触发，`test_core` 仍 `ALL CHECKPOINTS MATCH`（`f42d1802…`）
   - `main/dg_link.{c,h}`（与 `src/link_wave.{c,h}` 同源）：波形层，RX 回放 + TX 解码
   - `main/dg_link_session.{c,h}`：帧路由 + 8 s 看门狗，**还没接线**，M2 接传输层时用
   - `src/link_test.c`：单核 + 波形层 + 脚本化对手 → `M1 LINK WAVE: PASS`
   `digimon_app.c` 已在 `dg_init()` 后调 `dg_link_init()`、两个 `dg_step()` 循环里各加
   `dglink_tick(&g_dglink, dg_osc1_ticks() - t_link)`。固件 `0xd72f0`（880,880 B），
   仿真跑满 360 s 照常孵化（截图 `docs/ui-m1-v1/`）。
   **下一步 M2**：串口命令 `LINKMODE/LINKHELLO/LINKFRAME/LINKLOG` + 网页第 4 页「对战」。
   **M2 进度（2026-11-06，真机联调，详见 `docs/M2-DEVICE-LINK.md`）**：
   - 已上真机：`KEY <mask> <ms>`（注入按键，**bit0=C / bit1=B / bit2=A，允许 1..7 组合**，按住必须 ≥600 ms；
  组合键只到 ROM 层，ROM 实际不认 A+C，见 round 96）、
     `LINKHELLO` / `LINKLOG` / `LINKFRAME` / `LINKSTAT`。固件 `0xd7730`（882,480 B）
   - **真机收帧成功**：`LINKFRAME 3FC0` → 真机回 `LX 1 2BD4`（slot B / effort 2，反码对），
     `edges=36` 一帧；再回一帧得到 `LX 2 BF40`
   - **未打通**：真机交换完两帧仍停 `mode=5 sub=2`，不播动画、`win/bouts` 不变。
     `src/link_test.c`（单核 + 脚本对手）现象完全一致；已排除回放延迟、波形常数、
     帧尾电平、对手 effort。推测 ROM 需要"另一台真 ROM"才会结算（见 M2 文档 §4）
   - 网页端 `tools/digimon_studio.html` 已有第 4 页「对战」（我的卡 / 进入对战 / 发卡 /
     自动回帧 / 帧表），但**只验证了渲染与语法**，Web Serial 需要人工点授权，
     没做浏览器端到端
   - 脚本：`tools/dg_console.py`（串口收发）、`tools/dg_link_battle.py`（进对战 + 帧表）、
     `tools/dg_link_pair.py`（双机桥接，见下）
   **双机实测（2026-11-06 晚，两台 Passport 都在线）**：
   - COM4 已刷成一模一样的设备（`mk_full_bin.py --rom` 四套 ROM 的整片镜像 + `SAVEIN`
     灌了 COM5 的存档），两台 `slot=B`、同一只宠物
   - `dg_link_pair.py` 打通了**真实四帧交换**：`A→B 2BD4 / B→A 2BD4 / A→B BF40 / B→A BF40`
     （`frames relayed: 4`）
   - **A 侧完整结算**：`sub 2→5→7→0`、`bouts 0→1`、回主屏
   - **B 侧不结算**：应答方回的是自己的判定原值（`0x02`）而不是按位取反，发完就退出
   - 这次能打完，是 PC 在转发时把 B 的第二帧改写成 `~0x02`（`0x0D`）
   - ~~**待办**：读 `0x0B` 段的接收分支……~~ **已结案（round 83，坑 48）**：
     应答方**会**进动画，闸门是 `M(0x7A)==0xF`（收卡 / 发卡 / 收判定 / 发判定，四件事全做完）。
     它"不做取反"不是缺陷——应答方发的 `0x02` 就是它自己算出的判定值，两边的第二帧
     本来就不该同值；PC 把 B 的帧改写成 `0x0D` 反而是替 A 补上了 A 缺的那一步，歪打正着。
     真机 B 不结算 = 它的 `M(0x7A)` 没到 `0xF`。
   - **下一步（真机，只读，不动固件不动存档）**：双机对战中途 `RAM` 读两台的 `0x7A`，
     看 B 缺哪一位——`0x5`=缺"收到判定帧"、`0x7`=缺"自己发出判定"、
     `0xD`=自己两帧都发了，但没把对端那一帧认成判定。
1. **esptool-js 网页刷机**：同一个串口既要跑日志又要跑握手，得先断日志通道再交给 `ESPLoader`，
   刷完挂回来。计划代码在 `docs/studio-tool.md` 末尾
2. ~~**V2 / V3 / V4 的进化表**~~ **已完成（round 67/68）**：`care_rules_v2/v3/v4.json`
   各 14,112 行（另有 150,528 行全网格校验），`gen_evo_rules.py --version all` 各压成 12 条，
   `tools/check_evo_tables.mjs` 直接回放 HTML 里的四套规则，4×14,112 行 0 mismatch。
   V2/V3/V4 的卡片从 `page_img_03/04/05.jpg` 裁，已内嵌（共 56 张）。
   V4 的 0x0D10 与 V2/V3 逐字相同，所以规则表直接复用；只有物种名/卡片不同。
   剩 **V4 的 `perfect` / `injury_roll`** 没逐项测（网页只用 evolution），以及 V4 中文名可再校一轮。
3. **点阵可读性**：pitch 7 → 点阵 224×112（比 pitch 8 小 22%）。嫌小只能改回 pitch 8，
   但要先把 LVGL 的缓冲让出来（`s_canvas_buf` 挪到 LVGL 初始化之前，或玻璃画布降 8-bit 索引色）。
   **round 67 实测这是内存预算问题，不是稳定性问题**，详见坑 11 补充。
4. **ROM 上传提速**：现在是 hex 行（16 KB → 33 KB 文本），可以考虑 base64 分块或 XMODEM-1K，省一半时间
5. 可选：用 `ref/vpet_ui_assets/` 的 5 张边框（540×720 竖版、挖孔 400×200）横向重切成 224×203 孔的版本，
   做成真正的塑料外壳贴图；现在的三层边框是纯色块

## 工程约定

- 主分支模型：`main` 只与上游同步，功能改动走 `feature/*`（本项目是 fork，实际一直在主工作区改）
- 每次改动后跑 `test_core`（`src/` 里 `gcc -O2 -o test_core.exe test_core.c digimon_core.c`），必须 `ALL CHECKPOINTS MATCH`
  （本机 gcc 在 `<home>\w64devkit\bin\gcc.exe`）
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
- **发布包脱敏只替换前缀**：`<home>` → `<home>`，**不要连后面的路径一起吞掉**。
  曾经整段替换成 `<home>`，`esptool` 那条命令被切成 `$py = <home>` 这种残句（引号都没闭合），
  复制到终端直接报错；打包后跑 `python tools\check_release.py`，坏占位、断链、校验和不符全都会被抓出来
- 固件改动后固件三件套：`test_core`（必须 ALL CHECKPOINTS MATCH）→ 编译 → 刷机核对字节数 →
  串口 `STATE` + `DUMP` 各打一次，确认新命令真的在跑

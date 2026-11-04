# 第三方组件与素材

本仓库的代码以 MIT 分发（见 `LICENSE`）。它站在下面这些成果上，各自的授权条件如下。

## 代码来源

| 来源 | 用途 | 许可 | 说明 |
|---|---|---|---|
| [BrickEmuPy](https://github.com/azya52/BrickEmuPy) | E0C6200 参考实现（Python） | **CC0-1.0** | `firmware/.../core/digimon_core.c` 与 `src/digimon_core.c` 是它的 C99 移植。仓库根目录的 `LICENSE` 文件即 CC0 1.0 全文（2026-11 核实） |
| [FoloToy AI Passport](https://github.com/FoloToy) | 硬件抽象层 `components/bsp`、应用骨架 | MIT | 见上游 `LICENSE` |
| [MAME](https://github.com/mamedev/mame) | `hh_e0c6200.cpp` 与 `digimon.svg` 仅作**行为与版式参考** | GPL-2.0 / BSD-3-Clause | 未复制其代码或数据；段映射表由参考 SVG 逆向生成（`tools/gen_segmap.py`） |
| Espressif ESP-IDF | 构建系统与驱动 | Apache-2.0 | 作为依赖引入，未修改 |

> 早期版本的仓库说明把 BrickEmuPy 写成 GPL-3.0，是错的。核对上游 `LICENSE` 后确认为
> CC0 1.0，因此本项目的移植不构成 GPL 传染，可以 MIT 分发。

## 不随仓库分发的素材

| 内容 | 原因 |
|---|---|
| `roms/*.bin`（V1/V2/V3 固件镜像） | Bandai 版权作品。用你自己的镜像，见 `roms/README.md` |
| 《V-Pet Version 1-5》说明书扫描页 | 版权归原出版方。`tools/digimon_studio.html` 的公开版**不含**这些图；`tools/cut_evo_cards.py` 只是处理脚本，输入需你自备 |
| 数码兽立绘 / 名称 / 商标 | 归 Bandai / 东映所有。本项目仅作个人向的硬件移植与研究 |

## 关于 ROM 研究数据

仓库里的 `tools/ref-disasm/`（RAM 图、函数表、14112 组进化判定结果）是从**用户自己的
镜像**反汇编得到的**观测数据**，不含原镜像字节。它记录的是"这个程序怎么行为"，
不是程序本身。

## 商标

"Digital Monster"、"デジタルモンスター"、"Digimon"、"数码宝贝" 等名称与形象是
Bandai 的商标。本项目与 Bandai、FoloToy 均无隶属关系，也未获其背书。

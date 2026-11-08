# bouts / win 计数器位宽核实（todo #18）

> 2026-11-08。对象：`digimon.bin`（V1，CRC `08ffac1b`）。
> 反汇编源：`docs/probe_shots/disasm_v1_full.txt`（E0C6200 12-bit，地址 = 字地址）。
> 结论只针对 V1；V2–V4 见文末「未验证」。

## 结论

**`M(0x99)`（bouts）与 `M(0x98)`（win）都是单 nibble 的饱和计数器：加到 0xF 就停在 0xF，不回绕、不进位、不写相邻字节。**
「到 15 就满、之后只能靠 win 看变化」的旧说法成立，但依据从"观测"升级为指令级证据。

## 证据

### 1. RAM 单元本身就是 4 bit

`main/core/digimon_core.c` 的 `dg_set_mem()`：

```c
c->ram[addr] = (uint8_t)(value & 0xF);
```

E0C6200 的 RAM 是 4-bit 字，任何单地址字段值域 0..15。想存 16 以上必须占两个 nibble。

### 2. 0x00DE 是一个"加一并饱和"的子程序

```
00DE  C21  add MX, 1        ; op 0xC21: r=(op>>4)&3 = 2 = MX
00DF  3E1  jp nc, 0xE1      ; 没进位 -> 直接返回
00E0  E2F  ld MX, 0xF       ; 进位（原值 0xF）-> 写回 0xF
00E1  FDF  ret
```

反汇编器把这两条标成 `add r0,1` / `ld_r_i 0x2F`，是**寄存器名标错**：按 `dg_op_add_r_i` / `dg_op_ld_r_i`
的实现 `r=(op>>4)&3`，`0xC21` 的 r=2（MX）、`0xE2F` 的 r=2（MX）。`dg_reg_get(2)` 返回
`dg_get_mem(c, c->IX)`，所以这两个操作数就是 `M(X)`。

即：`M(X) = min(M(X)+1, 0xF)`。

### 3. 对战结算就是这么加 bouts / win 的

```
0732  ld x, 0x94
0733  call 0xFE             ; 副表/计分辅助
0734  ld x, 0x99            ; X = bouts
0735  call 0xDE             ; bouts = min(bouts+1, 0xF)
0736  ld x, 0x79
0737  cp M(X), 1            ; 某状态位 == 1 ?
0738  jp nz, 0x3E
0739  ld x, 0x90
073A  call 0xFE
073B  ld x, 0x98            ; X = win
073C  call 0xDE             ; win = min(win+1, 0xF)
```

0x0734/0x0735 与 0x073B/0x073C 是 `calz 0xDE` 的调用点里唯一先 `ld x,0x99` / `ld x,0x98` 的两处
（其余 5 处调用点 X 指向别的状态字节）。

### 4. ROM 自己在别处按"bouts == 0xF"分支

```
0D68  ld x, 0x99
0D69  cp M(X), 0xF
0D6A  jp nz, 0x0D78         ; 不满 15 -> 跳过
0D6B  ld x, 0x98
0D6C  ld A, 0xE
0D6D  cp M(X), 5            ; win < 5 ?
...
```

说明 ROM 自己也把 15 当"满值"状态，而不是当 15 继续往上加的中间值。

## 对项目的实际影响

- **不需要改固件**：`digimon_app.c` 的 bouts 看门狗只比较"值变没变"，`DUMP`/`STATE` 按 `%X` 原样打；
  满值后不再触发存档是预期行为（值确实没变）。
- **网页/菜单**可以放心把 15 标成 `MAX`，或用 `win` 做"还能不能看出新战绩"的判据。
- `src/card_test.c:251` 的 `bouts_before == 0xF` 特判是**正确的**，可以保留；它当初只是没写证据。

## 未验证

- V2 / V3 / V4 是否共用同一段代码：V2+ 的 `0x0596` 系列整体后移 7 字节（HANDOFF 坑 23），
  `0x00DE` 这个子程序是否也搬了位置、或换成等价实现，**没有逐 ROM 反汇编核对**。
  影响很小（RAM 地址与 4-bit 单元不变，饱和语义由硬件位宽决定），但如果以后要写"V2 战绩上限不同"
  之类的结论，先补这一步。
- 没有跑"预置 0xF 再打一场"的主机实测；上面 4 条已经能定性，实测只是重复确认。

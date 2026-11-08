/*
 * digimon_core.c -- Epson E0C6S48 (E0C6200 core, 4-bit MCU) emulator core.
 *
 * This is a C99 translation of the (verified) Python reference implementation
 * in ref/cores/E0C6200.py and ref/cores/E0C6200sound.py.
 *
 * Bit-exactness notes (things that look odd but are intentional, because the
 * reference does exactly this):
 *   - The OSC1 / timer / stopwatch / PTIMER prescalers are kept in `double`,
 *     exactly like the Python version, so the fractional 32768/100 = 327.68
 *     stopwatch reload accumulates with identical rounding.
 *   - get_VRAM() returns 160 bytes (all 0 or all 1) while the LCD is forced
 *     off/on, and 169 bytes otherwise.  dg_vram_len() reports which.
 *   - `_dummy` instructions do not advance PC (same as the reference).
 *   - No malloc, no libc beyond <string.h>.
 */

#include "digimon_core.h"

/* On the ESP32-C3 the core lives in flash and is fetched through a small
 * instruction cache; the 4096-entry dispatch table alone is 16 KB, so every
 * dg_step() is a cache-miss storm.  Putting the hot path in IRAM/DRAM is the
 * usual fix and costs ~46 KB of RAM.  Host builds (test_core) leave both macros
 * empty so the golden trace is untouched. */
#ifdef DG_HOST_ESP32
#include "esp_attr.h"
#define DG_IRAM  /* measured: no speedup, see HANDOFF pitfall 24 */
#define DG_DRAM
#else
#define DG_IRAM
#define DG_DRAM
#endif

#include <string.h>

/* ---------------------------------------------------------------- sizes -- */

#define RAM_SIZE            0x300
#define VRAM_SIZE           0x0A0
#define VRAM_PART1_OFFSET   0xE00
#define VRAM_PART2_OFFSET   0xE80
#define VRAM_PART_SIZE      0x050
#define IORAM_OFFSET        0xF00
#define IORAM_SIZE          0x07F

#define DG_VRAM_TOTAL       169

#define OSC1_CLOCK          32768

#define PORT_COUNT          11

enum {
    PORT_K0 = 0, PORT_K1, PORT_R0, PORT_R1, PORT_R2, PORT_R3, PORT_R4,
    PORT_P0, PORT_P1, PORT_P2, PORT_P3
};

/* ------------------------------------------------------------- io bits -- */

#define IO_IT1      8
#define IO_IT2      4
#define IO_IT8      2
#define IO_IT32     1
#define IO_ISW0     2
#define IO_ISW1     1
#define IO_IPT      1
#define IO_ISIO     1
#define IO_IK0      1
#define IO_IK1      1

#define IO_TM3      8
#define IO_TM2      4
#define IO_TM1      2
#define IO_TM0      1
#define IO_TM7      8
#define IO_TM6      4
#define IO_TM5      2
#define IO_TM4      1

#define IO_PTCOUT   8
#define IO_PTC      7
#define IO_SWRST    2
#define IO_SWRUN    1
#define IO_PTRST    2
#define IO_PTRUN    1
#define IO_TMRST    2

#define IO_R33      8

#define IO_CLKCHG   8
#define IO_ALOFF    8
#define IO_ALON     4
#define IO_LDUTY    2
#define IO_HLMOD    1

#define IO_BZFQ     7
#define IO_SHOTPW   8
#define IO_BZSHOT   8
#define IO_ENVRST   4
#define IO_ENVRT    2
#define IO_ENVON    1

#define IO_IOC0     1
#define IO_IOC1     2
#define IO_IOC2     4
#define IO_IOC3     8
#define IO_PUP0     1
#define IO_PUP1     2
#define IO_PUP2     4
#define IO_PUP3     8

/* ----------------------------------------------------------------- sound -- */

#define SOUND_CLOCK_DIV         128
#define BUZZER_FREQ_DIV_0       8

typedef struct {
    int     one_shot_counter;
    double  buzzer_freq;
    int     envelope_step;
    int     envelope_cycle;
    int     envelope_counter;
    uint8_t envelope_on;
    uint8_t sound_on;
} dg_sound;

static const int dg_buzzer_freq_div[8]   = { 8, 10, 12, 14, 16, 20, 24, 28 };
/* ENVELOP_STEP_DIV = {16,20,24,28,16,20,24,28} is unused by the reference too */
static const int dg_one_shot_pw_div[2]   = { 8 * SOUND_CLOCK_DIV, 16 * SOUND_CLOCK_DIV };
static const int dg_envelope_cycle_div[2] = { 16 * SOUND_CLOCK_DIV, 32 * SOUND_CLOCK_DIV };

/* ------------------------------------------------------------------- cpu -- */

typedef struct {
    const uint8_t *rom;
    uint32_t       rom_size;

    uint8_t  ram[RAM_SIZE];
    uint8_t  vram[VRAM_SIZE];

    uint16_t A, B;
    uint16_t IX, IY;
    uint8_t  SP;
    uint16_t PC, NPC;

    uint8_t  CF, ZF, DF, IF, HALT;
    uint8_t  if_delay;

    uint8_t  PDIR[PORT_COUNT];
    uint8_t  PUSHPULL[PORT_COUNT];
    uint8_t  PLATCH[PORT_COUNT];
    uint8_t  PULLUP[PORT_COUNT];
    uint8_t  pullup_ext[PORT_COUNT];
    uint8_t  port_input[PORT_COUNT][2];

    uint8_t  IT, ISW, IPT, ISIO, IK0, IK1;
    uint8_t  EIT, EISW, EIPT, EISIO, EIK0, EIK1;
    uint8_t  TM, SWL, SWH, PT, RD, SD, DFK0;
    uint8_t  CTRL_OSC, CTRL_LCD, LC, CTRL_SVD, CTRL_BZ1, CTRL_BZ2;
    uint8_t  CTRL_SW, CTRL_PT, PTC, SC, HZR, IOC, PUP;
    uint8_t  RESET;

    uint32_t instr_counter;

    /* Fixed point, x1024 for OSC1 (1060000/32768 is exact in 1/1024 units) and
     * x25 for the stopwatch (32768/100 is exact in 1/25 units). ESP32-C3 has no
     * FPU, and the double version cost ~20 us per step there (760x the x86
     * figure) because every clock() did software float math. */
    int32_t  osc1_div;
    int32_t  osc1_counter;
    int32_t  timer_counter;
    int32_t  ptimer_counter;
    int32_t  stopwatch_counter;
    uint32_t osc1_ticks;        /* free running OSC1 tick counter (32768 Hz) */

    dg_sound sound;

    uint8_t  vram_out[DG_VRAM_TOTAL];
    uint32_t vram_out_len;
} dg_cpu;

static dg_cpu g_cpu;

/* ------------------------------------------------------- prescaler table -- */

#define OSC1_FX_SHIFT     10                    /* OSC1 prescaler: 1/1024 cycle */
#define OSC1_DIV_FX       (1060000 / 32)        /* 33125 = 32.3486328125 * 1024, exact */
#define STOPWATCH_FX      25                    /* stopwatch: 1/25 tick units */
#define STOPWATCH_DIV_FX  (OSC1_CLOCK / 4)      /* 8192 = 327.68 * 25, exact */
static const int32_t k_timer_div     = OSC1_CLOCK / 256;    /* 128 */
static const int32_t k_ptimer_div[8] = {
    0.0, 0.0,
    OSC1_CLOCK / 256,
    OSC1_CLOCK / 512,
    OSC1_CLOCK / 1024,
    OSC1_CLOCK / 2048,
    OSC1_CLOCK / 4096,
    OSC1_CLOCK / 8192
};

/* ----------------------------------------------------------- prototypes -- */

static int  dg_get_mem(dg_cpu *c, int addr);
static void dg_set_mem(dg_cpu *c, int addr, int value);
static int  dg_io_get(dg_cpu *c, int addr);
static void dg_io_set(dg_cpu *c, int addr, int value);
static int  dg_reg_get(dg_cpu *c, int r);
static void dg_reg_set(dg_cpu *c, int r, int value);
static int  dg_port_read(dg_cpu *c, int p);
static int  dg_interrupt(dg_cpu *c, uint8_t vector);
static void dg_process_ptimer(dg_cpu *c);
static void dg_process_stopwatch(dg_cpu *c);
static void dg_process_timer(dg_cpu *c);
static void dg_clock_osc1(dg_cpu *c);
static void dg_pin_handler(dg_cpu *c, int port, uint8_t mask, int level);
static void dg_init_registers(dg_cpu *c);

#define DG_OP(name) static DG_IRAM int dg_op_##name(dg_cpu *c, uint16_t op)

DG_OP(jp_s);
DG_OP(retd_l);
DG_OP(jp_c_s);
DG_OP(jp_nc_s);
DG_OP(call_s);
DG_OP(calz_s);
DG_OP(jp_z_s);
DG_OP(jp_nz_s);
DG_OP(ld_y_y);
DG_OP(lbpx_mx_l);
DG_OP(adc_xh_i);
DG_OP(adc_xl_i);
DG_OP(adc_yh_i);
DG_OP(adc_yl_i);
DG_OP(cp_xh_i);
DG_OP(cp_xl_i);
DG_OP(cp_yh_i);
DG_OP(cp_yl_i);
DG_OP(add_r_q);
DG_OP(adc_r_q);
DG_OP(sub_r_q);
DG_OP(sbc_r_q);
DG_OP(and_r_q);
DG_OP(or_r_q);
DG_OP(xor_r_q);
DG_OP(rlc_r);
DG_OP(ld_x_x);
DG_OP(add_r_i);
DG_OP(adc_r_i);
DG_OP(and_r_i);
DG_OP(or_r_i);
DG_OP(xor_r_i);
DG_OP(sbc_r_i);
DG_OP(fan_r_i);
DG_OP(cp_r_i);
DG_OP(ld_r_i);
DG_OP(pset_p);
DG_OP(ldpx_mx_i);
DG_OP(ldpy_my_i);
DG_OP(ld_xp_r);
DG_OP(ld_xh_r);
DG_OP(ld_xl_r);
DG_OP(rrc_r);
DG_OP(ld_yp_r);
DG_OP(ld_yh_r);
DG_OP(ld_yl_r);
DG_OP(dummy);
DG_OP(ld_r_xp);
DG_OP(ld_r_xh);
DG_OP(ld_r_xl);
DG_OP(ld_r_yp);
DG_OP(ld_r_yh);
DG_OP(ld_r_yl);
DG_OP(ld_r_q);
DG_OP(ldpx_r_q);
DG_OP(ldpy_r_q);
DG_OP(cp_r_q);
DG_OP(fan_r_q);
DG_OP(acpx_mx_r);
DG_OP(acpy_my_r);
DG_OP(scpx_mx_r);
DG_OP(scpy_my_r);
DG_OP(set_f_i);
DG_OP(rst_f_i);
DG_OP(inc_mn);
DG_OP(dec_mn);
DG_OP(ld_mn_a);
DG_OP(ld_mn_b);
DG_OP(ld_a_mn);
DG_OP(ld_b_mn);
DG_OP(push_r);
DG_OP(push_xp);
DG_OP(push_xh);
DG_OP(push_xl);
DG_OP(push_yp);
DG_OP(push_yh);
DG_OP(push_yl);
DG_OP(push_f);
DG_OP(dec_sp);
DG_OP(pop_r);
DG_OP(pop_xp);
DG_OP(pop_xh);
DG_OP(pop_xl);
DG_OP(pop_yp);
DG_OP(pop_yh);
DG_OP(pop_yl);
DG_OP(pop_f);
DG_OP(inc_sp);
DG_OP(rets);
DG_OP(ret);
DG_OP(ld_sph_r);
DG_OP(ld_r_sph);
DG_OP(jpba);
DG_OP(ld_spl_r);
DG_OP(ld_r_spl);
DG_OP(halt);
DG_OP(nop5);
DG_OP(nop7);

/* ---- generated by tools/gen_op_tbl.py from ref/cores/E0C6200.py ---- */
static DG_DRAM int (*const dg_op_tbl[4096])(dg_cpu *c, uint16_t op) = {
    /* 0x000 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x008 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x010 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x018 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x020 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x028 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x030 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x038 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x040 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x048 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x050 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x058 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x060 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x068 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x070 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x078 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x080 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x088 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x090 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x098 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x0A0 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x0A8 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x0B0 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x0B8 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x0C0 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x0C8 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x0D0 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x0D8 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x0E0 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x0E8 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x0F0 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x0F8 */ dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s, dg_op_jp_s,
    /* 0x100 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x108 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x110 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x118 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x120 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x128 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x130 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x138 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x140 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x148 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x150 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x158 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x160 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x168 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x170 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x178 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x180 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x188 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x190 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x198 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x1A0 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x1A8 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x1B0 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x1B8 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x1C0 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x1C8 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x1D0 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x1D8 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x1E0 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x1E8 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x1F0 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x1F8 */ dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l, dg_op_retd_l,
    /* 0x200 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x208 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x210 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x218 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x220 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x228 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x230 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x238 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x240 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x248 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x250 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x258 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x260 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x268 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x270 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x278 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x280 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x288 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x290 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x298 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x2A0 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x2A8 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x2B0 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x2B8 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x2C0 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x2C8 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x2D0 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x2D8 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x2E0 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x2E8 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x2F0 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x2F8 */ dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s, dg_op_jp_c_s,
    /* 0x300 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x308 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x310 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x318 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x320 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x328 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x330 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x338 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x340 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x348 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x350 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x358 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x360 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x368 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x370 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x378 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x380 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x388 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x390 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x398 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x3A0 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x3A8 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x3B0 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x3B8 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x3C0 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x3C8 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x3D0 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x3D8 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x3E0 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x3E8 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x3F0 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x3F8 */ dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s, dg_op_jp_nc_s,
    /* 0x400 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x408 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x410 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x418 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x420 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x428 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x430 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x438 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x440 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x448 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x450 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x458 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x460 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x468 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x470 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x478 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x480 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x488 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x490 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x498 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x4A0 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x4A8 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x4B0 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x4B8 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x4C0 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x4C8 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x4D0 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x4D8 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x4E0 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x4E8 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x4F0 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x4F8 */ dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s, dg_op_call_s,
    /* 0x500 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x508 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x510 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x518 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x520 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x528 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x530 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x538 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x540 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x548 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x550 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x558 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x560 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x568 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x570 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x578 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x580 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x588 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x590 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x598 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x5A0 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x5A8 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x5B0 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x5B8 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x5C0 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x5C8 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x5D0 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x5D8 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x5E0 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x5E8 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x5F0 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x5F8 */ dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s, dg_op_calz_s,
    /* 0x600 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x608 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x610 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x618 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x620 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x628 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x630 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x638 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x640 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x648 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x650 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x658 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x660 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x668 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x670 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x678 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x680 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x688 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x690 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x698 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x6A0 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x6A8 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x6B0 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x6B8 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x6C0 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x6C8 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x6D0 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x6D8 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x6E0 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x6E8 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x6F0 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x6F8 */ dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s, dg_op_jp_z_s,
    /* 0x700 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x708 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x710 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x718 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x720 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x728 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x730 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x738 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x740 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x748 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x750 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x758 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x760 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x768 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x770 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x778 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x780 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x788 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x790 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x798 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x7A0 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x7A8 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x7B0 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x7B8 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x7C0 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x7C8 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x7D0 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x7D8 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x7E0 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x7E8 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x7F0 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x7F8 */ dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s, dg_op_jp_nz_s,
    /* 0x800 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x808 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x810 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x818 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x820 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x828 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x830 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x838 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x840 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x848 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x850 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x858 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x860 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x868 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x870 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x878 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x880 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x888 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x890 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x898 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x8A0 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x8A8 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x8B0 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x8B8 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x8C0 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x8C8 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x8D0 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x8D8 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x8E0 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x8E8 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x8F0 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x8F8 */ dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y, dg_op_ld_y_y,
    /* 0x900 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x908 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x910 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x918 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x920 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x928 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x930 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x938 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x940 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x948 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x950 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x958 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x960 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x968 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x970 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x978 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x980 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x988 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x990 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x998 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x9A0 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x9A8 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x9B0 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x9B8 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x9C0 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x9C8 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x9D0 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x9D8 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x9E0 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x9E8 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x9F0 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0x9F8 */ dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l, dg_op_lbpx_mx_l,
    /* 0xA00 */ dg_op_adc_xh_i, dg_op_adc_xh_i, dg_op_adc_xh_i, dg_op_adc_xh_i, dg_op_adc_xh_i, dg_op_adc_xh_i, dg_op_adc_xh_i, dg_op_adc_xh_i,
    /* 0xA08 */ dg_op_adc_xh_i, dg_op_adc_xh_i, dg_op_adc_xh_i, dg_op_adc_xh_i, dg_op_adc_xh_i, dg_op_adc_xh_i, dg_op_adc_xh_i, dg_op_adc_xh_i,
    /* 0xA10 */ dg_op_adc_xl_i, dg_op_adc_xl_i, dg_op_adc_xl_i, dg_op_adc_xl_i, dg_op_adc_xl_i, dg_op_adc_xl_i, dg_op_adc_xl_i, dg_op_adc_xl_i,
    /* 0xA18 */ dg_op_adc_xl_i, dg_op_adc_xl_i, dg_op_adc_xl_i, dg_op_adc_xl_i, dg_op_adc_xl_i, dg_op_adc_xl_i, dg_op_adc_xl_i, dg_op_adc_xl_i,
    /* 0xA20 */ dg_op_adc_yh_i, dg_op_adc_yh_i, dg_op_adc_yh_i, dg_op_adc_yh_i, dg_op_adc_yh_i, dg_op_adc_yh_i, dg_op_adc_yh_i, dg_op_adc_yh_i,
    /* 0xA28 */ dg_op_adc_yh_i, dg_op_adc_yh_i, dg_op_adc_yh_i, dg_op_adc_yh_i, dg_op_adc_yh_i, dg_op_adc_yh_i, dg_op_adc_yh_i, dg_op_adc_yh_i,
    /* 0xA30 */ dg_op_adc_yl_i, dg_op_adc_yl_i, dg_op_adc_yl_i, dg_op_adc_yl_i, dg_op_adc_yl_i, dg_op_adc_yl_i, dg_op_adc_yl_i, dg_op_adc_yl_i,
    /* 0xA38 */ dg_op_adc_yl_i, dg_op_adc_yl_i, dg_op_adc_yl_i, dg_op_adc_yl_i, dg_op_adc_yl_i, dg_op_adc_yl_i, dg_op_adc_yl_i, dg_op_adc_yl_i,
    /* 0xA40 */ dg_op_cp_xh_i, dg_op_cp_xh_i, dg_op_cp_xh_i, dg_op_cp_xh_i, dg_op_cp_xh_i, dg_op_cp_xh_i, dg_op_cp_xh_i, dg_op_cp_xh_i,
    /* 0xA48 */ dg_op_cp_xh_i, dg_op_cp_xh_i, dg_op_cp_xh_i, dg_op_cp_xh_i, dg_op_cp_xh_i, dg_op_cp_xh_i, dg_op_cp_xh_i, dg_op_cp_xh_i,
    /* 0xA50 */ dg_op_cp_xl_i, dg_op_cp_xl_i, dg_op_cp_xl_i, dg_op_cp_xl_i, dg_op_cp_xl_i, dg_op_cp_xl_i, dg_op_cp_xl_i, dg_op_cp_xl_i,
    /* 0xA58 */ dg_op_cp_xl_i, dg_op_cp_xl_i, dg_op_cp_xl_i, dg_op_cp_xl_i, dg_op_cp_xl_i, dg_op_cp_xl_i, dg_op_cp_xl_i, dg_op_cp_xl_i,
    /* 0xA60 */ dg_op_cp_yh_i, dg_op_cp_yh_i, dg_op_cp_yh_i, dg_op_cp_yh_i, dg_op_cp_yh_i, dg_op_cp_yh_i, dg_op_cp_yh_i, dg_op_cp_yh_i,
    /* 0xA68 */ dg_op_cp_yh_i, dg_op_cp_yh_i, dg_op_cp_yh_i, dg_op_cp_yh_i, dg_op_cp_yh_i, dg_op_cp_yh_i, dg_op_cp_yh_i, dg_op_cp_yh_i,
    /* 0xA70 */ dg_op_cp_yl_i, dg_op_cp_yl_i, dg_op_cp_yl_i, dg_op_cp_yl_i, dg_op_cp_yl_i, dg_op_cp_yl_i, dg_op_cp_yl_i, dg_op_cp_yl_i,
    /* 0xA78 */ dg_op_cp_yl_i, dg_op_cp_yl_i, dg_op_cp_yl_i, dg_op_cp_yl_i, dg_op_cp_yl_i, dg_op_cp_yl_i, dg_op_cp_yl_i, dg_op_cp_yl_i,
    /* 0xA80 */ dg_op_add_r_q, dg_op_add_r_q, dg_op_add_r_q, dg_op_add_r_q, dg_op_add_r_q, dg_op_add_r_q, dg_op_add_r_q, dg_op_add_r_q,
    /* 0xA88 */ dg_op_add_r_q, dg_op_add_r_q, dg_op_add_r_q, dg_op_add_r_q, dg_op_add_r_q, dg_op_add_r_q, dg_op_add_r_q, dg_op_add_r_q,
    /* 0xA90 */ dg_op_adc_r_q, dg_op_adc_r_q, dg_op_adc_r_q, dg_op_adc_r_q, dg_op_adc_r_q, dg_op_adc_r_q, dg_op_adc_r_q, dg_op_adc_r_q,
    /* 0xA98 */ dg_op_adc_r_q, dg_op_adc_r_q, dg_op_adc_r_q, dg_op_adc_r_q, dg_op_adc_r_q, dg_op_adc_r_q, dg_op_adc_r_q, dg_op_adc_r_q,
    /* 0xAA0 */ dg_op_sub_r_q, dg_op_sub_r_q, dg_op_sub_r_q, dg_op_sub_r_q, dg_op_sub_r_q, dg_op_sub_r_q, dg_op_sub_r_q, dg_op_sub_r_q,
    /* 0xAA8 */ dg_op_sub_r_q, dg_op_sub_r_q, dg_op_sub_r_q, dg_op_sub_r_q, dg_op_sub_r_q, dg_op_sub_r_q, dg_op_sub_r_q, dg_op_sub_r_q,
    /* 0xAB0 */ dg_op_sbc_r_q, dg_op_sbc_r_q, dg_op_sbc_r_q, dg_op_sbc_r_q, dg_op_sbc_r_q, dg_op_sbc_r_q, dg_op_sbc_r_q, dg_op_sbc_r_q,
    /* 0xAB8 */ dg_op_sbc_r_q, dg_op_sbc_r_q, dg_op_sbc_r_q, dg_op_sbc_r_q, dg_op_sbc_r_q, dg_op_sbc_r_q, dg_op_sbc_r_q, dg_op_sbc_r_q,
    /* 0xAC0 */ dg_op_and_r_q, dg_op_and_r_q, dg_op_and_r_q, dg_op_and_r_q, dg_op_and_r_q, dg_op_and_r_q, dg_op_and_r_q, dg_op_and_r_q,
    /* 0xAC8 */ dg_op_and_r_q, dg_op_and_r_q, dg_op_and_r_q, dg_op_and_r_q, dg_op_and_r_q, dg_op_and_r_q, dg_op_and_r_q, dg_op_and_r_q,
    /* 0xAD0 */ dg_op_or_r_q, dg_op_or_r_q, dg_op_or_r_q, dg_op_or_r_q, dg_op_or_r_q, dg_op_or_r_q, dg_op_or_r_q, dg_op_or_r_q,
    /* 0xAD8 */ dg_op_or_r_q, dg_op_or_r_q, dg_op_or_r_q, dg_op_or_r_q, dg_op_or_r_q, dg_op_or_r_q, dg_op_or_r_q, dg_op_or_r_q,
    /* 0xAE0 */ dg_op_xor_r_q, dg_op_xor_r_q, dg_op_xor_r_q, dg_op_xor_r_q, dg_op_xor_r_q, dg_op_xor_r_q, dg_op_xor_r_q, dg_op_xor_r_q,
    /* 0xAE8 */ dg_op_xor_r_q, dg_op_xor_r_q, dg_op_xor_r_q, dg_op_xor_r_q, dg_op_xor_r_q, dg_op_xor_r_q, dg_op_xor_r_q, dg_op_xor_r_q,
    /* 0xAF0 */ dg_op_rlc_r, dg_op_rlc_r, dg_op_rlc_r, dg_op_rlc_r, dg_op_rlc_r, dg_op_rlc_r, dg_op_rlc_r, dg_op_rlc_r,
    /* 0xAF8 */ dg_op_rlc_r, dg_op_rlc_r, dg_op_rlc_r, dg_op_rlc_r, dg_op_rlc_r, dg_op_rlc_r, dg_op_rlc_r, dg_op_rlc_r,
    /* 0xB00 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xB08 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xB10 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xB18 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xB20 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xB28 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xB30 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xB38 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xB40 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xB48 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xB50 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xB58 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xB60 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xB68 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xB70 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xB78 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xB80 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xB88 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xB90 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xB98 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xBA0 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xBA8 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xBB0 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xBB8 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xBC0 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xBC8 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xBD0 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xBD8 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xBE0 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xBE8 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xBF0 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xBF8 */ dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x, dg_op_ld_x_x,
    /* 0xC00 */ dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i,
    /* 0xC08 */ dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i,
    /* 0xC10 */ dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i,
    /* 0xC18 */ dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i,
    /* 0xC20 */ dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i,
    /* 0xC28 */ dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i,
    /* 0xC30 */ dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i,
    /* 0xC38 */ dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i, dg_op_add_r_i,
    /* 0xC40 */ dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i,
    /* 0xC48 */ dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i,
    /* 0xC50 */ dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i,
    /* 0xC58 */ dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i,
    /* 0xC60 */ dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i,
    /* 0xC68 */ dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i,
    /* 0xC70 */ dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i,
    /* 0xC78 */ dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i, dg_op_adc_r_i,
    /* 0xC80 */ dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i,
    /* 0xC88 */ dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i,
    /* 0xC90 */ dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i,
    /* 0xC98 */ dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i,
    /* 0xCA0 */ dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i,
    /* 0xCA8 */ dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i,
    /* 0xCB0 */ dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i,
    /* 0xCB8 */ dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i, dg_op_and_r_i,
    /* 0xCC0 */ dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i,
    /* 0xCC8 */ dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i,
    /* 0xCD0 */ dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i,
    /* 0xCD8 */ dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i,
    /* 0xCE0 */ dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i,
    /* 0xCE8 */ dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i,
    /* 0xCF0 */ dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i,
    /* 0xCF8 */ dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i, dg_op_or_r_i,
    /* 0xD00 */ dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i,
    /* 0xD08 */ dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i,
    /* 0xD10 */ dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i,
    /* 0xD18 */ dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i,
    /* 0xD20 */ dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i,
    /* 0xD28 */ dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i,
    /* 0xD30 */ dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i,
    /* 0xD38 */ dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i, dg_op_xor_r_i,
    /* 0xD40 */ dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i,
    /* 0xD48 */ dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i,
    /* 0xD50 */ dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i,
    /* 0xD58 */ dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i,
    /* 0xD60 */ dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i,
    /* 0xD68 */ dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i,
    /* 0xD70 */ dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i,
    /* 0xD78 */ dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i, dg_op_sbc_r_i,
    /* 0xD80 */ dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i,
    /* 0xD88 */ dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i,
    /* 0xD90 */ dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i,
    /* 0xD98 */ dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i,
    /* 0xDA0 */ dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i,
    /* 0xDA8 */ dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i,
    /* 0xDB0 */ dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i,
    /* 0xDB8 */ dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i, dg_op_fan_r_i,
    /* 0xDC0 */ dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i,
    /* 0xDC8 */ dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i,
    /* 0xDD0 */ dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i,
    /* 0xDD8 */ dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i,
    /* 0xDE0 */ dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i,
    /* 0xDE8 */ dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i,
    /* 0xDF0 */ dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i,
    /* 0xDF8 */ dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i, dg_op_cp_r_i,
    /* 0xE00 */ dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i,
    /* 0xE08 */ dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i,
    /* 0xE10 */ dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i,
    /* 0xE18 */ dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i,
    /* 0xE20 */ dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i,
    /* 0xE28 */ dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i,
    /* 0xE30 */ dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i,
    /* 0xE38 */ dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i, dg_op_ld_r_i,
    /* 0xE40 */ dg_op_pset_p, dg_op_pset_p, dg_op_pset_p, dg_op_pset_p, dg_op_pset_p, dg_op_pset_p, dg_op_pset_p, dg_op_pset_p,
    /* 0xE48 */ dg_op_pset_p, dg_op_pset_p, dg_op_pset_p, dg_op_pset_p, dg_op_pset_p, dg_op_pset_p, dg_op_pset_p, dg_op_pset_p,
    /* 0xE50 */ dg_op_pset_p, dg_op_pset_p, dg_op_pset_p, dg_op_pset_p, dg_op_pset_p, dg_op_pset_p, dg_op_pset_p, dg_op_pset_p,
    /* 0xE58 */ dg_op_pset_p, dg_op_pset_p, dg_op_pset_p, dg_op_pset_p, dg_op_pset_p, dg_op_pset_p, dg_op_pset_p, dg_op_pset_p,
    /* 0xE60 */ dg_op_ldpx_mx_i, dg_op_ldpx_mx_i, dg_op_ldpx_mx_i, dg_op_ldpx_mx_i, dg_op_ldpx_mx_i, dg_op_ldpx_mx_i, dg_op_ldpx_mx_i, dg_op_ldpx_mx_i,
    /* 0xE68 */ dg_op_ldpx_mx_i, dg_op_ldpx_mx_i, dg_op_ldpx_mx_i, dg_op_ldpx_mx_i, dg_op_ldpx_mx_i, dg_op_ldpx_mx_i, dg_op_ldpx_mx_i, dg_op_ldpx_mx_i,
    /* 0xE70 */ dg_op_ldpy_my_i, dg_op_ldpy_my_i, dg_op_ldpy_my_i, dg_op_ldpy_my_i, dg_op_ldpy_my_i, dg_op_ldpy_my_i, dg_op_ldpy_my_i, dg_op_ldpy_my_i,
    /* 0xE78 */ dg_op_ldpy_my_i, dg_op_ldpy_my_i, dg_op_ldpy_my_i, dg_op_ldpy_my_i, dg_op_ldpy_my_i, dg_op_ldpy_my_i, dg_op_ldpy_my_i, dg_op_ldpy_my_i,
    /* 0xE80 */ dg_op_ld_xp_r, dg_op_ld_xp_r, dg_op_ld_xp_r, dg_op_ld_xp_r, dg_op_ld_xh_r, dg_op_ld_xh_r, dg_op_ld_xh_r, dg_op_ld_xh_r,
    /* 0xE88 */ dg_op_ld_xl_r, dg_op_ld_xl_r, dg_op_ld_xl_r, dg_op_ld_xl_r, dg_op_rrc_r, dg_op_rrc_r, dg_op_rrc_r, dg_op_rrc_r,
    /* 0xE90 */ dg_op_ld_yp_r, dg_op_ld_yp_r, dg_op_ld_yp_r, dg_op_ld_yp_r, dg_op_ld_yh_r, dg_op_ld_yh_r, dg_op_ld_yh_r, dg_op_ld_yh_r,
    /* 0xE98 */ dg_op_ld_yl_r, dg_op_ld_yl_r, dg_op_ld_yl_r, dg_op_ld_yl_r, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy,
    /* 0xEA0 */ dg_op_ld_r_xp, dg_op_ld_r_xp, dg_op_ld_r_xp, dg_op_ld_r_xp, dg_op_ld_r_xh, dg_op_ld_r_xh, dg_op_ld_r_xh, dg_op_ld_r_xh,
    /* 0xEA8 */ dg_op_ld_r_xl, dg_op_ld_r_xl, dg_op_ld_r_xl, dg_op_ld_r_xl, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy,
    /* 0xEB0 */ dg_op_ld_r_yp, dg_op_ld_r_yp, dg_op_ld_r_yp, dg_op_ld_r_yp, dg_op_ld_r_yh, dg_op_ld_r_yh, dg_op_ld_r_yh, dg_op_ld_r_yh,
    /* 0xEB8 */ dg_op_ld_r_yl, dg_op_ld_r_yl, dg_op_ld_r_yl, dg_op_ld_r_yl, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy,
    /* 0xEC0 */ dg_op_ld_r_q, dg_op_ld_r_q, dg_op_ld_r_q, dg_op_ld_r_q, dg_op_ld_r_q, dg_op_ld_r_q, dg_op_ld_r_q, dg_op_ld_r_q,
    /* 0xEC8 */ dg_op_ld_r_q, dg_op_ld_r_q, dg_op_ld_r_q, dg_op_ld_r_q, dg_op_ld_r_q, dg_op_ld_r_q, dg_op_ld_r_q, dg_op_ld_r_q,
    /* 0xED0 */ dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy,
    /* 0xED8 */ dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy,
    /* 0xEE0 */ dg_op_ldpx_r_q, dg_op_ldpx_r_q, dg_op_ldpx_r_q, dg_op_ldpx_r_q, dg_op_ldpx_r_q, dg_op_ldpx_r_q, dg_op_ldpx_r_q, dg_op_ldpx_r_q,
    /* 0xEE8 */ dg_op_ldpx_r_q, dg_op_ldpx_r_q, dg_op_ldpx_r_q, dg_op_ldpx_r_q, dg_op_ldpx_r_q, dg_op_ldpx_r_q, dg_op_ldpx_r_q, dg_op_ldpx_r_q,
    /* 0xEF0 */ dg_op_ldpy_r_q, dg_op_ldpy_r_q, dg_op_ldpy_r_q, dg_op_ldpy_r_q, dg_op_ldpy_r_q, dg_op_ldpy_r_q, dg_op_ldpy_r_q, dg_op_ldpy_r_q,
    /* 0xEF8 */ dg_op_ldpy_r_q, dg_op_ldpy_r_q, dg_op_ldpy_r_q, dg_op_ldpy_r_q, dg_op_ldpy_r_q, dg_op_ldpy_r_q, dg_op_ldpy_r_q, dg_op_ldpy_r_q,
    /* 0xF00 */ dg_op_cp_r_q, dg_op_cp_r_q, dg_op_cp_r_q, dg_op_cp_r_q, dg_op_cp_r_q, dg_op_cp_r_q, dg_op_cp_r_q, dg_op_cp_r_q,
    /* 0xF08 */ dg_op_cp_r_q, dg_op_cp_r_q, dg_op_cp_r_q, dg_op_cp_r_q, dg_op_cp_r_q, dg_op_cp_r_q, dg_op_cp_r_q, dg_op_cp_r_q,
    /* 0xF10 */ dg_op_fan_r_q, dg_op_fan_r_q, dg_op_fan_r_q, dg_op_fan_r_q, dg_op_fan_r_q, dg_op_fan_r_q, dg_op_fan_r_q, dg_op_fan_r_q,
    /* 0xF18 */ dg_op_fan_r_q, dg_op_fan_r_q, dg_op_fan_r_q, dg_op_fan_r_q, dg_op_fan_r_q, dg_op_fan_r_q, dg_op_fan_r_q, dg_op_fan_r_q,
    /* 0xF20 */ dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy,
    /* 0xF28 */ dg_op_acpx_mx_r, dg_op_acpx_mx_r, dg_op_acpx_mx_r, dg_op_acpx_mx_r, dg_op_acpy_my_r, dg_op_acpy_my_r, dg_op_acpy_my_r, dg_op_acpy_my_r,
    /* 0xF30 */ dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy,
    /* 0xF38 */ dg_op_scpx_mx_r, dg_op_scpx_mx_r, dg_op_scpx_mx_r, dg_op_scpx_mx_r, dg_op_scpy_my_r, dg_op_scpy_my_r, dg_op_scpy_my_r, dg_op_scpy_my_r,
    /* 0xF40 */ dg_op_set_f_i, dg_op_set_f_i, dg_op_set_f_i, dg_op_set_f_i, dg_op_set_f_i, dg_op_set_f_i, dg_op_set_f_i, dg_op_set_f_i,
    /* 0xF48 */ dg_op_set_f_i, dg_op_set_f_i, dg_op_set_f_i, dg_op_set_f_i, dg_op_set_f_i, dg_op_set_f_i, dg_op_set_f_i, dg_op_set_f_i,
    /* 0xF50 */ dg_op_rst_f_i, dg_op_rst_f_i, dg_op_rst_f_i, dg_op_rst_f_i, dg_op_rst_f_i, dg_op_rst_f_i, dg_op_rst_f_i, dg_op_rst_f_i,
    /* 0xF58 */ dg_op_rst_f_i, dg_op_rst_f_i, dg_op_rst_f_i, dg_op_rst_f_i, dg_op_rst_f_i, dg_op_rst_f_i, dg_op_rst_f_i, dg_op_rst_f_i,
    /* 0xF60 */ dg_op_inc_mn, dg_op_inc_mn, dg_op_inc_mn, dg_op_inc_mn, dg_op_inc_mn, dg_op_inc_mn, dg_op_inc_mn, dg_op_inc_mn,
    /* 0xF68 */ dg_op_inc_mn, dg_op_inc_mn, dg_op_inc_mn, dg_op_inc_mn, dg_op_inc_mn, dg_op_inc_mn, dg_op_inc_mn, dg_op_inc_mn,
    /* 0xF70 */ dg_op_dec_mn, dg_op_dec_mn, dg_op_dec_mn, dg_op_dec_mn, dg_op_dec_mn, dg_op_dec_mn, dg_op_dec_mn, dg_op_dec_mn,
    /* 0xF78 */ dg_op_dec_mn, dg_op_dec_mn, dg_op_dec_mn, dg_op_dec_mn, dg_op_dec_mn, dg_op_dec_mn, dg_op_dec_mn, dg_op_dec_mn,
    /* 0xF80 */ dg_op_ld_mn_a, dg_op_ld_mn_a, dg_op_ld_mn_a, dg_op_ld_mn_a, dg_op_ld_mn_a, dg_op_ld_mn_a, dg_op_ld_mn_a, dg_op_ld_mn_a,
    /* 0xF88 */ dg_op_ld_mn_a, dg_op_ld_mn_a, dg_op_ld_mn_a, dg_op_ld_mn_a, dg_op_ld_mn_a, dg_op_ld_mn_a, dg_op_ld_mn_a, dg_op_ld_mn_a,
    /* 0xF90 */ dg_op_ld_mn_b, dg_op_ld_mn_b, dg_op_ld_mn_b, dg_op_ld_mn_b, dg_op_ld_mn_b, dg_op_ld_mn_b, dg_op_ld_mn_b, dg_op_ld_mn_b,
    /* 0xF98 */ dg_op_ld_mn_b, dg_op_ld_mn_b, dg_op_ld_mn_b, dg_op_ld_mn_b, dg_op_ld_mn_b, dg_op_ld_mn_b, dg_op_ld_mn_b, dg_op_ld_mn_b,
    /* 0xFA0 */ dg_op_ld_a_mn, dg_op_ld_a_mn, dg_op_ld_a_mn, dg_op_ld_a_mn, dg_op_ld_a_mn, dg_op_ld_a_mn, dg_op_ld_a_mn, dg_op_ld_a_mn,
    /* 0xFA8 */ dg_op_ld_a_mn, dg_op_ld_a_mn, dg_op_ld_a_mn, dg_op_ld_a_mn, dg_op_ld_a_mn, dg_op_ld_a_mn, dg_op_ld_a_mn, dg_op_ld_a_mn,
    /* 0xFB0 */ dg_op_ld_b_mn, dg_op_ld_b_mn, dg_op_ld_b_mn, dg_op_ld_b_mn, dg_op_ld_b_mn, dg_op_ld_b_mn, dg_op_ld_b_mn, dg_op_ld_b_mn,
    /* 0xFB8 */ dg_op_ld_b_mn, dg_op_ld_b_mn, dg_op_ld_b_mn, dg_op_ld_b_mn, dg_op_ld_b_mn, dg_op_ld_b_mn, dg_op_ld_b_mn, dg_op_ld_b_mn,
    /* 0xFC0 */ dg_op_push_r, dg_op_push_r, dg_op_push_r, dg_op_push_r, dg_op_push_xp, dg_op_push_xh, dg_op_push_xl, dg_op_push_yp,
    /* 0xFC8 */ dg_op_push_yh, dg_op_push_yl, dg_op_push_f, dg_op_dec_sp, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy,
    /* 0xFD0 */ dg_op_pop_r, dg_op_pop_r, dg_op_pop_r, dg_op_pop_r, dg_op_pop_xp, dg_op_pop_xh, dg_op_pop_xl, dg_op_pop_yp,
    /* 0xFD8 */ dg_op_pop_yh, dg_op_pop_yl, dg_op_pop_f, dg_op_inc_sp, dg_op_dummy, dg_op_dummy, dg_op_rets, dg_op_ret,
    /* 0xFE0 */ dg_op_ld_sph_r, dg_op_ld_sph_r, dg_op_ld_sph_r, dg_op_ld_sph_r, dg_op_ld_r_sph, dg_op_ld_r_sph, dg_op_ld_r_sph, dg_op_ld_r_sph,
    /* 0xFE8 */ dg_op_jpba, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_dummy,
    /* 0xFF0 */ dg_op_ld_spl_r, dg_op_ld_spl_r, dg_op_ld_spl_r, dg_op_ld_spl_r, dg_op_ld_r_spl, dg_op_ld_r_spl, dg_op_ld_r_spl, dg_op_ld_r_spl,
    /* 0xFF8 */ dg_op_halt, dg_op_dummy, dg_op_dummy, dg_op_nop5, dg_op_dummy, dg_op_dummy, dg_op_dummy, dg_op_nop7,
};

/* --------------------------------------------------------------- helpers -- */

#define DG_ADV()  do { c->PC = c->NPC = (uint16_t)((c->PC & 0x1000) | ((c->PC + 1) & 0xFFF)); } while (0)

static int dg_rom_get_word(const dg_cpu *c, uint32_t addr)
{
    uint32_t size = c->rom_size;
    if (size == 0 || c->rom == NULL) {
        return 0xFFFF;
    }
    return ((int)c->rom[addr % size] << 8) | (int)c->rom[(addr + 1) % size];
}

/* ---------------------------------------------------------------- sound -- */

static inline __attribute__((always_inline)) void dg_sound_clock(dg_cpu *c)
{
    dg_sound *s = &c->sound;
    if (s->one_shot_counter > 0) {
        s->one_shot_counter -= 1;
        /* emit_audio(None) -- no-op */
    }
    if (s->envelope_counter > 0) {
        s->envelope_counter -= 1;
        if (s->envelope_counter <= 0) {
            if (s->envelope_step > 0) {
                /* emit_audio(freq, ...) -- no-op */
                s->envelope_step -= 1;
            }
            s->envelope_counter = s->envelope_cycle;
        }
    }
}

static void dg_sound_set_freq(dg_cpu *c, int value)
{
    c->sound.buzzer_freq = (double)OSC1_CLOCK / (double)dg_buzzer_freq_div[value];
}

static void dg_sound_set_envelope_on(dg_cpu *c)
{
    c->sound.envelope_on = 1;
    c->sound.envelope_step = 7;
}

static void dg_sound_set_envelope_off(dg_cpu *c)
{
    c->sound.envelope_on = 0;
    c->sound.envelope_step = 7;
    c->sound.envelope_counter = 0;
}

static void dg_sound_set_envelope_cycle(dg_cpu *c, int cycle)
{
    c->sound.envelope_cycle = dg_envelope_cycle_div[cycle];
}

static void dg_sound_reset_envelope(dg_cpu *c)
{
    c->sound.envelope_step = 7;
}

static void dg_sound_one_shot(dg_cpu *c, int duration)
{
    if (c->sound.one_shot_counter == 0) {
        c->sound.one_shot_counter = dg_one_shot_pw_div[duration];
    }
}

static void dg_sound_set_buzzer_on(dg_cpu *c)
{
    c->sound.sound_on = 1;
    c->sound.one_shot_counter = 0;
    if (c->sound.envelope_on) {
        c->sound.envelope_counter = c->sound.envelope_cycle;
    }
}

static void dg_sound_set_buzzer_off(dg_cpu *c)
{
    c->sound.sound_on = 0;
    c->sound.envelope_counter = 0;
    c->sound.one_shot_counter = 0;
}

static int dg_sound_is_one_shot_ringing(const dg_cpu *c)
{
    return c->sound.one_shot_counter > 0;
}

/* ------------------------------------------------------------ io / mem -- */

static int dg_port_read(dg_cpu *c, int p)
{
    int pullup = (int)c->pullup_ext[p] | (int)c->PULLUP[p];
    return (int)(((~(int)c->PDIR[p]) &
                  ((int)c->port_input[p][1] |
                   (pullup & ~(int)c->port_input[p][0]))) |
                 ((int)c->PDIR[p] & (int)c->PLATCH[p] &
                  (pullup | (int)c->PUSHPULL[p])));
}

/* ---- probe instrumentation (not part of the shipped core) ---- */
struct dg_io_ev { uint16_t addr; uint8_t val; uint8_t rw; uint16_t pc; uint32_t instr; };
struct dg_io_ev g_io_ev[65536];
unsigned g_io_ev_n;
unsigned long g_io_hits[0x80];
/* probe hook: called on every write to the P2 data latch (the D-Link pin) */
void (*g_link_p2_hook)(int value);
void (*g_link_dir_hook)(int ioc);
void (*g_link_p2_read_hook)(int value);

static void dg_io_log(dg_cpu *c, int addr, int value, int rw)
{
    if (addr < 0xF00 || addr > 0xF7F) return;
    g_io_hits[addr & 0x7F]++;
    if (g_io_ev_n < 65536) {
        g_io_ev[g_io_ev_n].addr = (uint16_t)addr;
        g_io_ev[g_io_ev_n].val = (uint8_t)value;
        g_io_ev[g_io_ev_n].rw = (uint8_t)rw;
        g_io_ev[g_io_ev_n].pc = (uint16_t)c->PC;
        g_io_ev[g_io_ev_n].instr = dg_instr_count();
        g_io_ev_n++;
    }
}

static int dg_io_get(dg_cpu *c, int addr)
{
    int ret;

    switch (addr) {
    case 0xF00: ret = c->IT;   c->IT = 0;   return ret;
    case 0xF01: ret = c->ISW;  c->ISW = 0;  return ret;
    case 0xF02: ret = c->IPT;  c->IPT = 0;  return ret;
    case 0xF03: ret = c->ISIO; c->ISIO = 0; return ret;
    case 0xF04: ret = c->IK0;  c->IK0 = 0;  return ret;
    case 0xF05: ret = c->IK1;  c->IK1 = 0;  return ret;

    case 0xF10: return c->EIT;
    case 0xF11: return c->EISW;
    case 0xF12: return c->EIPT;
    case 0xF13: return c->EISIO;
    case 0xF14: return c->EIK0;
    case 0xF15: return c->EIK1;

    case 0xF20: return c->TM & 0xF;
    case 0xF21: return (c->TM >> 4) & 0xF;
    case 0xF22: return c->SWL & 0xF;
    case 0xF23: return c->SWH & 0xF;
    case 0xF24: return c->PT & 0xF;
    case 0xF25: return (c->PT >> 4) & 0xF;
    case 0xF26: return c->RD & 0xF;
    case 0xF27: return (c->RD >> 4) & 0xF;

    case 0xF30: return c->SD & 0xF;
    case 0xF31: return (c->SD >> 4) & 0xF;

    case 0xF40: return dg_port_read(c, PORT_K0);
    case 0xF41: return c->DFK0;
    case 0xF42: return dg_port_read(c, PORT_K1);

    case 0xF50: return c->PLATCH[PORT_R0];
    case 0xF51: return c->PLATCH[PORT_R1];
    case 0xF52: return c->PLATCH[PORT_R2];
    case 0xF53: return c->PLATCH[PORT_R3];
    case 0xF54: return c->PLATCH[PORT_R4];

    case 0xF60: return dg_port_read(c, PORT_P0);
    case 0xF61: return dg_port_read(c, PORT_P1);
    case 0xF62: return dg_port_read(c, PORT_P2);
    case 0xF63: return dg_port_read(c, PORT_P3);

    case 0xF70: return c->CTRL_OSC;
    case 0xF71: return c->CTRL_LCD;
    case 0xF72: return c->LC;
    case 0xF73: return 0;                       /* ctrl_svd read -> 0 */
    case 0xF74: return c->CTRL_BZ1;
    case 0xF75: return (int)((c->CTRL_BZ2 & (IO_ENVRT | IO_ENVON)) |
                             (IO_BZSHOT * dg_sound_is_one_shot_ringing(c)));
    case 0xF76: return 0;                       /* get dummy */
    case 0xF77: return c->CTRL_SW & IO_SWRUN;
    case 0xF78: return c->CTRL_PT & IO_PTRUN;
    case 0xF79: return c->PTC;
    case 0xF7A: return 0;                       /* get dummy */
    case 0xF7B: return 0;                       /* get dummy */
    case 0xF7D: return c->IOC;
    case 0xF7E: return c->PUP;
    default:    return 0;
    }
}

static void dg_io_set(dg_cpu *c, int addr, int value)
{
    dg_io_log(c, addr, value, 1);
    if (addr == 0xF62 && g_link_p2_hook) g_link_p2_hook(value);
    if (addr == 0xF7D && g_link_dir_hook) g_link_dir_hook(value);
    switch (addr) {
    case 0xF10: c->EIT = (uint8_t)value; break;
    case 0xF11: c->EISW = (uint8_t)(value & 0x3); break;
    case 0xF12: c->EIPT = (uint8_t)(value & 0x1); break;
    case 0xF13: c->EISIO = (uint8_t)(value & 0x1); break;
    case 0xF14: c->EIK0 = (uint8_t)value; break;
    case 0xF15: c->EIK1 = (uint8_t)value; break;

    case 0xF26: c->RD = (uint8_t)((c->RD & 0xF0) | (value & 0x0F)); break;
    case 0xF27: c->RD = (uint8_t)((c->RD & 0x0F) | ((value << 4) & 0xF0)); break;
    case 0xF30: c->SD = (uint8_t)((c->SD & 0xF0) | (value & 0x0F)); break;
    case 0xF31: c->SD = (uint8_t)((c->SD & 0x0F) | ((value << 4) & 0xF0)); break;

    case 0xF41: c->DFK0 = (uint8_t)value; break;

    case 0xF50: c->PLATCH[PORT_R0] = (uint8_t)value; break;
    case 0xF51: c->PLATCH[PORT_R1] = (uint8_t)value; break;
    case 0xF52: c->PLATCH[PORT_R2] = (uint8_t)value; break;
    case 0xF53: c->PLATCH[PORT_R3] = (uint8_t)value; break;
    case 0xF54:
        c->PLATCH[PORT_R4] = (uint8_t)value;
        if (value & IO_R33) {
            dg_sound_set_buzzer_off(c);
        } else {
            dg_sound_set_buzzer_on(c);
        }
        break;

    case 0xF60: c->PLATCH[PORT_P0] = (uint8_t)value; break;
    case 0xF61: c->PLATCH[PORT_P1] = (uint8_t)value; break;
    case 0xF62: c->PLATCH[PORT_P2] = (uint8_t)value; break;
    case 0xF63: c->PLATCH[PORT_P3] = (uint8_t)value; break;

    case 0xF70: c->CTRL_OSC = (uint8_t)value; break;
    case 0xF71: c->CTRL_LCD = (uint8_t)value; break;
    case 0xF72: c->LC = (uint8_t)value; break;
    case 0xF74:
        c->CTRL_BZ1 = (uint8_t)value;
        dg_sound_set_freq(c, c->CTRL_BZ1 & IO_BZFQ);
        break;
    case 0xF75:
        c->CTRL_BZ2 = (uint8_t)(value & (IO_ENVRT | IO_ENVON));
        dg_sound_set_envelope_cycle(c, (int)((value & IO_ENVRT) > 0));
        if (value & IO_BZSHOT) {
            dg_sound_one_shot(c, (int)((c->CTRL_BZ1 & IO_SHOTPW) > 0));
        }
        if (value & IO_ENVON) {
            dg_sound_set_envelope_on(c);
        } else {
            dg_sound_set_envelope_off(c);
        }
        if (value & IO_ENVRST) {
            dg_sound_reset_envelope(c);
        }
        break;
    case 0xF76:
        if (value & IO_TMRST) {
            c->TM = 0;
        }
        break;
    case 0xF77:
        if (value & IO_SWRST) {
            c->SWL = c->SWH = 0;
        }
        c->CTRL_SW = (uint8_t)(value & IO_SWRUN);
        break;
    case 0xF78:
        if (value & IO_PTRST) {
            c->PT = c->RD;
        }
        c->CTRL_PT = (uint8_t)(value & IO_PTRUN);
        break;
    case 0xF79: c->PTC = (uint8_t)value; break;

    case 0xF7D:
        c->IOC = (uint8_t)value;
        c->PDIR[PORT_P0] = (uint8_t)(((value & IO_IOC0) > 0) * 0xF);
        c->PDIR[PORT_P1] = (uint8_t)(((value & IO_IOC1) > 0) * 0xF);
        c->PDIR[PORT_P2] = (uint8_t)(((value & IO_IOC2) > 0) * 0xF);
        c->PDIR[PORT_P3] = (uint8_t)(((value & IO_IOC3) > 0) * 0xF);
        break;

    case 0xF7E:
        c->PUP = (uint8_t)value;
        c->PULLUP[PORT_P0] = (uint8_t)(((value & IO_PUP0) == 0) * 0xF);
        c->PULLUP[PORT_P1] = (uint8_t)(((value & IO_PUP1) == 0) * 0xF);
        c->PULLUP[PORT_P2] = (uint8_t)(((value & IO_PUP2) == 0) * 0xF);
        c->PULLUP[PORT_P3] = (uint8_t)(((value & IO_PUP3) == 0) * 0xF);
        break;

    default:
        break;
    }
}

static int dg_get_mem(dg_cpu *c, int addr)
{
    if (addr < RAM_SIZE) {
        return (int)c->ram[addr];
    }
    if (addr >= VRAM_PART1_OFFSET && addr < VRAM_PART1_OFFSET + VRAM_PART_SIZE) {
        return (int)c->vram[addr - VRAM_PART1_OFFSET];
    }
    if (addr >= VRAM_PART2_OFFSET && addr < VRAM_PART2_OFFSET + VRAM_PART_SIZE) {
        return (int)c->vram[addr - VRAM_PART2_OFFSET + VRAM_PART_SIZE];
    }
    if (addr >= IORAM_OFFSET && addr < IORAM_OFFSET + IORAM_SIZE) {
        int v = dg_io_get(c, addr);
        dg_io_log(c, addr, v, 0);
        if (addr == 0xF62 && g_link_p2_read_hook) g_link_p2_read_hook(v);
        return v;
    }
    return 0;
}

static void dg_set_mem(dg_cpu *c, int addr, int value)
{
    if (addr < RAM_SIZE) {
        c->ram[addr] = (uint8_t)(value & 0xF);
    } else if (addr >= VRAM_PART1_OFFSET && addr < VRAM_PART1_OFFSET + VRAM_PART_SIZE) {
        c->vram[addr - VRAM_PART1_OFFSET] = (uint8_t)(value & 0xF);
    } else if (addr >= VRAM_PART2_OFFSET && addr < VRAM_PART2_OFFSET + VRAM_PART_SIZE) {
        c->vram[addr - VRAM_PART2_OFFSET + VRAM_PART_SIZE] = (uint8_t)(value & 0xF);
    } else if (addr >= IORAM_OFFSET && addr < IORAM_OFFSET + IORAM_SIZE) {
        dg_io_set(c, addr, value);
    }
}

/* A / B / MX / MY accessors (index order matches the reference tables). */

static int dg_reg_get(dg_cpu *c, int r)
{
    switch (r) {
    case 0:  return (int)c->A;
    case 1:  return (int)c->B;
    case 2:  return dg_get_mem(c, (int)c->IX);
    default: return dg_get_mem(c, (int)c->IY);
    }
}

static void dg_reg_set(dg_cpu *c, int r, int value)
{
    switch (r) {
    case 0:  c->A = (uint16_t)(value & 0xF); break;
    case 1:  c->B = (uint16_t)(value & 0xF); break;
    case 2:  dg_set_mem(c, (int)c->IX, value); break;
    default: dg_set_mem(c, (int)c->IY, value); break;
    }
}

/* ------------------------------------------------------------ sub-systems -- */

static void dg_process_ptimer(dg_cpu *c)
{
    c->PT = (uint8_t)((c->PT - 1) & 0xFF);
    if (c->PT == 0) {
        c->PT = c->RD;
        c->IPT |= IO_IPT;
    }
    if (c->PTC & IO_PTCOUT) {
        c->PLATCH[PORT_R3] ^= IO_R33;
    }
}

static void dg_process_stopwatch(dg_cpu *c)
{
    if (c->CTRL_SW & IO_SWRUN) {
        c->SWL = (uint8_t)((c->SWL + 1) % 10);
        if (c->SWL == 0) {
            c->SWH = (uint8_t)((c->SWH + 1) % 10);
            c->ISW |= IO_ISW1;
            if (c->SWH == 0) {
                c->ISW |= IO_ISW0;
            }
        }
    }
}

static void dg_process_timer(dg_cpu *c)
{
    int new_tm = (int)((c->TM + 1) & 0xFF);
    int old_tm = (int)c->TM;

    if ((new_tm & IO_TM2) < (old_tm & IO_TM2)) {
        c->IT |= IO_IT32;
    }
    if (((new_tm >> 4) & IO_TM4) < ((old_tm >> 4) & IO_TM4)) {
        c->IT |= IO_IT8;
    }
    if (((new_tm >> 4) & IO_TM6) < ((old_tm >> 4) & IO_TM6)) {
        c->IT |= IO_IT2;
    }
    if (((new_tm >> 4) & IO_TM7) < ((old_tm >> 4) & IO_TM7)) {
        c->IT |= IO_IT1;
    }
    c->TM = (uint8_t)new_tm;
}

/* Called ~7 times per dg_step(), so it must contain no floating point at all:
 * int32_t decremented by a double literal would be promoted to double and cost
 * a software floating-point op on the FPU-less C3. */
static inline __attribute__((always_inline)) void dg_clock_osc1(dg_cpu *c)
{
    c->osc1_ticks++;
    dg_sound_clock(c);

    if ((c->PTC & IO_PTC) > 1) {
        c->ptimer_counter -= 1;
        if (c->ptimer_counter <= 0) {
            c->ptimer_counter += k_ptimer_div[c->PTC & IO_PTC];
            dg_process_ptimer(c);
        }
    }

    c->stopwatch_counter -= STOPWATCH_FX;
    if (c->stopwatch_counter <= 0) {
        c->stopwatch_counter += STOPWATCH_DIV_FX;
        dg_process_stopwatch(c);
    }

    c->timer_counter -= 1;
    if (c->timer_counter <= 0) {
        c->timer_counter += k_timer_div;
        dg_process_timer(c);
    }
}

static int dg_interrupt(dg_cpu *c, uint8_t vector)
{
    dg_set_mem(c, (int)((c->SP - 1) & 0xFF), (int)((c->PC >> 8) & 0x0F));
    dg_set_mem(c, (int)((c->SP - 2) & 0xFF), (int)((c->PC >> 4) & 0x0F));
    c->SP = (uint8_t)((c->SP - 3) & 0xFF);
    dg_set_mem(c, (int)c->SP, (int)(c->PC & 0x0F));
    c->IF = 0;
    c->HALT = 0;
    c->PC = c->NPC = (uint16_t)((c->NPC & 0x1000) | 0x0100 | vector);
    return 13;
}

static void dg_pin_handler(dg_cpu *c, int port, uint8_t mask, int level)
{
    int prev_state, new_state;

    if (port < 0) {  /* 'RES' */
        if (level == -1) {
            c->RESET = 0;
        } else {
            dg_reset();
            c->RESET = 1;
        }
        return;
    }

    prev_state = dg_port_read(c, port) & mask;
    c->port_input[port][0] = (uint8_t)(c->port_input[port][0] & ~mask);
    c->port_input[port][1] = (uint8_t)(c->port_input[port][1] & ~mask);
    if (level >= 0) {
        c->port_input[port][level] |= mask;
    }

    if (port == PORT_K0) {
        new_state = dg_port_read(c, PORT_K0) & mask;
        if ((prev_state & c->EIK0) != (new_state & c->EIK0)) {
            if ((c->DFK0 & mask) != new_state) {
                c->IK0 |= IO_IK0;
                if (mask & 0x8) {
                    dg_process_ptimer(c);
                }
            }
        }
    }
    if (port == PORT_K1) {
        new_state = dg_port_read(c, PORT_K1) & mask;
        if ((prev_state & c->EIK1) != (new_state & c->EIK1)) {
            if (mask != new_state) {
                c->IK1 |= IO_IK1;
            }
        }
    }
}

static void dg_init_registers(dg_cpu *c)
{
    c->A = 0;
    c->B = 0;
    c->IX = 0;
    c->IY = 0;
    c->SP = 0;

    c->PC = 0x100;
    c->NPC = 0x100;

    c->CF = 0;
    c->ZF = 0;
    c->DF = 0;
    c->IF = 0;

    memset(c->ram, 0, sizeof(c->ram));
    memset(c->vram, 0, sizeof(c->vram));

    c->HALT = 0;

    c->PDIR[PORT_K0] = 0;
    c->PDIR[PORT_K1] = 0;
    c->PDIR[PORT_R0] = 0xF;
    c->PDIR[PORT_R1] = 0xF;
    c->PDIR[PORT_R2] = 0xF;
    c->PDIR[PORT_R3] = 0xF;
    c->PDIR[PORT_R4] = 0xF;
    c->PDIR[PORT_P0] = 0;
    c->PDIR[PORT_P1] = 0;
    c->PDIR[PORT_P2] = 0;
    c->PDIR[PORT_P3] = 0;

    c->PUSHPULL[PORT_K0] = 0;
    c->PUSHPULL[PORT_K1] = 0;
    c->PUSHPULL[PORT_R0] = 0;
    c->PUSHPULL[PORT_R1] = 0;
    c->PUSHPULL[PORT_R2] = 0;
    c->PUSHPULL[PORT_R3] = 0;
    c->PUSHPULL[PORT_R4] = 0;
    c->PUSHPULL[PORT_P0] = 0xF;
    c->PUSHPULL[PORT_P1] = 0xF;
    c->PUSHPULL[PORT_P2] = 0xF;
    c->PUSHPULL[PORT_P3] = 0xF;

    memset(c->PLATCH, 0, sizeof(c->PLATCH));

    c->PULLUP[PORT_K0] = 0;
    c->PULLUP[PORT_K1] = 0;
    c->PULLUP[PORT_R0] = 0;
    c->PULLUP[PORT_R1] = 0;
    c->PULLUP[PORT_R2] = 0;
    c->PULLUP[PORT_R3] = 0;
    c->PULLUP[PORT_R4] = 0;
    c->PULLUP[PORT_P0] = 0xF;
    c->PULLUP[PORT_P1] = 0xF;
    c->PULLUP[PORT_P2] = 0xF;
    c->PULLUP[PORT_P3] = 0xF;

    c->IT = 0;
    c->ISW = 0;
    c->IPT = 0;
    c->ISIO = 0;
    c->IK0 = 0;
    c->IK1 = 0;
    c->EIT = 0;
    c->EISW = 0;
    c->EIPT = 0;
    c->EISIO = 0;
    c->EIK0 = 0;
    c->EIK1 = 0;
    c->TM = 0;
    c->SWL = 0;
    c->SWH = 0;
    c->PT = 0;
    c->RD = 0;
    c->SD = 0;
    c->DFK0 = 0xF;
    c->CTRL_OSC = 0;
    c->CTRL_LCD = IO_ALOFF;
    c->LC = 0;
    c->CTRL_SVD = 0;
    c->CTRL_BZ1 = 0;
    c->CTRL_BZ2 = 0;
    c->CTRL_SW = 0;
    c->CTRL_PT = 0;
    c->PTC = 0;
    c->SC = 0;
    c->HZR = 0;
    c->IOC = 0;
    c->PUP = 0;
}

/* ---------------------------------------------------------- instructions -- */

DG_OP(jp_s)
{
    c->PC = (uint16_t)((c->NPC & 0x1F00) | (op & 0x0FF));
    return 5;
}

DG_OP(retd_l)
{
    c->PC = c->NPC = (uint16_t)((c->PC & 0x1000) |
                                ((uint16_t)c->ram[c->SP + 2] << 8) |
                                ((uint16_t)c->ram[c->SP + 1] << 4) |
                                (uint16_t)c->ram[c->SP]);
    c->SP = (uint8_t)((c->SP + 3) & 0xFF);
    dg_set_mem(c, (int)c->IX, op & 0x00F);
    dg_set_mem(c, (int)((c->IX & 0xF00) | ((c->IX + 1) & 0xFF)),
               (op >> 4) & 0x00F);
    c->IX = (uint16_t)((c->IX & 0xF00) | ((c->IX + 2) & 0xFF));
    return 12;
}

DG_OP(jp_c_s)
{
    if (c->CF) {
        c->PC = (uint16_t)((c->NPC & 0x1F00) | (op & 0x0FF));
    } else {
        DG_ADV();
    }
    return 5;
}

DG_OP(jp_nc_s)
{
    if (!c->CF) {
        c->PC = (uint16_t)((c->NPC & 0x1F00) | (op & 0x0FF));
    } else {
        DG_ADV();
    }
    return 5;
}

DG_OP(call_s)
{
    dg_set_mem(c, (int)((c->SP - 1) & 0xFF), (int)(((c->PC + 1) >> 8) & 0x0F));
    dg_set_mem(c, (int)((c->SP - 2) & 0xFF), (int)(((c->PC + 1) >> 4) & 0x0F));
    c->SP = (uint8_t)((c->SP - 3) & 0xFF);
    dg_set_mem(c, (int)c->SP, (int)((c->PC + 1) & 0x0F));
    c->PC = (uint16_t)((c->NPC & 0x1F00) | (op & 0x0FF));
    return 7;
}

DG_OP(calz_s)
{
    dg_set_mem(c, (int)((c->SP - 1) & 0xFF), (int)(((c->PC + 1) >> 8) & 0x0F));
    dg_set_mem(c, (int)((c->SP - 2) & 0xFF), (int)(((c->PC + 1) >> 4) & 0x0F));
    c->SP = (uint8_t)((c->SP - 3) & 0xFF);
    dg_set_mem(c, (int)c->SP, (int)((c->PC + 1) & 0x0F));
    c->PC = c->NPC = (uint16_t)((c->NPC & 0x1000) | (op & 0x0FF));
    return 7;
}

DG_OP(jp_z_s)
{
    if (c->ZF) {
        c->PC = (uint16_t)((c->NPC & 0x1F00) | (op & 0x0FF));
    } else {
        DG_ADV();
    }
    return 5;
}

DG_OP(jp_nz_s)
{
    if (!c->ZF) {
        c->PC = (uint16_t)((c->NPC & 0x1F00) | (op & 0x0FF));
    } else {
        DG_ADV();
    }
    return 5;
}

DG_OP(ld_y_y)
{
    c->IY = (uint16_t)((c->IY & 0xF00) | (op & 0x0FF));
    DG_ADV();
    return 5;
}

DG_OP(lbpx_mx_l)
{
    dg_set_mem(c, (int)c->IX, op & 0x00F);
    dg_set_mem(c, (int)((c->IX & 0xF00) | ((c->IX + 1) & 0xFF)),
               (op >> 4) & 0x00F);
    c->IX = (uint16_t)((c->IX & 0xF00) | ((c->IX + 2) & 0xFF));
    DG_ADV();
    return 5;
}

DG_OP(adc_xh_i)
{
    int xh = (int)((c->IX >> 4) & 0x00F) + (int)(op & 0x00F) + (int)c->CF;
    c->ZF = (uint8_t)((xh & 0xF) == 0);
    c->CF = (uint8_t)(xh > 15);
    c->IX = (uint16_t)((c->IX & 0xF0F) | ((xh << 4) & 0x0F0));
    DG_ADV();
    return 7;
}

DG_OP(adc_xl_i)
{
    int xl = (int)(c->IX & 0x00F) + (int)(op & 0x00F) + (int)c->CF;
    c->ZF = (uint8_t)((xl & 0xF) == 0);
    c->CF = (uint8_t)(xl > 15);
    c->IX = (uint16_t)((c->IX & 0xFF0) | (xl & 0x00F));
    DG_ADV();
    return 7;
}

DG_OP(adc_yh_i)
{
    int yh = (int)((c->IY >> 4) & 0x00F) + (int)(op & 0x00F) + (int)c->CF;
    c->ZF = (uint8_t)((yh & 0xF) == 0);
    c->CF = (uint8_t)(yh > 15);
    c->IY = (uint16_t)((c->IY & 0xF0F) | ((yh << 4) & 0x0F0));
    DG_ADV();
    return 7;
}

DG_OP(adc_yl_i)
{
    int yl = (int)(c->IY & 0x00F) + (int)(op & 0x00F) + (int)c->CF;
    c->ZF = (uint8_t)((yl & 0xF) == 0);
    c->CF = (uint8_t)(yl > 15);
    c->IY = (uint16_t)((c->IY & 0xFF0) | (yl & 0x00F));
    DG_ADV();
    return 7;
}

DG_OP(cp_xh_i)
{
    int cp = (int)((c->IX >> 4) & 0x00F) - (int)(op & 0x00F);
    c->ZF = (uint8_t)(cp == 0);
    c->CF = (uint8_t)(cp < 0);
    DG_ADV();
    return 7;
}

DG_OP(cp_xl_i)
{
    int cp = (int)(c->IX & 0x00F) - (int)(op & 0x00F);
    c->ZF = (uint8_t)(cp == 0);
    c->CF = (uint8_t)(cp < 0);
    DG_ADV();
    return 7;
}

DG_OP(cp_yh_i)
{
    int cp = (int)((c->IY >> 4) & 0x00F) - (int)(op & 0x00F);
    c->ZF = (uint8_t)(cp == 0);
    c->CF = (uint8_t)(cp < 0);
    DG_ADV();
    return 7;
}

DG_OP(cp_yl_i)
{
    int cp = (int)(c->IY & 0x00F) - (int)(op & 0x00F);
    c->ZF = (uint8_t)(cp == 0);
    c->CF = (uint8_t)(cp < 0);
    DG_ADV();
    return 7;
}

DG_OP(add_r_q)
{
    int r = (op >> 2) & 0x3;
    int q = op & 0x3;
    int a = dg_reg_get(c, r);
    int b = dg_reg_get(c, q);
    int res = a + b;
    c->CF = (uint8_t)(res > 15);
    if (c->DF && res > 9) {
        res += 6;
        c->CF = 1;
    }
    c->ZF = (uint8_t)((res & 0xF) == 0);
    dg_reg_set(c, r, res & 0xF);
    DG_ADV();
    return 7;
}

DG_OP(adc_r_q)
{
    int r = (op >> 2) & 0x3;
    int q = op & 0x3;
    int a = dg_reg_get(c, r);
    int b = dg_reg_get(c, q);
    int res = a + b + (int)c->CF;
    c->CF = (uint8_t)(res > 15);
    if (c->DF && res > 9) {
        res += 6;
        c->CF = 1;
    }
    c->ZF = (uint8_t)((res & 0xF) == 0);
    dg_reg_set(c, r, res & 0xF);
    DG_ADV();
    return 7;
}

DG_OP(sub_r_q)
{
    int r = (op >> 2) & 0x3;
    int q = op & 0x3;
    int a = dg_reg_get(c, r);
    int b = dg_reg_get(c, q);
    int res = a - b;
    c->CF = (uint8_t)(res < 0);
    if (c->DF && res < 0) {
        res += 10;
    }
    c->ZF = (uint8_t)((res & 0xF) == 0);
    dg_reg_set(c, r, res & 0xF);
    DG_ADV();
    return 7;
}

DG_OP(sbc_r_q)
{
    int r = (op >> 2) & 0x3;
    int q = op & 0x3;
    int a = dg_reg_get(c, r);
    int b = dg_reg_get(c, q);
    int res = a - b - (int)c->CF;
    c->CF = (uint8_t)(res < 0);
    if (c->DF && res < 0) {
        res += 10;
    }
    c->ZF = (uint8_t)((res & 0xF) == 0);
    dg_reg_set(c, r, res & 0xF);
    DG_ADV();
    return 7;
}

DG_OP(and_r_q)
{
    int r = (op >> 2) & 0x3;
    int q = op & 0x3;
    int a = dg_reg_get(c, r);
    int b = dg_reg_get(c, q);
    int res = a & b;
    c->ZF = (uint8_t)(res == 0);
    dg_reg_set(c, r, res);
    DG_ADV();
    return 7;
}

DG_OP(or_r_q)
{
    int r = (op >> 2) & 0x3;
    int q = op & 0x3;
    int a = dg_reg_get(c, r);
    int b = dg_reg_get(c, q);
    int res = a | b;
    c->ZF = (uint8_t)(res == 0);
    dg_reg_set(c, r, res);
    DG_ADV();
    return 7;
}

DG_OP(xor_r_q)
{
    int r = (op >> 2) & 0x3;
    int q = op & 0x3;
    int a = dg_reg_get(c, r);
    int b = dg_reg_get(c, q);
    int res = a ^ b;
    c->ZF = (uint8_t)(res == 0);
    dg_reg_set(c, r, res);
    DG_ADV();
    return 7;
}

DG_OP(rlc_r)
{
    int r = op & 0x3;
    int a = dg_reg_get(c, r);
    int res = (a << 1) + (int)c->CF;
    c->CF = (uint8_t)(res > 15);
    dg_reg_set(c, r, res & 0xF);
    DG_ADV();
    return 7;
}

DG_OP(ld_x_x)
{
    c->IX = (uint16_t)((c->IX & 0xF00) | (op & 0x0FF));
    DG_ADV();
    return 5;
}

DG_OP(add_r_i)
{
    int r = (op >> 4) & 0x3;
    int a = dg_reg_get(c, r);
    int res = a + (int)(op & 0x00F);
    c->CF = (uint8_t)(res > 15);
    if (c->DF && res > 9) {
        res += 6;
        c->CF = 1;
    }
    c->ZF = (uint8_t)((res & 0xF) == 0);
    dg_reg_set(c, r, res & 0xF);
    DG_ADV();
    return 7;
}

DG_OP(adc_r_i)
{
    int r = (op >> 4) & 0x3;
    int a = dg_reg_get(c, r);
    int res = a + (int)(op & 0x00F) + (int)c->CF;
    c->CF = (uint8_t)(res > 15);
    if (c->DF && res > 9) {
        res += 6;
        c->CF = 1;
    }
    c->ZF = (uint8_t)((res & 0xF) == 0);
    dg_reg_set(c, r, res & 0xF);
    DG_ADV();
    return 7;
}

DG_OP(and_r_i)
{
    int r = (op >> 4) & 0x3;
    int a = dg_reg_get(c, r);
    int res = a & (int)(op & 0x00F);
    c->ZF = (uint8_t)(res == 0);
    dg_reg_set(c, r, res);
    DG_ADV();
    return 7;
}

DG_OP(or_r_i)
{
    int r = (op >> 4) & 0x3;
    int a = dg_reg_get(c, r);
    int res = a | (int)(op & 0x00F);
    c->ZF = (uint8_t)(res == 0);
    dg_reg_set(c, r, res);
    DG_ADV();
    return 7;
}

DG_OP(xor_r_i)
{
    int r = (op >> 4) & 0x3;
    int a = dg_reg_get(c, r);
    int res = a ^ (int)(op & 0x00F);
    c->ZF = (uint8_t)(res == 0);
    dg_reg_set(c, r, res);
    DG_ADV();
    return 7;
}

DG_OP(sbc_r_i)
{
    int r = (op >> 4) & 0x3;
    int a = dg_reg_get(c, r);
    int res = a - (int)(op & 0x00F) - (int)c->CF;
    c->CF = (uint8_t)(res < 0);
    if (c->DF && c->CF) {
        res += 10;
    }
    c->ZF = (uint8_t)((res & 0xF) == 0);
    dg_reg_set(c, r, res & 0xF);
    DG_ADV();
    return 7;
}

DG_OP(fan_r_i)
{
    int r = (op >> 4) & 0x3;
    int a = dg_reg_get(c, r);
    c->ZF = (uint8_t)((a & (int)(op & 0x00F)) == 0);
    DG_ADV();
    return 7;
}

DG_OP(cp_r_i)
{
    int r = (op >> 4) & 0x3;
    int a = dg_reg_get(c, r);
    int cp = a - (int)(op & 0x00F);
    c->ZF = (uint8_t)(cp == 0);
    c->CF = (uint8_t)(cp < 0);
    DG_ADV();
    return 7;
}

DG_OP(ld_r_i)
{
    int r = (op >> 4) & 0x3;
    dg_reg_set(c, r, op & 0x00F);
    DG_ADV();
    return 5;
}

DG_OP(pset_p)
{
    c->if_delay = 1;
    c->NPC = (uint16_t)((op << 8) & 0x1F00);
    c->PC = (uint16_t)((c->PC & 0x1000) | ((c->PC + 1) & 0xFFF));
    return 5;
}

DG_OP(ldpx_mx_i)
{
    dg_set_mem(c, (int)c->IX, op & 0x00F);
    c->IX = (uint16_t)((c->IX & 0xF00) | ((c->IX + 1) & 0xFF));
    DG_ADV();
    return 5;
}

DG_OP(ldpy_my_i)
{
    dg_set_mem(c, (int)c->IY, op & 0x00F);
    c->IY = (uint16_t)((c->IY & 0xF00) | ((c->IY + 1) & 0xFF));
    DG_ADV();
    return 5;
}

DG_OP(ld_xp_r)
{
    int r = op & 0x3;
    c->IX = (uint16_t)((dg_reg_get(c, r) << 8) | (c->IX & 0x0FF));
    DG_ADV();
    return 5;
}

DG_OP(ld_xh_r)
{
    int r = op & 0x3;
    c->IX = (uint16_t)((dg_reg_get(c, r) << 4) | (c->IX & 0xF0F));
    DG_ADV();
    return 5;
}

DG_OP(ld_xl_r)
{
    int r = op & 0x3;
    c->IX = (uint16_t)(dg_reg_get(c, r) | (c->IX & 0xFF0));
    DG_ADV();
    return 5;
}

DG_OP(rrc_r)
{
    int r = op & 0x3;
    int a = dg_reg_get(c, r);
    int res = a + ((int)c->CF << 4);
    c->CF = (uint8_t)(res & 0x1);
    dg_reg_set(c, r, res >> 1);
    DG_ADV();
    return 5;
}

DG_OP(ld_yp_r)
{
    int r = op & 0x3;
    c->IY = (uint16_t)((dg_reg_get(c, r) << 8) | (c->IY & 0x0FF));
    DG_ADV();
    return 5;
}

DG_OP(ld_yh_r)
{
    int r = op & 0x3;
    c->IY = (uint16_t)((dg_reg_get(c, r) << 4) | (c->IY & 0xF0F));
    DG_ADV();
    return 5;
}

DG_OP(ld_yl_r)
{
    int r = op & 0x3;
    c->IY = (uint16_t)(dg_reg_get(c, r) | (c->IY & 0xFF0));
    DG_ADV();
    return 5;
}

DG_OP(dummy)
{
    (void)c;
    (void)op;
    return 5;
}

DG_OP(ld_r_xp)
{
    int r = op & 0x3;
    dg_reg_set(c, r, (int)(c->IX >> 8));
    DG_ADV();
    return 5;
}

DG_OP(ld_r_xh)
{
    int r = op & 0x3;
    dg_reg_set(c, r, (int)((c->IX >> 4) & 0x00F));
    DG_ADV();
    return 5;
}

DG_OP(ld_r_xl)
{
    int r = op & 0x3;
    dg_reg_set(c, r, (int)(c->IX & 0x00F));
    DG_ADV();
    return 5;
}

DG_OP(ld_r_yp)
{
    int r = op & 0x3;
    dg_reg_set(c, r, (int)(c->IY >> 8));
    DG_ADV();
    return 5;
}

DG_OP(ld_r_yh)
{
    int r = op & 0x3;
    dg_reg_set(c, r, (int)((c->IY >> 4) & 0x00F));
    DG_ADV();
    return 5;
}

DG_OP(ld_r_yl)
{
    int r = op & 0x3;
    dg_reg_set(c, r, (int)(c->IY & 0x00F));
    DG_ADV();
    return 5;
}

DG_OP(ld_r_q)
{
    int r = (op >> 2) & 0x3;
    int q = op & 0x3;
    int v = dg_reg_get(c, q);
    dg_reg_set(c, r, v);
    DG_ADV();
    return 5;
}

DG_OP(ldpx_r_q)
{
    int r = (op >> 2) & 0x3;
    int q = op & 0x3;
    int v = dg_reg_get(c, q);
    dg_reg_set(c, r, v);
    c->IX = (uint16_t)((c->IX & 0xF00) | ((c->IX + 1) & 0xFF));
    DG_ADV();
    return 5;
}

DG_OP(ldpy_r_q)
{
    int r = (op >> 2) & 0x3;
    int q = op & 0x3;
    int v = dg_reg_get(c, q);
    dg_reg_set(c, r, v);
    c->IY = (uint16_t)((c->IY & 0xF00) | ((c->IY + 1) & 0xFF));
    DG_ADV();
    return 5;
}

DG_OP(cp_r_q)
{
    int r = (op >> 2) & 0x3;
    int q = op & 0x3;
    int a = dg_reg_get(c, r);
    int b = dg_reg_get(c, q);
    int cp = a - b;
    c->ZF = (uint8_t)(cp == 0);
    c->CF = (uint8_t)(cp < 0);
    DG_ADV();
    return 7;
}

DG_OP(fan_r_q)
{
    int r = (op >> 2) & 0x3;
    int q = op & 0x3;
    int a = dg_reg_get(c, r);
    int b = dg_reg_get(c, q);
    c->ZF = (uint8_t)((a & b) == 0);
    DG_ADV();
    return 7;
}

DG_OP(acpx_mx_r)
{
    int r = op & 0x3;
    int m = dg_get_mem(c, (int)c->IX);
    int b = dg_reg_get(c, r);
    int res = m + b + (int)c->CF;
    c->CF = (uint8_t)(res > 15);
    if (c->DF && res > 9) {
        res += 6;
        c->CF = 1;
    }
    c->ZF = (uint8_t)((res & 0xF) == 0);
    dg_set_mem(c, (int)c->IX, res & 0xF);
    c->IX = (uint16_t)((c->IX & 0xF00) | ((c->IX + 1) & 0xFF));
    DG_ADV();
    return 7;
}

DG_OP(acpy_my_r)
{
    int r = op & 0x3;
    int m = dg_get_mem(c, (int)c->IY);
    int b = dg_reg_get(c, r);
    int res = m + b + (int)c->CF;
    c->CF = (uint8_t)(res > 15);
    if (c->DF && res > 9) {
        res += 6;
        c->CF = 1;
    }
    c->ZF = (uint8_t)((res & 0xF) == 0);
    dg_set_mem(c, (int)c->IY, res & 0xF);
    c->IY = (uint16_t)((c->IY & 0xF00) | ((c->IY + 1) & 0xFF));
    DG_ADV();
    return 7;
}

DG_OP(scpx_mx_r)
{
    int r = op & 0x3;
    int m = dg_get_mem(c, (int)c->IX);
    int b = dg_reg_get(c, r);
    int res = m - b - (int)c->CF;
    c->CF = (uint8_t)(res < 0);
    if (c->DF && res < 0) {
        res += 10;
    }
    c->ZF = (uint8_t)((res & 0xF) == 0);
    dg_set_mem(c, (int)c->IX, res & 0xF);
    c->IX = (uint16_t)((c->IX & 0xF00) | ((c->IX + 1) & 0xFF));
    DG_ADV();
    return 7;
}

DG_OP(scpy_my_r)
{
    int r = op & 0x3;
    int m = dg_get_mem(c, (int)c->IY);
    int b = dg_reg_get(c, r);
    int res = m - b - (int)c->CF;
    c->CF = (uint8_t)(res < 0);
    if (c->DF && res < 0) {
        res += 10;
    }
    c->ZF = (uint8_t)((res & 0xF) == 0);
    dg_set_mem(c, (int)c->IY, res & 0xF);
    c->IY = (uint16_t)((c->IY & 0xF00) | ((c->IY + 1) & 0xFF));
    DG_ADV();
    return 7;
}

DG_OP(set_f_i)
{
    int new_if;
    c->CF |= (uint8_t)(op & 0x001);
    c->ZF |= (uint8_t)((op >> 1) & 0x001);
    c->DF |= (uint8_t)((op >> 2) & 0x001);
    new_if = (op >> 3) & 0x001;
    c->if_delay = (uint8_t)(new_if && !c->IF);
    c->IF |= (uint8_t)new_if;
    DG_ADV();
    return 7;
}

DG_OP(rst_f_i)
{
    c->CF &= (uint8_t)op;
    c->ZF &= (uint8_t)(op >> 1);
    c->DF &= (uint8_t)(op >> 2);
    c->IF &= (uint8_t)(op >> 3);
    DG_ADV();
    return 7;
}

DG_OP(inc_mn)
{
    int mn = op & 0x00F;
    int res = dg_get_mem(c, mn) + 1;
    c->ZF = (uint8_t)(res == 16);
    c->CF = (uint8_t)(res > 15);
    dg_set_mem(c, mn, res & 0xF);
    DG_ADV();
    return 7;
}

DG_OP(dec_mn)
{
    int mn = op & 0x00F;
    int res = dg_get_mem(c, mn) - 1;
    c->ZF = (uint8_t)(res == 0);
    c->CF = (uint8_t)(res < 0);
    dg_set_mem(c, mn, res & 0xF);
    DG_ADV();
    return 7;
}

DG_OP(ld_mn_a)
{
    dg_set_mem(c, op & 0x00F, (int)c->A);
    DG_ADV();
    return 5;
}

DG_OP(ld_mn_b)
{
    dg_set_mem(c, op & 0x00F, (int)c->B);
    DG_ADV();
    return 5;
}

DG_OP(ld_a_mn)
{
    c->A = (uint16_t)dg_get_mem(c, op & 0x00F);
    DG_ADV();
    return 5;
}

DG_OP(ld_b_mn)
{
    c->B = (uint16_t)dg_get_mem(c, op & 0x00F);
    DG_ADV();
    return 5;
}

DG_OP(push_r)
{
    int r = op & 0x3;
    int v = dg_reg_get(c, r);
    c->SP = (uint8_t)((c->SP - 1) & 0xFF);
    dg_set_mem(c, (int)c->SP, v);
    DG_ADV();
    return 5;
}

DG_OP(push_xp)
{
    (void)op;
    c->SP = (uint8_t)((c->SP - 1) & 0xFF);
    dg_set_mem(c, (int)c->SP, (int)(c->IX >> 8));
    DG_ADV();
    return 5;
}

DG_OP(push_xh)
{
    (void)op;
    c->SP = (uint8_t)((c->SP - 1) & 0xFF);
    dg_set_mem(c, (int)c->SP, (int)((c->IX >> 4) & 0x00F));
    DG_ADV();
    return 5;
}

DG_OP(push_xl)
{
    (void)op;
    c->SP = (uint8_t)((c->SP - 1) & 0xFF);
    dg_set_mem(c, (int)c->SP, (int)(c->IX & 0x00F));
    DG_ADV();
    return 5;
}

DG_OP(push_yp)
{
    (void)op;
    c->SP = (uint8_t)((c->SP - 1) & 0xFF);
    dg_set_mem(c, (int)c->SP, (int)(c->IY >> 8));
    DG_ADV();
    return 5;
}

DG_OP(push_yh)
{
    (void)op;
    c->SP = (uint8_t)((c->SP - 1) & 0xFF);
    dg_set_mem(c, (int)c->SP, (int)((c->IY >> 4) & 0x00F));
    DG_ADV();
    return 5;
}

DG_OP(push_yl)
{
    (void)op;
    c->SP = (uint8_t)((c->SP - 1) & 0xFF);
    dg_set_mem(c, (int)c->SP, (int)(c->IY & 0x00F));
    DG_ADV();
    return 5;
}

DG_OP(push_f)
{
    (void)op;
    int f = (int)(((unsigned)c->IF << 3) | ((unsigned)c->DF << 2) |
                  ((unsigned)c->ZF << 1) | (unsigned)c->CF);
    c->SP = (uint8_t)((c->SP - 1) & 0xFF);
    dg_set_mem(c, (int)c->SP, f);
    DG_ADV();
    return 5;
}

DG_OP(dec_sp)
{
    (void)op;
    c->SP = (uint8_t)((c->SP - 1) & 0xFF);
    DG_ADV();
    return 5;
}

DG_OP(pop_r)
{
    int r = op & 0x3;
    int v = dg_get_mem(c, (int)c->SP);
    dg_reg_set(c, r, v);
    c->SP = (uint8_t)((c->SP + 1) & 0xFF);
    DG_ADV();
    return 5;
}

DG_OP(pop_xp)
{
    (void)op;
    int v = dg_get_mem(c, (int)c->SP);
    c->IX = (uint16_t)((v << 8) | (c->IX & 0x0FF));
    c->SP = (uint8_t)((c->SP + 1) & 0xFF);
    DG_ADV();
    return 5;
}
DG_OP(pop_xh)
{
    (void)op;
    int v = dg_get_mem(c, (int)c->SP);
    c->IX = (uint16_t)((v << 4) | (c->IX & 0xF0F));
    c->SP = (uint8_t)((c->SP + 1) & 0xFF);
    DG_ADV();
    return 5;
}
DG_OP(pop_xl)
{
    (void)op;
    int v = dg_get_mem(c, (int)c->SP);
    c->IX = (uint16_t)(v | (c->IX & 0xFF0));
    c->SP = (uint8_t)((c->SP + 1) & 0xFF);
    DG_ADV();
    return 5;
}
DG_OP(pop_yp)
{
    (void)op;
    int v = dg_get_mem(c, (int)c->SP);
    c->IY = (uint16_t)((v << 8) | (c->IY & 0x0FF));
    c->SP = (uint8_t)((c->SP + 1) & 0xFF);
    DG_ADV();
    return 5;
}
DG_OP(pop_yh)
{
    (void)op;
    int v = dg_get_mem(c, (int)c->SP);
    c->IY = (uint16_t)((v << 4) | (c->IY & 0xF0F));
    c->SP = (uint8_t)((c->SP + 1) & 0xFF);
    DG_ADV();
    return 5;
}
DG_OP(pop_yl)
{
    (void)op;
    int v = dg_get_mem(c, (int)c->SP);
    c->IY = (uint16_t)(v | (c->IY & 0xFF0));
    c->SP = (uint8_t)((c->SP + 1) & 0xFF);
    DG_ADV();
    return 5;
}
DG_OP(pop_f)
{
    (void)op;
    int f = dg_get_mem(c, (int)c->SP);
    int new_if = (f >> 3) & 0x1;
    c->CF = (uint8_t)(f & 0x1);
    c->ZF = (uint8_t)((f >> 1) & 0x1);
    c->DF = (uint8_t)((f >> 2) & 0x1);
    c->if_delay = (uint8_t)(new_if && !c->IF);
    c->IF = (uint8_t)new_if;
    c->SP = (uint8_t)((c->SP + 1) & 0xFF);
    DG_ADV();
    return 5;
}
DG_OP(inc_sp)
{
    (void)op;
    c->SP = (uint8_t)((c->SP + 1) & 0xFF);
    DG_ADV();
    return 5;
}

DG_OP(rets)
{
    (void)op;
    int l = dg_get_mem(c, (int)c->SP);
    int h = dg_get_mem(c, (int)(c->SP + 1));
    int p = dg_get_mem(c, (int)(c->SP + 2));
    c->PC = (uint16_t)((c->PC & 0x1000) | (l | (h << 4) | (p << 8)));
    c->SP = (uint8_t)((c->SP + 3) & 0xFF);
    c->PC = c->NPC = (uint16_t)((c->PC & 0x1000) | ((c->PC + 1) & 0xFFF));
    return 12;
}

DG_OP(ret)
{
    (void)op;
    int l = dg_get_mem(c, (int)c->SP);
    int h = dg_get_mem(c, (int)(c->SP + 1));
    int p = dg_get_mem(c, (int)(c->SP + 2));
    c->PC = c->NPC = (uint16_t)((c->PC & 0x1000) | (l | (h << 4) | (p << 8)));
    c->SP = (uint8_t)((c->SP + 3) & 0xFF);
    return 7;
}

DG_OP(ld_sph_r)
{
    int r = op & 0x3;
    int v = dg_reg_get(c, r);
    c->SP = (uint8_t)((v << 4) | (c->SP & 0x0F));
    DG_ADV();
    return 5;
}

DG_OP(ld_r_sph)
{
    int r = op & 0x3;
    dg_reg_set(c, r, (int)(c->SP >> 4));
    DG_ADV();
    return 5;
}

DG_OP(jpba)
{
    (void)op;
    c->PC = (uint16_t)((c->NPC & 0x1F00) | (c->B << 4) | c->A);
    return 5;
}
DG_OP(ld_spl_r)
{
    int r = op & 0x3;
    int v = dg_reg_get(c, r);
    c->SP = (uint8_t)(v | (c->SP & 0xF0));
    DG_ADV();
    return 5;
}

DG_OP(ld_r_spl)
{
    int r = op & 0x3;
    dg_reg_set(c, r, (int)(c->SP & 0x0F));
    DG_ADV();
    return 5;
}

DG_OP(halt)
{
    (void)op;
    c->HALT = 1;
    DG_ADV();
    return 5;
}

DG_OP(nop5)
{
    (void)op;
    DG_ADV();
    return 5;
}

DG_OP(nop7)
{
    (void)op;
    DG_ADV();
    return 7;
}

/* ------------------------------------------------------------- public API -- */

void dg_init(const uint8_t *rom, uint32_t rom_len)
{
    dg_cpu *c = &g_cpu;

    memset(c, 0, sizeof(*c));

    c->rom = rom;
    c->rom_size = rom_len;

    /* External pull-ups on the key matrix: the reference model is built with
     * mask port_pullup = {"K0": 15, "K1": 15}. */
    c->pullup_ext[PORT_K0] = 0xF;
    c->pullup_ext[PORT_K1] = 0xF;
    c->pullup_ext[PORT_R0] = 0;
    c->pullup_ext[PORT_R1] = 0;
    c->pullup_ext[PORT_R2] = 0;
    c->pullup_ext[PORT_R3] = 0;
    c->pullup_ext[PORT_R4] = 0;
    c->pullup_ext[PORT_P0] = 0;
    c->pullup_ext[PORT_P1] = 0;
    c->pullup_ext[PORT_P2] = 0;
    c->pullup_ext[PORT_P3] = 0;

    dg_init_registers(c);

    c->osc1_div = OSC1_DIV_FX;

    c->osc1_counter = 0.0;
    c->timer_counter = 0.0;
    c->ptimer_counter = 0.0;
    c->stopwatch_counter = 0.0;
    c->instr_counter = 0;

    c->if_delay = 0;
    c->RESET = 0;

    c->sound.one_shot_counter = 0;
    c->sound.buzzer_freq = (double)OSC1_CLOCK / (double)BUZZER_FREQ_DIV_0;
    c->sound.envelope_step = 7;
    c->sound.envelope_cycle = dg_envelope_cycle_div[0];
    c->sound.envelope_counter = 0;
    c->sound.envelope_on = 0;
    c->sound.sound_on = 0;

    c->vram_out_len = DG_VRAM_TOTAL;
    memset(c->vram_out, 0, sizeof(c->vram_out));
}

void dg_reset(void)
{
    dg_cpu *c = &g_cpu;

    dg_init_registers(c);

    c->osc1_counter = 0.0;
    c->timer_counter = 0.0;
    c->stopwatch_counter = 0.0;

    dg_sound_set_buzzer_off(c);
    dg_sound_set_envelope_off(c);
}

void dg_set_rom_ptr(const uint8_t *rom)
{
    g_cpu.rom = rom;
}

void dg_set_rom_len(uint32_t rom_len)
{
    g_cpu.rom_size = rom_len;
}

DG_IRAM int dg_step(void)
{
    dg_cpu *c = &g_cpu;
    int cycles = 7;
    int total = 7;

    if (!c->RESET) {
        if (!c->HALT) {
            uint16_t op;
            c->if_delay = 0;
            op = (uint16_t)dg_rom_get_word(c, (uint32_t)c->PC * 2u);
            cycles = dg_op_tbl[op](c, op);
            c->instr_counter += 1;
        }
        total = cycles;

        if (c->IF && !c->if_delay) {
            if (c->IPT & c->EIPT) {
                total += dg_interrupt(c, 0xC);
            } else if (c->ISIO & c->EISIO) {
                total += dg_interrupt(c, 0xA);
            } else if (c->IK1) {
                total += dg_interrupt(c, 0x8);
            } else if (c->IK0) {
                total += dg_interrupt(c, 0x6);
            } else if (c->ISW & c->EISW) {
                total += dg_interrupt(c, 0x4);
            } else if (c->IT & c->EIT) {
                total += dg_interrupt(c, 0x2);
            }
        }

        {
            /* 1/1024-cycle units; exact equivalent of the reference double math */
            int32_t ec = total;
            if (!(c->CTRL_OSC & IO_CLKCHG)) {
                ec *= c->osc1_div;              /* osc1_div = 33125 */
            } else {
                ec *= (1 << OSC1_FX_SHIFT);     /* clock change: 1.0 in fx units */
            }
            c->osc1_counter -= ec;
            while (c->osc1_counter <= 0) {
                c->osc1_counter += c->osc1_div;   /* reference adds osc1_div unconditionally */
                dg_clock_osc1(c);
            }
        }
    }

    return total;
}

const uint8_t *dg_vram(void)
{
    dg_cpu *c = &g_cpu;
    int i;

    if ((c->CTRL_LCD & IO_ALOFF) || c->RESET) {
        for (i = 0; i < DG_VRAM_TOTAL; i++) {
            c->vram_out[i] = 0;
        }
        c->vram_out_len = VRAM_SIZE;   /* reference returns EMPTY_VRAM (160) */
        return c->vram_out;
    }
    if (c->CTRL_LCD & IO_ALON) {
        for (i = 0; i < DG_VRAM_TOTAL; i++) {
            c->vram_out[i] = 1;
        }
        c->vram_out_len = VRAM_SIZE;   /* reference returns FULL_VRAM (160) */
        return c->vram_out;
    }

    memcpy(c->vram_out, c->vram, VRAM_SIZE);
    c->vram_out[160] = c->PLATCH[PORT_P0];
    c->vram_out[161] = c->PLATCH[PORT_P1];
    c->vram_out[162] = c->PLATCH[PORT_P2];
    c->vram_out[163] = c->PLATCH[PORT_P3];
    c->vram_out[164] = (uint8_t)dg_port_read(c, PORT_R0);
    c->vram_out[165] = (uint8_t)dg_port_read(c, PORT_R1);
    c->vram_out[166] = (uint8_t)dg_port_read(c, PORT_R2);
    c->vram_out[167] = (uint8_t)dg_port_read(c, PORT_R3);
    c->vram_out[168] = (uint8_t)dg_port_read(c, PORT_R4);
    c->vram_out_len = DG_VRAM_TOTAL;
    return c->vram_out;
}

uint32_t dg_vram_len(void)
{
    return g_cpu.vram_out_len;
}

uint16_t dg_pc(void)
{
    return (uint16_t)(g_cpu.PC & 0x1FFF);
}

uint32_t dg_instr_count(void)
{
    return g_cpu.instr_counter;
}

void dg_set_button(uint8_t mask, int pressed)
{
    /* K0 is active low with an external pull-up: pressing the button drives the
     * pin low, which the reference models as level 0; releasing it means "no
     * input group selected" (level -1), so the pull-up reads high again.
     * (Previously this passed 1 for pressed and 0 for released, i.e. inverted,
     * which left the ROM believing no button was ever pressed.) */
    dg_pin_handler(&g_cpu, PORT_K0, mask, pressed ? 0 : -1);
}

/* debugging helpers (not used by the test driver, handy in firmware) */
uint16_t dg_npc(void) { return (uint16_t)(g_cpu.NPC & 0x1F00); }
uint16_t dg_sp(void)  { return (uint16_t)g_cpu.SP; }
uint8_t  dg_flags(void)
{
    return (uint8_t)(((unsigned)g_cpu.IF << 3) | ((unsigned)g_cpu.DF << 2) |
                     ((unsigned)g_cpu.ZF << 1) | (unsigned)g_cpu.CF);
}

void dg_dbg_state(uint16_t *out)
{
    out[0] = g_cpu.A;
    out[1] = g_cpu.B;
    out[2] = g_cpu.IX;
    out[3] = g_cpu.IY;
    out[4] = (uint16_t)g_cpu.SP;
    out[5] = g_cpu.PC;
    out[6] = g_cpu.NPC;
    out[7] = (uint16_t)dg_flags();
}
/* ------------------------------------------------------- sound accessors -- */

double dg_sound_freq(void)
{
    if (!(g_cpu.sound.sound_on || g_cpu.sound.one_shot_counter > 0)) return 0.0;
    return g_cpu.sound.buzzer_freq;
}

int dg_ik0_pending(void) { return g_cpu.IK0; }

/* Free-running OSC1 tick counter (32768 Hz). The firmware uses it to advance
 * exactly as much virtual time as real time has passed, independent of whether
 * the CPU currently runs from OSC1 or OSC3. */
/* Temporary time-lapse for SKIP TIME: scaling osc1_div makes each executed
 * instruction consume more OSC1 time, so the ROM walks through game minutes in
 * fewer steps.  mult=1 restores normal speed.  This deliberately breaks the
 * 1:1 pacing between ROM timers and instruction issue -- the ROM sees several
 * OSC1 ticks per instruction -- so the caller has to verify the game still
 * behaves (hatching/evolution) at the chosen multiplier. */
void dg_set_osc1_speed(int mult)
{
    if (mult < 1) mult = 1;
    if (mult > 16) mult = 16;
    g_cpu.osc1_div = OSC1_DIV_FX * mult;
}

int dg_get_osc1_speed(void)
{
    return g_cpu.osc1_div / OSC1_DIV_FX;
}

uint32_t dg_osc1_ticks(void) { return g_cpu.osc1_ticks; }

/* 1 when the CPU is clocked from OSC3 (CLKCHG set), 0 when from OSC1. */
int dg_cpu_fast_mode(void) { return (g_cpu.CTRL_OSC & IO_CLKCHG) ? 1 : 0; }

int dg_sound_active(void)
{
    return (g_cpu.sound.sound_on || g_cpu.sound.one_shot_counter > 0) ? 1 : 0;
}

double dg_sound_amp(void)
{
    return (double)g_cpu.sound.envelope_step / 7.0;
}










/* ---- 调试/RAM 观察访问器（功能图标与 UI 状态需要）-------------------- */
/* 直接读写 page-0 的 RAM nibble（0x000..0x2FF）。仅供探针与"按 ROM 状态
 * 渲染 UI"的功能使用，不参与游戏逻辑。地址见反汇编附录 appendix-ram.md。 */
uint8_t dg_ram(int addr)
{
    return g_cpu.ram[addr & (RAM_SIZE - 1)];
}

void dg_ram_write(int addr, uint8_t value)
{
    g_cpu.ram[addr & (RAM_SIZE - 1)] = (uint8_t)(value & 0xF);
}
/* ---------------------------------------------------------------- state IO -- */

/* Snapshot / restore of the whole machine (registers, RAM, VRAM, prescalers).
 * The ROM pointer is not part of the snapshot: it always comes from the
 * currently loaded ROM, so a blob written for V1 cannot be applied to V2. */
uint32_t dg_state_size(void)
{
    return (uint32_t)sizeof(dg_cpu);
}

void dg_state_save(uint8_t *dst)
{
    memcpy(dst, &g_cpu, sizeof g_cpu);
}

int dg_state_load(const uint8_t *src, uint32_t len)
{
    dg_cpu snap;
    const uint8_t *rom;
    uint32_t rom_size;

    if (len != sizeof(dg_cpu)) return -1;
    memcpy(&snap, src, sizeof snap);
    rom = g_cpu.rom;
    rom_size = g_cpu.rom_size;
    memcpy(&g_cpu, &snap, sizeof g_cpu);
    g_cpu.rom = rom;
    g_cpu.rom_size = rom_size;
    return 0;
}
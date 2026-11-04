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

    double   osc1_div;
    double   osc1_counter;
    double   timer_counter;
    double   ptimer_counter;
    double   stopwatch_counter;

    dg_sound sound;

    uint8_t  vram_out[DG_VRAM_TOTAL];
    uint32_t vram_out_len;
} dg_cpu;

static dg_cpu g_cpu;

/* ------------------------------------------------------- prescaler table -- */

static const double k_timer_div     = (double)OSC1_CLOCK / 256.0;   /* 128.0   */
static const double k_stopwatch_div = (double)OSC1_CLOCK / 100.0;   /* 327.68  */
static const double k_ptimer_div[8] = {
    0.0, 0.0,
    (double)OSC1_CLOCK / 256.0,
    (double)OSC1_CLOCK / 512.0,
    (double)OSC1_CLOCK / 1024.0,
    (double)OSC1_CLOCK / 2048.0,
    (double)OSC1_CLOCK / 4096.0,
    (double)OSC1_CLOCK / 8192.0
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

#define DG_OP(name) static int dg_op_##name(dg_cpu *c, uint16_t op)

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

/*@OP_TABLE@*/

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

static void dg_sound_clock(dg_cpu *c)
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
        return dg_io_get(c, addr);
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

static void dg_clock_osc1(dg_cpu *c)
{
    dg_sound_clock(c);

    if ((c->PTC & IO_PTC) > 1) {
        c->ptimer_counter -= 1.0;
        if (c->ptimer_counter <= 0.0) {
            c->ptimer_counter += k_ptimer_div[c->PTC & IO_PTC];
            dg_process_ptimer(c);
        }
    }

    c->stopwatch_counter -= 1.0;
    if (c->stopwatch_counter <= 0.0) {
        c->stopwatch_counter += k_stopwatch_div;
        dg_process_stopwatch(c);
    }

    c->timer_counter -= 1.0;
    if (c->timer_counter <= 0.0) {
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

    c->osc1_div = 1060000.0 / (double)OSC1_CLOCK;

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

int dg_step(void)
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
            double ec = (double)total;
            if (!(c->CTRL_OSC & IO_CLKCHG)) {
                ec *= c->osc1_div;
            }
            c->osc1_counter -= ec;
            while (c->osc1_counter <= 0.0) {
                c->osc1_counter += c->osc1_div;
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
    dg_pin_handler(&g_cpu, PORT_K0, mask, pressed ? 1 : 0);
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
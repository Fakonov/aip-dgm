/*
 * digimon_core.h -- Epson E0C6S48 (E0C6200) emulator core, C99.
 *
 * Portable: no dynamic allocation, no external libraries, safe for both
 * xtensa (ESP32) and x86 hosts.
 */

#ifndef DIGIMON_CORE_H
#define DIGIMON_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load a ROM image and reset the CPU.  The pointer is kept (not copied):
 * rom must stay valid for the lifetime of the core.  The reference model
 * wraps ROM accesses modulo rom_len, so 16 KiB images are fine. */
void dg_init(const uint8_t *rom, uint32_t rom_len);

/* Same as the reference _reset(): registers + prescalers + buzzer/envelope.
 * Does not clear the instruction counter nor the PTIMER prescaler (matching
 * the Python reference). */
void dg_reset(void);

/* Execute exactly one instruction (one call == one reference clock()).
 * Returns the consumed CPU cycles. */
int dg_step(void);

/* 169-byte view: 0..159 VRAM, 160..163 P0-P3 latches, 164..168 R0-R4.
 * NOTE: like the reference get_VRAM(), when the LCD driver is forced off or
 * on (or the CPU is held in reset) the model reports only 160 bytes; use
 * dg_vram_len() to know how many of the 169 bytes are valid. */
const uint8_t *dg_vram(void);

/* 160 or 169: number of valid bytes in the buffer returned by dg_vram(). */
uint32_t dg_vram_len(void);

/* 13-bit program counter. */
uint16_t dg_pc(void);

/* Number of instructions executed since dg_init(). */
uint32_t dg_instr_count(void);

/* mask: 4 = top button, 2 = centre button, 1 = bottom button (K0 bits). */
void dg_set_button(uint8_t mask, int pressed);

/* Swap the ROM image without reinitialising the CPU state. */
void dg_set_rom_ptr(const uint8_t *rom);
void dg_set_rom_len(uint32_t rom_len);

/* Buzzer state for the host audio mixer: frequency in Hz (0 when silent)
 * and a 0..1 amplitude multiplier derived from the envelope. */
double dg_sound_freq(void);
int    dg_sound_active(void);
double dg_sound_amp(void);

/* 时间加速（SKIP TIME 用）：mult>1 时每条指令消耗更多 OSC1 时间 */
void dg_set_osc1_speed(int mult);
int  dg_get_osc1_speed(void);

/* Free-running 32768 Hz tick counter and current clock source, used by the
 * firmware to pace emulation by real time. */
uint32_t dg_osc1_ticks(void);
int      dg_cpu_fast_mode(void);

/* K0 interrupt latch, for input-path testing. */
int dg_ik0_pending(void);

/* Small debug accessors. */
uint16_t dg_npc(void);
uint16_t dg_sp(void);
uint8_t  dg_flags(void);
/* out[0..7] = A, B, IX, IY, SP, PC, NPC, F */
void     dg_dbg_state(uint16_t *out);

/* ������ RAM �۲죺ֱ�Ӷ�д page-0 nibble���� appendix-ram.md �ı��������
 * mode@0x009 / submode@0x00A / species_slot@0x00B / overlay_icon@0x00C /
 * menu_index@0x015 / call_timer@0x040 �� UI ״̬�����Դ���������� */
/* ״̬���գ��������Ĵ浵���ã�һ������ memcpy ���ɣ����ڲ�ָ�룩�� */
uint32_t dg_state_size(void);
void     dg_state_save(uint8_t *dst);
int      dg_state_load(const uint8_t *src, uint32_t len);
uint8_t dg_ram(int addr);
void    dg_ram_write(int addr, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif /* DIGIMON_CORE_H */




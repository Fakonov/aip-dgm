// Console command probe: boots the verification build (console on UART0) and
// asks it for STATE / AGES / RAM / SAVEDEL / DUMP, printing exactly what comes
// back.  Used to prove the new commands live in the firmware even when the
// board's USB port is busy.
//
//   node tools/dg_sim_state.mjs <firmware-full-sim.bin>

import { readFile } from 'node:fs/promises';
import { pathToFileURL } from 'node:url';
import path from 'node:path';

/* 模拟器仓库：设 PASSPORT_SIM 指向 FoloToy-Passport-Simulator 的检出目录 */
const SIM = process.env.PASSPORT_SIM || './passport-sim';
const { initSync, WasmEmulator } = await import(pathToFileURL(path.join(SIM, 'public/wasm/pkg/esp_emu.js')).href);
const { AiPassportBoard } = await import(pathToFileURL(path.join(SIM, 'public/wasm/ai-passport-board.js')).href);

const fwPath = process.argv[2];
const firmware = await readFile(fwPath);
const wasm = initSync({ module: await readFile(path.join(SIM, 'public/wasm/pkg/esp_emu_bg.wasm')) });
const emulator = new WasmEmulator('esp32c3');
emulator.load_default_rom();
emulator.set_boot_from_rom(true);
emulator.set_wifi_config('Emulator Host Bridge', '');
emulator.load_firmware(firmware);

const board = new AiPassportBoard(wasm, { emulator, onFrame() {}, onAudio() {} });
board.releaseButtons();

let uartBuf = '';
const lines = [];
function drain() {
  let nl;
  while ((nl = uartBuf.indexOf('\n')) >= 0) {
    const line = uartBuf.slice(0, nl).replace(/\r$/, '');
    uartBuf = uartBuf.slice(nl + 1);
    if (line.trim()) lines.push(line);
  }
}
function runUntil(seconds) {
  const target = seconds * 160e6;
  while (emulator.cycles() < target) {
    const u = emulator.run_batch(200_000);
    if (u) { uartBuf += u; drain(); }
    board.drain();
  }
}
const now = () => emulator.cycles() / 160e6;
function uartSend(text) {
  const bytes = new TextEncoder().encode(text);
  for (let i = 0; i < bytes.length; i += 64) {
    emulator.uart_input(bytes.subarray(i, Math.min(i + 64, bytes.length)));
    runUntil(now() + 0.003);
  }
}

runUntil(8);                       /* boot, scan roms, restore save */
console.log('booted, console lines so far:', lines.length);

for (const cmd of ['ROMINFO', 'STATE', 'AGES', 'RAM', 'DUMP', 'PET']) {
  const before = lines.length;
  uartSend(cmd + '\n');
  runUntil(now() + (cmd === 'RAM' ? 2.0 : 1.2));
  const got = lines.slice(before).filter((l) =>
    /^(STATE|AGES|RAM |RAMEND|DUMP|RAW|ROMSLOT|ROMSLOTS|ROMCUR|pet )/.test(l));
  console.log(`\n=== ${cmd} :: ${got.length ? 'REPLY' : 'NO REPLY'} ===`);
  for (const l of got.slice(0, 6)) console.log('  ' + l);
}

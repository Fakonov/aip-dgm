// Console command probe: boots the verification build (console on UART0) and
// asks it for STATE / AGES / RAM / SAVEDEL / DUMP, printing exactly what comes
// back.  Used to prove the new commands live in the firmware even when the
// board's USB port is busy.
//
//   node tools/dg_sim_state.mjs <firmware-full-sim.bin>

import { readFile } from 'node:fs/promises';
import { pathToFileURL } from 'node:url';
import path from 'node:path';

const SIM = '<home>/passport-sim/FoloToy-Passport-Simulator-main';
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

/* Optional "live" pass: set the ROM clock and let the egg hatch, so STATE and
 * DUMP carry real stage/next numbers instead of the all-zero clock page. */
if (process.argv[3] === 'live') {
  function press(name, hold = 0.4, gap = 0.4) {
    board.setButton(name, true);
    runUntil(now() + hold);
    board.setButton(name, false);
    runUntil(now() + gap);
  }
  await press('DOWN', 0.5, 0.5);
  await press('UP', 0.5, 0.5);
  await press('DOWN', 0.5, 0.5);
  await press('OK', 0.5, 0.6);
  await press('UP', 0.5, 0.5);
  await press('OK', 0.5, 0.5);
  await press('DOWN', 0.5, 0.6);
  console.log('clock set');
  for (let i = 0; i < 20; i++) {
    runUntil(now() + 60);
    const before = lines.length;
    uartSend('DUMP\n');
    runUntil(now() + 0.8);
    const got = lines.slice(before).find((l) => /^DUMP slot=/.test(l));
    if (got) console.log('  poll ' + (i + 1) + ': ' + got.slice(0, 80));
    if (got && !/^DUMP slot=0 /.test(got)) break;
  }
}

for (const cmd of ['ROMINFO', 'STATE', 'AGES', 'RAM', 'DUMP', 'PET']) {
  const before = lines.length;
  uartSend(cmd + '\n');
  runUntil(now() + (cmd === 'RAM' ? 2.0 : 1.2));
  const got = lines.slice(before).filter((l) =>
    /^(STATE|AGES|RAM |RAMEND|DUMP|RAW|ROMSLOT|ROMSLOTS|ROMCUR|pet )/.test(l));
  console.log(`\n=== ${cmd} :: ${got.length ? 'REPLY' : 'NO REPLY'} ===`);
  for (const l of got.slice(0, 6)) console.log('  ' + l);
}

/* Pitch-7 diagnostic build prints DIAG lines at boot and every 250 frames. */
const diag = lines.filter((l) => /^DIAG /.test(l));
if (diag.length) {
  console.log('\n=== DIAG ===');
  for (const l of diag.slice(-8)) console.log('  ' + l);
}

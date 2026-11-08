// Waking a dark screen must only wake it.
//
// Bug being tested: a short OK tap on the auto-off screen woke the panel AND
// opened SETTINGS, because only the press was swallowed and the release used a
// stale timestamp ("held" looked like > 1.2 s).
//
//   node tools/dg_sim_wake.mjs <firmware-full.bin> <outDir>

import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { deflateSync } from 'node:zlib';
import { pathToFileURL } from 'node:url';
import path from 'node:path';

const SIM = '<home>/passport-sim/FoloToy-Passport-Simulator-main';
const { initSync, WasmEmulator } = await import(pathToFileURL(path.join(SIM, 'public/wasm/pkg/esp_emu.js')).href);
const { AiPassportBoard } = await import(pathToFileURL(path.join(SIM, 'public/wasm/ai-passport-board.js')).href);

const fwPath = process.argv[2];
const outDir = process.argv[3] || '.';
await mkdir(outDir, { recursive: true });

const firmware = await readFile(fwPath);
const wasm = initSync({ module: await readFile(path.join(SIM, 'public/wasm/pkg/esp_emu_bg.wasm')) });
const emulator = new WasmEmulator('esp32c3');
emulator.load_default_rom();
emulator.set_boot_from_rom(true);
emulator.set_wifi_config('Emulator Host Bridge', '');
emulator.load_firmware(firmware);

const board = new AiPassportBoard(wasm, { emulator, onFrame() {}, onAudio() {} });
board.releaseButtons();

function crc32b(buf) {
  let c, crc = 0xFFFFFFFF;
  for (let n = 0; n < buf.length; n++) {
    c = (crc ^ buf[n]) & 0xFF;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    crc = c ^ (crc >>> 8);
  }
  return (crc ^ 0xFFFFFFFF) >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32b(td));
  return Buffer.concat([len, td, crc]);
}
const SW = 240, SH = 320, DW = 320, DH = 240;
function pngFromFb(fb) {
  const out = new Uint8Array(DW * DH * 4);
  for (let y = 0; y < DH; y++) {
    for (let x = 0; x < DW; x++) {
      const s = ((SH - 1 - x) * SW + y) * 4, d = (y * DW + x) * 4;
      out[d] = fb[s]; out[d + 1] = fb[s + 1]; out[d + 2] = fb[s + 2]; out[d + 3] = 255;
    }
  }
  const raw = Buffer.alloc((DW * 4 + 1) * DH);
  for (let y = 0; y < DH; y++) {
    raw[y * (DW * 4 + 1)] = 0;
    Buffer.from(out.buffer, y * DW * 4, DW * 4).copy(raw, y * (DW * 4 + 1) + 1);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(DW, 0); ihdr.writeUInt32BE(DH, 4); ihdr[8] = 8; ihdr[9] = 6;
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
    chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw, { level: 6 })), chunk('IEND', Buffer.alloc(0)),
  ]);
}

let uartBuf = '';
const log = [];
function drain() {
  let nl;
  while ((nl = uartBuf.indexOf('\n')) >= 0) {
    const line = uartBuf.slice(0, nl).replace(/\r$/, '');
    uartBuf = uartBuf.slice(nl + 1);
    if (line.trim()) { log.push(line); if (/digimon/.test(line)) console.log('  [uart] ' + line); }
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
async function press(name, hold = 0.35, gap = 0.45) {
  board.setButton(name, true);
  runUntil(emulator.cycles() / 160e6 + hold);
  board.setButton(name, false);
  runUntil(emulator.cycles() / 160e6 + gap);
}
async function shot(tag) {
  await writeFile(path.join(outDir, `${tag}.png`), pngFromFb(board.display.framebuffer));
  console.log(`[shot] ${tag} t=${(emulator.cycles() / 160e6).toFixed(1)}s`);
}

/* boot + clock */
runUntil(3);
for (const [k, h] of [['DOWN', 0.5], ['UP', 0.5], ['DOWN', 0.5], ['OK', 0.5], ['UP', 0.5], ['OK', 0.5], ['DOWN', 0.5]]) {
  await press(k, h);
}

/* turn AUTO OFF to 15S: A x4 to the AUTO OFF row, B twice */
await press('OK', 3.4, 0.6);                 // open menu
for (let i = 0; i < 4; i++) await press('UP', 0.3, 0.3);   // ROM -> SHELL -> BRIGHT -> VOLUME -> AUTO OFF
await shot('w0_auto_row');
await press('DOWN', 0.3, 0.3);               // NEVER -> 15S
await shot('w1_auto_15s');
await press('OK', 0.3, 0.6);                 // close
await shot('w2_game');

/* wait past the idle timeout: backlight should go off */
runUntil(emulator.cycles() / 160e6 + 18);
await shot('w3_screen_off');

/* short OK tap: must only wake, not open settings */
await press('OK', 0.25, 1.0);
await shot('w4_after_wake_tap');

/* and a long OK hold still opens the menu */
await press('OK', 3.4, 0.6);
await shot('w5_long_hold_opens');

console.log('\nlog:');
console.log(log.filter((l) => /menu|screen/.test(l)).join('\n'));

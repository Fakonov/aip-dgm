// PLAY TIME reset + ROM two-level flow, photographed.
//   node tools/dg_sim_menu4.mjs <firmware-full-with-rom.bin> <outDir>
//
// Sequence: open panel (cursor on SKIP TIME) -> A to PLAY TIME -> B arms the
// wipe ("RESET? B") -> A moves away and must cancel it -> B on ROM opens the
// slot list.

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

let frames = 0;
const board = new AiPassportBoard(wasm, { emulator, onFrame() { frames++; }, onAudio() {} });
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
function encodePng(rgba, w, h) {
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (w * 4 + 1)] = 0;
    Buffer.from(rgba.buffer, rgba.byteOffset + y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; ihdr[9] = 6;
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
    chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw, { level: 6 })), chunk('IEND', Buffer.alloc(0)),
  ]);
}
const SW = 240, SH = 320, DW = 320, DH = 240;
function rotateCW(src) {
  const out = new Uint8Array(DW * DH * 4);
  for (let y = 0; y < DH; y++) {
    for (let x = 0; x < DW; x++) {
      const sx = y, sy = SH - 1 - x;
      const s = (sy * SW + sx) * 4, d = (y * DW + x) * 4;
      out[d] = src[s]; out[d + 1] = src[s + 1]; out[d + 2] = src[s + 2]; out[d + 3] = 255;
    }
  }
  return out;
}
async function shot(tag) {
  await writeFile(path.join(outDir, `${tag}.png`), encodePng(rotateCW(board.display.framebuffer), DW, DH));
  console.log(`[shot] ${tag}`);
}

let uartBuf = '';
const lines = [];
function drain() {
  let nl;
  while ((nl = uartBuf.indexOf('\n')) >= 0) {
    const line = uartBuf.slice(0, nl).replace(/\r$/, '');
    uartBuf = uartBuf.slice(nl + 1);
    if (!line.trim()) continue;
    lines.push(line);
    if (/digimon/.test(line)) console.log('  [uart] ' + line);
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
async function press(name, hold = 0.35, gap = 0.35) {
  board.setButton(name, true);
  runUntil(emulator.cycles() / 160e6 + hold);
  board.setButton(name, false);
  runUntil(emulator.cycles() / 160e6 + gap);
}

runUntil(3);
await press('DOWN', 0.5, 0.5);
await press('UP', 0.5, 0.5);
await press('DOWN', 0.5, 0.5);
await press('OK', 0.5, 0.6);
await press('UP', 0.5, 0.5);
await press('OK', 0.5, 0.5);
await press('DOWN', 0.5, 0.6);

await press('OK', 3.4, 0.8);
await shot('40_menu_top');

await press('UP', 0.35, 0.35);        /* cursor -> PLAY TIME */
await shot('41_play_row');

await press('DOWN', 0.35, 0.5);       /* first B only arms */
await shot('42_play_armed');

await press('UP', 0.35, 0.35);        /* moving away must cancel the wipe */
await shot('43_play_cancelled');

await press('UP', 0.35, 0.35);        /* back around to ROM */
await press('DOWN', 0.35, 0.8);       /* enter the slot list */
await shot('44_rom_slots');

console.log('\n--- log tail ---');
console.log(lines.filter((l) => /menu |play time|rom store|restored/.test(l)).slice(-8).join('\n'));

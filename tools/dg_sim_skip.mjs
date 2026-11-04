// SKIP TIME / AGE regression on the settings menu.
//
//   node tools/dg_sim_skip.mjs <firmware-full.bin> <outDir>
//
// Boots with a ROM already in slot 0 (build the image with
// `mk_full_bin.py --rom 0=...:V1`), opens the settings panel, walks to SKIP TIME,
// cycles it to 5M, closes the panel (which is what applies the jump) and reopens
// it to read the AGE row back.

import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { deflateSync } from 'node:zlib';
import { pathToFileURL } from 'node:url';
import path from 'node:path';

/* 模拟器仓库：设 PASSPORT_SIM 指向 FoloToy-Passport-Simulator 的检出目录 */
const SIM = process.env.PASSPORT_SIM || './passport-sim';
const { initSync, WasmEmulator } = await import(pathToFileURL(path.join(SIM, 'public/wasm/pkg/esp_emu.js')).href);
const { AiPassportBoard } = await import(pathToFileURL(path.join(SIM, 'public/wasm/ai-passport-board.js')).href);

const fwPath = process.argv[2];
const outDir = process.argv[3] || '.';
await mkdir(outDir, { recursive: true });

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
    if (!line.trim()) continue;
    lines.push(line);
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
const sleepSim = (s) => runUntil(now() + s);

const shots = [];
async function shot(tag) {
  await writeFile(path.join(outDir, `${tag}.png`), encodePng(rotateCW(board.display.framebuffer), DW, DH));
  shots.push(tag);
  console.log(`[shot] ${tag}  t=${now().toFixed(1)}s`);
}
async function press(name, hold = 0.4, gap = 0.6) {
  board.setButton(name, true);
  sleepSim(hold);
  board.setButton(name, false);
  sleepSim(gap);
}

/* ---- scenario ---- */
runUntil(3.0);
await shot('50_boot');

await press('OK', 3.4, 0.9);          // long press opens SETTINGS
await shot('51_menu_9rows');

for (let i = 0; i < 5; i++) await press('UP', 0.3, 0.3);     // A(UP) = next row -> SKIP TIME
await shot('52_on_skip');

for (let i = 0; i < 4; i++) await press('DOWN', 0.3, 0.35);   // B x4 -> 2H
await shot('53_skip_2h');

await press('OK', 0.35, 1.2);         // C = close, which applies the jump
await shot('54_after_skip');

await press('OK', 3.4, 0.9);          // reopen to read AGE back
await shot('55_menu_after');
await press('UP', 0.3, 0.3);                              // SKIP -> AGE (selection is kept)
await shot('56_on_age');
await press('OK', 0.35, 0.8);

const interesting = lines.filter((l) => /skip|menu |AGE/i.test(l));
console.log('\n--- console ---\n' + (interesting.join('\n') || '(device build: logs on USB-JTAG)'));
console.log('shots: ' + shots.join(', '));

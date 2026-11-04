// UI-focused simulator run: boots the firmware, photographs the layout, the
// settings panel and a lit icon, and saves everything already rotated to the
// way the panel is mounted (320x240 landscape).
//
//   node tools/dg_sim_ui.mjs <firmware-full.bin> <outDir> [seconds]
//
// The simulator framebuffer is 240x320 portrait; rotating it clockwise gives
// exactly what the eye sees on the toy (BSP_LCD_ROTATED_90 = swap_xy + mirror_x).

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

/* ---- PNG ---------------------------------------------------------------- */
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

const shots = [];
async function shot(tag) {
  const png = encodePng(rotateCW(board.display.framebuffer), DW, DH);
  const file = path.join(outDir, `${tag}.png`);
  await writeFile(file, png);
  shots.push(tag);
  console.log(`[shot] ${tag}  t=${(emulator.cycles() / 160e6).toFixed(1)}s`);
}

/* ---- emulation ---------------------------------------------------------- */
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
async function press(name, hold = 0.4, gap = 0.4) {
  board.setButton(name, true);
  runUntil(emulator.cycles() / 160e6 + hold);
  board.setButton(name, false);
  runUntil(emulator.cycles() / 160e6 + gap);
}

/* ---- scenario ----------------------------------------------------------- */
runUntil(3);
await shot('10_boot');

// The ROM boots into the clock-set screen and will not advance until it is
// done: DOWN(enter) UP(hours) DOWN(minutes) OK(confirm), then a couple of
// extra presses to leave the screen (same sequence as dg_sim_test.mjs).
await press('DOWN', 0.5, 0.5);
await press('UP', 0.5, 0.5);
await press('DOWN', 0.5, 0.5);
await press('OK', 0.5, 0.6);
await press('UP', 0.5, 0.5);
await press('OK', 0.5, 0.5);
await press('DOWN', 0.5, 0.6);
await shot('11_clock_done');

// settings panel: long OK opens it, then walk to EXIT (last row) and press OK
const MENU_ROWS = 7;          // ROM / SHELL / BRIGHT / VOLUME / AUTO OFF / SAVE OUT / SAVE IN (C closes, no EXIT row)
await press('OK', 3.4, 0.6);
await shot('12_menu');
for (let i = 0; i < MENU_ROWS - 1; i++) await press('DOWN', 0.3, 0.25);
await shot('13_menu_last_row');
await press('OK', 0.3, 0.8);
await shot('14_back');

// let the egg hatch, then light the status icon with A
const hatch = Number(process.argv[4] || 360);
runUntil(hatch);
await shot('15_hatched');
await press('UP', 0.6, 0.8);
await shot('16_icon_lit');
await press('UP', 0.6, 0.8);
await shot('17_icon_lit2');

console.log('\nshots: ' + shots.join(', '));
const interesting = lines.filter((l) => /icons=|menu |panel |scale |restored|saved/.test(l));
console.log(interesting.slice(-12).join('\n'));

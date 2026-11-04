// Headless test of the Digimon firmware inside the FoloToy AI Passport
// simulator core (the same WASM/QEMU the web page uses).
//
// usage: node dg_sim_test.mjs <firmware.bin> <outDir> [maxEmulatedSeconds]
//
// It boots the firmware, drives UP/DOWN/OK like a player, and dumps PNG
// screenshots (240x320) at the points that matter.

import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { deflateSync } from 'node:zlib';
import { pathToFileURL } from 'node:url';
import path from 'node:path';

/* 模拟器仓库：设 PASSPORT_SIM 指向 FoloToy-Passport-Simulator 的检出目录 */
const SIM = process.env.PASSPORT_SIM || './passport-sim';
const { initSync, WasmEmulator } = await import(pathToFileURL(path.join(SIM, 'public/wasm/pkg/esp_emu.js')).href);
const { AiPassportBoard } = await import(pathToFileURL(path.join(SIM, 'public/wasm/ai-passport-board.js')).href);

const fwPath = process.argv[2];
const outDir = process.argv[3] || '.';
const maxSeconds = Number(process.argv[4] || 420);
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

// ---- PNG writer (RGBA, no dependencies) ---------------------------------
function crc32(buf) {
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
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
function encodePng(rgba, w, h) {
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (w * 4 + 1)] = 0;
    Buffer.from(rgba.buffer, rgba.byteOffset + y * w * 4, w * 4)
      .copy(raw, y * (w * 4 + 1) + 1);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
    chunk('IHDR', ihdr),
    chunk('IDAT', deflateSync(raw, { level: 6 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}
const shots = [];
async function shot(tag) {
  const fb = board.display.framebuffer;
  const png = encodePng(fb, 240, 320);
  const file = path.join(outDir, `shot_${tag}.png`);
  await writeFile(file, png);
  const sha = createHash('sha256').update(fb).digest('hex').slice(0, 12);
  shots.push({ tag, file, sha });
  console.log(`[shot] ${tag}  t=${(emulator.cycles() / 160e6).toFixed(1)}s  sha=${sha}`);
}

// ---- emulation helpers ---------------------------------------------------
let lastReport = 0;
let uartBuf = '';
function runUntil(seconds) {
  const target = seconds * 160e6;
  while (emulator.cycles() < target) {
    const uart = emulator.run_batch(200_000);
    if (uart) {
      uartBuf += uart;
      let nl;
      while ((nl = uartBuf.indexOf('\n')) >= 0) {
        const line = uartBuf.slice(0, nl).replace(/\r$/, '');
        uartBuf = uartBuf.slice(nl + 1);
        if (line.trim()) console.log('  [uart] ' + line);
      }
    }
    board.drain();
    const t = emulator.cycles() / 160e6;
    if (t - lastReport >= 30) {
      lastReport = t;
      console.log(`[run] t=${t.toFixed(0)}s frames=${frames}`);
    }
  }
}
async function press(name, seconds = 0.5, gapSeconds = 0.5) {
  board.setButton(name, true);
  runUntil(emulator.cycles() / 160e6 + seconds);
  board.setButton(name, false);
  runUntil(emulator.cycles() / 160e6 + gapSeconds);
  console.log(`[key] ${name} held ${seconds}s  (t=${(emulator.cycles() / 160e6).toFixed(1)}s)`);
}

// ---- scenario ------------------------------------------------------------
console.log('booting firmware, sha256=' + createHash('sha256').update(firmware).digest('hex'));
runUntil(3);
await shot('00_boot');

// clock set as on the real toy: DOWN(enter) UP(hours) DOWN(minutes) OK(confirm)
await press('DOWN'); await shot('01_clock_down');
await press('UP');   await shot('02_clock_up');
await press('DOWN');
await press('OK');   await shot('03_clock_ok');

// the ROM needs a couple of extra presses to leave the clock screen
await press('UP'); await press('OK'); await press('DOWN');
await shot('04_after_clock');

// wait for the egg to hatch; icons must stay dark until then
runUntil(120); await shot('05_egg_120s');
await press('UP', 0.6, 0.6);      // pressing A before hatching must NOT highlight
await shot('06_up_before_hatch');

runUntil(Math.max(340, Math.min(maxSeconds, 400))); await shot('07_hatched');

// after hatching: A cycles the seven function icons, B enters, C cancels
for (let i = 1; i <= 4; i++) {
  await press('UP', 0.5, 0.6);
  await shot(`10_A_${i}`);
}
await press('DOWN', 0.5, 0.8);    // enter the highlighted function
await shot('11_B_enter');
await press('OK', 0.5, 0.8);      // cancel
await shot('12_C_cancel');

console.log('\nsummary:');
console.log(JSON.stringify({ frames, emulatedSeconds: emulator.cycles() / 160e6, shots }, null, 2));

// Simulator test for the settings menu + per-ROM save slots.
// Boots the firmware in the FoloToy WASM/QEMU core, opens the menu with a long
// OK, walks it, switches ROM V1 -> V2 -> V3 -> V1 and checks the save/restore
// log lines on UART.
//
// usage: node dg_sim_menu_test.mjs <firmware.bin> <outDir>

import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { createHash } from 'node:crypto';
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

let uartBuf = '';
const log = [];
function drainUart() {
  let nl;
  while ((nl = uartBuf.indexOf('\n')) >= 0) {
    const line = uartBuf.slice(0, nl).replace(/\r$/, '');
    uartBuf = uartBuf.slice(nl + 1);
    if (line.trim() && /digimon|icons|menu|saved|restored|ROM/.test(line)) {
      log.push(line);
      console.log('  [uart] ' + line);
    }
  }
}
const shots = [];
async function shot(tag) {
  const png = encodePng(board.display.framebuffer, 240, 320);
  const file = path.join(outDir, `menu_${tag}.png`);
  await writeFile(file, png);
  shots.push({ tag, file });
  console.log(`[shot] ${tag} t=${(emulator.cycles() / 160e6).toFixed(1)}s`);
}

function runUntil(seconds) {
  const target = seconds * 160e6;
  while (emulator.cycles() < target) {
    const u = emulator.run_batch(200_000);
    if (u) { uartBuf += u; drainUart(); }
    board.drain();
  }
}
async function press(name, holdSeconds, gapSeconds = 0.4) {
  board.setButton(name, true);
  runUntil(emulator.cycles() / 160e6 + holdSeconds);
  board.setButton(name, false);
  runUntil(emulator.cycles() / 160e6 + gapSeconds);
  console.log(`[key] ${name} ${holdSeconds}s (t=${(emulator.cycles() / 160e6).toFixed(1)}s)`);
}

console.log('sha256=' + createHash('sha256').update(firmware).digest('hex'));
runUntil(3);
await shot('00_boot');

// long OK opens the settings menu
await press('OK', 1.5, 0.6);
await shot('01_menu');

// row 2: brightness
await press('DOWN', 0.35); await press('OK', 0.35); await shot('02_brightness');
// row 3: volume
await press('DOWN', 0.35); await press('OK', 0.35); await shot('03_volume');
// row 1 again: cycle ROM V1 -> V2
await press('UP', 0.35); await press('UP', 0.35);
await press('OK', 0.35, 1.0); await shot('04_rom_v2');
await press('OK', 0.35, 1.0); await shot('05_rom_v3');
await press('OK', 0.35, 1.0); await shot('06_rom_v1_restored');
// exit
await press('DOWN', 0.35); await press('DOWN', 0.35); await press('DOWN', 0.35);
await press('OK', 0.35, 0.8); await shot('07_back_to_game');

console.log('\n--- firmware log ---');
for (const l of log) console.log(l);
console.log('\nshots:', shots.map(s => path.basename(s.file)).join(', '));

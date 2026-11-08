// Multi-ROM regression: fills three slots, checks the partition table reports
// all three, then drives the on-device settings menu to switch between them.
//
//   node tools/dg_sim_rom_multi.mjs <firmware-full.bin> <outDir>
//        --rom file:V1 --rom file:V2 --rom file:V3
//
// Needs the simulator build (build_sim.ps1): the device build keeps its console
// on USB-Serial-JTAG, which the emulator does not model.

import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { deflateSync } from 'node:zlib';
import { pathToFileURL } from 'node:url';
import path from 'node:path';

const SIM = '<home>/passport-sim/FoloToy-Passport-Simulator-main';
const { initSync, WasmEmulator } = await import(pathToFileURL(path.join(SIM, 'public/wasm/pkg/esp_emu.js')).href);
const { AiPassportBoard } = await import(pathToFileURL(path.join(SIM, 'public/wasm/ai-passport-board.js')).href);

const fwPath = process.argv[2];
const outDir = process.argv[3] || '.';
const specs = [];
for (let i = 4; i < process.argv.length; i++) {
  if (process.argv[i] === '--rom') {
    const s = process.argv[++i] || '';
    const k = s.lastIndexOf(':');
    specs.push(k > 1 ? { path: s.slice(0, k), name: s.slice(k + 1) } : { path: s, name: 'R' + i });
  }
}
await mkdir(outDir, { recursive: true });

/* ---- PNG ---- */
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

/* ---- emulator ---- */
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
const replies = [];
function drain() {
  let nl;
  while ((nl = uartBuf.indexOf('\n')) >= 0) {
    const line = uartBuf.slice(0, nl).replace(/\r$/, '');
    uartBuf = uartBuf.slice(nl + 1);
    if (!line.trim()) continue;
    if (/^(ROM|SAVE|IMPORT)/.test(line)) replies.push(line);
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
function uartSend(text, slice = 128, settle = 0.003) {
  const bytes = new TextEncoder().encode(text);
  for (let i = 0; i < bytes.length; i += slice) {
    emulator.uart_input(bytes.subarray(i, Math.min(i + slice, bytes.length)));
    sleepSim(settle);
  }
}
function crc32(u8) {
  let c = 0xFFFFFFFF;
  for (let i = 0; i < u8.length; i++) {
    c ^= u8[i];
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
  }
  return (c ^ 0xFFFFFFFF) >>> 0;
}
function hexLines(u8) {
  let out = '';
  for (let i = 0; i < u8.length; i += 32) out += u8.subarray(i, Math.min(i + 32, u8.length)).toString('hex') + '\n';
  return out;
}
async function press(name, hold = 0.4, gap = 0.5) {
  board.setButton(name, true);
  sleepSim(hold);
  board.setButton(name, false);
  sleepSim(gap);
}

const results = [];
function check(name, ok, detail) {
  results.push({ name, ok });
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '  (' + detail + ')' : ''}`);
}

/* ---- scenario ---- */
runUntil(3.0);
await shot('40_start');

for (let i = 0; i < specs.length; i++) {
  const bytes = await readFile(specs[i].path);
  const crc = crc32(bytes);
  uartSend(`ROMIN ${i} ${bytes.length} ${crc.toString(16)} ${specs[i].name}\n`);
  sleepSim(0.5);
  uartSend(hexLines(bytes));
  uartSend('ROMEND\n');
  sleepSim(2.0);
  const ok = replies.some((l) => l === `ROMOK ${i} ${bytes.length} ${crc.toString(16).padStart(8, '0')}`);
  check(`slot ${i} accepts ${specs[i].name}`, ok, replies.filter((l) => l.startsWith('ROMOK')).slice(-1)[0] || '');
}

replies.length = 0;
uartSend('ROMINFO\n');
sleepSim(0.5);
const filled = replies.filter((l) => /^ROMSLOT \d+ \S+ \d+ [0-9a-f]{8}/.test(l) && !/^ROMSLOT \d+ - /.test(l));
check('all slots report a ROM', filled.length === specs.length,
      `${filled.length}/${specs.length} filled`);
console.log('  ' + filled.join('\n  '));
await shot('41_three_slots');

/* the menu's ROM row cycles only through committed slots */
await press('OK', 3.4, 0.8);
await shot('42_menu');

const cycle = [];
for (let k = 0; k < specs.length; k++) {
  await press('DOWN', 0.35, 0.9);        // B = change value: next committed slot
  replies.length = 0;
  await press('OK', 0.35, 1.2);          // C = close (also flushes settings)
  uartSend('ROMINFO\n');
  sleepSim(0.5);
  const cur = replies.find((l) => l.startsWith('ROMCUR'));
  cycle.push(cur);
  console.log(`  after B#${k + 1}: ${cur}`);
  await shot(`43_cycle_${k + 1}`);
  await press('OK', 3.4, 0.6);           // reopen for the next round
}

check('menu cycles through every committed slot',
      cycle.length === specs.length && new Set(cycle.map((c) => (c || '').split(' ')[1])).size === specs.length,
      cycle.map((c) => (c || '?').split(' ')[2]).join(' -> '));

const failed = results.filter((r) => !r.ok);
console.log('\n' + (failed.length ? `${failed.length} CHECK(S) FAILED` : 'ALL MULTI-ROM CHECKS PASSED'));
console.log('shots: ' + shots.join(', '));
process.exitCode = failed.length ? 1 : 0;

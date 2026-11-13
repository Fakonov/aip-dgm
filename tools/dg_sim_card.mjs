// Offline battle-card (M3 / G2) simulator check: does the new BATTLE row exist,
// does the card picker render, and does CARDGO arm the responder?
//
//   node tools/dg_sim_card.mjs <firmware-full.bin> <outDir>
//
// Same trick as dg_sim_rom.mjs: a verification build (-DDG_SIM_UART0_INPUT=ON)
// rebinds the console to UART0, so the card commands can be spoken from here.
// Screenshots land in <outDir>: menu (BATTLE row), the card picker, and the
// glass right after CARDGO armed a card.

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

/* ---- emulator ----------------------------------------------------------- */
const firmware = await readFile(fwPath);
const wasm = initSync({ module: await readFile(path.join(SIM, 'public/wasm/pkg/esp_emu_bg.wasm')) });
const emulator = new WasmEmulator('esp32c3');
emulator.load_default_rom();
emulator.set_boot_from_rom(true);
emulator.set_wifi_config('Emulator Host Bridge', '');
emulator.load_firmware(firmware);

const board = new AiPassportBoard(wasm, { emulator, onFrame() {}, onAudio() {} });
board.releaseButtons();

/* ---- console ------------------------------------------------------------ */
let uartBuf = '';
let serialLog = '';
function drain() {
  let nl;
  while ((nl = uartBuf.indexOf('\n')) >= 0) {
    const line = uartBuf.slice(0, nl).replace(/\r$/, '');
    uartBuf = uartBuf.slice(nl + 1);
    if (!line.trim()) continue;
    serialLog += line + '\n';
    if (/^(CARD|LINK|STATE|ROMINFO|DUMP|LX)/.test(line)) console.log('  [uart] ' + line);
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

function uartSend(text, settle = 0.05) {
  const bytes = new TextEncoder().encode(text);
  emulator.uart_input(bytes);
  sleepSim(settle);
}

async function shot(tag) {
  const png = encodePng(rotateCW(board.display.framebuffer), DW, DH);
  await writeFile(path.join(outDir, `${tag}.png`), png);
  console.log(`[shot] ${tag}  t=${now().toFixed(1)}s`);
}

async function press(name, hold = 0.4, gap = 0.4) {
  board.setButton(name, true);
  sleepSim(hold);
  board.setButton(name, false);
  sleepSim(gap);
}

/* ---- script ------------------------------------------------------------- */
console.log('== M3 offline card: simulator check ==');
runUntil(4.0);
uartSend('ROMINFO\n', 0.4);
uartSend('CARDW 0 A 3 1 CARDTEST\n', 0.3);
uartSend('CARDL\n', 0.4);
uartSend('CARDST\n', 0.3);

/* settings menu: long OK opens it, then A (=UP) walks TIME -> ROM -> BATTLE.
 * Two presses since the five-row menu landed in round 107; the old three-step
 * version came from the nine-row list and silently opened APPEAR instead. */
console.log('-- opening the settings menu');
await press('OK', 3.4, 0.8);
await shot('01_menu_skip');
for (let i = 0; i < 2; i++) await press('UP', 0.35, 0.35);
await shot('02_menu_battle_row');

console.log('-- opening the card picker (B on the BATTLE row)');
await press('DOWN', 0.35, 0.8);
await shot('03_card_picker');

console.log('-- 2 s hold on B arms the tally wipe, B x3 confirms');
await press('DOWN', 2.6, 0.6);            // B held past CARD_CLEAR_HOLD_US
await shot('05_clear_prompt');
await press('DOWN', 0.3, 0.4);            // confirm 3 -> 2
await press('DOWN', 0.3, 0.4);            // confirm 2 -> 1
await press('DOWN', 0.3, 0.8);            // commit
await shot('06_after_clear');
uartSend('CARDL\n', 0.3);

uartSend('CARDST\n', 0.3);
console.log('-- back out and arm the card from the console');
await press('OK', 0.35, 0.8);            // C = back to the settings list
await press('OK', 0.35, 0.8);            // C = close the menu
uartSend('CARDGO 0\n', 0.4);
sleepSim(4.0);
uartSend('CARDST\n', 0.4);
uartSend('LINKSTAT\n', 0.3);
await shot('04_after_cardgo');

await writeFile(path.join(outDir, 'serial.log'), serialLog);
console.log('\nserial log -> ' + path.join(outDir, 'serial.log'));

// One simulator driver for the whole project (todo #13).
//
//   node tools/dg_sim.mjs <firmware-full.bin> [outDir] [--scenario=NAME] [--verbose]
//
// Scenarios:
//   boot     start, get past the clock screen, one shot
//   menu     open SETTINGS, walk all five rows, one shot each
//   groups   enter TIME / APPEAR / POWER and shoot each second level
//   clock    stop on the clock screen (mode=1) for time-set work
//   ac20     long-hold A on the clock screen and report how long A+C was held
//   rom      open the ROM slot list (needs a ROM in the image)
//   retail   switch to style C (brick wall) and shoot it, plus style A for reference
//   all      boot + menu + groups (the usual "did I break the UI" pass)
//
// Why this exists: before, every check copied 90 lines of wasm/PNG/rotate
// boilerplate and then drifted (dg_sim_ui.mjs still says the settings list has
// 7 rows).  Everything shared lives here; add a scenario instead of a script.
//
// The simulator framebuffer is 240x320 portrait; rotating it clockwise gives
// exactly what the eye sees on the toy (BSP_LCD_ROTATED_90 = swap_xy + mirror_x).

import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { deflateSync } from 'node:zlib';
import { pathToFileURL } from 'node:url';
import path from 'node:path';

const SIM = '<home>/passport-sim/FoloToy-Passport-Simulator-main';

/* ---- args --------------------------------------------------------------- */
function parseArgs(argv) {
  const out = { _: [] };
  for (const a of argv) {
    const m = /^--([\w-]+)(?:=(.*))?$/.exec(a);
    if (m) out[m[1]] = m[2] === undefined ? true : m[2];
    else out._.push(a);
  }
  return out;
}
const args = parseArgs(process.argv.slice(2));
const fwPath = args._[0];
const outDir = args._[1] || 'docs/ui-sim';
const scenario = String(args.scenario || 'boot');
const verbose = !!args.verbose;
if (!fwPath) {
  console.error('usage: node tools/dg_sim.mjs <firmware-full.bin> [outDir] [--scenario=NAME]');
  process.exit(2);
}
await mkdir(outDir, { recursive: true });

/* ---- emulator ----------------------------------------------------------- */
const { initSync, WasmEmulator } = await import(pathToFileURL(path.join(SIM, 'public/wasm/pkg/esp_emu.js')).href);
const { AiPassportBoard } = await import(pathToFileURL(path.join(SIM, 'public/wasm/ai-passport-board.js')).href);

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
  await writeFile(path.join(outDir, `${tag}.png`), encodePng(rotateCW(board.display.framebuffer), DW, DH));
  shots.push(tag);
  console.log(`[shot] ${tag}  t=${now().toFixed(1)}s`);
}

/* ---- uart --------------------------------------------------------------- */
const seen = [];
let uartBuf = '';
function drain() {
  let nl;
  while ((nl = uartBuf.indexOf('\n')) >= 0) {
    const line = uartBuf.slice(0, nl).replace(/\r$/, '');
    uartBuf = uartBuf.slice(nl + 1);
    if (!line.trim()) continue;
    seen.push(line);
    if (verbose) console.log('  [uart] ' + line);
  }
}
const now = () => emulator.cycles() / 160e6;
function runUntil(seconds) {
  const target = seconds * 160e6;
  while (emulator.cycles() < target) {
    const u = emulator.run_batch(200_000);
    if (u) { uartBuf += u; drain(); }
    board.drain();
  }
}
function uartSend(text) {
  const bytes = Buffer.from(text, 'ascii');
  for (let i = 0; i < bytes.length; i += 128) {
    emulator.uart_input(bytes.subarray(i, Math.min(i + 128, bytes.length)));
    runUntil(now() + 0.003);
  }
}
/** Ask the firmware for STATE and parse the reply into an object. */
function state(settle = 0.6) {
  const from = seen.length;
  uartSend('STATE\n');
  runUntil(now() + settle);
  const line = seen.slice(from).reverse().find((l) => l.startsWith('STATE '));
  if (!line) return null;
  const kv = {};
  for (const m of line.slice(6).matchAll(/(\w+)=([^\s]+)/g)) kv[m[1]] = m[2];
  return kv;
}
function waitFor(re, seconds, step = 0.1) {
  const end = now() + seconds;
  while (now() < end) {
    const hit = seen.slice(-40).reverse().find((l) => re.test(l));
    if (hit) return hit;
    runUntil(now() + step);
  }
  return null;
}

/* ---- input -------------------------------------------------------------- */
async function press(name, hold = 0.4, gap = 0.4) {
  board.setButton(name, true);
  runUntil(now() + hold);
  board.setButton(name, false);
  runUntil(now() + gap);
}
async function hold(name, seconds) {
  board.setButton(name, true);
  runUntil(now() + seconds);
  board.setButton(name, false);
  runUntil(now() + 0.4);
}

/** The ROM boots into the clock-set screen; this is the sequence that clears it. */
async function passClockScreen() {
  await press('DOWN', 0.5, 0.5);
  await press('UP', 0.5, 0.5);
  await press('DOWN', 0.5, 0.5);
  await press('OK', 0.5, 0.6);
  await press('UP', 0.5, 0.5);
  await press('OK', 0.5, 0.5);
  await press('DOWN', 0.5, 0.6);
}

/* ---- scenarios ---------------------------------------------------------- */
const MENU_ROWS = 5;                 /* TIME / ROM / BATTLE / APPEAR / POWER */

async function scBoot() {
  runUntil(3);
  await shot('01_boot');
  await passClockScreen();
  await shot('02_after_clock');
  const hatch = Number(args.hatch || 0);
  if (hatch > 0) {
    runUntil(hatch);
    await shot('03_hatched');
    await press('UP', 0.6, 0.8);      /* light the status icon */
    await shot('04_icon_lit');
  }
  const st = state();
  console.log('state:', st ? `mode=${st.mode} sub=${st.sub}` : '(no reply)');
}

async function scMenu() {
  runUntil(3);
  await passClockScreen();
  await hold('OK', 3.4);              /* long OK opens SETTINGS */
  for (let i = 0; i < MENU_ROWS; i++) {
    await shot(`menu_${i + 1}`);
    await press('UP', 0.3, 0.35);     /* A = next row */
  }
}

async function scGroups() {
  runUntil(3);
  await passClockScreen();
  await hold('OK', 3.4);
  await press('DOWN', 0.4, 0.6);      /* B on TIME */
  await shot('group_time');
  await press('OK', 0.4, 0.6);        /* C back */
  for (let i = 0; i < 3; i++) await press('UP', 0.3, 0.3);   /* TIME -> ROM -> BATTLE -> APPEAR */
  await press('DOWN', 0.4, 0.6);
  await shot('group_appear');
  await press('OK', 0.4, 0.6);
  await press('UP', 0.3, 0.3);        /* APPEAR -> POWER */
  await press('DOWN', 0.4, 0.6);
  await shot('group_power');
}

async function scClock() {
  runUntil(3);
  await passClockScreen();
  console.log('== force the clock screen ==');
  uartSend('RAMW 03 0\n');
  uartSend('RAMW 02 2\n');
  runUntil(now() + 2.5);
  const st = state();
  console.log('state:', st ? `mode=${st.mode} sub=${st.sub}` : '(no reply)');
  await shot('clock_screen');
}

async function scAc20() {
  await scClock();
  console.log('== hold A for 2.0 s on sub=0 (should inject and let go fast) ==');
  await hold('UP', 2.0);
  runUntil(now() + 0.6);
  const readDbg = () => {
    const from = seen.length;
    uartSend('DBG\n');
    runUntil(now() + 0.5);
    const line = seen.slice(from).reverse().find((l) => l.startsWith('DBG '));
    const m = line && line.match(/last=(-?\d+)/);
    return { line, last: m ? Number(m[1]) : null };
  };
  let d = readDbg();
  console.log('dbg:', d.line || '(none)');
  if (d.last > 0 && d.last < 3000) {
    console.log(`PASS: A+C held ${d.last} ms (watchdog cap 3200 ms)`);
  } else {
    console.log(`CHECK: A+C held ${d.last} ms`);
  }
  const st = state();
  console.log('state:', st ? `mode=${st.mode} sub=${st.sub}` : '(no reply)');
  await shot('ac20_after_first');

  /* Second hold, now in the time-set page (sub=1).  This must NOT inject:
   * a held A there is the ROM's own fast-increment, and injecting on top of it
   * was what made the hour jump three times and then freeze for ~2 s. */
  console.log('== hold A again on sub=1 (must not inject) ==');
  const before = d.last;
  await hold('UP', 2.0);
  runUntil(now() + 0.6);
  d = readDbg();
  console.log('dbg:', d.line || '(none)');
  if (d.last === before) {
    console.log(`PASS: no new injection (last still ${d.last} ms)`);
  } else {
    console.log(`FAIL: injected again, last changed ${before} -> ${d.last} ms`);
  }
  const st2 = state();
  console.log('state:', st2 ? `mode=${st2.mode} sub=${st2.sub}` : '(no reply)');
  await shot('ac20_after_second');
}

async function scRom() {
  runUntil(3);
  await passClockScreen();
  await hold('OK', 3.4);
  await press('UP', 0.3, 0.35);       /* TIME -> ROM */
  await press('DOWN', 0.4, 0.8);      /* open the slot list */
  await shot('rom_slots');
  const from = seen.length;
  uartSend('ROMINFO\n');
  runUntil(now() + 0.8);
  for (const l of seen.slice(from)) if (/^ROM/.test(l)) console.log('  ' + l);
}

async function scAll() {
  await scMenu();
  await scGroups();
}

/* Style C is the only place the brick tile shows, and it is not the default, so
 * every check of the wall needs this hop through the console command. */
async function scRetail() {
  runUntil(3);
  await passClockScreen();
  uartSend('STYLE 1\n');
  runUntil(now() + 1.5);
  await shot('retail_main');
  await hold('OK', 3.4);              /* the settings panel sits on the wall */
  await shot('retail_menu');
  await press('OK', 0.4, 0.6);        /* C closes */
  uartSend('STYLE 0\n');
  runUntil(now() + 1.5);
  await shot('classic_main');
  uartSend('STYLE 1\n');
  runUntil(now() + 0.8);
}

/* A rapid-tap probe: does the firmware merge fast repeats into one long press?
 * 5 taps at 80 ms hold / 80 ms gap (160 ms period) vs 5 taps at 450/450 ms.
 * The ROM advances the icon row on every accepted key event, so `mode` counts them. */
async function scRapid() {
  runUntil(3);
  await passClockScreen();
  runUntil(Number(args.hatch || 400));     /* the icon row only responds after hatching */
  let st = state();
  console.log('baseline mode=%s', st ? st.mode : '?');
  await shot('rapid_0_baseline');

  for (let i = 0; i < 5; i++) await press('UP', 0.08, 0.08);
  runUntil(now() + 1.2);
  st = state();
  console.log('after 5 fast taps (160 ms period): mode=%s', st ? st.mode : '?');
  await shot('rapid_1_fast');

  for (let i = 0; i < 5; i++) await press('UP', 0.45, 0.45);
  runUntil(now() + 1.2);
  st = state();
  console.log('after 5 slow taps (900 ms period): mode=%s', st ? st.mode : '?');
  await shot('rapid_2_slow');

  /* 100 ms period: 50 ms hold / 50 ms gap. Exercises the minimum-hold floor. */
  const from = st ? Number(st.mode) : 0;
  for (let i = 0; i < 5; i++) await press('UP', 0.05, 0.05);
  runUntil(now() + 1.2);
  st = state();
  console.log('after 5 very fast taps (100 ms period): mode=%s (delta %s)',
    st ? st.mode : '?', st ? Number(st.mode) - from : '?');
  await shot('rapid_3_veryfast');
}

/* Bypass the firmware's minimum-hold padding entirely: the console KEY command
 * drives dg_set_button() directly, so this measures what the ROM itself needs. */
async function scKeyProbe() {
  runUntil(3);
  await passClockScreen();
  runUntil(Number(args.hatch || 420));
  let st = state();
  console.log('baseline mode=%s', st ? st.mode : '?');

  for (const [hold, gap] of [[50, 100], [120, 250], [50, 200]]) {
    const from = st ? Number(st.mode) : 0;
    for (let i = 0; i < 5; i++) {
      uartSend(`KEY 4 ${hold}\n`);
      runUntil(now() + gap / 1000);
    }
    runUntil(now() + 1.0);
    st = state();
    console.log('5x KEY 4 %dms gap %dms -> mode %s (delta %s)',
      hold, gap, st ? st.mode : '?', st ? Number(st.mode) - from : '?');
  }
  await shot('keyprobe_end');
}

const scenarios = { boot: scBoot, menu: scMenu, groups: scGroups, clock: scClock, ac20: scAc20, rom: scRom, retail: scRetail, rapid: scRapid, keyprobe: scKeyProbe, all: scAll };
const fn = scenarios[scenario];
if (!fn) {
  console.error(`unknown scenario "${scenario}" (have: ${Object.keys(scenarios).join(', ')})`);
  process.exit(2);
}
await fn();
console.log('\nshots: ' + shots.join(', '));
console.log('frames: ' + frames + '  wall-clock: ' + now().toFixed(1) + 's emulated');

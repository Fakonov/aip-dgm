// ROM partition regression: boots the firmware with an empty 'roms' partition
// (the glass must say NO ROM), then exercises the whole console protocol the PC
// tool speaks -- bad CRC, upload, ROMINFO, ROMOUT, ROMBOOT, ROMDEL -- and
// photographs the glass between steps.
//
//   node tools/dg_sim_rom.mjs <firmware-full.bin> <outDir> [--rom file[:name]]...
//
// Console traffic goes over UART0: the emulator has no USB-Serial-JTAG model, so
// the verification build (-DDG_SIM_UART0_INPUT=ON) rebinds stdio there and this
// script feeds UART0 RX.  Device builds are untouched.

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
const romSpecs = [];
for (let i = 4; i < process.argv.length; i++) {
  if (process.argv[i] === '--rom') {
    const spec = process.argv[++i] || '';
    const [p, name] = spec.includes(':') ? [spec.slice(0, spec.lastIndexOf(':')), spec.slice(spec.lastIndexOf(':') + 1)] : [spec, 'V1'];
    romSpecs.push({ path: p, name });
  }
}
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

/* ---- console scraping --------------------------------------------------- */
let uartBuf = '';
const replies = [];
let romDump = null;                 // { slot, len, crc, name, hex }
function drain() {
  let nl;
  while ((nl = uartBuf.indexOf('\n')) >= 0) {
    const line = uartBuf.slice(0, nl).replace(/\r$/, '');
    uartBuf = uartBuf.slice(nl + 1);
    if (!line.trim()) continue;
    if (/^(ROM|SAVE|IMPORT)/.test(line)) replies.push(line);
    if (/^ROMBEGIN /.test(line)) {
      const m = /^ROMBEGIN (\d+) (\d+) ([0-9a-fA-F]{8}) (\S+)/.exec(line);
      romDump = { slot: +m[1], len: +m[2], crc: parseInt(m[3], 16) >>> 0, name: m[4], hex: '' };
    } else if (line === 'ROMEND' && romDump) {
      romDump.bytes = Buffer.from(romDump.hex, 'hex');
      delete romDump.hex;
      console.log(`  [dump] slot ${romDump.slot} ${romDump.bytes.length} B`);
    } else if (romDump && /^[0-9a-fA-F]+$/.test(line) && line.length % 2 === 0) {
      romDump.hex += line;
    }
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
  const png = encodePng(rotateCW(board.display.framebuffer), DW, DH);
  await writeFile(path.join(outDir, `${tag}.png`), png);
  shots.push(tag);
  console.log(`[shot] ${tag}  t=${now().toFixed(1)}s  dark=${(glassDark() * 100).toFixed(1)}%`);
}

/* dark-ink coverage where NO ROM is printed: the base glass tone is mid-green,
 * so any dark pixels there are either the letters or the running LCD */
function glassDark() {
  const fb = board.display.framebuffer;
  let dark = 0, total = 0;
  for (let xl = 96; xl < 224; xl++) {
    for (let yl = 96; yl < 148; yl++) {
      const sx = 239 - yl, sy = xl;
      const i = (sy * SW + sx) * 4;
      total++;
      if (fb[i] < 60 && fb[i + 1] < 60 && fb[i + 2] < 60) dark++;
    }
  }
  return total ? dark / total : 0;
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

/* ---- assertions --------------------------------------------------------- */
const results = [];
function check(name, ok, detail) {
  results.push({ name, ok, detail });
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '  (' + detail + ')' : ''}`);
}
const replyCount = (re) => replies.filter((l) => re.test(l)).length;

/* ---- scenario ----------------------------------------------------------- */
runUntil(3.0);
await shot('30_no_rom');
const darkNoRom = glassDark();
check('empty partition shows NO ROM', darkNoRom < 0.15, `dark=${(darkNoRom * 100).toFixed(1)}%`);

uartSend('ROMINFO\n');
sleepSim(0.6);

/* A device build keeps its console on USB-Serial-JTAG, which the emulator does
 * not model: the ROM area is still real (the glass check above passed), but no
 * reply ever comes back.  Say so instead of failing on protocol assertions. */
const consoleOnUart0 = replies.some((l) => l.startsWith('ROMSLOTS'));
if (!consoleOnUart0) {
  const failedStatic = results.filter((r) => !r.ok);
  console.log('\n(this is a device build: console is on USB-Serial-JTAG, UART0 sees no replies)');
  console.log('static checks only -- run build_sim.ps1 + this script for the full protocol run');
  console.log(failedStatic.length ? `${failedStatic.length} CHECK(S) FAILED` : 'NO ROM CHECK PASSED (static)');
  console.log('shots: ' + shots.join(', '));
  process.exitCode = failedStatic.length ? 1 : 0;
} else {
  check('ROMINFO reports 8 empty slots',
        replies.includes('ROMSLOTS 8') && replyCount(/^ROMSLOT \d+ - /) === 8 && replies.includes('ROMCUR -1 --'));

  if (romSpecs.length) {
    const first = await readFile(romSpecs[0].path);
    const goodCrc = crc32(first);

    /* ---- wrong CRC must be rejected and leave the slot empty ---- */
    uartSend(`ROMIN 0 ${first.length} ${(goodCrc ^ 0xFFFF).toString(16)} ${romSpecs[0].name}\n`);
    sleepSim(0.5);
    uartSend(hexLines(first));
    uartSend('ROMEND\n');
    sleepSim(2.0);
    check('bad CRC is rejected', replyCount(/^ROMBAD /) >= 1 && replyCount(/^ROMOK /) === 0,
          replies.filter((l) => /^ROMBAD/.test(l)).join('; '));
    await shot('31_badcrc_still_no_rom');

    /* ---- real upload ---- */
    for (let i = 0; i < romSpecs.length; i++) {
      const spec = romSpecs[i];
      const bytes = await readFile(spec.path);
      const crc = crc32(bytes);
      uartSend(`ROMIN ${i} ${bytes.length} ${crc.toString(16)} ${spec.name}\n`);
      sleepSim(0.5);
      uartSend(hexLines(bytes));
      uartSend('ROMEND\n');
      sleepSim(2.0);
      const okLine = replies.find((l) => l.startsWith(`ROMOK ${i} `));
      check(`upload slot ${i} (${spec.name})`,
            !!okLine && okLine.endsWith(crc.toString(16).padStart(8, '0')),
            okLine || 'no ROMOK');
    }

    await shot('32_running');
    const darkRunning = glassDark();
    check('glass is alive after upload', darkRunning > 0.15, `dark=${(darkRunning * 100).toFixed(1)}%`);

    uartSend('ROMINFO\n');
    sleepSim(0.4);
    const lastSlot = romSpecs.length - 1;
    check('the slot uploaded last is the active ROM',
          replies.some((l) => new RegExp(`^ROMSLOT ${lastSlot} \\S+ \\d+ [0-9a-f]{8} ACTIVE`).test(l)) &&
          replies.some((l) => new RegExp(`^ROMCUR ${lastSlot} `).test(l)),
          replies.filter((l) => /ACTIVE|ROMCUR/.test(l)).slice(-2).join(' | '));

    /* ---- read it back and compare ---- */
    romDump = null;
    uartSend('ROMOUT 0\n');
    sleepSim(1.5);
    if (romDump && romDump.bytes) {
      const back = crc32(romDump.bytes);
      const same = romDump.bytes.length === first.length &&
                   Buffer.compare(Buffer.from(romDump.bytes), Buffer.from(first)) === 0;
      check('ROMOUT returns the exact image', same, `crc ${back.toString(16)}`);
    } else {
      check('ROMOUT returns the exact image', false, 'no dump');
    }

    /* ---- switch + erase ---- */
    if (romSpecs.length > 1) {
      uartSend('ROMBOOT 1\n');
      sleepSim(1.5);
      check('ROMBOOT switches slot', replies.some((l) => /^ROMBOOT 1 /.test(l)));
      await shot('33_slot1');
      uartSend('ROMDEL 1\n');
      sleepSim(1.0);
      check('ROMDEL reports the slot', replies.some((l) => /^ROMDELETED 1$/.test(l)));
    }

    uartSend('ROMDEL 0\n');
    sleepSim(1.5);
    await shot('34_after_delete');
    check('erasing the last ROM goes back to NO ROM',
          replies.includes('ROMDELETED 0') && glassDark() < 0.15,
          `dark=${(glassDark() * 100).toFixed(1)}%`);
  }

  const failed = results.filter((r) => !r.ok);
  console.log('\n' + (failed.length ? `${failed.length} CHECK(S) FAILED` : 'ALL ROM CHECKS PASSED'));
  console.log('shots: ' + shots.join(', '));
  process.exitCode = failed.length ? 1 : 0;
}

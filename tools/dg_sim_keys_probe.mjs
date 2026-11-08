import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { deflateSync } from 'node:zlib';
import { pathToFileURL } from 'node:url';
import path from 'node:path';

const SIM = '<home>/passport-sim/FoloToy-Passport-Simulator-main';
const { initSync, WasmEmulator } = await import(pathToFileURL(path.join(SIM, 'public/wasm/pkg/esp_emu.js')).href);
const { AiPassportBoard } = await import(pathToFileURL(path.join(SIM, 'public/wasm/ai-passport-board.js')).href);

const fw = await readFile(process.argv[2]);
const outDir = process.argv[3];
await mkdir(outDir, { recursive: true });

const wasm = initSync({ module: await readFile(path.join(SIM, 'public/wasm/pkg/esp_emu_bg.wasm')) });
const emulator = new WasmEmulator('esp32c3');
emulator.load_default_rom();
emulator.set_boot_from_rom(true);
emulator.set_wifi_config('Emulator Host Bridge', '');
emulator.load_firmware(fw);
const board = new AiPassportBoard(wasm, { emulator, onFrame() {}, onAudio() {} });
board.releaseButtons();

function crc32b(b){let c,crc=0xFFFFFFFF;for(let n=0;n<b.length;n++){c=(crc^b[n])&0xFF;for(let k=0;k<8;k++)c=c&1?0xEDB88320^(c>>>1):c>>>1;crc=c^(crc>>>8);}return (crc^0xFFFFFFFF)>>>0;}
function ch(t,d){const l=Buffer.alloc(4);l.writeUInt32BE(d.length);const td=Buffer.concat([Buffer.from(t,'ascii'),d]);const c=Buffer.alloc(4);c.writeUInt32BE(crc32b(td));return Buffer.concat([l,td,c]);}
function png(rgba,w,h){const raw=Buffer.alloc((w*4+1)*h);for(let y=0;y<h;y++){raw[y*(w*4+1)]=0;Buffer.from(rgba.buffer,rgba.byteOffset+y*w*4,w*4).copy(raw,y*(w*4+1)+1);}const ih=Buffer.alloc(13);ih.writeUInt32BE(w,0);ih.writeUInt32BE(h,4);ih[8]=8;ih[9]=6;return Buffer.concat([Buffer.from([0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A]),ch('IHDR',ih),ch('IDAT',deflateSync(raw,{level:6})),ch('IEND',Buffer.alloc(0))]);}

async function shot(tag){
  const fb = board.display.framebuffer;
  await writeFile(path.join(outDir, tag + '.png'), png(fb, 240, 320));
  // quick signature of the LCD area (centre region), to compare states
  let lit = 0;
  for (let y = 60; y < 260; y += 4) for (let x = 20; x < 220; x += 4) {
    const i = (y * 240 + x) * 4;
    if (fb[i] < 120) lit++;
  }
  console.log(`[shot] ${tag} t=${(emulator.cycles()/160e6).toFixed(2)}s dark_px=${lit}`);
}
function run(sec){const t=emulator.cycles()/160e6+sec;while(emulator.cycles()<t*160e6){emulator.run_batch(100_000);board.drain();}}
async function tap(name, hold, gap = 0.4){board.setButton(name,true);run(hold);board.setButton(name,false);run(gap);console.log(`[key] ${name} ${hold}s`);}

run(3); await shot('p0_boot');
await tap('UP', 0.6); await shot('p1_after_A');       // A at home => menu mode, LCD blanks
await tap('OK', 2.2); await shot('p2_after_longOK');  // long C => settings menu?
await tap('UP', 0.6); await shot('p3_after_A2');      // if the ROM got this, LCD changes
await tap('OK', 0.5); await shot('p4_after_OK');

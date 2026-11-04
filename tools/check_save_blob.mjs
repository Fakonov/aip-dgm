// Sanity-check the save-blob logic that tools/digimon_studio.html uses, against
// a real export from the device (docs/saves/*.hex).
//
//   node tools/check_save_blob.mjs docs/saves/digimon_v1_20261101-2259.hex
import { readFileSync } from 'node:fs';

const MAGIC = 0x31474744, HDR = 20;
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    t[i] = c >>> 0;
  }
  return t;
})();
const crc32 = (u8) => {
  let c = 0xFFFFFFFF;
  for (const b of u8) c = CRC_TABLE[(c ^ b) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
};
const u32 = (u8, o) => (u8[o] | (u8[o + 1] << 8) | (u8[o + 2] << 16) | (u8[o + 3] << 24)) >>> 0;

function fromHex(s) {
  const t = s.replace(/[^0-9a-fA-F]/g, '');
  const u8 = new Uint8Array(t.length >> 1);
  for (let i = 0; i < u8.length; i++) u8[i] = parseInt(t.substr(i * 2, 2), 16);
  return u8;
}
function parseSaveText(text) {
  const lines = text.split(/\r?\n/);
  const chunks = [];
  const hasMarker = /^\s*SAVEBEGIN/im.test(text);
  if (!hasMarker) {
    for (const raw of lines) {
      const line = raw.trim();
      if (!line || !/^[0-9a-fA-F]+$/.test(line) || line.length % 2) continue;
      chunks.push(fromHex(line));
    }
  } else {
    let started = false;
    for (const raw of lines) {
      const line = raw.trim();
      if (!line) continue;
      if (/^SAVEBEGIN/i.test(line)) { started = true; continue; }
      if (/^SAVEEND/i.test(line)) break;
      if (started) {
        if (!/^[0-9a-fA-F]+$/.test(line) || line.length % 2) continue;
        chunks.push(fromHex(line));
      }
    }
  }
  const total = chunks.reduce((n, c) => n + c.length, 0);
  const u8 = new Uint8Array(total);
  let off = 0;
  for (const c of chunks) { u8.set(c, off); off += c.length; }
  return u8;
}

const file = process.argv[2];
const text = readFileSync(file, 'utf8');
const m = /^SAVEBEGIN (\w+) (\d+)/m.exec(text);
const u8 = parseSaveText(text);
const magic = u32(u8, 0), romCrc = u32(u8, 4), stateSize = u32(u8, 8), stateCrc = u32(u8, 12), seq = u32(u8, 16);
const calc = crc32(u8.subarray(HDR));

console.log('file        :', file);
console.log('SAVEBEGIN   :', m ? m[1] + ' ' + m[2] : '(missing)');
console.log('bytes       :', u8.length, '/', m ? Number(m[2]) : '?');
console.log('magic       :', '0x' + magic.toString(16).padStart(8, '0'), magic === MAGIC ? 'OK' : 'BAD');
console.log('rom_crc     :', '0x' + romCrc.toString(16).padStart(8, '0'));
console.log('state_size  :', stateSize, 'len - 20 =', u8.length - HDR);
console.log('seq         :', seq);
console.log('state_crc   :', '0x' + stateCrc.toString(16).padStart(8, '0'),
            calc === stateCrc ? 'CRC MATCH' : 'CRC MISMATCH 0x' + calc.toString(16));
const ok = magic === MAGIC && u8.length === HDR + stateSize && calc === stateCrc;
console.log(ok ? '\nRESULT: blob accepted by the tool logic' : '\nRESULT: blob REJECTED');
process.exit(ok ? 0 : 1);

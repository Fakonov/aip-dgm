// Replay every care_rules_v*.json row against the rule arrays that are actually
// embedded in tools/digimon_studio.html.  This is the "100% reproduce" gate for
// the PC tool, independent of gen_evo_rules.py.
//
//   node tools/check_evo_tables.mjs [studio.html]
import { readFileSync } from 'node:fs';
import path from 'node:path';

const root = path.resolve(process.argv[2] ? path.dirname(process.argv[2]) : 'tools');
const htmlPath = process.argv[2] || path.join(root, 'digimon_studio.html');
const html = readFileSync(htmlPath, 'utf8');

function grabArray(name) {
  const marker = 'const ' + name + ' = [';
  const at = html.indexOf(marker);
  if (at < 0) throw new Error('missing ' + name);
  const start = html.indexOf('[', at);
  let depth = 0, inStr = false, quote = '', esc = false;
  for (let i = start; i < html.length; i++) {
    const ch = html[i];
    if (inStr) {
      if (esc) esc = false;
      else if (ch === '\\') esc = true;
      else if (ch === quote) inStr = false;
      continue;
    }
    if (ch === '"' || ch === "'" || ch === '`') { inStr = true; quote = ch; continue; }
    if (ch === '[') depth++;
    else if (ch === ']' && --depth === 0) return html.slice(start, i + 1);
  }
  throw new Error('unterminated ' + name);
}

function grabSpecies(name) {
  const marker = 'const ' + name + ' = [';
  const at = html.indexOf(marker);
  if (at < 0) throw new Error('missing ' + name);
  const start = html.indexOf('[', at);
  let depth = 0, inStr = false, quote = '', esc = false;
  for (let i = start; i < html.length; i++) {
    const ch = html[i];
    if (inStr) {
      if (esc) esc = false;
      else if (ch === '\\') esc = true;
      else if (ch === quote) inStr = false;
      continue;
    }
    if (ch === '"' || ch === "'" || ch === '`') { inStr = true; quote = ch; continue; }
    if (ch === '[') depth++;
    else if (ch === ']' && --depth === 0) return eval('(' + html.slice(start, i + 1) + ')');
  }
  throw new Error('unterminated ' + name);
}

const hit = (r, c, t, o, s) =>
  c >= r.care[0] && c <= r.care[1] && t >= r.train[0] && t <= r.train[1] &&
  o >= r.over[0] && o <= r.over[1] && s >= r.sleep[0] && s <= r.sleep[1];

let failed = 0;
for (const ver of ['V1', 'V2', 'V3', 'V4']) {
  const rules = eval('(' + grabArray('EVO_RULES_' + ver) + ')');
  const species = grabSpecies('SPECIES_' + ver);
  const json = JSON.parse(readFileSync(path.join(root, 'ref-disasm', `care_rules_${ver.toLowerCase()}.json`), 'utf8'));
  let bad = 0, n = 0;
  for (const [key, want] of Object.entries(json.evolution)) {
    const [slot, care, train, over, sleep] = key.split(',').map(Number);
    let got = null;
    for (const r of rules) {
      if (r.from === slot && hit(r, care, train, over, sleep)) { got = r.to; break; }
    }
    n++;
    if (got !== want) {
      bad++;
      if (bad <= 5) console.log(`  ${ver} mismatch ${key}: want ${want}, got ${got}`);
    }
  }
  console.log(`${ver}: species=${species.length} rules=${rules.length} rows=${n} mismatches=${bad}`);
  if (bad || species.length !== 16) failed = 1;
}
process.exit(failed);

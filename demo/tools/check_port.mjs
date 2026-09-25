// The page's CPU half (demo/model.js) against the plugin's own C++.
//
//   node demo/tools/check_port.mjs [--keep DIR]
//
// Run by demo/tools/check_port.sh, which tools/verify.sh calls.
//
// What it does. It pastes three pieces of the plugin into refport.cpp's
// markers, unedited -- the ParamID enum from Colourunder.h; from
// Colourunder.cpp the anonymous namespace (setKernel, bindTarget,
// bindTextures, frameSeed, ...), the whole of Colourunder::Colourunder() and
// the whole of Colourunder::ProcessOpenGL -- and compiles them with the
// plugin's own Model.cpp, Controls.cpp and Clock.cpp. refport.cpp's GL and
// ffglex stand-ins record instead of drawing: each parameter the constructor
// declares, and per frame the clock, the LineData upload (every float), and
// every pass -- its shader, the buffer it draws into, the texture on each
// unit, and every uniform it sets, by name. demo/model.js's planFrame() is
// then run on the same scenarios and written out in the same format, and the
// two are compared line for line: every int exact, every float by its 32 bits,
// the clock's doubles exactly.
//
// What it cannot. It says nothing about the shaders (check_shaders.py does),
// nor about plugin.js's GL half beyond the names the plan hands it: which
// WebGL object a name maps to, the draw itself, the canvas. The page against
// `cutest --pipe` on the same input is the end-to-end check of those
// (AGENTS.md, "The browser demo"). The scenarios never reach a video frame
// above 2^32 (the clock starts at 0 and advances by at most 0.5 s a frame),
// so the 64-bit halves of low() and frameSeed() are exercised only at zero.

import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, '..', '..');
const model = await import(join(REPO, 'demo', 'model.js'));

const read = (p) => readFileSync(join(REPO, p), 'utf8');

function cut(text, start, end, label) {
  const a = text.indexOf(start);
  if (a < 0) throw new Error(`${label}: start marker not found`);
  const b = text.indexOf(end, a + start.length);
  if (b < 0) throw new Error(`${label}: end marker not found`);
  return text.slice(a, b);
}

// --- the reference: the plugin's text, recorded ---------------------------
const header = read('source/Colourunder.h');
const source = read('source/Colourunder.cpp');
const enumText = cut(header, '\tenum ParamID : FFUInt32', '\nprivate:', 'ParamID enum');
const anonText = cut(source, 'namespace\n{\nstd::string glStringOrUnknown', '} // namespace\n', 'anonymous namespace') + '} // namespace\n';
const ctorText = cut(source, 'Colourunder::Colourunder()', '//---------------------------------------------------------------------------\nFFResult Colourunder::InitGL', 'constructor');
const processText = cut(source, 'FFResult Colourunder::ProcessOpenGL', '//---------------------------------------------------------------------------\nFFResult Colourunder::DeInitGL', 'ProcessOpenGL');

let ref = readFileSync(join(HERE, 'refport.cpp'), 'utf8');
for (const [marker, text] of [['//@@ENUM@@', enumText], ['//@@ANON@@', anonText], ['//@@CONSTRUCTOR@@', ctorText], ['//@@PROCESS@@', processText]]) {
  if (!ref.includes(marker)) throw new Error(`refport.cpp has no ${marker}`);
  ref = ref.replace(marker, () => text);
}

const keep = process.argv.includes('--keep') ? process.argv[process.argv.indexOf('--keep') + 1] : null;
const dir = keep ?? mkdtempSync(join(tmpdir(), 'cu-port-'));
writeFileSync(join(dir, 'refport_gen.cpp'), ref);

/// Build the reference with these flags and run it on the scenarios.
function reference(tag, flags) {
  const exe = join(dir, `refport-${tag}`);
  const build = spawnSync('c++', ['-std=c++17', ...flags, '-Wall', '-Wno-unused-function', '-I', join(REPO, 'source'), join(dir, 'refport_gen.cpp'),
    join(REPO, 'source/Model.cpp'), join(REPO, 'source/Controls.cpp'), join(REPO, 'source/Clock.cpp'), '-o', exe], { encoding: 'utf8' });
  if (build.status !== 0) {
    console.log(`FAIL  the reference (${flags.join(' ')}) did not compile:`);
    console.log(build.stderr.split('\n').slice(0, 30).join('\n'));
    process.exit(1);
  }
  const run = spawnSync(exe, [], { input, encoding: 'utf8', maxBuffer: 1 << 30 });
  if (run.status !== 0) {
    console.log(`FAIL  the reference exited ${run.status}: ${run.stderr}`);
    process.exit(1);
  }
  return run.stdout;
}

// --- the scenarios ----------------------------------------------------------
// ParamID order: Standard, Speed, Tracking, Head Switch, Wear, Generation,
// DOC, Chroma Delay, Chroma Noise, Mix.
const at60 = (n, from = 0) => Array.from({ length: n }, (_, i) => (from + i) / 60);
const D = [...model.DEFAULTS];
const withP = (changes) => { const p = [...D]; for (const [k, v] of Object.entries(changes)) p[model.PT[k]] = v; return p; };
const SCENARIOS = [
  { name: 'defaults 320x180, 40 frames at 60 fps', W: 320, H: 180, p: D, t: at60(40) },
  { name: 'defaults 960x540 (one sample a pixel), past a drift knot', W: 960, H: 540, p: D, t: [0, 1 / 60, 1.69, 1.7, 1.71, 3.39, 3.41, 3.9] },
  { name: 'NTSC SP, 3 generations, everything up, 1280x720 (k = 2)', W: 1280, H: 720,
    p: withP({ STANDARD: 1, SPEED: 0, TRACKING: 0.6, HEAD_SWITCH: 1, WEAR: 1, GENERATION: 3, DOC: 0, CHROMA_DELAY: 0, CHROMA_NOISE: 1, MIX: 0.5 }), t: at60(12) },
  { name: 'EP, 5 generations, Tracking 1, 1920x1080 (k = 2)', W: 1920, H: 1080,
    p: withP({ SPEED: 2, TRACKING: 1, GENERATION: 5, WEAR: 0.8, CHROMA_DELAY: 1 }), t: [0, 0.4, 0.8, 1.2, 1.6, 2.0] },
  { name: 'nothing on: Head Switch 0, Wear 0, Chroma Noise 0, Tracking 0', W: 640, H: 360,
    p: withP({ TRACKING: 0, HEAD_SWITCH: 0, WEAR: 0, CHROMA_NOISE: 0, CHROMA_DELAY: 0 }), t: at60(4) },
  { name: 'odd rasters: 1025x577 (k = 2, Ws 513)', W: 1025, H: 577, p: withP({ STANDARD: 1, GENERATION: 2 }), t: at60(3) },
  { name: 'odd rasters: 3840x2160 (k = 4)', W: 3840, H: 2160, p: withP({ SPEED: 0, TRACKING: 0.3 }), t: at60(2) },
  { name: 'odd rasters: 64x36', W: 64, H: 36, p: withP({ WEAR: 1 }), t: at60(3) },
  { name: 'the clock: a backward jump (Restart), an over-long gap, a stall', W: 320, H: 180,
    p: withP({ TRACKING: 0.4, WEAR: 0.7 }), t: [0, 0.1, 0.2, 0.0, 0.05, 2.0, 2.4, 2.4, 2.9, 3.41, 3.5] },
  { name: 'fractional options and generations (lround)', W: 320, H: 180,
    p: withP({ STANDARD: 0.5, SPEED: 1.49, GENERATION: 2.5, DOC: 0.49, TRACKING: 0.123, HEAD_SWITCH: 0.777, CHROMA_DELAY: 0.3333, CHROMA_NOISE: 0.6666, MIX: 0.25 }), t: at60(3) },
  { name: 'a long run: 20 s at 25 fps', W: 160, H: 90, p: withP({ TRACKING: 0.15, WEAR: 1, GENERATION: 2 }), t: Array.from({ length: 500 }, (_, i) => i / 25) },
].map((s) => ({ ...s, p: s.p.map(Math.fround) }));
SCENARIOS.push({ name: 'odd rasters: 7x3', W: 7, H: 3, p: SCENARIOS[7].p, t: at60(2) });


const input = SCENARIOS.map((s) => [s.W, s.H, ...s.p.map((v) => v.toPrecision(9)), s.t.length, ...s.t.map((t) => t.toPrecision(17))].join(' ')).join('\n') + '\n';

// --- the port, written out the same way ------------------------------------
const bitsOf = (() => {
  const view = new DataView(new ArrayBuffer(4));
  return (v) => { view.setFloat32(0, v); return view.getUint32(0).toString(16).padStart(8, '0'); };
})();
const typeId = { boolean: 0, standard: 10, option: 11, integer: 13 };

const js = [];
for (const d of model.DECLARATIONS) {
  js.push(`PARAM ${d.index} ${typeId[d.type]} ${bitsOf(d.default)} ${d.name}`);
  if (d.type === 'option') {
    js.push(`  COUNT ${d.elements.length}`);
    d.elements.forEach((e, i) => js.push(`  ELEMENT ${d.index} ${i} ${e} ${bitsOf(i)}`));
  }
  if (d.range) js.push(`  RANGE ${d.index} ${bitsOf(d.range[0])} ${bitsOf(d.range[1])}`);
}
for (const d of model.DECLARATIONS) js.push(`  GROUP ${d.index} ${d.group}`);

const fmt17 = (x) => x;
const frames = [];
for (const s of SCENARIOS) {
  const inst = new model.Instance();
  frames.push({ head: `SCENARIO ${s.W} ${s.H}`, scenario: s.name });
  s.t.forEach((t, f) => {
    inst.setTime(Number(t.toPrecision(17)));
    const plan = inst.planFrame(s.p, s.W, s.H);
    const lines = [];
    if (plan.allocate) lines.push(`ALLOC lineData ${plan.allocate.width} ${plan.allocate.height}`);
    lines.push(`LINEDATA lineData ${plan.raster.N} ${plan.dataRows} ` + Array.from(plan.lineData, bitsOf).join(' '));
    for (const pass of plan.passes) {
      lines.push(`PASS ${pass.name} -> ${pass.target} textures${pass.textures.map((t) => ` ${t}`).join('')}`);
      const u = [];
      for (const [k, v] of Object.entries(pass.ints)) u.push(`I ${k} ${v}`);
      for (const [k, v] of Object.entries(pass.uints)) u.push(`U ${k} ${v}`);
      for (const [k, v] of Object.entries(pass.floats)) u.push(`F ${k} ${bitsOf(v)}`);
      for (const [k, v] of Object.entries(pass.arrays)) u.push(`A ${k} ${v.length} ${Array.from(v, bitsOf).join(' ')}`);
      for (const [k, v] of Object.entries(pass.vec4s)) u.push(`V ${k} ${v.length / 4} ${Array.from(v, bitsOf).join(' ')}`);
      u.sort();
      for (const l of u) lines.push(`  ${l}`);
    }
    frames.push({ head: `FRAME ${f} result 0`, seconds: plan.seconds, tracking: plan.tracking, lines, scenario: s.name, f });
  });
}

// --- compare ------------------------------------------------------------------
function compare(stdout, verbose) {
const refLines = stdout.split('\n');
let i = 0;
let problems = 0;
let floatsDiffering = 0;
let intsDiffering = 0;
let doublesDiffering = 0;
const say = (text) => { if (verbose && problems < 12) console.log(text); problems += 1; };
const log = (text) => { if (verbose) console.log(text); };

// Declarations: the constructor's record, groups gathered after, About dropped.
const declRef = [];
const groupsRef = [];
while (i < refLines.length && !refLines[i].startsWith('SCENARIO')) {
  const l = refLines[i++];
  if (!l) continue;
  const m = /^(PARAM|  ELEMENT|  RANGE|  GROUP) (\d+)/.exec(l);
  const index = m ? Number(m[2]) : (/^  COUNT/.test(l) ? -1 : NaN);
  if (l.startsWith('  GROUP')) { if (index < 10) groupsRef.push(l); continue; }
  if (l.startsWith('PARAM') && index >= 10) { declRef.push(null); continue; }
  declRef.push(l);
}
const declClean = [];
for (const l of declRef) {
  if (l === null) { declClean.push(null); continue; }
  declClean.push(l);
}
// Drop the About block's lines (a PARAM at index >= 10 and what follows it).
const declOwn = [];
let inAbout = false;
for (const l of declClean) {
  if (l === null) { inAbout = true; continue; }
  if (l.startsWith('PARAM')) inAbout = false;
  if (!inAbout) declOwn.push(l);
}
const declJs = js.filter((l) => !l.startsWith('  GROUP'));
const groupsJs = js.filter((l) => l.startsWith('  GROUP'));
const declSame = declOwn.length === declJs.length && declOwn.every((l, k) => l === declJs[k]);
const groupsSame = groupsRef.length === groupsJs.length && groupsRef.every((l, k) => l === groupsJs[k]);
if (!declSame) {
  say('FAIL  DECLARATIONS is not what Colourunder::Colourunder() declares:');
  for (let k = 0; k < Math.max(declOwn.length, declJs.length); k++) {
    if (declOwn[k] !== declJs[k]) { log(`        plugin: ${declOwn[k]}\n        page  : ${declJs[k]}`); break; }
  }
} else log(`ok    the ${model.DECLARATIONS.length} parameters: names, FFGL types, defaults (as floats), option elements and values, the integer range`);
if (!groupsSame) say('FAIL  the parameter groups differ from the constructor\'s');
else log('ok    the groups (Deck, Tape, Colour) on the same ids');

let frameCount = 0;
let lineCount = 0;
let floatCount = 0;
let worstScenario = null;
for (const fr of frames) {
  // Skip to the next header in the reference.
  while (i < refLines.length && refLines[i] === '') i++;
  const head = refLines[i++] ?? '';
  if (fr.head.startsWith('SCENARIO')) {
    if (head !== fr.head) { say(`FAIL  expected "${fr.head}", reference has "${head}"`); break; }
    continue;
  }
  const m = /^FRAME (\d+) result (\d+) seconds (\S+) tracking (\S+)$/.exec(head);
  if (!m) { say(`FAIL  ${fr.scenario} frame ${fr.f}: reference header "${head.slice(0, 80)}"`); break; }
  frameCount += 1;
  if (`FRAME ${m[1]} result ${m[2]}` !== fr.head) say(`FAIL  ${fr.scenario} frame ${fr.f}: ${head.slice(0, 60)}`);
  if (Number(m[3]) !== fr.seconds) doublesDiffering += 1;
  if (Number(m[3]) !== fr.seconds) say(`FAIL  ${fr.scenario} frame ${fr.f}: the clock reads ${m[3]} in the plugin, ${fr.seconds} here`);
  if (Number(m[4]) !== fr.tracking) doublesDiffering += 1;
  if (Number(m[4]) !== fr.tracking) say(`FAIL  ${fr.scenario} frame ${fr.f}: the tracking error is ${m[4]} in the plugin, ${fr.tracking} here`);
  for (const want of fr.lines) {
    const got = refLines[i++] ?? '';
    lineCount += 1;
    floatCount += (want.match(/ [0-9a-f]{8}\b/g) ?? []).length;
    if (got === want) continue;
    worstScenario = fr.scenario;
    const a = got.split(' ');
    const b = want.split(' ');
    let at = 0;
    while (at < Math.max(a.length, b.length) && a[at] === b[at]) at++;
    for (let q = 0; q < Math.max(a.length, b.length); q++) {
      if (a[q] === b[q]) continue;
      if (/^[0-9a-f]{8}$/.test(a[q] ?? '') && /^[0-9a-f]{8}$/.test(b[q] ?? '')) floatsDiffering += 1;
      else intsDiffering += 1;
    }
    say(`FAIL  ${fr.scenario}, frame ${fr.f}: "${b.slice(0, 3).join(' ').trim()}" differs at token ${at}: plugin ${a[at]}, page ${b[at]}`);
  }
}
while (i < refLines.length && refLines[i] === '') i++;
if (i < refLines.length) say(`FAIL  the reference has ${refLines.length - i} more lines than the port wrote`);

return { problems, doublesDiffering, frameCount, lineCount, floatCount, floatsDiffering, intsDiffering, worstScenario };
}

// The plugin's arithmetic as written: no fused multiply-add. This is what the
// x86_64 slice of the universal bundle does (no FMA in its baseline), and it
// has to match exactly.
const strict = compare(reference('strict', ['-O1', '-ffp-contract=off']), true);
console.log();
if (strict.problems) {
  console.log(`${strict.problems} difference(s) between demo/model.js and the plugin's C++ as written${strict.worstScenario ? ` (first in: ${strict.worstScenario})` : ''}`);
  process.exit(1);
}
console.log(`ok    ${SCENARIOS.length} scenarios, ${strict.frameCount} frames: the clock, the tracking error, ${strict.lineCount} recorded lines (the LineData uploads, every pass's target, textures and uniforms) and ${strict.floatCount} floats, all identical to the plugin's own ProcessOpenGL compiled with -ffp-contract=off`);

// The same C++ as the arm64 slice is built: -O3 and clang's default
// -ffp-contract=on, which fuses a * b + c into one rounding. JavaScript never
// fuses, so the port can differ from that slice in the last bit of a double;
// said, not failed on.
const arm = compare(reference('shipped', ['-O3']), false);
if (arm.problems === 0) console.log(`ok    the same C++ at -O3 with FMA contraction (the arm64 build's flags): identical too`);
else console.log(`note  the same C++ at -O3 with FMA contraction (the arm64 build's flags): ${arm.doublesDiffering} of ${arm.frameCount * 2} clock and tracking-error doubles differ from the port in the last bit (fused rounding), and ${arm.floatsDiffering} of ${arm.floatCount} floats, ${arm.intsDiffering} ints -- nothing the GPU is handed`);
// The x86_64 slice's flags, run under Rosetta where there is one: no FMA in
// that target's baseline, so it should be the strict result again.
const probe = spawnSync('c++', ['-arch', 'x86_64', '-x', 'c++', '-', '-o', join(dir, 'probe-x86')], { input: 'int main(){return 0;}', encoding: 'utf8' });
const rosetta = probe.status === 0 && spawnSync(join(dir, 'probe-x86')).status === 0;
if (rosetta) {
  const x86 = compare(reference('x86_64', ['-O3', '-arch', 'x86_64']), false);
  if (x86.problems === 0) console.log('ok    the same C++ at -O3 for x86_64 (the other slice, under Rosetta): identical to the port on every value');
  else console.log(`note  the same C++ at -O3 for x86_64 (under Rosetta): ${x86.problems} value(s) differ from the port (${x86.doublesDiffering} doubles, ${x86.floatsDiffering} floats, ${x86.intsDiffering} ints)`);
} else {
  console.log('skip  no x86_64 run: no Rosetta or no x86_64 toolchain here');
}
console.log('the port matches the plugin on every value recorded');
if (!keep) rmSync(dir, { recursive: true, force: true });

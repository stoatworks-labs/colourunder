/**
 * Colourunder's CPU half, PORTED to JavaScript.
 *
 * In the plugin everything that is not a convolution, a noise sample or a
 * display tap is computed once a frame on the CPU in double and handed to the
 * GPU as float uniforms or as one small RGBA32F texture (`LineData`). That C++
 * cannot run in a browser, so this file is a hand translation of it:
 *
 *   source/Model.cpp      the two standards, the band edges, the Gaussian and
 *                         noise kernels, the display and box gains, the PCG
 *                         hash, the seeded drift behind the tracking error,
 *                         the tracking geometry (RF, bar weight, noise gain),
 *                         the head switch's line and offset, the dropouts, the
 *                         per-line phase error, the bar's jitter
 *   source/Controls.cpp   every slider's law
 *   source/Clock.cpp      the part that runs with the unit declared (the
 *                         harness's and this page's case): origin + offset in
 *                         double, a jump of one nominal frame on a backward or
 *                         over-long delta. The unit vote never runs here.
 *   Colourunder.cpp       the CPU half of ProcessOpenGL: the settings, the
 *                         raster, the clock, the video frame, the nine
 *                         kernels, LineData (per generation: noise gain, bar
 *                         weight, cos and sin of the phase error; the display
 *                         row: displacement and the sample it starts at),
 *                         the dropouts in samples, the noise seeds and every
 *                         uniform of every pass, in the order they are set.
 *
 * `planFrame()` returns all of it as a plan -- the LineData floats and the
 * passes with their uniforms -- which the page's GL half (plugin.js) executes.
 * `demo/tools/check_port.sh` compiles the plugin's own Model.cpp,
 * Controls.cpp and Clock.cpp with Colourunder.cpp's ProcessOpenGL cut out of
 * the file at run time (GL stubbed to record every uniform and upload), and
 * compares its record with `planFrame()`'s, value for value. Only a reader
 * checks the GL half.
 *
 * Arithmetic is kept in the C++'s order, in doubles; a C++ `float` is
 * `Math.fround`, `std::lround` and `std::round` round half away from zero,
 * and the 32-bit hash is `Math.imul` and `>>> 0`.
 */

const f32 = Math.fround;

/** std::round / std::lround: half away from zero. */
export const roundAway = (x) => (x < 0 ? -Math.round(-x) : Math.round(x));
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

//---------------------------------------------------------------------------
// Model.h
//---------------------------------------------------------------------------

export const kPi = 3.14159265358979323846;

export const kPAL = 0;
export const kNTSC = 1;
export const kStandardCount = 2;

export const kSP = 0;
export const kLP = 1;
export const kEP = 2;
export const kSpeedCount = 3;

export const kChromaHalfHz = 0.5e6;
export const kSwitchBeforeVSync = 6.5;
export const kAfcLines = 12.0;
export const kSwitchBurstUs = 2.0;
export const kDeemphasisHz = 273755.82;
export const kTrackSlope = 2.0;
export const kRfThreshold = 0.06;
export const kRfNoiseKnee = 0.15;
export const kDriftKnotSeconds = 1.7;
export const kGenerationsMin = 1;
export const kGenerationsMax = 5;
export const kMaxDropouts = 24;
export const kMaxTaps = 96;
export const kMaxSamples = 1024;

//---------------------------------------------------------------------------
// Model.cpp
//---------------------------------------------------------------------------

const kFhPal = 15625.0;
const kFhNtsc = 15750000.0 / 1001.0;

function standard(name, line, frontPorch, sync, backPorch, frameLines, fieldLines, preEqualising, frameNum, frameDen, colourUnderHz, lineHz) {
  return {
    name, line, frontPorch, sync, backPorch, frameLines, fieldLines, preEqualising, frameNum, frameDen, colourUnderHz, lineHz,
    active() { return this.line - this.frontPorch - this.sync - this.backPorch; },
    fieldActive() { return Math.trunc(this.frameLines / 2); },
    frameRate() { return this.frameNum / this.frameDen; },
  };
}

const kStandards = [
  standard('625/50 (PAL)', 64.0, 1.65, 4.7, 5.7, 576, 312.5, 2.5, 25, 1, 40.125 * kFhPal, kFhPal),
  standard('525/59.94 (NTSC)', 1001.0 / 15.75, 1.5, 4.7, 4.7, 480, 262.5, 3.0, 30000, 1001, 40.0 * kFhNtsc, kFhNtsc),
];

export const standardOf = (index) => kStandards[clamp(index, 0, kStandardCount - 1)];

const frac = (x) => x - Math.floor(x);
const gauss = (x, sigma) => Math.exp(-x * x / (2.0 * sigma * sigma));

/** Model::Hash, the fleet's PCG output mix, exact in 32 bits. */
export function hash(v) {
  v >>>= 0;
  const state = (Math.imul(v, 747796405) + 2891336453) >>> 0;
  const word = Math.imul(((state >>> ((state >>> 28) + 4)) ^ state) >>> 0, 277803737) >>> 0;
  return ((word >>> 22) ^ word) >>> 0;
}

export const hashUnit = (h) => (h >>> 8) * (1.0 / 16777216.0);
const hashSigned = (h) => hashUnit(h) * 2.0 - 1.0;

function mix(a, b) {
  a >>>= 0;
  const inner = (hash(b) + 0x9e3779b9 + ((a << 6) >>> 0) + (a >>> 2)) >>> 0;
  return hash((a ^ inner) >>> 0);
}

/** low( int64 ): the two 32-bit halves of the two's complement, xored. */
function low(v) {
  const b = BigInt.asUintN(64, BigInt(v));
  return Number((b & 0xffffffffn) ^ (b >> 32n)) >>> 0;
}

const kDriftSeed = 0x43550001;
const kDropSeed = 0x43550002;
const kPhaseSeed = 0x43550003;
const kJitterSeed = 0x43550004;

export function lumaHalfHz(speed) {
  switch (speed) {
    case kLP: return 3.0e6 * 230.0 / 250.0;
    case kEP: return 2.4e6;
    default: return 3.0e6;
  }
}

export function lumaNoise(speed) {
  switch (speed) {
    case kLP: return 0.018;
    case kEP: return 0.026;
    default: return 0.012;
  }
}

export function chromaNoiseFactor(speed) {
  switch (speed) {
    case kLP: return 1.25;
    case kEP: return 1.6;
    default: return 1.0;
  }
}

export function makeRaster(s, W, H) {
  const r = {};
  r.W = Math.max(1, W);
  r.H = Math.max(1, H);
  r.k = Math.max(1, Math.trunc((r.W + kMaxSamples - 1) / kMaxSamples));
  r.Ws = Math.trunc((r.W + r.k - 1) / r.k);
  r.N = s.frameLines;
  r.usPerPixel = s.active() / r.W;
  r.usPerSample = r.usPerPixel * r.k;
  return r;
}

const fieldOf = (line) => line & 1;
const fieldLineOf = (line) => line >> 1;

export function sigmaUs(fHalfHz) {
  return Math.sqrt(Math.log(2.0) / 2.0) / (kPi * fHalfHz) * 1e6;
}

export function sigmaUsWith(fHalfHz, others) {
  if (others >= 1.0) return sigmaUs(fHalfHz);
  if (others <= 0.5) return 0.0;
  return Math.sqrt(Math.log(2.0 * others) / 2.0) / (kPi * fHalfHz) * 1e6;
}

export function displayGain(cyclesPerPixel, k) {
  if (k <= 1) return 1.0;
  const nu = cyclesPerPixel * k;
  let re = 0.0;
  for (let c = 0; c < k; ++c) {
    const s = (c - 0.5 * (k - 1)) / k;
    const t = s - Math.floor(s);
    const t2 = t * t;
    const t3 = t2 * t;
    const w = [0.5 * (-t3 + 2.0 * t2 - t), 0.5 * (3.0 * t3 - 5.0 * t2 + 2.0), 0.5 * (-3.0 * t3 + 4.0 * t2 + t), 0.5 * (t3 - t2)];
    for (let n = -1; n <= 2; ++n) re += w[n + 1] * Math.cos(2.0 * kPi * nu * (n - t));
  }
  return re / k;
}

export function boxGain(cyclesPerPixel, k) {
  if (k <= 1) return 1.0;
  return Math.abs(Math.sin(kPi * k * cyclesPerPixel) / (k * Math.sin(kPi * cyclesPerPixel)));
}

/** Model::Kernel: `first`, `count` and float `weights`. */
function kernel(first, weights) {
  return { first, count: weights.length, weights: Float32Array.from(weights) };
}

export function gaussian(sigma, delay, centre = 0.0) {
  const shift = delay + centre;
  if (sigma <= 1e-9) return kernel(-roundAway(shift), [1.0]);
  const reach = 4.0 * sigma;
  let lo = Math.floor(-shift - reach);
  let hi = Math.ceil(-shift + reach);
  if (hi - lo + 1 > kMaxTaps) {
    const excess = hi - lo + 1 - kMaxTaps;
    lo += Math.trunc(excess / 2);
    hi = lo + kMaxTaps - 1;
  }
  const w = [];
  let total = 0.0;
  for (let o = lo; o <= hi; ++o) {
    w.push(gauss(o + shift, sigma));
    total += w[o - lo];
  }
  return kernel(lo, w.map((x) => f32(x / total)));
}

export function noiseShape(sigma, lo) {
  const wide = Math.sqrt(sigma * sigma + lo * lo);
  const a = gaussian(sigma, 0.0);
  const b = gaussian(wide, 0.0);
  const first = Math.min(a.first, b.first);
  const last = Math.max(a.first + a.count, b.first + b.count) - 1;
  const count = Math.min(kMaxTaps, last - first + 1);
  const w = new Array(count).fill(0.0);
  let power = 0.0;
  for (let i = 0; i < count; ++i) {
    const o = first + i;
    const ia = o - a.first;
    const ib = o - b.first;
    let v = 0.0;
    if (ia >= 0 && ia < a.count) v += a.weights[ia];
    if (ib >= 0 && ib < b.count) v -= b.weights[ib];
    w[i] = v;
    power += v * v;
  }
  const scale = power > 0.0 ? 1.0 / Math.sqrt(power) : 0.0;
  return kernel(first, w.map((x) => f32(x * scale)));
}

export function unitPower(k) {
  let power = 0.0;
  for (let i = 0; i < k.count; ++i) power += k.weights[i] * k.weights[i];
  const scale = power > 0.0 ? 1.0 / Math.sqrt(power) : 0.0;
  return kernel(k.first, Array.from(k.weights, (x) => f32(x * scale)));
}

export function drift(seconds, stream) {
  const x = seconds / kDriftKnotSeconds;
  const k0 = Math.floor(x);
  const t = x - k0;
  const knot = (k) => hashSigned(mix(mix(kDriftSeed, stream), low(k)));
  const p0 = knot(k0 - 1);
  const p1 = knot(k0);
  const p2 = knot(k0 + 1);
  const p3 = knot(k0 + 2);
  const v = 0.5 * (2.0 * p1 + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t * t
    + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t * t * t);
  return clamp(v, -1.0, 1.0);
}

export const trackingError = (amount, seconds, stream) => amount * (1.0 + 0.8 * drift(seconds, stream));

function geometry(s) {
  const Lf = s.fieldLines;
  const Nf = s.fieldActive();
  return { a0: Lf - Nf - s.preEqualising, uBlank: frac((Lf - s.preEqualising + (Lf - Nf) / 2.0) / Lf) };
}

export function rfAt(s, e, m) {
  const g = geometry(s);
  const u = (m + 0.5 + g.a0) / s.fieldLines;
  const u0 = g.uBlank - 1.0 / kTrackSlope;
  const p = e + kTrackSlope * (u - u0);
  const d = Math.abs(p - 2.0 * roundAway(p / 2.0));
  return 1.0 - d;
}

export const barWeight = (rf) => clamp((kRfThreshold - rf) / kRfThreshold, 0.0, 1.0);

export function noiseGain(rf) {
  const x = clamp((kRfNoiseKnee - rf) / kRfNoiseKnee, 0.0, 1.0);
  return 1.0 + 3.0 * x * x;
}

export function switchLine(s) {
  return s.fieldActive() - (kSwitchBeforeVSync - s.preEqualising);
}

export function switchOffsetUs(s, line) {
  const f = frac(line);
  if (f <= 0.0) return 0.0;
  return Math.max(0.0, f * s.line - s.sync - s.backPorch);
}

export function dropouts(frame, generation, perFrame, N, activeUs) {
  const list = [];
  if (perFrame <= 0.0) return list;
  const base = mix(mix(kDropSeed, generation), low(frame));
  const p = Math.min(1.0, perFrame / kMaxDropouts);
  for (let i = 0; i < kMaxDropouts; ++i) {
    const h = mix(base, i);
    if (hashUnit(h) >= p) continue;
    const line = Math.min(N - 1, Math.trunc(hashUnit(hash((h + 1) >>> 0)) * N));
    const u2 = hashUnit(hash((h + 3) >>> 0));
    const len = 1.0 + 20.0 * u2 * u2;
    const us0 = hashUnit(hash((h + 2) >>> 0)) * activeUs;
    list.push({ line, us0, us1: Math.min(activeUs, us0 + len) });
  }
  return list;
}

export function phaseError(frame, generation, line, sigmaRad) {
  if (sigmaRad <= 0.0) return 0.0;
  const field = fieldOf(line);
  const m = fieldLineOf(line) / 6.0;
  const k = Math.floor(m);
  const t = m - k;
  const s = t * t * (3.0 - 2.0 * t);
  const b = mix(mix(mix(kPhaseSeed, generation), low(frame)), field);
  const a = hashSigned(mix(b, k));
  const c = hashSigned(mix(b, k + 1));
  return sigmaRad * (a + (c - a) * s);
}

export const barJitter = (frame, line) => hashSigned(mix(mix(kJitterSeed, low(frame)), line));

//---------------------------------------------------------------------------
// Controls.cpp. Every host value arrives as a float.
//---------------------------------------------------------------------------

const unit = (value) => clamp(f32(value), 0.0, 1.0);

export const controls = {
  optionIndex: (value, count) => clamp(roundAway(f32(value)), 0, count - 1),
  standardName: (index) => (index === kNTSC ? 'NTSC' : 'PAL'),
  speedName: (index) => (index === kLP ? 'LP' : index === kEP ? 'EP' : 'SP'),
  tracking: (value) => unit(value),
  headSwitchUs: (value) => 2.0 * unit(value),
  dropoutsPerFrame: (value) => { const v = unit(value); return 12.0 * v * v; },
  wearNoiseGain: (value) => 1.0 + 0.8 * unit(value),
  generation: (value) => clamp(roundAway(f32(value)), kGenerationsMin, kGenerationsMax),
  chromaDelayUs: (value) => unit(value),
  chromaDelayParam: (us) => f32(clamp(us, 0.0, 1.0)),
  chromaNoise: (value) => 0.04 * unit(value),
  phaseSigmaRad: (value) => 15.0 * unit(value) * kPi / 180.0,
  amount: (value) => f32(unit(value)),
};

//---------------------------------------------------------------------------
// Clock.cpp, with the unit declared (seconds): what cutest does and what this
// page does. The vote on the host's unit and the wall-clock fallback before it
// never run, because the unit is never undecided.
//---------------------------------------------------------------------------

export const kMaxFrameSeconds = 0.5;
export const kNominalFrameSeconds = 1.0 / 60.0;

export class Clock {
  constructor() { this.reset(); }

  reset() {
    this.started = false;
    this.lastScaled = -1.0;
    this.anchor = 0.0;
    this.offset = 0.0;
    this.now = 0.0;
    this.jumped = false;
  }

  update(hostTime) {
    this.jumped = false;
    const scaled = hostTime * 1.0;
    if (!this.started) {
      this.started = true;
      this.anchor = scaled;
      this.offset = 0.0;
      this.now = 0.0;
      this.lastScaled = scaled;
      return;
    }
    const delta = scaled - this.lastScaled;
    if (delta < 0.0 || delta > kMaxFrameSeconds) {
      this.jumped = true;
      this.offset = this.now + kNominalFrameSeconds;
      this.anchor = scaled;
      this.now = this.offset;
    } else {
      this.now = this.offset + (scaled - this.anchor);
    }
    this.lastScaled = scaled;
  }
}

//---------------------------------------------------------------------------
// Colourunder.cpp
//---------------------------------------------------------------------------

/** The parameter ids, in the plugin's enum order. */
export const PT = {
  STANDARD: 0, SPEED: 1, TRACKING: 2, HEAD_SWITCH: 3,
  WEAR: 4, GENERATION: 5, DOC: 6,
  CHROMA_DELAY: 7, CHROMA_NOISE: 8, MIX: 9,
};

/** Colourunder::Colourunder()'s defaults, as the floats it stores. */
export const DEFAULTS = [
  f32(kPAL), f32(kLP), f32(0.03), f32(0.45),
  f32(0.3), f32(1.0), f32(1.0),
  controls.chromaDelayParam(0.45), f32(0.4), f32(1.0),
];

/**
 * Every parameter Colourunder::Colourunder() declares, in its order: name,
 * FFGL type, group, the option elements (each element's value is its index)
 * and the default float, and Generation's SetParamRange. The About block is
 * left out, as on every page in this suite. check_port.sh compares this table
 * with what the plugin's own constructor declares, run under a recorder.
 */
export const FF_TYPE = { boolean: 0, standard: 10, option: 11, integer: 13 };
export const DECLARATIONS = [
  { index: PT.STANDARD, name: 'Standard', type: 'option', group: 'Deck', elements: ['PAL', 'NTSC'], default: DEFAULTS[PT.STANDARD] },
  { index: PT.SPEED, name: 'Speed', type: 'option', group: 'Deck', elements: ['SP', 'LP', 'EP'], default: DEFAULTS[PT.SPEED] },
  { index: PT.TRACKING, name: 'Tracking', type: 'standard', group: 'Deck', default: DEFAULTS[PT.TRACKING] },
  { index: PT.HEAD_SWITCH, name: 'Head Switch', type: 'standard', group: 'Deck', default: DEFAULTS[PT.HEAD_SWITCH] },
  { index: PT.WEAR, name: 'Wear', type: 'standard', group: 'Tape', default: DEFAULTS[PT.WEAR] },
  { index: PT.GENERATION, name: 'Generation', type: 'integer', group: 'Tape', default: DEFAULTS[PT.GENERATION], range: [kGenerationsMin, kGenerationsMax] },
  { index: PT.DOC, name: 'DOC', type: 'boolean', group: 'Tape', default: DEFAULTS[PT.DOC] },
  { index: PT.CHROMA_DELAY, name: 'Chroma Delay', type: 'standard', group: 'Colour', default: DEFAULTS[PT.CHROMA_DELAY] },
  { index: PT.CHROMA_NOISE, name: 'Chroma Noise', type: 'standard', group: 'Colour', default: DEFAULTS[PT.CHROMA_NOISE] },
  { index: PT.MIX, name: 'Mix', type: 'standard', group: 'Colour', default: DEFAULTS[PT.MIX] },
];

/** frameSeed() in Colourunder.cpp's anonymous namespace. */
export function frameSeed(frame, generation) {
  const b = BigInt.asUintN(64, BigInt(frame));
  const lo32 = Number(b & 0xffffffffn) >>> 0;
  const hi32 = Number(b >> 32n) >>> 0;
  return hash((lo32 ^ hash((hi32 + 0x4355AA00 + Math.imul(977, generation) >>> 0) >>> 0)) >>> 0);
}

/**
 * The plugin instance's CPU state across frames: the clock, the host time
 * last handed to SetTime and the last raster size. Colourunder's members of
 * the same names.
 */
export class Instance {
  constructor() {
    this.clock = new Clock();
    this.hostTimeSeen = false;
    this.hostTime = 0.0;
    this.lastWidth = 0;
    this.lastHeight = 0;
    this.lastTracking = 0.0;
    this.lineDataN = 0;
  }

  /** Colourunder::SetTime. */
  setTime(time) {
    this.hostTimeSeen = true;
    this.hostTime = time;
  }

  /**
   * The CPU half of Colourunder::ProcessOpenGL for one frame, as a plan: the
   * LineData texture's floats, and every pass in order with the uniforms the
   * plugin sets on it. `params` are the plugin's ten floats, PT order.
   */
  planFrame(params, W, H) {
    const p = params.map(f32);

    // The settings.
    const standardIndex = controls.optionIndex(p[PT.STANDARD], kStandardCount);
    const speed = controls.optionIndex(p[PT.SPEED], kSpeedCount);
    const tracking = controls.tracking(p[PT.TRACKING]);
    const switchUs = controls.headSwitchUs(p[PT.HEAD_SWITCH]);
    const dropsPerFrame = controls.dropoutsPerFrame(p[PT.WEAR]);
    const wearGain = controls.wearNoiseGain(p[PT.WEAR]);
    const generations = controls.generation(p[PT.GENERATION]);
    const doc = p[PT.DOC] >= 0.5;
    const delayUs = controls.chromaDelayUs(p[PT.CHROMA_DELAY]);
    const chromaNoise = controls.chromaNoise(p[PT.CHROMA_NOISE]) * chromaNoiseFactor(speed);
    const phaseSigma = controls.phaseSigmaRad(p[PT.CHROMA_NOISE]);
    const lumaNoiseSigma = lumaNoise(speed);
    const mixAmount = controls.amount(p[PT.MIX]);

    const s = standardOf(standardIndex);
    const R = makeRaster(s, W, H);

    // LineData is (re)allocated when the line count changes.
    let allocate = null;
    if (this.lineDataN !== R.N) {
      allocate = { width: R.N, height: kGenerationsMax + 1 };
      this.lineDataN = R.N;
    }

    // The clock. A resize is not a reason to touch it.
    this.lastWidth = W;
    this.lastHeight = H;
    this.clock.update(this.hostTimeSeen ? this.hostTime : -1.0);
    const seconds = this.clock.now;
    const frame = Math.floor(seconds * s.frameRate());

    // The kernels, in double.
    const lumaHalf = lumaHalfHz(speed);
    const chromaHalf = kChromaHalfHz;
    const lumaSigmaUs = sigmaUs(lumaHalf);
    const chromaSigmaUs = sigmaUs(chromaHalf);
    const lumaFirstUs = sigmaUsWith(lumaHalf, displayGain(lumaHalf * 1e-6 * R.usPerPixel, R.k));
    const chromaFirstUs = sigmaUsWith(chromaHalf, displayGain(chromaHalf * 1e-6 * R.usPerPixel, R.k) * boxGain(chromaHalf * 1e-6 * R.usPerPixel, R.k));
    const lowSigmaUs = sigmaUs(kDeemphasisHz);
    const intakeLuma = gaussian(lumaFirstUs / R.usPerPixel, 0.0, -0.5 * (R.k - 1));
    const lumaLater = gaussian(lumaSigmaUs / R.usPerSample, 0.0);
    const identity = gaussian(0.0, 0.0);
    const chromaFirst = gaussian(chromaFirstUs / R.usPerSample, delayUs / R.usPerSample);
    const chroma = gaussian(chromaSigmaUs / R.usPerSample, delayUs / R.usPerSample);
    const chromaNoiseShape = unitPower(gaussian(chromaSigmaUs / Math.sqrt(2.0) / R.usPerSample, delayUs / R.usPerSample));
    const lumaNoiseShape = noiseShape(lumaSigmaUs / R.usPerSample, lowSigmaUs / R.usPerSample);

    // Per line, in double; LineData is N wide and kGenerationsMax + 1 high.
    const sLine = switchLine(s);
    const switchAt = Math.floor(sLine);
    const switchSample = switchOffsetUs(s, sLine) / R.usPerSample;
    const dataRows = kGenerationsMax + 1;
    const data = new Float32Array(R.N * dataRows * 4);
    const finalBar = new Float64Array(R.N);
    for (let g = 1; g <= generations; ++g) {
      const e = trackingError(tracking, seconds, g);
      if (g === 1) this.lastTracking = e;
      for (let l = 0; l < R.N; ++l) {
        const m = fieldLineOf(l);
        const rf = rfAt(s, e, m);
        const w = barWeight(rf);
        const t = ((g - 1) * R.N + l) * 4;
        data[t + 0] = noiseGain(rf) * wearGain;
        data[t + 1] = w;
        const phi = phaseError(frame, g, l, phaseSigma);
        data[t + 2] = Math.cos(phi);
        data[t + 3] = Math.sin(phi);
        if (g === generations) finalBar[l] = w;
      }
    }
    for (let l = 0; l < R.N; ++l) {
      const m = fieldLineOf(l);
      let shift = 0.0;
      let from = 1e9;
      if (switchUs > 0.0 && m >= switchAt) {
        shift = generations * switchUs * Math.exp(-Math.max(0.0, m - sLine) / kAfcLines) / R.usPerSample;
        from = m === switchAt ? switchSample : -1e9;
      }
      const w = finalBar[l];
      if (w > 0.0) {
        shift += w * 1.5 * barJitter(frame, l) / R.usPerSample;
        from = -1e9;
      }
      const t = (kGenerationsMax * R.N + l) * 4;
      data[t + 0] = shift;
      data[t + 1] = from;
    }

    // The passes, with every uniform ProcessOpenGL sets, in its order.
    const passes = [];
    const kernelUniforms = (u, first, count, weights, k) => {
      u.ints[first] = k.first;
      u.ints[count] = k.count;
      u.arrays[weights] = k.weights;
    };
    // `target` is the buffer drawn into ('host' is the host's framebuffer);
    // `textures` what is bound on units 0, 1, 2 -- the plugin's names for them.
    const pass = (name, target, textures, extra = {}) => {
      const u = { name, target, textures, ints: {}, uints: {}, floats: {}, arrays: {}, vec4s: {}, ...extra };
      passes.push(u);
      return u;
    };

    let u = pass('intakev', 'columns', ['picture']);
    u.ints.Source = 0;
    u.ints.HostH = H;
    u.ints.Lines = R.N;

    u = pass('intakeh', 'intake', ['columns']);
    u.ints.Lines = 0;
    u.ints.HostW = W;
    u.ints.K = R.k;
    kernelUniforms(u, 'YFirst', 'YCount', 'YW', intakeLuma);

    let source = 'intake';
    for (let g = 1; g <= generations; ++g) {
      const drops = dropouts(frame, g, dropsPerFrame, R.N, s.active());
      const dropData = new Float32Array(4 * kMaxDropouts);
      const dropCount = Math.min(kMaxDropouts, drops.length);
      for (let i = 0; i < dropCount; ++i) {
        dropData[4 * i + 0] = drops[i].line;
        dropData[4 * i + 1] = drops[i].us0 / R.usPerSample;
        dropData[4 * i + 2] = drops[i].us1 / R.usPerSample;
      }

      u = pass('noise', 'noise', [], { generation: g });
      u.ints.Samples = R.Ws;
      u.ints.LineCount = R.N;
      u.uints.Seed = frameSeed(frame, g);

      u = pass('tape', 'work0', [source, 'noise', 'lineData'], { generation: g });
      u.ints.Src = 0;
      u.ints.NoiseTex = 1;
      u.ints.LineData = 2;
      u.ints.Samples = R.Ws;
      u.ints.Gen = g - 1;
      u.ints.Pal = standardIndex === kPAL ? 1 : 0;
      kernelUniforms(u, 'YFirst', 'YCount', 'YW', g === 1 ? identity : lumaLater);
      kernelUniforms(u, 'CFirst', 'CCount', 'CW', g === 1 ? chromaFirst : chroma);
      kernelUniforms(u, 'NYFirst', 'NYCount', 'NYW', lumaNoiseShape);
      kernelUniforms(u, 'NCFirst', 'NCCount', 'NCW', chromaNoiseShape);
      u.floats.LumaSigma = f32(lumaNoiseSigma);
      u.floats.ChromaSigma = f32(chromaNoise);
      u.ints.BurstLine = switchUs > 0.0 ? switchAt : -1;
      u.floats.BurstStart = f32(switchSample);
      u.ints.BurstSamples = Math.max(1, roundAway(kSwitchBurstUs / R.usPerSample));
      u.floats.BurstAmount = f32(Math.min(1.0, p[PT.HEAD_SWITCH] * 1.5));
      u.ints.DropCount = dropCount;
      u.vec4s.Drops = dropData;

      u = pass('comb', 'work1', ['work0'], { generation: g });
      u.ints.Src = 0;
      u.ints.Skip = 0;

      u = pass('doc', 'work2', ['work1'], { generation: g });
      u.ints.Src = 0;
      u.ints.Enabled = doc ? 1 : 0;
      u.ints.Step = 2;
      source = 'work2';
    }

    u = pass('display', 'host', ['work2', 'picture', 'lineData']);
    u.ints.Lines = 0;
    u.ints.Source = 1;
    u.ints.LineData = 2;
    u.ints.HostW = W;
    u.ints.HostH = H;
    u.ints.LineCount = R.N;
    u.ints.Samples = R.Ws;
    u.ints.K = R.k;
    u.ints.TimeRow = kGenerationsMax;
    u.floats.MixAmount = mixAmount;

    return {
      allocate, raster: R, standard: s, standardIndex, speed, generations, seconds, frame, lineData: data, dataRows, passes,
      tracking: this.lastTracking, switchLine: sLine,
    };
  }
}

# AGENTS.md — Colourunder

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you tell
anybody this works.

---

## What the plugin is

VHS's helical-scan colour-under recording as an FFGL 2.1 effect (`CU01`, shown as
`SW Colourunder`) for Resolume Arena and Avenue. C++17 + GLSL 4.10, CMake, universal
macOS `.bundle` and (by CI, untried) a Windows `.dll`. MIT, intended home
`github.com/stoatworks-labs/colourunder`.

Built 2026-09-25 (tranche five; Allan's own pick) in one session from
`specs/SPEC-colourunder.md` with `BRIEF.md` and `BRIEF-ADDENDUM.md`: gate for the plugin
shape, the harness, `--pipe`, the software-renderer pass, verify and the sweep; clamp for
the two rasters and the clock (standards' before it); tinsel for `PassBuffer` and the trap
list; toner for the output-alpha decision; ferric and old-cathode read as the neighbours
it must not duplicate.

---

## The one idea

**VHS cannot record colour at its broadcast frequency, so it heterodynes the chroma down
to about 627 kHz (PAL) or 629 kHz (NTSC) under the luma's FM carrier, and back up on
playback.** Model that and the spinning heads it rides on, and the look falls out:

| the stage | what comes out |
| --- | --- |
| chroma recorded as a narrow band under the FM luma | **colour smears**: half amplitude at 0.5 MHz, 39 lines of chroma against luma's 234 (SP) |
| the playback filters' group delay | **colour arrives late**, right of every edge |
| noise added to the colour-under band, then the playback filter | **blotches, not grain**: horizontal streaks of colour noise |
| FM luma: noise rising with frequency, de-emphasis | fine horizontal luma noise, more at LP and EP |
| the playback phase error, per line, smooth down the field | **NTSC: hue wanders. PAL: the alternating V axis and the 1H average turn it into desaturation** |
| the 1H comb (NTSC's crosstalk canceller, PAL's delay line) | chroma's vertical resolution halved, both standards |
| two heads, switching 6.5 H before V sync | **the bottom lines tear sideways** and a switching transient crosses them |
| the head's path against the recorded track | **a noise bar** where it reads the wrong track; it rests in the vertical interval and walks up the picture as the tracking error drifts |
| missing oxide | a white streak, or with **DOC** the line 1H before repeated |
| a copy of a copy | the whole chain again: chroma at 0.35 MHz after two, the delay twice, the noise added again |

### The pipeline (Shaders.h has the pass list)

    host --intakev--> N lines x W       (area average of host rows onto the standard's lines)
         --intakeh--> N lines x Ws      (Y' through the Speed's Gaussian; U, V a k-pixel box)
    per generation:
         noise  -> white noise, seeded by ( video frame, generation )
         tape   -> luma filter + FM noise; chroma filter (with delay) + chroma noise;
                   the bar; the switching transient; the phase rotation; dropouts flagged
         comb   -> chroma averaged with line l - 2 (1H earlier, same field)
         doc    -> flagged samples from the nearest clean line 1H, 2H ... earlier (DOC on)
    display --> each host row its nearest line; Catmull-Rom across, displaced by the line's
                time-base (the head switch recovering, the bar's jitter); Y'UV -> R'G'B'; Mix

### How this differs from ferric and old-cathode

- **ferric** is time-base error (wow, flutter, scrape as one scalar error in tape time)
  and the noise-reduction round trip. Colourunder has neither: its only time-base is the
  head switch's step and the bar's lost sync, and it has no compander.
- **old-cathode** is the broadcast composite route: a subcarrier, dot crawl,
  cross-colour, ghosting, interlace twitter, a CRT. Colourunder never makes a composite:
  its chroma is band-limited as a baseband U/V pair (the colour-under band's equivalent),
  so there is no dot crawl and no cross-colour. Where they overlap:
  - **The head switch.** old-cathode's is a smoothstep over the bottom 0–7% of the frame
    with a random per-line shift. Here it is the colour-under recorder's own mechanism:
    the switch lands at a stated line — 6.5 H before each field's V sync, which from
    BT.470 / SMPTE 170M line numbering is 4.0 lines before the end of a PAL field's active
    picture and 3.5 on NTSC (so NTSC's switch cuts a line in half, 22.38 us in) — and the
    lines after it are displaced by the head-to-head step, recovering with the TV's AFC
    (e^-n/12 lines), plus a 2 us switching transient on the tape. Every generation's deck
    switches at the same place, so the step accumulates with Generation.
  - **Tracking.** old-cathode's band is a sine-driven position with a fixed width. Here
    the bar is where the head crosses between two tracks of its azimuth, from a stated
    geometry (the head crosses 2 track pitches a field; the FM demodulator loses lock below
    6% RF), resting in the vertical interval at zero error and moved by a seeded drifting
    error.
  - **PAL.** old-cathode averages two decoded lines of a composite. Here the average is
    of the colour-under chroma and applies to NTSC too (the crosstalk comb), which is why
    NTSC's hue error survives it and PAL's does not.

---

## The shape of the code

| file | what |
| --- | --- |
| `source/Model.{h,cpp}` | The deck as arithmetic: the two standards, the band edges, the Gaussian kernels, the display and box gains, the tracking geometry and drift, the head switch, dropouts, the phase error, the hash, the `Perturb` bits. No GL. |
| `source/Controls.{h,cpp}` | Every 0..1 slider to its physical unit. |
| `source/Clock.{h,cpp}` | clamp's clock: unit voting, origin + offset in double. |
| `source/Shaders.{h,cpp}` | Eight shaders: vertex, intakev, intakeh, noise, tape, comb, doc, display. |
| `source/Colourunder.{h,cpp}` | The plugin: parameters, the per-frame CPU half, the passes, the test hooks. |
| `source/PassBuffer.*` | tinsel's FFGLFBO with the leak fixed. |
| `source/Diag.{h,cpp}` | A log file, for the shader that will not compile. |
| `tools/cutest/main.cpp` | The harness. |
| `tools/sweep.py`, `tools/verify.sh` | Every control alive; everything, in one go. |

---

## Traps

### ☠️ GLSL's sin and cos are the driver's, and the software renderer's are 8e-4 rad out

The phase rotation was first `cos( phi )`, `sin( phi )` in the tape shader. On this Mac's
GPU `--pal` read 2e-7; on Apple's software renderer (CI's) PAL's saturation was 3.6e-4 off
cos 20° and NTSC's hue 8.1e-4 rad off 20°. GLSL 4.10 does not specify their precision.
The cos and sin now come from double on the CPU in `LineData`; the shaders do no
trigonometry at all. Found only because verify runs every check on the software renderer.

### ☠️ The display and the intake cost bandwidth, so the band edges must be the chain's

At 1280 wide the line raster is 640 samples (k = 2) and the display's Catmull-Rom has a
gain of ~0.96 at 3 MHz: `--chroma` measured SP luma half amplitude at **2.886 MHz**, not 3.
The first generation's Gaussians are now designed with the display's gain and the
intake's box gain in (`Model::SigmaUsWith`, `DisplayGain`, `BoxGain`), so the chain is
half at the stated frequency at every raster; the check compares with the stated number,
not with a copy of the design.

### ☠️ `floor( 4 sigma )` taps is not 4 sigma

At σ = 1.24 px (NTSC LP luma at 960 wide) a kernel reaching `floor( 4σ )` = 4 taps stops
at 3.2σ, drops a 1.3e-3 tail, and moved the band edge 3e-4 MHz — over a tolerance that
had been derived assuming a 4σ truncation. `--chroma` at 960x540 in verify caught it. The
kernel now reaches `ceil` past 4σ on both sides, which is what the tolerance assumes;
every band edge then measured within 1e-5 MHz.

### ☠️ The demo clips are black where they are transparent, so do not multiply by alpha

The intake first recorded `rgb × a` ("the picture over black"). On Resolume's clips with
alpha (Trinity_09 is 93% alpha 0, BattleWeapon_Tank_09 61%) rgb ≤ a on 99.6% of pixels:
they are already premultiplied, and a second multiply darkens every soft edge. The
intake records the picture as handed over; output alpha is `mix( src.a, 1, Mix )`
(toner's rule: a tape has no alpha).

### ☠️ Fewer host rows than lines means lines you cannot see

At 320x180 PAL the display shows every 3.2th line, so the dropout on line 289 and its
replacement, line 287, are never both on screen. `--doc` reads the line raster the
display samples (`ReadLinesForTest`) at every raster, and also compares the on-screen rows
where both lines are shown (H ≥ N: 1280x720).

### ☠️ The tracking geometry cannot be narrow, absent at zero error and linear in RF all at once

With a fixed slope S across the field and RF = 1 − distance to the nearest own track, a
bar of width w needs S = 2 RF_th / w; a small S makes a wide bar, and a large S puts the
crossover somewhere in every field. The resolution: S = 2 pitches a field (exactly one
crossover per field), a sharp FM threshold (RF_th = 0.06, so the core is ~19 lines), and
the crossover resting in the middle of the vertical interval at zero error, where it
hides. The noise flank (the gain rising below 15% RF) still reaches the bottom few lines
at zero error; a well-tracked VHS is noisier at the bottom, so it stays.

### Tolerance arithmetic done once wrongly

The first estimate of the half-amplitude error from truncation was 2.3e-5 MHz; the right
bound for a normalised truncated Gaussian at H = 1/2 is 3T relative (T the two tails),
7e-5 MHz for chroma. Generation 2 measured 9e-5 against a bound of 9.7e-5 before the
reach fix. The tolerances are now computed by `halfTolerance()` from that derivation.

### The worktree guard did not fire

`~/dev/colourunder` is outside `~/Projects`, and every git call was `git -C
~/dev/colourunder`; no `CLAUDE_WORKTREE_EXEMPT=1` was needed.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured first and put back
before the display pass); every `ffglex::Scoped*` clears to 0 on exit, so every `Ensure()`
and the `LineData` upload happen before the passes bind anything; `FFGLFBO::Release()` leaks
the colour texture (`PassBuffer::Destroy()` deletes it first); `FFGLShader::Set` has no array
overload (kernels go through `glUniform1fv`); `SetParamInfo` clamps a STANDARD default into
0..1; the core is an **OBJECT** library; `SetTextParameter` must return `FF_SUCCESS` for the
About block; the harness drives a synthetic clock; an option's range reads back 0..1 (the
harness lists options by element count and integers by their real range); Resolume's clock
overflows a float, so time is double and frame-relative and the shaders see no clock;
GLSL 4.10's reserved words (`packed`, `sample`, `input`, `output`, `filter`, `common`,
`active`, `half`, `patch`, `flat`, `layout`) are not identifiers (verify greps); no `M_PI`
(`kPi`), `<cmath>` included, no `far` or `near`; randomness is PCG integer hashing; the demo
clips carry alpha.

---

## The colour-under facts, and which are confirmed

ATTRIBUTIONS.md has the sources in full.

| figure | value here | status |
| --- | --- | --- |
| PAL colour-under carrier | 40.125 fH = 626.953 kHz | **confirmed** (vhs-decode citing IEC 774-1; digital-archivist.com; US 5,500,739) |
| NTSC colour-under carrier | 40 fH = 629.371 kHz | **confirmed** (same three) |
| chroma bandwidth | half amplitude at 0.5 MHz (39.0 lines PAL, 39.5 NTSC) | **unconfirmed**: US 5,500,739 gives sidebands ±500 kHz; Wikipedia gives 300 kHz baseband. The spec's "about 40 lines" matches 0.5 MHz |
| luma SP | half amplitude 3.0 MHz (234 lines PAL) | **confirmed** (Wikipedia's 3 MHz and 240 TVL agree with each other) |
| luma LP | 2.76 MHz (230/250 of SP) | **unconfirmed**: one source's line counts |
| luma EP | 2.4 MHz | **unconfirmed**: no source gives a number |
| head switch | 6.5 H before V sync (±1.5 H) | **confirmed** (four US patents quoting the VHS standard) |
| where that is in the picture | 4.0 lines before a PAL field's active end, 3.5 NTSC | **derived** from BT.470-6 / SMPTE 170M line numbering and the 576/480 digital rasters; the half-line placement is simplified (see Decisions) |
| de-emphasis corner (FM noise shape) | 273.76 kHz | **unconfirmed**: vhs-decode, citing IEC 774-1 p. 67 |
| noise levels, AFC time constant, head-switch step, tracking slope and threshold, dropout rate and length | see Model.h | **chosen**, not sourced |

The IEC 774-1 standard itself was not read; every "IEC" figure above is second-hand.

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every check ran at 320x180, 960x540 and 1280x720 and on Apple's
software renderer at 320x180 in `verify.sh`. T = erfc(4/√2) = 6.3e-5, the two tails of a
Gaussian truncated at 4σ. `kU` = 2^-24.

| check | what it measures | tolerance and where it comes from | raster / renderer dependence |
| --- | --- | --- | --- |
| `--chroma` chroma | the frequency where a rendered chroma cosine's fitted amplitude is 1/2, by 20-step bisection, both standards | `halfTolerance`: 2 × ε f / (2 ln 2), ε = 3T (truncation) + 1e-5 (float) + 1e-3 at k > 1 (Catmull-Rom images leaking into the fit); 1.4e-4 MHz (k = 1), 8.7e-4 (k = 2). Measured ≤ 1e-5 | none by design: the chain is designed to the stated edge at every k |
| `--chroma` luma | the same for Y' at SP, LP, EP | the same formula at the luma edge: 6.9e-4–8.7e-4 MHz (k = 1), 4.2e-3–5.2e-3 (k = 2). Measured ≤ 1e-4 | **only where σ ≥ 1 host px** (960, 1280): at 320 wide Nyquist is 3.08 MHz and a sampled Gaussian cannot hold a 3 MHz edge; there the check asserts luma ≥ 0.95 at 0.5 MHz instead, and says why |
| `--delay` edge | the lag between the centroids of the chroma and luma edges' derivatives | **3e-3 px**: the truncated kernel's centroid moves at most one edge tap's weight (G(4σ) / σ√2π ≈ 2e-5) times 4σ ≈ 28 px, 6e-4, ×5. Measured ≤ 1e-4 px | k = 1 only: at k = 2 the display's two phases make the derivative centroid a Catmull-Rom artefact with no clean bound; reported, not asserted |
| `--delay` phase | the lag from the fitted phases of 0.12 MHz luma and chroma cosines | **1e-2 px**: the fit is exact for a sinusoid; the reconstruction's phase classes are symmetric so bias no phase; image leakage ≤ 1e-3 rad at this frequency. Measured ≤ 1e-4 px | any k; whole-pixel (5 px) and fractional delays |
| `--switch` | which rows of a Head Switch 1 render differ from Head Switch 0: none above the stated line, the first where predicted, every row below torn across most of its width, NTSC's cut line untouched before 22.38 µs | **exact** (bitwise equality of the rows above; row mapping is integer) | none: line positions are integers on the line raster, the row→line map is exact |
| `--pal` | per displayed row, hue and saturation of a flat colour with a forced 20° phase error | **1e-4** rad and relative: ~10 float operations on U, V ≈ 0.1 give ≤ 1e-6 absolute, 1e-5 relative, ×10; cos and sin from double. Measured ≤ 2.2e-7 | the first two lines have no line 1H before; skipped |
| `--doc` | the dropout's samples against line l − 2's, and on screen (H ≥ N) the rows of both lines where every Catmull-Rom tap is inside the dropout | **exact**, bitwise, with noise on | none: a texel copy |
| `--generation` | generation 2's gain over generation 1's at 0.15–0.6 MHz against the stated tape Gaussian; the two-generation half amplitude; the lag doubled | ratio **5e-4** (the extra kernel's truncation ≤ T(1 + 1/G) ≈ 2.3e-4 at G ≥ 0.37, ×2); half at k = 1 `halfTolerance` for two Gaussians, 2e-4 MHz; lag 1e-2 px. Measured ≤ 1.2e-4, 4e-5 MHz | at k > 1 the first generation's own Gaussian is narrower than the tape's (it carries the display's gain), so the 0.5/√2 figure is k = 1 only; the ratio holds everywhere |
| `--tracking` profile | the chroma lost on every field line (from the line raster) against the stated geometry, 9 forced errors and 51 drifting frames over 20 s | **1e-4**: float U, V ≈ 0.09 (1e-5 relative) and a float weight; ×10. Measured ≤ 2.4e-7 | none: per-line, CPU-computed |
| `--tracking` widening | visible bar lines non-decreasing as the error rises 0–0.15 pitch, 0 at rest | exact integer counts | none |
| `--tracking` walk | the drifting error's range over 20 s | ≥ 0.3 pitch (a property of the seed, stated) | none |
| `--tracking` frame rate | the error after 1.5 s at 60 and at 144 fps | **1e-12**: double clock, two different sums of deltas | none |
| `--resize` | every subpixel of frames 40–59 after a resize to 1.5x and back, noise, bar, dropouts, switch, 2 generations on | **4 × 2 kU** relative (gate's allowance for a non-repeatable software renderer). Measured 0 | none: nothing on the GPU survives a frame |

Deliberately NOT relied on: hardware filtering (every read is `texelFetch`, the
reconstruction is written out), GLSL trigonometry (none left), exact cancellation between
two formulas, `pow( 1, x ) == 1`.

### The negative controls

`cutest --negative` runs eight; `--perturb BITS` runs any check verbosely against one.
Each perturbs the plugin's model (a `Perturb` bit it carries at zero), never the harness.

| perturbation | fails |
| --- | --- |
| chroma given luma's bandwidth (the spec's) | `--chroma`: no half-amplitude crossing below 1.5 MHz, both standards |
| the group delay left out | `--delay`: every chroma lag reads 0 |
| the switch counted 6.5 lines from the end of the active picture, not from V sync | `--switch`: rows above the stated line torn, the first disturbed row 2.5 lines early |
| PAL's line average skipped (the spec's) | `--pal`: each PAL line ±20° of hue, saturation 1 |
| DOC repeating the adjacent frame line (the other field) | `--doc`: the dropout is not line l − 2's |
| chroma band-limited in generation 1 only | `--generation`: the ratio is 1, not the Gaussian; the half stays at 0.5 MHz |
| the bar ignoring the tracking error | `--tracking`: every profile wrong once e ≠ 0 |
| a resize restarting the clock | `--resize`: frames after the resize differ |

All eight fail at 320x180, 960x540, 1280x720 and on the software renderer.

### The mutation

One character of the shipped GLSL, in the tape shader: the PAL V switch
`( ( l >> 1 ) & 1 )` → `( ( l >> 2 ) & 1 )` (the V axis inverted every other PAIR of
field lines). Built in a scratch build directory and run through the checks at 320x180:
`--pal` failed 2 of 4 (PAL hue 0.349 rad = the full 20° on some lines, saturation off
cos 20° by 0.060); NTSC's two passed, as they should (NTSC has no V switch). Reverted
(the line reads `l >> 1` again), rebuilt, `--pal` 4 of 4.

---

## Decisions taken without asking

- **Defaults**, chosen on Resolume's demo clips through `--pipe` (Beat 001, Trinity_09,
  IntoTheGlow_02, FogAndDust_3, BattleWeapon_Tank_09, OrganicMotions_06 at 1280x720):
  PAL, **LP** (the characteristic home-recording look), Tracking 0.03 (the bar mostly in
  the vertical interval, occasionally flicking the bottom lines), Head Switch 0.45
  (0.9 µs), Wear 0.3, Generation 1, DOC on, Chroma Delay 0.45 µs (a 2nd-order Butterworth's
  group delay at 0.5 MHz, √2 / 2π f), Chroma Noise 0.4, Mix 1. None floods, none blanks;
  dark clips keep their blacks with the noise on top.
- **Output alpha** `mix( src.a, 1, Mix )`: at Mix 1 opaque, a tape has no alpha. The
  intake does NOT multiply by alpha (see the trap).
- **Band edges are half-amplitude (−6 dB) points**, for luma and chroma alike, and a
  Gaussian response (zero phase) plus a pure delay for chroma. A real deck's filters are
  not Gaussian and not linear-phase; the Gaussian is what makes the band edge and the
  delay separately measurable.
- **The chroma path is baseband U/V**, not an actual heterodyne at 627 kHz: for the linear
  parts the two are the same, and the carrier appears only through the bandwidth.
  `--model` holds the carriers and that the band fits under them.
- **Y'UV is BT.601 with PAL's U/V scalings for both standards** (no YIQ for NTSC).
- **The 1H comb on NTSC too**, as the crosstalk canceller; it halves chroma's vertical
  resolution on both.
- **The raster.** Host rows → the standard's 576 / 480 lines by area average; lines →
  host rows by nearest line (a TV draws a line as a stripe). Two fields interleaved: frame
  line l is field l & 1, line l >> 1. Each field is treated as N/2 whole lines, so PAL's
  and NTSC's real half lines are not modelled; the head switch sits (6.5 − pre-equalising)
  line periods before the end of each field's active picture in BOTH fields (a real deck's
  two fields put it at a line start in one field and mid-line in the other).
- **The head-switch step has the same sign at both switches** (track-length/tension skew
  dominating head placement), accumulates with Generation, and recovers over 12 lines of
  TV AFC; 2 µs at Head Switch 1.
- **Noise refreshes per video frame** (25 or 29.97 a second), not per host frame; the
  tracking error is continuous in time.
- **Tracking error** e(t) = Tracking × (1 + 0.8 v(t)), v a seeded Catmull-Rom wander through
  knots 1.7 s apart; one stream per generation.
- **Dropouts**: up to 24 a generation a frame, expected 12 × Wear² a frame, 1–21 µs long;
  DOC off is a white streak (Y' 0.92 ± noise, no chroma). DOC replaces Y and C together.
- **Line raster cap** 1024 samples (k = ceil(W / 1024)).
- **About block**: provisional hand copy, `guide = ""` (no guide yet), like graticule.

---

## What is actually verified, and what is assumed

### Verified by measurement, on this Mac (Apple Silicon, macOS 26.4), 2026-09-25

- `tools/verify.sh` passes (see its summary in the final report / Status).
- Every figure in "Would this hold" above, at three rasters and the software renderer.
- The sweep: all 10 controls change the picture.
- `lipo`: arm64 + x86_64. oxbow probe: `SW Colourunder`, `CU01`, effect; selftest passes.
- `--pipe`: 2.5 frames in, 2 out; unknown cue exit 2; `| head -c 1` exit 1 (SIGPIPE
  ignored); a failed render exit 1; option, boolean and integer cues step, a slider ramps.
- Render cost (`cutest --bench`, defaults / Generation 5 + Tracking 1 + Wear 1):
  1280x720 0.48 / 1.80 ms, 1920x1080 0.93 / 3.65 ms, 3840x2160 1.42 / 4.10 ms; 39–76 MB.
- On footage, by eye only: the six demo clips above.

### Assumed, or not done

- ☠️ **Never loaded into Resolume.** Everything was measured offline against the real
  plugin class in a headless CGL context, and loaded by oxbow, which is a real FFGL host and
  is not Resolume. No Arena gate, no Windows build run.
- The format figures marked unconfirmed above; every noise level and the tracking geometry
  are chosen.
- No pre-/de-emphasis nonlinearity (the white-clip streaking after sharp edges), no
  composite Y/C separation between generations (a dub over composite would add
  cross-colour), no azimuth crosstalk, no audio.
- No OpenFX port, no browser demo, no user guide.

---

## Open questions

- **Chroma bandwidth: 0.5 or 0.3 MHz?** The patent and the spec say 500 kHz; Wikipedia
  says 300 kHz baseband. 0.3 MHz would be 23 lines, not 40. It is one constant
  (`kChromaHalfHz`) and the checks follow it.
- **"Its width grows with the error."** In this geometry the bar's core is a fixed ~19
  lines once fully on screen; what grows with the error is how much of it has left the
  vertical interval (0 → 19 lines as e goes 0 → 0.15 pitch) and the noise flank. A bar
  whose own width grows would need the slope to grow with the error too — a speed
  mismatch rather than a phase error — which makes it narrower, not wider.
- **Should Tracking's drift be tempo-synced** or exposed as a rate? It is fixed at 1.7 s
  knots.
- **EP's luma bandwidth** (2.4 MHz) has no source.
- **Both fields' switch at the same place**, and the same-sign step: a real deck puts one
  field's switch mid-line and the step's sign is a property of the deck pair.

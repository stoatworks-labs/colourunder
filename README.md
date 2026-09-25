# colourunder

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The deck is not asserted but
> measured: an offline harness drives the real plugin class in a headless GL context on a
> synthetic clock and reads every property back out of the picture it renders — chroma's
> response, found by bisection on rendered sinusoids, falls to half at 0.50000 MHz (39
> lines) on PAL and NTSC at three rasters, and luma's at the Speed's 3.00 / 2.76 / 2.40 MHz,
> each within 1e-4 MHz; a colour edge's chroma lags its luma by the stated group delay to
> 1e-4 px; the head switch disturbs exactly the lines from 6.5 H before V sync and not one
> row above; a 20° playback phase error is a hue shift of 20° on every NTSC line and, on
> PAL, no hue shift on any line and a saturation of cos 20° to 2e-7; a forced dropout with
> DOC on is, bit for bit, the line 1H before it; a second generation multiplies the chroma
> response by the tape's filter again; the tracking bar is on every field line where the
> stated geometry puts it, for any error and over 20 s of drift; and a resize changes
> nothing — with eight negative controls that prove each check can fail. It has **never
> been loaded into Resolume**; it is loaded by [oxbow](https://github.com/stoatworks-labs/oxbow),
> which is a real FFGL host and is not Resolume. See [Status](#status).

VHS's helical-scan colour-under recording, as an FFGL effect for
[Resolume](https://resolume.com) Arena and Avenue.

![Resolume's demo clip IntoTheGlow_02 through the deck: a symmetrical tunnel of lit panels, soft, the colour late and blotchy on the copper edges, and a band of tracking noise across the bottom](docs/hero.jpg)

<sub>One frame, rendered by `cutest --pipe`, the offline harness — not captured from
Resolume. Resolume's bundled demo clip IntoTheGlow_02 at the defaults, with Tracking up to
0.12 so the bar has left the vertical interval.</sub>

## The one idea

A VHS deck cannot record colour at its broadcast frequency. It heterodynes the chroma
down to about 627 kHz (PAL) or 629 kHz (NTSC), records it under the luma's FM carrier —
the colour-under system — and heterodynes it back up on playback. That one choice, and the
two heads on the spinning drum that lay the picture down diagonally a field at a time, are
what make VHS look like VHS.

## What falls out

None of these is drawn as an effect. Each is the recorder doing what it does:

- **Colour smears, and arrives late.** The colour-under band is about half a megahertz
  wide, so chroma has about 40 lines of horizontal resolution against luma's 240, and the
  playback filters delay it: colour bleeds right of every edge.
- **Colour noise is blotches.** Noise on the colour-under band goes through the same
  narrow filter, so it comes out as streaks of colour a few microseconds long, not grain.
- **Hue wanders on NTSC; PAL goes pale instead.** The playback's phase error rotates the
  colour. PAL inverts its V axis on alternate lines, so after the 1H average the rotation
  cancels and only a loss of saturation is left. NTSC keeps the hue shift.
- **The bottom tears.** The deck switches heads 6.5 lines before vertical sync; the new
  head's timing is not the old one's, so the last lines of each field jump sideways and the
  TV takes a dozen lines to catch up.
- **The tracking bar.** Off-track, the head reads the wrong track where it crosses between
  two of its own, and the FM demodulator loses lock: a band of noise, colour gone, lines
  jittering. At good tracking it rests in the vertical interval; as the error drifts it
  walks up through the picture.
- **Dropouts.** Missing oxide is a white streak — or, with DOC on, the same stretch of the
  line before, repeated.
- **SP, LP, EP.** Slower tape: less luma bandwidth, more noise.
- **Copies of copies.** A dub runs the whole chain again: colour at 0.35 MHz after two
  generations, twice the delay, the noise and the dropouts of both tapes.

### The honest limit

The chroma path is the colour-under band's baseband equivalent — a Gaussian band edge and
a pure delay, not a heterodyne or a real deck's filters — and the chroma bandwidth itself
is one of the less certain numbers (the sources say 300 or 500 kHz; this is 500). There is
no composite between generations, so a dub adds no cross-colour; no pre-emphasis clipping,
so there is no white streaking after sharp edges. The noise levels, the tracking geometry
and the head-switch step are chosen, not measured from a deck. ATTRIBUTIONS.md says which
figures are sourced.

## Controls

| Group | |
| --- | --- |
| **Deck** | Standard (PAL, NTSC), Speed (SP, LP, EP), Tracking (the tracking error: at 0 the bar hides in the vertical interval), Head Switch (the head-to-head time-base step, 0–2 µs). |
| **Tape** | Wear (dropouts, and a little RF), Generation (1–5 copies), DOC (the dropout compensator). |
| **Colour** | Chroma Delay (the playback filters' group delay, 0–1 µs), Chroma Noise (band-limited colour noise and the playback phase error), Mix. |

The defaults are a PAL LP recording played on a well-adjusted deck: a little tracking
error, a visible head-switch tear at the bottom, a few dropouts compensated, the colour
0.45 µs late with some blotch. Chosen on Resolume's demo clips. The output is opaque at
Mix 1: a tape has no alpha.

## Status

**v0.1.0, not released (2026-09-25).** No user guide, no project page yet.

### Measured offline, on macOS

`tools/verify.sh` passes on this machine against a fresh universal Release build, running
every check at 320×180, 960×540 and 1280×720 and again on Apple's software renderer at
320×180. What it establishes:

- **Chroma** half amplitude at 0.50000 MHz on PAL (39.0 lines) and NTSC (39.5 lines), at
  every raster, within 1e-5 MHz; **luma** at 3.0000, 2.7600 and 2.4000 MHz for SP, LP and EP
  where the raster can carry the band edge (960 and 1280 wide; at 320 wide Nyquist is 3.08
  MHz, and the check says so and asserts only that luma is not chroma's).
- **Delay.** Chroma lags luma by the stated delay, 5.00000 px when it is a whole number of
  pixels and 2.77192 against 2.77190 px when it is not, from the edge and from the phase.
- **Head switch.** On PAL frame line 568 (field line 284) is the first disturbed line and on
  NTSC line 236 is cut 22.38 µs in; no row above changes, every row below is torn.
- **PAL and NTSC.** A 20° phase error: PAL no hue shift on any row (≤ 2.2e-7 rad),
  saturation cos 20° (≤ 2.0e-7); NTSC 20° of hue on every row, saturation kept.
- **DOC.** A forced dropout on line 289 (PAL) or 241 (NTSC) with the noise on is line 287's
  or 239's exactly, on the line raster and on screen; with DOC off a white streak.
- **Generations.** Generation 2 over generation 1 is the tape's 0.5 MHz Gaussian to 1.2e-4;
  two generations' chroma half amplitude 0.35356 MHz against 0.5/√2; the delay doubled.
- **Tracking.** The chroma lost on every field line matches the stated geometry to 2.4e-7
  for nine errors and 51 frames of 20 s of drift; the bar hides at no error and widens as it
  enters; the same second is the same error at 60 and 144 fps.
- **Resize.** A resize to 1.5× and back leaves every subpixel as it would have been.
- **Negative controls.** Chroma given luma's bandwidth fails the chroma; no delay, the
  delay; a switch counted from the wrong place, the switch; PAL without its average, PAL;
  DOC repeating the other field, DOC; one generation's filter, the generation; a bar that
  ignores the error, the tracking; a resize that restarts the clock, the resize.
- **No dead controls**: all 10 change the picture.
- **The bundle** is universal, and oxbow sees `SW Colourunder`, `CU01`, an effect.

Render cost, `cutest --bench` (best of three, `glFinish` both sides, a shared GPU):

| | defaults ms/frame | Generation 5, Tracking 1, Wear 1 | state held |
| --- | --- | --- | --- |
| 1280×720 | 0.48 | 1.80 | 39 MB |
| 1920×1080 | 0.93 | 3.65 | 59 MB |
| 3840×2160 | 1.42 | 4.10 | 76 MB |

4K costs little more than 1080p because the signal chain runs on a line raster of at most
1024 samples by 576 lines, whatever the host's size.

### Not done

- **Never loaded into Resolume**, on either platform. No Windows build has been run.
- Seen only on Resolume's bundled demo clips.
- No OpenFX port, no user guide, no factory presets.

## Browser demo

[colourunder-demo.stoatworks-labs.com](https://colourunder-demo.stoatworks-labs.com/) runs
the plugin's own intake, noise, tape, comb, compensator and display shaders in WebGL2,
spliced in from `source/Shaders.cpp` by `demo/tools/sync_shaders.py` and checked by
`demo/tools/check_shaders.py` from `tools/verify.sh` against what `cutest --dump-shaders`
says the plugin compiles, over the same RGBA32F line raster. Its CPU half — the kernels,
the tracking error and bar, the head switch, the dropouts, the phase error, the clock and
every control's law — is a hand port to JavaScript (`demo/model.js`), and the page says so.
`demo/tools/check_port.sh` (also run by verify) compiles the plugin's own constructor and
`ProcessOpenGL` with GL replaced by a recorder and finds the port identical on every
declaration, LineData float and uniform. Driven frame by frame against `cutest --pipe
--fps 60` on the same input it agrees to 1/255 on every pixel. Generated clips only, or your
own image or video, which never leaves the page.

## Build

Needs CMake 3.15+, a C++17 compiler and the FFGL SDK submodule.

```sh
git clone --recurse-submodules https://github.com/stoatworks-labs/colourunder
cd colourunder
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The macOS bundle is universal (Apple Silicon and Intel). `cmake --install build` copies it
into `~/Documents/Resolume Arena/Extra Effects`; for Avenue, pass
`--prefix "$HOME/Documents/Resolume Avenue/Extra Effects"`.

## Building and testing

```sh
tools/verify.sh                                   # everything, ~15 minutes
./build/cutest --list                             # the parameters
./build/cutest --chroma --size 320x180            # one check
./build/cutest --negative                         # every check can fail
python3 tools/sweep.py                            # no dead controls
ffmpeg -i clip.mov -vf fps=60 -f rawvideo -pix_fmt rgba - | ./build/cutest --pipe --size 1280x720 | ffplay -f rawvideo -pixel_format rgba -video_size 1280x720 -framerate 60 -
```

`CLAUDE.md` is the command reference and `AGENTS.md` the reasoning: the recorder, the
traps, which figures are sourced, and where every tolerance comes from.

## License

MIT — see [LICENSE](LICENSE). What it builds on is in [ATTRIBUTIONS.md](ATTRIBUTIONS.md).

<!-- attributions:start -->
This project is built on other people's work — see [ATTRIBUTIONS.md](ATTRIBUTIONS.md).
<!-- attributions:end -->

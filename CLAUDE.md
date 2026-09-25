# colourunder

VHS's helical-scan colour-under recording — chroma heterodyned down under the FM luma
(about 40 lines of it, late, with blotchy noise and a playback phase error that PAL
turns into desaturation and NTSC shows as hue), the head switch 6.5 lines before V sync,
a tracking bar that walks as the error drifts, dropouts and the compensator, SP/LP/EP and
copies of copies — as an FFGL **effect** (`SW Colourunder`, `CU01`) for Resolume
Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before changing the chain, a band edge, the geometry or a check's
tolerance.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/cutest --out /tmp/f.png --size 1920x1080`
  (90 frames of the moving card at a synthetic 60 fps, then the last one;
  `--average` writes the mean of every frame instead)
- Set anything by name: `--set "Speed=2" --set "Tracking=0.6" --set "Generation=3"`
  (0..1 for sliders, the element index for options, 0/1 for DOC, 1..5 for Generation)
- List parameters, kinds, defaults and ranges: `./build/cutest --list`
- Other sources: `--source flat --level 0.5`, `--source white`, `--source black`
- The exact GLSL the plugin compiles: `./build/cutest --dump-shaders DIR`
- Footage through the real shaders — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH`, `--fps N` (frame n is clocked at n / fps) and an optional
  `--script` of `frame Parameter Name value` cues. A slider ramps linearly between
  cues; an option, a boolean and an integer STEP (they hold the last cue at or before
  the frame); an event fires on its cue frame only. A cue naming no parameter is refused
  with exit 2, a partial frame at the end of stdin ends the stream cleanly, a failed
  render or a closed stdout exits 1 (SIGPIPE is ignored, so never 141):
  `ffmpeg -i clip.mov -vf fps=60 -f rawvideo -pix_fmt rgba - | ./build/cutest --pipe --size 1280x720 [--script cues.txt] | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -r 60 -i - out.mov`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + glslc + the reserved-word grep
  + every check at 320x180, 960x540 AND 1280x720 AND on the software renderer + --pipe +
  the sweep + the bundle + oxbow; ~15 min on this Mac, most of it the software renderer)
- The colour-under band and the luma band by speed: `./build/cutest --chroma`
- The chroma's group delay, whole-pixel and fractional: `./build/cutest --delay`
- The head switch on its lines, both standards: `./build/cutest --switch`
- PAL desaturates, NTSC turns hue: `./build/cutest --pal`
- DOC repeats the line 1H before, exactly: `./build/cutest --doc`
- Two generations compose the chroma filter: `./build/cutest --generation`
- The tracking bar's geometry, widening, walk and frame-rate independence: `./build/cutest --tracking`
- The state survives a resize: `./build/cutest --resize`
- The checks can fail: `./build/cutest --negative`; one perturbation verbosely:
  `./build/cutest --perturb BITS --pal` (bits in `Model.h`)
- The plugin's numbers against the stated ones, no GL: `./build/cutest --model`
- Every check takes `--size WxH`; CI runs them at 320x180
- CI's renderer, on this Mac: `CUTEST_RENDERER=software ./build/cutest --pal --size 320x180`
  (Apple's software renderer, not repeatable at the last bit; verify.sh runs every check on it)
- No name over 16 characters, none duplicated: `./build/cutest --names`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost and the state held: `./build/cutest --bench`
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Colourunder.bundle`

## Notes
- **The deck, not the look.** `Model.{h,cpp}` holds the numbers and the CPU half in
  double: the two rasters, the band edges, the kernels, the tracking geometry and drift,
  the head switch, the dropouts, the phase error. `Shaders.cpp` convolves, adds noise,
  combs, compensates and displays. A wrong format number is a fix in `Model.cpp`.
- **The line raster.** N lines (576 PAL, 480 NTSC) by Ws = ceil(W / k) samples, k =
  ceil(W / 1024) host pixels a sample. Every line mechanism (the 1H comb, DOC, the head
  switch, the bar) is exact on it; the display shows each host row's nearest line.
- **The band edges are the chain's.** At k > 1 the intake's box and the display's
  Catmull-Rom cost bandwidth, so the first generation's Gaussians are designed with them
  in (`SigmaUsWith`). Chroma is half at 0.5 MHz and luma at the Speed's figure at every
  raster the checks run.
- **Nothing trigonometric runs on the GPU.** The phase error's cos and sin arrive in
  `LineData` from double: GLSL leaves their precision to the driver.
- **Time is seconds since the first frame, in double** (clamp's Clock). The tracking
  error is a pure function of it; the noise and the dropouts of the video frame index
  floor(t x 25) or floor(t x 29.97). Nothing on the GPU survives a frame.
- **Output alpha is 1 at Mix 1** (`mix( src.a, 1, Mix )`): a tape has no alpha. The input
  is recorded as handed over, not multiplied by alpha: the demo clips are already black
  where they are transparent.
- **`Perturb` bits and the `...ForTest` hooks are test hooks**, inert in the plugin.
- **Parameter names must be unique** — `--set` and the sweep find them by name.
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can widen
  it; Generation is `FF_TYPE_INTEGER` with a real 1..5 range.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `colourunder_core` is an OBJECT library, not STATIC — the plugin registers itself from
  a file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `CU01`. Display name `SW Colourunder`.
- Local only (v0.1.0, unreleased): no GitHub repo, no tag, not on the website.

## Not done yet
- Never loaded into Resolume. oxbow (a real FFGL host that is not Resolume) loads it.
- No release, no website registration; `StoatworksAbout.h` and `ATTRIBUTIONS.md` are
  provisional hand copies (`guide=""`).
- No OpenFX port, no user guide.

## Browser demo

`demo/` is the page at **colourunder-demo.stoatworks-labs.com**, deployed from
`wrangler.toml` (a Worker route over a proxied `AAAA 100::` DNS record, not a custom
domain) with `cf-run npx wrangler deploy` or by any push to main
(`.github/workflows/deploy.yml`) — no build step; what is committed is what is served.
`demo/vendor/` is copied in by
`~/Projects/infrastructure/stoatworks-backend/resolume-demo/sync.sh colourunder` and is not a
place to edit.
- **A shader change in the plugin: `python3 demo/tools/sync_shaders.py`**, then
  `python3 demo/tools/check_shaders.py --dump DIR` after `cutest --dump-shaders DIR`
  (verify.sh does both). Never hand-edit the generated block.
- **A change to Model.cpp, Controls.cpp, Clock.cpp, the constructor or ProcessOpenGL's CPU
  half means the same change by hand in `demo/model.js`**, then `demo/tools/check_port.sh`
  (verify.sh runs it): it compiles the plugin's own code under a recorder and compares
  every declaration, LineData float and uniform with the port. A renamed marker it cuts
  on (`enum ParamID`, the anonymous namespace, `Colourunder::Colourunder()`,
  `ProcessOpenGL`, `DeInitGL`) breaks it loudly.
- Verify a deploy **by content**:
  `curl -s 'https://colourunder-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'`.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/colourunder/colourunder.YYYY-MM-DD.log

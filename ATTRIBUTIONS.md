# Attributions

Colourunder is built on other people's work. This file lists what that work is,
who did it, and what it is doing here.

PROVISIONAL: a hand copy in the shape `stoatworks-backend`'s
`scripts/sync-attributions.py` generates. Colourunder is not yet registered there;
once it is, the sync overwrites this file.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Plugin shape, harness, --pipe contract and verify — Stoatworks gate, toner, filament

<https://github.com/stoatworks-labs/gate>  
Licence: MIT  
Copyright: Stoatworks Labs

The plugin's shape (the OBJECT core, the About block, the Diag logger), the harness shape, the --pipe contract with SIGPIPE ignored and cues that step for options, booleans, integers and events, the software-renderer pass, the verify script, the sweep and the negative-control pattern are gate's, which had them from filament, toner and wetplate, and they from rebate and pitch.

### The clock — Stoatworks clamp and standards

<https://github.com/stoatworks-labs/clamp>  
Licence: MIT  
Copyright: Stoatworks Labs

`Clock.{h,cpp}` is clamp's (standards' before it): the host clock-unit voting (readout's), an origin and an offset in double, no per-frame clamp.

### The two raster timings — Stoatworks clamp

<https://github.com/stoatworks-labs/clamp>  
Licence: MIT  
Copyright: Stoatworks Labs

The 625/50 and 525/59.94 line, porch, sync and active-line figures, and their sources, are clamp's `Model.cpp` table.

### PassBuffer — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

PassBuffer is tinsel's, with wetplate's Swap, by way of gate.

### The resize-mid-run guard — Stoatworks photofinish

<https://github.com/stoatworks-labs/photofinish>  
Licence: MIT  
Copyright: Stoatworks Labs

The trap that a reallocated buffer is a cleared buffer, and the check that guards it, are photofinish's.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl, pinned to b1afaf9 like the fleet.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud. Each figure is marked **confirmed** (two or more independent sources agree) or **unconfirmed** (one source, or none: a decision recorded in AGENTS.md).

### The colour-under carriers — vhs-decode (oyvindln/vhs-decode, `vhsdecode/format_defs/vhs.py`, citing IEC 774-1); "How vhs-decode actually works", digital-archivist.com; US patent 5,500,739

PAL: 40 fH + 1953 Hz = 40.125 fH = **626.953 kHz**; NTSC: 40 fH = **629.371 kHz**. vhs-decode states both as fH multiples and cites IEC 774-1; the digital-archivist article gives "627 kHz" and "around 629 kHz"; the patent gives a nominal 629 kHz. **Confirmed** (three sources; the IEC standard itself was not read). In this model the carrier sets nothing directly: a baseband-equivalent chroma path needs only the bandwidth. `cutest --model` holds the carrier numbers and the fact that the 0.5 MHz band fits under them.

### The chroma bandwidth — US patent 5,500,739; Wikipedia, "VHS"

The patent: AM colour sidebands extending "approximately 500 kHz on both sides" of the 629 kHz carrier. Wikipedia: "300 kHz of baseband chroma bandwidth". The spec's "about 500 kHz … roughly 40 lines" matches the patent. **Unconfirmed**, and the sources disagree: the plugin puts chroma's half-amplitude point at **0.5 MHz** (39.0 lines PAL, 39.5 NTSC), reading the patent's sideband extent as the half-amplitude edge. 300 kHz would be 23 lines.

### Luma bandwidth by speed — Wikipedia, "VHS"

"VHS machines record up to 3 MHz of baseband video bandwidth" and 240 TVL; an earlier revision of the same article gives 250 lines at SP and 230 at LP, "and even less in EP/SLP". SP **3.0 MHz: confirmed** (the 3 MHz and 240-line figures agree: 3 MHz over 51.95 us is 234 lines). LP **2.76 MHz** (3.0 x 230/250): **unconfirmed**, one source. EP **2.4 MHz**: **unconfirmed**, no source gives a number; chosen below LP.

### The FM luma carrier — digital-archivist.com; Wikipedia, "S-VHS"

Sync tip to peak white 3.8-4.8 MHz (PAL), 3.4-4.4 MHz (NTSC). Informational: the FM channel is modelled only through its output (bandwidth and noise), not as a carrier.

### The head-switching point — US patents 5,675,698, 6,304,399, 6,650,825 and 6,021,014

"In a VHS system, the head switching point … leads a vertical sync signal by 6.5±1.5H", "5H through 8H centred at 6.5H". **Confirmed** (four patents; the IEC 774 text itself was not read). Where 6.5 H before V sync falls in the active picture is derived here from BT.470-6 / SMPTE 170M line numbering (2.5 and 3 lines of pre-equalising): 4.0 lines before the end of each field's active picture on PAL, 3.5 on NTSC.

### The de-emphasis corner — vhs-decode, citing IEC 774-1 (1994) p. 67

`deemph_mid = 273755.82` Hz. Used only to shape the FM noise (cut below it). **Unconfirmed** beyond vhs-decode.

### The chroma crosstalk canceller — US patents 4,698,694 and 5,845,040

The 90-degree-per-line phase rotation on alternate tracks and the 1H comb that cancels adjacent-track crosstalk on playback. That comb is why this plugin averages chroma over 1H on NTSC as well as PAL.

### The 625/50 and 525/59.94 timing — ITU-R BT.470-6 and BT.1700; SMPTE 170M (through clamp)

Line periods, porches, sync, active lines, field lines and the pre-equalising intervals.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### The VHS format (JVC, 1976)

The helical scan, the two heads, the colour-under chroma and the look of every home video recorded on it.

## Standards and published specifications

What the implementation is measured against.

- **ITU-R BT.470-6, BT.1700; SMPTE 170M** — the two rasters.
- **ITU-R BT.601** — the Y' weights, and the U and V scalings PAL and NTSC use.
- **IEC 774-1 (VHS)** — through vhs-decode and the patents above; not read directly.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.

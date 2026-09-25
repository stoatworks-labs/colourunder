# Attributions

Colourunder is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Plugin shape, harness, --pipe contract and verify — Stoatworks gate, toner, filament

<https://github.com/stoatworks-labs/gate>  
Licence: MIT  
Copyright: Stoatworks Labs

The plugin's shape (the OBJECT core, the About block, the Diag logger), the harness shape, the --pipe contract with SIGPIPE ignored and cues that step for options, booleans, integers and events, the software-renderer pass, the verify script, the sweep and the negative-control pattern are gate's, which had them from filament, toner and wetplate, and they from rebate and pitch.

### The clock and the two raster timings — Stoatworks clamp and standards

<https://github.com/stoatworks-labs/clamp>  
Licence: MIT  
Copyright: Stoatworks Labs

Clock.{h,cpp} is clamp's (standards' before it): the host clock-unit voting (readout's), an origin and an offset in double. The 625/50 and 525/59.94 line, porch, sync and active-line figures, and their sources, are clamp's Model.cpp table.

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

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

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

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### The colour-under carriers — vhs-decode (oyvindln/vhs-decode, vhsdecode/format_defs/vhs.py, citing IEC 774-1); "How vhs-decode actually works", digital-archivist.com; US patent 5,500,739

<https://github.com/oyvindln/vhs-decode>

PAL 40.125 fH = 626.953 kHz, NTSC 40 fH = 629.371 kHz. Confirmed by three sources; the IEC standard itself was not read. In this model the carrier sets nothing directly: the chroma path is the band's baseband equivalent and needs only the bandwidth.

### The chroma bandwidth — US patent 5,500,739; Wikipedia, "VHS"

The patent gives colour sidebands of about 500 kHz either side of the carrier; Wikipedia gives 300 kHz of baseband chroma. Unconfirmed, and the sources disagree: the plugin puts chroma's half-amplitude point at 0.5 MHz (39 lines), reading the patent's sideband extent as the half-amplitude edge. 0.3 MHz would be 23 lines.

### Luma bandwidth by speed — Wikipedia, "VHS" (current and an earlier revision)

SP 3.0 MHz, confirmed (3 MHz and 240 TVL agree). LP 2.76 MHz from one revision's 230 and 250 lines: unconfirmed, one source. EP 2.4 MHz: no source gives a number; chosen below LP.

### The head-switching point — US patents 5,675,698, 6,304,399, 6,650,825 and 6,021,014

The head switch leads vertical sync by 6.5 +/- 1.5 H. Confirmed by four patents quoting the VHS standard. Where that falls in the active picture (4.0 lines before a PAL field's active end, 3.5 on NTSC) is derived from BT.470-6 and SMPTE 170M line numbering.

### The de-emphasis corner — vhs-decode, citing IEC 774-1 (1994) p. 67

<https://github.com/oyvindln/vhs-decode>

273.76 kHz, used only to shape the FM luma noise. Unconfirmed beyond vhs-decode.

### The chroma crosstalk canceller — US patents 4,698,694 and 5,845,040

The 90-degree-per-line phase rotation on alternate tracks and the 1H comb that cancels adjacent-track crosstalk on playback, which is why the plugin averages chroma over 1H on NTSC as well as PAL.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### The VHS format (JVC, 1976)

The helical scan, the two heads, the colour-under chroma and the look of every home video recorded on it. Built from what the format is rather than from anyone's implementation: no code, assets or binaries from any product were used or examined.

## Standards and published specifications

What the implementation is measured against.

- **ITU-R BT.470-6, BT.1700; SMPTE 170M** — the 625/50 and 525/59.94 rasters, their line numbering and pre-equalising intervals.
- **ITU-R BT.601** — the Y' weights, and the U and V scalings used for both standards.
- **IEC 774-1 (VHS)** — through vhs-decode and the patents above; not read directly.
- **PCG (M. E. O'Neill, Harvey Mudd College, 2014)** — the pcg_hash output mix used for the noise, the dropouts and the tracking drift, written out rather than copied from anyone's source.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.

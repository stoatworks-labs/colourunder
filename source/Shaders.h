#pragma once

#include <string>

/**
	Seven passes. Every read of a line-raster buffer is `texelFetch` at
	integer coordinates, so nothing depends on a texture unit's filtering;
	every coefficient (the kernels, the per-line time-base, RF and phase, the
	dropout list) is computed on the CPU in double (`Model.cpp`,
	`Colourunder.cpp`) and handed over as float uniforms or as one small
	RGBA32F texture, `LineData`, N wide and kGenerationsMax + 1 high.

	Once a frame:

	  intakev   host picture -> N x W: each line the area average of the host
	            rows it covers (exact integer overlaps), composited over black
	  intakeh   N x W -> N x Ws: Y' through the Speed's luma Gaussian, U and V
	            through a box of the k host pixels a sample covers (BT.601)

	Per generation (1..5), ping-ponged:

	  noise     -> N x Ws: seeded white noise, four channels
	  tape      src + noise -> Y through the generation's luma kernel, C
	            through the colour-under kernel (its group delay included);
	            the FM noise (rising with frequency) and the chroma noise
	            (band-limited: blotches); the tracking bar; the switching
	            transient; the playback phase error, with PAL's V switch; the
	            dropouts, flagged in alpha
	  comb      the 1H chroma average: line l with line l - 2 (1H earlier, the
	            same field)
	  doc       a flagged sample replaced by the nearest clean one 1H, 2H, ...
	            earlier (DOC on), or left as the white streak (DOC off)

	And to the host:

	  display   each host row shows its nearest line; across, Catmull-Rom from
	            the samples, displaced by the line's time-base (the head switch
	            and the AFC recovering, the bar's jitter); Y'UV to R'G'B', Mix,
	            alpha mix( src.a, 1, Mix )

	`LineData` row g < kGenerationsMax, per line: ( noise gain, bar weight,
	cos and sin of the playback phase error, from double ). Row
	kGenerationsMax: ( display displacement in samples, the sample it starts
	at, 0, 0 ).

	`cutest --dump-shaders DIR` writes exactly the strings the plugin compiles.
*/
namespace colourunder::shaders
{

std::string Vertex();
std::string IntakeV();
std::string IntakeH();
std::string Noise();
std::string Tape();
std::string Comb();
std::string Doc();
std::string Display();

} // namespace colourunder::shaders

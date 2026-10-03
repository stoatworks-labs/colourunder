#pragma once

#include "Frame.h"

#include <functional>

/**
	The FFGL build's per-pixel passes, on the CPU, for the OpenFX build.

	**What exists twice.** Everything the passes are handed -- the kernels,
	LineData, the seeds, the dropouts, every scalar -- is `frame::Make`, which
	both builds call. What has to be written twice is only the per-pixel work
	of the seven fragment shaders in `Shaders.cpp`: intakev, intakeh, noise,
	tape, comb, doc and display. Each function in CpuPasses.cpp mirrors one of
	them line for line -- the same taps in the same order, the same float
	arithmetic, the same clamps -- and carries a `//= mirrored` marker naming
	its shader; the shaders' C++ comments carry the same marker naming this
	file. **A change to either is a change to both.** `cutest --cpu` renders
	both on the same frames and compares them per pixel; `tools/verify.sh`
	runs it.

	**Lines, not pixels.** Every pass but the display works on whole lines of
	the line raster, because every mechanism in it is per line: the
	convolutions run along a line, the comb and the compensator read the same
	sample on earlier lines of the field. So the work is split over lines and
	the passes run one after the other: the intake, then per generation the
	tape (with its noise, which only the tape reads, made per line) and the
	comb with the compensator (fused: the compensator reads the comb's output
	at most 8 lines back, and that is the comb of two tape lines), then the
	display over host rows.

	Nothing here is kept between calls. Each call allocates its own line
	raster, so concurrent renders share nothing.
*/
namespace colourunder::cpu
{

/// Runs body( first, last ) over the whole of [ 0, count ), in ranges split
/// however the caller likes, and returns when every range is done. Ranges may
/// run concurrently: `body` writes only what its own range owns.
using ParallelFor = std::function< void( int count, const std::function< void( int first, int last ) >& body ) >;

/// The whole range, on the calling thread.
void Serial( int count, const std::function< void( int first, int last ) >& body );

/**
	Render one frame on the CPU, as the FFGL build's passes would on the GPU.

	`picture` is the host's picture, plan.raster.W x plan.raster.H RGBA floats,
	row 0 at the BOTTOM -- GL's orientation and OpenFX's, so the indexing below
	is the shaders' own. It is recorded as handed over (see Shaders.cpp's
	intakev: premultiplied, the picture over black). `out` has the same shape
	and must not alias it; only rows [ rowFirst, rowLast ) are written, though
	the line passes always run over the whole picture.
*/
void Render( const frame::Plan& plan, const float* picture, float* out, const ParallelFor& parallel, int rowFirst, int rowLast );

} // namespace colourunder::cpu

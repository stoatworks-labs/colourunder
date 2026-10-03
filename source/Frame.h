#pragma once

#include "Controls.h"
#include "Model.h"

#include <cstdint>
#include <vector>

/**
	One frame's plan: everything the passes are handed, worked out on the CPU in
	double, once a frame. No GL, no FFGL, no OpenFX.

	This is the CPU half of the FFGL build's ProcessOpenGL, lifted out of it so
	that both builds run the SAME arithmetic rather than two transcriptions of
	it: the FFGL plugin turns a Plan into uniforms and one small texture, the
	OpenFX plugin hands it to the CPU passes in `CpuPasses.h`. What it holds is
	exactly what the GPU was handed before the split -- `demo/tools/check_port.sh`
	compiles the plugin's ProcessOpenGL (which now calls `Make`) under a GL
	recorder and compares every LineData float and every uniform with the
	browser demo's port, so a change here that moves any of them fails there.

	A Plan is a pure function of the controls, the host's size and the seconds:
	nothing in it depends on an earlier frame. That is what lets the OpenFX
	build render frames out of order, alone and concurrently.
*/
namespace colourunder::frame
{

/// The ten operator controls as a host hands them over: sliders 0..1, the
/// options as their element index, Generation as the integer, DOC 0 or 1.
///
/// The member initialisers ARE the defaults, for both builds: the FFGL
/// constructor and the OpenFX describe read them from here. Chosen on
/// Resolume's demo clips (AGENTS.md, "Decisions").
struct Values
{
	float standard    = static_cast< float >( model::kPAL );
	float speed       = static_cast< float >( model::kLP );
	float tracking    = 0.03f;
	float headSwitch  = 0.45f;
	float wear        = 0.3f;
	float generation  = 1.0f;
	float doc         = 1.0f;
	float chromaDelay = controls::ChromaDelayParam( 0.45 );//the 2nd-order Butterworth's delay at 0.5 MHz
	float chromaNoise = 0.4f;
	float mix         = 1.0f;
};

/// The harness's test hooks (Colourunder.h's `...ForTest`). Every one is off
/// in the plugin, and the OpenFX build never sets them.
struct Hooks
{
	int perturb           = 0;    ///< a bitmask of `model::Perturb`
	bool quiet            = false;///< no luma noise, no chroma noise, no random phase error
	bool forcePhase       = false;///< every line's phase error is `forcedPhase`, radians
	double forcedPhase    = 0.0;
	bool forceTracking    = false;///< the tracking error is `forcedTracking`, pitches
	double forcedTracking = 0.0;
	bool forceDropout     = false;///< one more dropout in generation 1
	model::Dropout forcedDrop;
};

/// One generation's own uniforms.
struct Generation
{
	model::Kernel luma;  ///< the tape's luma filter (identity in generation 1)
	model::Kernel chroma;///< record and playback together, the group delay included
	uint32_t seed = 0;   ///< the noise pass's, from ( video frame, generation )
	int dropCount = 0;
	float drops[ 4 * model::kMaxDropouts ] = {};///< ( frame line, first sample, end sample, 0 )
};

struct Plan
{
	/// LineData's rows: one per generation, then the display's.
	static constexpr int kDataRows = model::kGenerationsMax + 1;
	static constexpr int kTimeRow  = model::kGenerationsMax;

	int standardIndex = model::kPAL;
	model::Raster raster;
	double seconds  = 0.0;///< the clock the plan was made at
	int64_t frame   = 0;  ///< the video frame: floor( seconds x the standard's frame rate )
	double tracking = 0.0;///< generation 1's tracking error, pitches
	int generations = 1;

	model::Kernel intakeLuma;///< intakeh's, in host pixels
	model::Kernel lumaNoiseShape;
	model::Kernel chromaNoiseShape;
	Generation gen[ model::kGenerationsMax ];

	float lumaSigma   = 0.0f;
	float chromaSigma = 0.0f;
	int pal           = 1;
	int burstLine     = -1;  ///< the field line the head switch falls on, or -1
	float burstStart  = 0.0f;///< the sample it falls at
	int burstSamples  = 1;
	float burstAmount = 0.0f;
	int combSkip      = 0;
	int docEnabled    = 1;
	int docStep       = 2;
	float mixAmount   = 1.0f;

	/// kDataRows rows of N RGBA texels. Row g - 1 is generation g's ( noise
	/// gain, bar weight, cos, sin ) per line; row kTimeRow is the display's
	/// ( shift, from, 0, 0 ) per line, in samples.
	std::vector< float > lineData;

	const float* LineAt( int row, int line ) const
	{
		return lineData.data() + ( static_cast< size_t >( row ) * raster.N + static_cast< size_t >( line ) ) * 4;
	}
};

/// The frame's plan for a host picture W x H at `seconds` on the clock.
Plan Make( const Values& values, int W, int H, double seconds, const Hooks& hooks = Hooks() );

/// The noise pass's seed for a video frame and a generation.
uint32_t FrameSeed( int64_t frame, int generation );

} // namespace colourunder::frame

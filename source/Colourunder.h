#pragma once

#include "Clock.h"
#include "Model.h"
#include "PassBuffer.h"

#include <FFGLSDK.h>

#include <string>
#include <vector>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

/**
	Colourunder -- VHS's helical-scan colour-under recording, as an FFGL
	effect.

	**The one idea.** A VHS deck cannot record colour at its broadcast
	frequency, so it heterodynes the chroma down to about 627 kHz (PAL) or
	629 kHz (NTSC) under the luma's FM carrier, and back up on playback. So
	chroma has a fraction of luma's bandwidth -- about 40 lines of it -- and
	arrives late; its noise is band-limited, blotches rather than grain; and
	the playback's phase error is a hue shift on NTSC and, through PAL's
	alternating V axis and the 1H line average, a desaturation on PAL. The
	two heads on the spinning drum add the rest: the switch 6.5 lines before
	V sync tears the bottom lines sideways, a tracking error walks a noise
	bar up the picture, missing oxide drops a line to white or, with the
	compensator on, to a repeat of the line above.

	Time is frame-relative: the clock is seconds since the first frame, in
	double (clamp's Clock), and every moving thing -- the tracking error's
	drift, the video frame that seeds the noise and the dropouts -- is a pure
	function of it and a seed. Nothing on the GPU outlives a frame, so a
	resize has nothing to lose. See AGENTS.md for the traps.
*/
class Colourunder : public CFFGLPlugin
{
public:
	Colourunder();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	//--- test hooks. Read by cutest; the plugin's own operation never uses
	//--- them, and every one is off outside the harness.

	/// The harness DECLARES its clock unit rather than leaving the voting to
	/// infer one: it renders as fast as the GPU allows.
	void SetClockScaleForTest( double scale )
	{
		clock.SetScaleForTest( scale );
	}
	/// Negative-control hooks, a bitmask of `model::Perturb`.
	void SetPerturbForTest( int bits )
	{
		perturb = bits;
	}
	/// No luma noise, no chroma noise, no random phase error.
	void SetQuietForTest( bool on )
	{
		quiet = on;
	}
	/// Every line's playback phase error, radians, in place of the random one.
	void SetPhaseForTest( bool on, double radians )
	{
		forcePhase = on;
		forcedPhase = radians;
	}
	/// The tracking error, in track pitches, in place of the drift.
	void SetTrackingForTest( bool on, double pitches )
	{
		forceTracking = on;
		forcedTracking = pitches;
	}
	/// One dropout in generation 1, on a frame line, over active-line time.
	void SetDropoutForTest( bool on, int line, double us0, double us1 )
	{
		forceDropout = on;
		forcedDrop   = { line, us0, us1 };
	}
	/// The tracking error generation 1 used on the last frame.
	double LastTrackingForTest() const
	{
		return lastTracking;
	}
	/// Seconds on the plugin's clock at the last frame.
	double LastSecondsForTest() const
	{
		return clock.Now();
	}
	/// The raster the last frame used.
	colourunder::model::Raster LastRasterForTest() const
	{
		return raster;
	}
	/// The last frame's final line raster, Y U V per sample, line 0 first.
	std::vector< float > ReadLinesForTest();
	/// Bytes of GPU buffers held.
	size_t StateBytesForTest() const;

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Deck
		PT_STANDARD,
		PT_SPEED,
		PT_TRACKING,
		PT_HEAD_SWITCH,

		//Tape
		PT_WEAR,
		PT_GENERATION,
		PT_DOC,

		//Colour
		PT_CHROMA_DELAY,
		PT_CHROMA_NOISE,
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	ffglex::FFGLShader intakeVShader;
	ffglex::FFGLShader intakeHShader;
	ffglex::FFGLShader noiseShader;
	ffglex::FFGLShader tapeShader;
	ffglex::FFGLShader combShader;
	ffglex::FFGLShader docShader;
	ffglex::FFGLShader displayShader;
	ffglex::FFGLScreenQuad quad;

	colourunder::PassBuffer columns;  ///< N x W: the lines, at the host's width
	colourunder::PassBuffer intake;   ///< N x Ws: generation 1's input
	colourunder::PassBuffer work[ 3 ];///< N x Ws: tape -> comb -> doc
	colourunder::PassBuffer noise;    ///< N x Ws: white noise
	GLuint lineData  = 0;             ///< N x ( kGenerationsMax + 1 ), RGBA32F
	int lineDataN    = 0;
	int finalBuffer  = 2;

	colourunder::Clock clock;
	bool hostTimeSeen = false;
	colourunder::model::Raster raster;
	int lastWidth = 0, lastHeight = 0;
	double lastTracking = 0.0;

	int perturb        = 0;
	bool quiet         = false;
	bool forcePhase    = false;
	double forcedPhase = 0.0;
	bool forceTracking = false;
	double forcedTracking = 0.0;
	bool forceDropout  = false;
	colourunder::model::Dropout forcedDrop;

	/// Zero-initialised: the About block's ids are never stored to.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};

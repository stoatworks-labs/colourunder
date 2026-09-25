#include "Colourunder.h"

#include "Controls.h"
#include "Diag.h"
#include "Model.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9). The symptom without it is an unknown-type error on
//ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

using namespace ffglex;
using namespace colourunder;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Colourunder >,// Create method
	"CU01",                      // Plugin unique ID of maximum length 4.
	"SW Colourunder",            // Plugin name
	2,                           // API major version number
	1,                           // API minor version number
	0,                           // Plugin major version number
	1,                           // Plugin minor version number
	FF_EFFECT,                   // Plugin type
	"VHS's helical-scan colour-under recording.\n\nThe chroma is heterodyned down under the FM luma, so it keeps about 40 lines of resolution, arrives late and picks up band-limited blotches of noise; its playback phase error is a hue shift on NTSC and a desaturation on PAL. The head switch 6.5 lines before V sync tears the bottom lines, a tracking error walks a noise bar up the picture, and a dropout is a white streak or, with DOC on, the line above repeated. SP, LP and EP; copies of copies.\n\nStart with Speed, Tracking and Generation.",// Plugin description
	"Colourunder FFGL effect"    // About
);

namespace
{
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

GLint loc( const FFGLShader& shader, const char* name )
{
	return glGetUniformLocation( shader.GetGLID(), name );
}

void bindTarget( PassBuffer& target )
{
	glBindFramebuffer( GL_FRAMEBUFFER, target.GetGLID() );
	target.ResizeViewPort();
}

void bindTextures( std::initializer_list< GLuint > textures )
{
	int unit = 0;
	for( GLuint texture : textures )
	{
		glActiveTexture( GL_TEXTURE0 + unit );
		glBindTexture( GL_TEXTURE_2D, texture );
		++unit;
	}
	glActiveTexture( GL_TEXTURE0 );
}

void unbindTextures( int count )
{
	for( int unit = count - 1; unit >= 0; --unit )
	{
		glActiveTexture( GL_TEXTURE0 + unit );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	glActiveTexture( GL_TEXTURE0 );
}

/// FFGLShader::Set has no array overload: an array through the float one is
/// a GL_INVALID_OPERATION that leaves the uniform at zero (ferric's trap).
void setKernel( const FFGLShader& shader, const char* first, const char* count, const char* weights, const model::Kernel& k )
{
	glUniform1i( loc( shader, first ), k.first );
	glUniform1i( loc( shader, count ), k.count );
	glUniform1fv( loc( shader, weights ), k.count, k.weights );
}

uint32_t frameSeed( int64_t frame, int generation )
{
	const uint64_t f = static_cast< uint64_t >( frame );
	return model::Hash( static_cast< uint32_t >( f ) ^ model::Hash( static_cast< uint32_t >( f >> 32 ) + 0x4355AA00u + 977u * static_cast< uint32_t >( generation ) ) );
}
} // namespace

//---------------------------------------------------------------------------
Colourunder::Colourunder()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The tracking error drifts, the noise and the dropouts move with the
	//video frame: host time, frame-relative.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults, chosen on Resolume's demo clips (AGENTS.md, "Decisions").
	// Filled BEFORE any declaration: SetParamInfof reads its default out of
	// GetFloatParameter.
	//---------------------------------------------------------------------
	params[ PT_STANDARD ]     = static_cast< float >( model::kPAL );
	params[ PT_SPEED ]        = static_cast< float >( model::kLP );
	params[ PT_TRACKING ]     = 0.03f;
	params[ PT_HEAD_SWITCH ]  = 0.45f;
	params[ PT_WEAR ]         = 0.3f;
	params[ PT_GENERATION ]   = 1.0f;
	params[ PT_DOC ]          = 1.0f;
	params[ PT_CHROMA_DELAY ] = controls::ChromaDelayParam( 0.45 );//the 2nd-order Butterworth's delay at 0.5 MHz
	params[ PT_CHROMA_NOISE ] = 0.4f;
	params[ PT_MIX ]          = 1.0f;

	auto declareOptions = [ this ]( unsigned int id, const char* name, int count, const char* ( *nameAt )( int ) ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), nameAt( i ), static_cast< float >( i ) );
	};

	declareOptions( PT_STANDARD, "Standard", model::kStandardCount, controls::StandardName );
	declareOptions( PT_SPEED, "Speed", model::kSpeedCount, controls::SpeedName );
	SetParamInfof( PT_TRACKING, "Tracking", FF_TYPE_STANDARD );
	SetParamInfof( PT_HEAD_SWITCH, "Head Switch", FF_TYPE_STANDARD );

	SetParamInfof( PT_WEAR, "Wear", FF_TYPE_STANDARD );
	//A real integer with a real range: FF_TYPE_INTEGER is exempt from the
	//0..1 clamp of a STANDARD default.
	SetParamInfo( PT_GENERATION, "Generation", FF_TYPE_INTEGER, params[ PT_GENERATION ] );
	SetParamRange( PT_GENERATION, static_cast< float >( model::kGenerationsMin ), static_cast< float >( model::kGenerationsMax ) );
	SetParamInfo( PT_DOC, "DOC", FF_TYPE_BOOLEAN, true );

	SetParamInfof( PT_CHROMA_DELAY, "Chroma Delay", FF_TYPE_STANDARD );
	SetParamInfof( PT_CHROMA_NOISE, "Chroma Noise", FF_TYPE_STANDARD );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	//SetParamGroup collapses consecutive ids under one header, so each group
	//is a contiguous run of the enum.
	for( FFUInt32 i = PT_STANDARD; i <= PT_HEAD_SWITCH; ++i )
		SetParamGroup( i, "Deck" );
	for( FFUInt32 i = PT_WEAR; i <= PT_DOC; ++i )
		SetParamGroup( i, "Tape" );
	for( FFUInt32 i = PT_CHROMA_DELAY; i <= PT_MIX; ++i )
		SetParamGroup( i, "Colour" );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Colourunder effect" );
	diag::init();
}

//---------------------------------------------------------------------------
FFResult Colourunder::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	const std::string vertex = shaders::Vertex();
	struct
	{
		FFGLShader* shader;
		std::string fragment;
		const char* name;
	} const stages[] = {
		{ &intakeVShader, shaders::IntakeV(), "intakev" },
		{ &intakeHShader, shaders::IntakeH(), "intakeh" },
		{ &noiseShader, shaders::Noise(), "noise" },
		{ &tapeShader, shaders::Tape(), "tape" },
		{ &combShader, shaders::Comb(), "comb" },
		{ &docShader, shaders::Doc(), "doc" },
		{ &displayShader, shaders::Display(), "display" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( vertex, stage.fragment ) )
			continue;
		//FF_FAIL is invisible to an operator: the effect simply does nothing.
		//This line is the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Colourunder: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Colourunder::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

//---------------------------------------------------------------------------
FFResult Colourunder::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	//The host's viewport, before anything of ours changes it:
	//ScopedFBOBinding restores the framebuffer binding and only that.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	const int W = static_cast< int >( picture.Width );
	const int H = static_cast< int >( picture.Height );

	//---------------------------------------------------------------------
	// The settings.
	//---------------------------------------------------------------------
	const int standardIndex   = controls::OptionIndex( params[ PT_STANDARD ], model::kStandardCount );
	const int speed           = controls::OptionIndex( params[ PT_SPEED ], model::kSpeedCount );
	const double tracking     = controls::Tracking( params[ PT_TRACKING ] );
	const double switchUs     = controls::HeadSwitchUs( params[ PT_HEAD_SWITCH ] );
	const double dropsPerFrame = controls::DropoutsPerFrame( params[ PT_WEAR ] );
	const double wearGain     = controls::WearNoiseGain( params[ PT_WEAR ] );
	const int generations     = controls::Generation( params[ PT_GENERATION ] );
	const bool doc            = params[ PT_DOC ] >= 0.5f;
	const double delayUs      = ( perturb & model::kPerturbNoDelay ) ? 0.0 : controls::ChromaDelayUs( params[ PT_CHROMA_DELAY ] );
	const double chromaNoise  = quiet ? 0.0 : controls::ChromaNoise( params[ PT_CHROMA_NOISE ] ) * model::ChromaNoiseFactor( speed );
	const double phaseSigma   = quiet ? 0.0 : controls::PhaseSigmaRad( params[ PT_CHROMA_NOISE ] );
	const double lumaNoise    = quiet ? 0.0 : model::LumaNoise( speed );
	const float mixAmount     = controls::Amount( params[ PT_MIX ] );

	const model::Standard& standard = model::StandardOf( standardIndex );
	raster                          = model::MakeRaster( standard, W, H );
	const model::Raster& R          = raster;

	//---------------------------------------------------------------------
	// The clock. A resize is not a reason to touch it (the negative control
	// makes it one).
	//---------------------------------------------------------------------
	const bool rasterChanged = lastWidth != 0 && ( lastWidth != W || lastHeight != H );
	lastWidth                = W;
	lastHeight               = H;
	if( rasterChanged && ( perturb & model::kPerturbResizeResetsClock ) )
		clock.Reset();
	clock.Update( hostTimeSeen ? hostTime : -1.0 );
	const double seconds = clock.Now();
	const int64_t frame  = static_cast< int64_t >( std::floor( seconds * standard.FrameRate() ) );

	//---------------------------------------------------------------------
	// Buffers. Every allocation here, before anything binds a texture:
	// FFGLFBO::Initialise sizes its colour texture under a scoped binding,
	// and every ffglex Scoped* binding CLEARS to 0 on exit.
	//---------------------------------------------------------------------
	const auto nearest = PassBuffer::Sampling::Nearest;
	if( !columns.Ensure( W, R.N, GL_RGBA32F, nearest ) || !intake.Ensure( R.Ws, R.N, GL_RGBA32F, nearest )
	    || !work[ 0 ].Ensure( R.Ws, R.N, GL_RGBA32F, nearest ) || !work[ 1 ].Ensure( R.Ws, R.N, GL_RGBA32F, nearest )
	    || !work[ 2 ].Ensure( R.Ws, R.N, GL_RGBA32F, nearest ) || !noise.Ensure( R.Ws, R.N, GL_RGBA32F, nearest ) )
	{
		diag::error( "could not allocate the line raster: " + std::to_string( R.Ws ) + " x " + std::to_string( R.N ) + " (host " + std::to_string( W ) + ")" );
		return FF_FAIL;
	}
	const int dataRows = model::kGenerationsMax + 1;
	if( lineData == 0 || lineDataN != R.N )
	{
		if( lineData != 0 )
			glDeleteTextures( 1, &lineData );
		glGenTextures( 1, &lineData );
		glBindTexture( GL_TEXTURE_2D, lineData );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, R.N, dataRows, 0, GL_RGBA, GL_FLOAT, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		glBindTexture( GL_TEXTURE_2D, 0 );
		lineDataN = R.N;
	}

	//---------------------------------------------------------------------
	// The kernels, in double, once a frame.
	//---------------------------------------------------------------------
	//The band edges are the CHAIN's: the intake's box and the display's
	//reconstruction both cost a little at k > 1, so the first generation's
	//Gaussians are designed with them in, and the stated bandwidths hold at
	//every raster. Later generations are the tape's own filters.
	const double lumaHalf      = model::LumaHalfHz( speed );
	const double chromaHalf    = ( perturb & model::kPerturbChromaAsLuma ) ? lumaHalf : model::kChromaHalfHz;
	const double lumaSigmaUs   = model::SigmaUs( lumaHalf );
	const double chromaSigmaUs = model::SigmaUs( chromaHalf );
	const double lumaFirstUs   = model::SigmaUsWith( lumaHalf, model::DisplayGain( lumaHalf * 1e-6 * R.usPerPixel, R.k ) );
	const double chromaFirstUs = model::SigmaUsWith( chromaHalf, model::DisplayGain( chromaHalf * 1e-6 * R.usPerPixel, R.k ) * model::BoxGain( chromaHalf * 1e-6 * R.usPerPixel, R.k ) );
	const double lowSigmaUs    = model::SigmaUs( model::kDeemphasisHz );
	const model::Kernel intakeLuma = model::Gaussian( lumaFirstUs / R.usPerPixel, 0.0, -0.5 * ( R.k - 1 ) );
	const model::Kernel lumaLater  = model::Gaussian( lumaSigmaUs / R.usPerSample, 0.0 );
	const model::Kernel identity   = model::Gaussian( 0.0, 0.0 );
	const model::Kernel chromaFirst = model::Gaussian( chromaFirstUs / R.usPerSample, delayUs / R.usPerSample );
	const model::Kernel chroma     = model::Gaussian( chromaSigmaUs / R.usPerSample, delayUs / R.usPerSample );
	const model::Kernel delayOnly  = model::Gaussian( 0.0, delayUs / R.usPerSample );
	const model::Kernel chromaNoiseShape = model::UnitPower( model::Gaussian( chromaSigmaUs / std::sqrt( 2.0 ) / R.usPerSample, delayUs / R.usPerSample ) );
	const model::Kernel lumaNoiseShape   = model::NoiseShape( lumaSigmaUs / R.usPerSample, lowSigmaUs / R.usPerSample );

	//---------------------------------------------------------------------
	// Per line: the tracking bar, the noise gain, the phase error, the
	// switch; and the display's time-base. In double.
	//---------------------------------------------------------------------
	const double switchLine = model::SwitchLine( standard, perturb );
	const int switchAt      = static_cast< int >( std::floor( switchLine ) );
	const double switchSample = model::SwitchOffsetUs( standard, switchLine ) / R.usPerSample;
	std::vector< float > data( static_cast< size_t >( R.N ) * dataRows * 4, 0.0f );
	std::vector< double > finalBar( static_cast< size_t >( R.N ), 0.0 );
	for( int g = 1; g <= generations; ++g )
	{
		const double e = forceTracking ? forcedTracking : model::TrackingError( tracking, seconds, static_cast< uint32_t >( g ) );
		if( g == 1 )
			lastTracking = e;
		for( int l = 0; l < R.N; ++l )
		{
			const int m     = model::FieldLineOf( l );
			const double rf = model::RfAt( standard, e, m, perturb );
			const double w  = model::BarWeight( rf );
			float* t        = data.data() + ( static_cast< size_t >( g - 1 ) * R.N + l ) * 4;
			t[ 0 ]          = static_cast< float >( model::NoiseGain( rf ) * wearGain );
			t[ 1 ]          = static_cast< float >( w );
			const double phi = forcePhase ? forcedPhase : model::PhaseError( frame, g, l, phaseSigma );
			t[ 2 ]          = static_cast< float >( std::cos( phi ) );
			t[ 3 ]          = static_cast< float >( std::sin( phi ) );
			if( g == generations )
				finalBar[ static_cast< size_t >( l ) ] = w;
		}
	}
	for( int l = 0; l < R.N; ++l )
	{
		const int m  = model::FieldLineOf( l );
		double shift = 0.0;
		double from  = 1e9;
		if( switchUs > 0.0 && m >= switchAt )
		{
			//Every generation's deck switches heads at the same place and
			//the steps are recorded along with the picture; the TV's AFC
			//pulls the last of it back over kAfcLines.
			shift = generations * switchUs * std::exp( -std::max( 0.0, m - switchLine ) / model::kAfcLines ) / R.usPerSample;
			from  = m == switchAt ? switchSample : -1e9;
		}
		const double w = finalBar[ static_cast< size_t >( l ) ];
		if( w > 0.0 )
		{
			//In the bar the sync goes with the picture: each line lands
			//where the TV's flywheel guesses.
			shift += w * 1.5 * model::BarJitter( frame, l ) / R.usPerSample;
			from = -1e9;
		}
		float* t = data.data() + ( static_cast< size_t >( model::kGenerationsMax ) * R.N + l ) * 4;
		t[ 0 ]   = static_cast< float >( shift );
		t[ 1 ]   = static_cast< float >( from );
	}
	glBindTexture( GL_TEXTURE_2D, lineData );
	glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, R.N, dataRows, GL_RGBA, GL_FLOAT, data.data() );
	glBindTexture( GL_TEXTURE_2D, 0 );

	//---------------------------------------------------------------------
	// Intake: the host's rows onto the lines, then the pixels onto samples.
	//---------------------------------------------------------------------
	{
		bindTarget( columns );
		ScopedShaderBinding shader( intakeVShader.GetGLID() );
		bindTextures( { picture.Handle } );
		glUniform1i( loc( intakeVShader, "Source" ), 0 );
		glUniform1i( loc( intakeVShader, "HostH" ), H );
		glUniform1i( loc( intakeVShader, "Lines" ), R.N );
		quad.Draw();
		unbindTextures( 1 );
	}
	{
		bindTarget( intake );
		ScopedShaderBinding shader( intakeHShader.GetGLID() );
		bindTextures( { columns.TextureID() } );
		glUniform1i( loc( intakeHShader, "Lines" ), 0 );
		glUniform1i( loc( intakeHShader, "HostW" ), W );
		glUniform1i( loc( intakeHShader, "K" ), R.k );
		setKernel( intakeHShader, "YFirst", "YCount", "YW", intakeLuma );
		quad.Draw();
		unbindTextures( 1 );
	}

	//---------------------------------------------------------------------
	// The generations.
	//---------------------------------------------------------------------
	GLuint source = intake.TextureID();
	for( int g = 1; g <= generations; ++g )
	{
		//This generation's dropouts, in samples.
		std::vector< model::Dropout > drops = model::Dropouts( frame, g, dropsPerFrame, R.N, standard.Active() );
		if( g == 1 && forceDropout )
			drops.insert( drops.begin(), forcedDrop );
		float dropData[ 4 * model::kMaxDropouts ] = {};
		const int dropCount = std::min( model::kMaxDropouts, static_cast< int >( drops.size() ) );
		for( int i = 0; i < dropCount; ++i )
		{
			dropData[ 4 * i + 0 ] = static_cast< float >( drops[ i ].line );
			dropData[ 4 * i + 1 ] = static_cast< float >( drops[ i ].us0 / R.usPerSample );
			dropData[ 4 * i + 2 ] = static_cast< float >( drops[ i ].us1 / R.usPerSample );
		}

		{
			bindTarget( noise );
			ScopedShaderBinding shader( noiseShader.GetGLID() );
			glUniform1i( loc( noiseShader, "Samples" ), R.Ws );
			glUniform1i( loc( noiseShader, "LineCount" ), R.N );
			glUniform1ui( loc( noiseShader, "Seed" ), frameSeed( frame, g ) );
			quad.Draw();
		}
		{
			bindTarget( work[ 0 ] );
			ScopedShaderBinding shader( tapeShader.GetGLID() );
			bindTextures( { source, noise.TextureID(), lineData } );
			glUniform1i( loc( tapeShader, "Src" ), 0 );
			glUniform1i( loc( tapeShader, "NoiseTex" ), 1 );
			glUniform1i( loc( tapeShader, "LineData" ), 2 );
			glUniform1i( loc( tapeShader, "Samples" ), R.Ws );
			glUniform1i( loc( tapeShader, "Gen" ), g - 1 );
			glUniform1i( loc( tapeShader, "Pal" ), standardIndex == model::kPAL ? 1 : 0 );
			setKernel( tapeShader, "YFirst", "YCount", "YW", g == 1 ? identity : lumaLater );
			const bool bandLimit = g == 1 || !( perturb & model::kPerturbGenerationOnce );
			setKernel( tapeShader, "CFirst", "CCount", "CW", g == 1 ? chromaFirst : bandLimit ? chroma : delayOnly );
			setKernel( tapeShader, "NYFirst", "NYCount", "NYW", lumaNoiseShape );
			setKernel( tapeShader, "NCFirst", "NCCount", "NCW", chromaNoiseShape );
			glUniform1f( loc( tapeShader, "LumaSigma" ), static_cast< float >( lumaNoise ) );
			glUniform1f( loc( tapeShader, "ChromaSigma" ), static_cast< float >( chromaNoise ) );
			glUniform1i( loc( tapeShader, "BurstLine" ), switchUs > 0.0 ? switchAt : -1 );
			glUniform1f( loc( tapeShader, "BurstStart" ), static_cast< float >( switchSample ) );
			glUniform1i( loc( tapeShader, "BurstSamples" ), std::max( 1, static_cast< int >( std::lround( model::kSwitchBurstUs / R.usPerSample ) ) ) );
			glUniform1f( loc( tapeShader, "BurstAmount" ), static_cast< float >( std::min( 1.0, params[ PT_HEAD_SWITCH ] * 1.5 ) ) );
			glUniform1i( loc( tapeShader, "DropCount" ), dropCount );
			glUniform4fv( loc( tapeShader, "Drops" ), model::kMaxDropouts, dropData );
			quad.Draw();
			unbindTextures( 3 );
		}
		{
			bindTarget( work[ 1 ] );
			ScopedShaderBinding shader( combShader.GetGLID() );
			bindTextures( { work[ 0 ].TextureID() } );
			glUniform1i( loc( combShader, "Src" ), 0 );
			glUniform1i( loc( combShader, "Skip" ), ( standardIndex == model::kPAL && ( perturb & model::kPerturbNoPalAverage ) ) ? 1 : 0 );
			quad.Draw();
			unbindTextures( 1 );
		}
		{
			bindTarget( work[ 2 ] );
			ScopedShaderBinding shader( docShader.GetGLID() );
			bindTextures( { work[ 1 ].TextureID() } );
			glUniform1i( loc( docShader, "Src" ), 0 );
			glUniform1i( loc( docShader, "Enabled" ), doc ? 1 : 0 );
			glUniform1i( loc( docShader, "Step" ), ( perturb & model::kPerturbDocAdjacent ) ? 1 : 2 );
			quad.Draw();
			unbindTextures( 1 );
		}
		source = work[ 2 ].TextureID();
	}
	finalBuffer = 2;

	//---------------------------------------------------------------------
	// Display.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
		ScopedShaderBinding shader( displayShader.GetGLID() );
		bindTextures( { work[ 2 ].TextureID(), picture.Handle, lineData } );
		glUniform1i( loc( displayShader, "Lines" ), 0 );
		glUniform1i( loc( displayShader, "Source" ), 1 );
		glUniform1i( loc( displayShader, "LineData" ), 2 );
		glUniform1i( loc( displayShader, "HostW" ), W );
		glUniform1i( loc( displayShader, "HostH" ), H );
		glUniform1i( loc( displayShader, "LineCount" ), R.N );
		glUniform1i( loc( displayShader, "Samples" ), R.Ws );
		glUniform1i( loc( displayShader, "K" ), R.k );
		glUniform1i( loc( displayShader, "TimeRow" ), model::kGenerationsMax );
		glUniform1f( loc( displayShader, "MixAmount" ), mixAmount );
		quad.Draw();
		unbindTextures( 3 );
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Colourunder::DeInitGL()
{
	for( FFGLShader* shader : { &intakeVShader, &intakeHShader, &noiseShader, &tapeShader, &combShader, &docShader, &displayShader } )
		shader->FreeGLResources();
	quad.Release();
	for( PassBuffer* buffer : { &columns, &intake, &work[ 0 ], &work[ 1 ], &work[ 2 ], &noise } )
		buffer->Destroy();
	if( lineData != 0 )
	{
		glDeleteTextures( 1, &lineData );
		lineData = 0;
	}
	lineDataN = 0;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
std::vector< float > Colourunder::ReadLinesForTest()
{
	const PassBuffer& b = work[ finalBuffer ];
	std::vector< float > out( static_cast< size_t >( b.GetWidth() ) * b.GetHeight() * 4 );
	GLint previous = 0;
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previous );
	glBindFramebuffer( GL_FRAMEBUFFER, b.GetGLID() );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, b.GetWidth(), b.GetHeight(), GL_RGBA, GL_FLOAT, out.data() );
	glBindFramebuffer( GL_FRAMEBUFFER, static_cast< GLuint >( previous ) );
	return out;
}

size_t Colourunder::StateBytesForTest() const
{
	size_t bytes = 0;
	for( const PassBuffer* b : { &columns, &intake, &work[ 0 ], &work[ 1 ], &work[ 2 ], &noise } )
		if( b->IsValid() )
			bytes += static_cast< size_t >( b->GetWidth() ) * b->GetHeight() * 16;
	return bytes + static_cast< size_t >( lineDataN ) * ( model::kGenerationsMax + 1 ) * 16;
}

//---------------------------------------------------------------------------
FFResult Colourunder::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Colourunder::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

char* Colourunder::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Colourunder::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only; it has to say so
	// successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

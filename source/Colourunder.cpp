#include "Colourunder.h"

#include "Controls.h"
#include "Diag.h"
#include "Frame.h"
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
	// Defaults, chosen on Resolume's demo clips (AGENTS.md, "Decisions"),
	// and written down once in frame::Values, which the OpenFX build's
	// describe reads too. Filled BEFORE any declaration: SetParamInfof reads
	// its default out of GetFloatParameter.
	//---------------------------------------------------------------------
	const frame::Values defaults;
	params[ PT_STANDARD ]     = defaults.standard;
	params[ PT_SPEED ]        = defaults.speed;
	params[ PT_TRACKING ]     = defaults.tracking;
	params[ PT_HEAD_SWITCH ]  = defaults.headSwitch;
	params[ PT_WEAR ]         = defaults.wear;
	params[ PT_GENERATION ]   = defaults.generation;
	params[ PT_DOC ]          = defaults.doc;
	params[ PT_CHROMA_DELAY ] = defaults.chromaDelay;
	params[ PT_CHROMA_NOISE ] = defaults.chromaNoise;
	params[ PT_MIX ]          = defaults.mix;

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
	SetParamInfo( PT_DOC, "DOC", FF_TYPE_BOOLEAN, defaults.doc >= 0.5f );

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
	// The clock. A resize is not a reason to touch it (the negative control
	// makes it one).
	//---------------------------------------------------------------------
	const bool rasterChanged = lastWidth != 0 && ( lastWidth != W || lastHeight != H );
	lastWidth                = W;
	lastHeight               = H;
	if( rasterChanged && ( perturb & model::kPerturbResizeResetsClock ) )
		clock.Reset();
	clock.Update( hostTimeSeen ? hostTime : -1.0 );

	//---------------------------------------------------------------------
	// The frame's plan: the settings, the kernels, LineData, every
	// generation's seed and dropouts -- in double, on the CPU, in Frame.cpp,
	// which the OpenFX build's CPU render runs as well.
	//---------------------------------------------------------------------
	frame::Values values;
	values.standard    = params[ PT_STANDARD ];
	values.speed       = params[ PT_SPEED ];
	values.tracking    = params[ PT_TRACKING ];
	values.headSwitch  = params[ PT_HEAD_SWITCH ];
	values.wear        = params[ PT_WEAR ];
	values.generation  = params[ PT_GENERATION ];
	values.doc         = params[ PT_DOC ];
	values.chromaDelay = params[ PT_CHROMA_DELAY ];
	values.chromaNoise = params[ PT_CHROMA_NOISE ];
	values.mix         = params[ PT_MIX ];
	frame::Hooks hooks;
	hooks.perturb        = perturb;
	hooks.quiet          = quiet;
	hooks.forcePhase     = forcePhase;
	hooks.forcedPhase    = forcedPhase;
	hooks.forceTracking  = forceTracking;
	hooks.forcedTracking = forcedTracking;
	hooks.forceDropout   = forceDropout;
	hooks.forcedDrop     = forcedDrop;
	const frame::Plan plan = frame::Make( values, W, H, clock.Now(), hooks );
	raster                 = plan.raster;
	lastTracking           = plan.tracking;
	const model::Raster& R = plan.raster;

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
	const int dataRows = frame::Plan::kDataRows;
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
	glBindTexture( GL_TEXTURE_2D, lineData );
	glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, R.N, dataRows, GL_RGBA, GL_FLOAT, plan.lineData.data() );
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
		setKernel( intakeHShader, "YFirst", "YCount", "YW", plan.intakeLuma );
		quad.Draw();
		unbindTextures( 1 );
	}

	//---------------------------------------------------------------------
	// The generations.
	//---------------------------------------------------------------------
	GLuint source = intake.TextureID();
	for( int g = 1; g <= plan.generations; ++g )
	{
		const frame::Generation& G = plan.gen[ g - 1 ];
		{
			bindTarget( noise );
			ScopedShaderBinding shader( noiseShader.GetGLID() );
			glUniform1i( loc( noiseShader, "Samples" ), R.Ws );
			glUniform1i( loc( noiseShader, "LineCount" ), R.N );
			glUniform1ui( loc( noiseShader, "Seed" ), G.seed );
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
			glUniform1i( loc( tapeShader, "Pal" ), plan.pal );
			setKernel( tapeShader, "YFirst", "YCount", "YW", G.luma );
			setKernel( tapeShader, "CFirst", "CCount", "CW", G.chroma );
			setKernel( tapeShader, "NYFirst", "NYCount", "NYW", plan.lumaNoiseShape );
			setKernel( tapeShader, "NCFirst", "NCCount", "NCW", plan.chromaNoiseShape );
			glUniform1f( loc( tapeShader, "LumaSigma" ), plan.lumaSigma );
			glUniform1f( loc( tapeShader, "ChromaSigma" ), plan.chromaSigma );
			glUniform1i( loc( tapeShader, "BurstLine" ), plan.burstLine );
			glUniform1f( loc( tapeShader, "BurstStart" ), plan.burstStart );
			glUniform1i( loc( tapeShader, "BurstSamples" ), plan.burstSamples );
			glUniform1f( loc( tapeShader, "BurstAmount" ), plan.burstAmount );
			glUniform1i( loc( tapeShader, "DropCount" ), G.dropCount );
			glUniform4fv( loc( tapeShader, "Drops" ), model::kMaxDropouts, G.drops );
			quad.Draw();
			unbindTextures( 3 );
		}
		{
			bindTarget( work[ 1 ] );
			ScopedShaderBinding shader( combShader.GetGLID() );
			bindTextures( { work[ 0 ].TextureID() } );
			glUniform1i( loc( combShader, "Src" ), 0 );
			glUniform1i( loc( combShader, "Skip" ), plan.combSkip );
			quad.Draw();
			unbindTextures( 1 );
		}
		{
			bindTarget( work[ 2 ] );
			ScopedShaderBinding shader( docShader.GetGLID() );
			bindTextures( { work[ 1 ].TextureID() } );
			glUniform1i( loc( docShader, "Src" ), 0 );
			glUniform1i( loc( docShader, "Enabled" ), plan.docEnabled );
			glUniform1i( loc( docShader, "Step" ), plan.docStep );
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
		glUniform1i( loc( displayShader, "TimeRow" ), frame::Plan::kTimeRow );
		glUniform1f( loc( displayShader, "MixAmount" ), plan.mixAmount );
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

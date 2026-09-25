/**
	cutest -- render Colourunder offline, and measure the deck out of it.

	Every check here drives the REAL plugin class through a headless GL
	context on a synthetic clock and measures the property it claims from the
	picture the plugin renders, against a prediction derived here from the
	format's stated numbers by a different route from the one the plugin
	takes:

		cutest --out /tmp/frame.png     a picture, on the moving test card
		cutest --list                   every parameter, its kind and default
		cutest --chroma                 chroma's horizontal response falls to half
		                                at the colour-under bandwidth (about 40
		                                lines), found by bisection on rendered
		                                sinusoids; luma holds to the Speed's
		cutest --delay                  a colour edge's chroma lags its luma edge
		                                by the stated group delay, whole-pixel and
		                                fractional
		cutest --switch                 the head switch disturbs exactly the lines
		                                from 6.5 H before V sync, both standards
		cutest --pal                    a phase error is a desaturation on PAL,
		                                with no hue shift on any line, and a hue
		                                shift on NTSC
		cutest --doc                    a forced dropout with DOC on is the line
		                                1H before it, exactly
		cutest --generation             two generations: the chroma response is the
		                                composed filter, half at 0.5 / sqrt 2 MHz
		cutest --tracking               the noise bar sits where the tracking error
		                                puts it, widens as the error grows, walks as
		                                it drifts, and is the same at any frame rate
		cutest --resize                 the state survives a resize
		cutest --negative               every check above can FAIL
		cutest --names --model          the checks that need no GL
		cutest --bench                  the render cost
		cutest --dump-shaders DIR       the exact GLSL the plugin compiles
		cutest --pipe                   raw frames in, raw frames out

	CUTEST_RENDERER=software runs any of it on Apple's software renderer (what
	a GPU-less CI runner has). AGENTS.md has one line per check on where each
	tolerance comes from.
*/

#include "Colourunder.h"
#include "Controls.h"
#include "Model.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
namespace model    = colourunder::model;
namespace controls = colourunder::controls;

int g_checks   = 0;
int g_failures = 0;

constexpr double kU  = 5.9604644775390625e-8;//2^-24, half a float ulp at 1
constexpr double kPi = 3.14159265358979323846;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	//CUTEST_RENDERER=software asks for Apple's software renderer by id, on a
	//Mac that has a GPU. It is what a GPU-less CI runner falls back to, and it
	//is not bit-repeatable frame to frame (repousse's resize check failed CI
	//by one ulp), so a check that would fail only in CI can be run here first.
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "CUTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		std::fprintf( stderr, "cutest: CUTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

template< typename T >
std::vector< T > flipRows( const std::vector< T >& image, int width, int height )
{
	std::vector< T > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}


//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Colourunder::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Colourunder& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Colourunder::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		//An integer carries a real range.
		if( p.type == FF_TYPE_INTEGER )
		{
			const RangeStruct range = plugin.GetParamRange( i );
			p.low                   = range.min;
			p.high                  = range.max;
		}
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Colourunder& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

bool applySetting( Colourunder& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.rfind( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name = assignment.substr( 0, equals );
	const int index        = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
	return true;
}

bool set( Colourunder& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

/// The projector a check runs: the controls it moves, everything else OFF
/// (no print, no weave, no lens falloff, centred), so each check sees only

//---------------------------------------------------------------------------
// The format, stated HERE from the sources (ATTRIBUTIONS.md), never read out
// of the plugin: a constant typed wrong in Model.cpp has to show up as a
// failed check, not as an agreement.
//---------------------------------------------------------------------------
struct Stated
{
	const char* name;
	double line, front, sync, back;///< us: BT.470-6 / SMPTE 170M
	int frameLines;                ///< the digital raster's active lines
	double fieldLines;
	double preEq;                  ///< lines from the last active line's end to V sync
	double fH;
	double carrierLines;           ///< the colour-under carrier in multiples of fH
	double Active() const
	{
		return line - front - sync - back;
	}
	int FieldActive() const
	{
		return frameLines / 2;
	}
};

const Stated kStated[ 2 ] = {
	{ "PAL", 64.0, 1.65, 4.7, 5.7, 576, 312.5, 2.5, 15625.0, 40.125 },
	{ "NTSC", 1001.0 / 15.75, 1.5, 4.7, 4.7, 480, 262.5, 3.0, 15750000.0 / 1001.0, 40.0 },
};

constexpr double kChromaHalfMHz = 0.5;          ///< the colour-under band's half amplitude
constexpr double kLumaHalfMHz[ 3 ] = { 3.0, 3.0 * 230.0 / 250.0, 2.4 };
constexpr double kSwitchBefore   = 6.5;         ///< head switch, line periods before V sync
constexpr double kSlope          = 2.0;         ///< track pitches crossed per field
constexpr double kThreshold      = 0.06;        ///< RF below which the FM demodulator loses lock

/// The Gaussian whose response is 1/2 at fHalf: exp( -2 pi^2 s^2 f^2 ).
double gaussianGain( double fMHz, double fHalfMHz, double generations = 1.0 )
{
	return std::pow( 0.5, generations * ( fMHz / fHalfMHz ) * ( fMHz / fHalfMHz ) );
}

/// The frame line a host row (from the top) shows: the nearest line centre.
int lineOfRow( int r, int N, int H )
{
	//Row r's centre is ( r + 1/2 ) / H of the height; line l covers
	//[ l / N, ( l + 1 ) / N ).
	return static_cast< int >( std::floor( ( r + 0.5 ) * N / static_cast< double >( H ) ) );
}

/// Host pixels per sample, the way MakeRaster states it: the fewest whole
/// pixels that keep a line at 1024 samples or under.
int stepOf( int W )
{
	int k = 1;
	while( ( W + k - 1 ) / k > 1024 )
		++k;
	return k;
}

/// Where a decreasing gain function crosses 1/2, by bisection.
double halfOf( const std::function< double( double ) >& gain, double lo, double hi )
{
	for( int i = 0; i < 60; ++i )
	{
		const double mid = 0.5 * ( lo + hi );
		( gain( mid ) > 0.5 ? lo : hi ) = mid;
	}
	return 0.5 * ( lo + hi );
}

//---------------------------------------------------------------------------
// BT.601 Y' and the U, V scalings, both ways, in double.
//---------------------------------------------------------------------------
constexpr double kUs = 0.492111, kVs = 0.877283;

void yuvToRgb( double y, double u, double v, float* rgb )
{
	const double r = y + v / kVs, b = y + u / kUs;
	const double g = ( y - 0.299 * r - 0.114 * b ) / 0.587;
	rgb[ 0 ]       = static_cast< float >( r );
	rgb[ 1 ]       = static_cast< float >( g );
	rgb[ 2 ]       = static_cast< float >( b );
}

void rgbToYuv( const float* c, double& y, double& u, double& v )
{
	y = 0.299 * c[ 0 ] + 0.587 * c[ 1 ] + 0.114 * c[ 2 ];
	u = kUs * ( c[ 2 ] - y );
	v = kVs * ( c[ 0 ] - y );
}

//---------------------------------------------------------------------------
// Pictures, float RGBA, top-first.
//---------------------------------------------------------------------------
using Picture = std::vector< float >;

Picture yuvPicture( int W, int H, const std::function< void( int, int, double&, double&, double& ) >& at )
{
	Picture p( static_cast< size_t >( W ) * H * 4 );
	for( int r = 0; r < H; ++r )
		for( int x = 0; x < W; ++x )
		{
			double y = 0.5, u = 0.0, v = 0.0;
			at( x, r, y, u, v );
			float* px = p.data() + ( static_cast< size_t >( r ) * W + x ) * 4;
			yuvToRgb( y, u, v, px );
			px[ 3 ] = 1.0f;
		}
	return p;
}

Picture flat( int W, int H, double level )
{
	Picture p( static_cast< size_t >( W ) * H * 4 );
	for( size_t i = 0; i < p.size(); i += 4 )
	{
		p[ i ] = p[ i + 1 ] = p[ i + 2 ] = static_cast< float >( level );
		p[ i + 3 ]                       = 1.0f;
	}
	return p;
}


/// The deck a check runs: the controls it moves, everything else OFF (no
/// tracking error, no switch, no wear, one generation, no chroma noise), so
/// each check sees only what it measures.
struct Knobs
{
	int standard      = 0;
	int speed         = 0;
	double tracking   = 0.0;
	double headSwitch = 0.0;
	double wear       = 0.0;
	int generation    = 1;
	bool doc          = true;
	double delayUs    = 0.45;
	double chromaNoise = 0.0;
	double mix        = 1.0;
};

void apply( Colourunder& p, const Knobs& k )
{
	set( p, "Standard", static_cast< float >( k.standard ) );
	set( p, "Speed", static_cast< float >( k.speed ) );
	set( p, "Tracking", static_cast< float >( k.tracking ) );
	set( p, "Head Switch", static_cast< float >( k.headSwitch ) );
	set( p, "Wear", static_cast< float >( k.wear ) );
	set( p, "Generation", static_cast< float >( k.generation ) );
	set( p, "DOC", k.doc ? 1.0f : 0.0f );
	set( p, "Chroma Delay", controls::ChromaDelayParam( k.delayUs ) );
	set( p, "Chroma Noise", static_cast< float >( k.chromaNoise ) );
	set( p, "Mix", static_cast< float >( k.mix ) );
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output, and the clock that drives it.
//---------------------------------------------------------------------------
struct Session
{
	Colourunder plugin;
	int width  = 0;
	int height = 0;
	double fps = 60.0;
	bool floatOutput = true;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	void makeTargets()
	{
		sourceTexture = makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr );
		outputTexture = floatOutput ? makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr )
		                            : makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
	}

	void dropTargets()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool begin( int w, int h )
	{
		width  = w;
		height = h;
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		makeTargets();
		return true;
	}

	/// What a host does when the clip or the composition changes size: hand
	/// the SAME instance a differently sized input. No DeInitGL.
	void resize( int w, int h )
	{
		dropTargets();
		width  = w;
		height = h;
		makeTargets();
	}

	void upload( const Picture& pixels )
	{
		const std::vector< float > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void upload( const std::vector< unsigned char >& pixels )
	{
		const std::vector< unsigned char > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	/// A synthetic clock, and it has to be synthetic: frame n is clocked at
	/// n / fps seconds, the unit declared, not inferred.
	bool renderAt( long frame )
	{
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( static_cast< double >( frame ) / fps );

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed on frame %ld\n", frame );
		return ok;
	}

	template< typename P >
	bool render( long frame, const P& pixels )
	{
		upload( pixels );
		return renderAt( frame );
	}

	/// A row from the TOP, RGBA floats.
	std::vector< float > readRow( int y )
	{
		std::vector< float > row( static_cast< size_t >( width ) * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, height - 1 - y, width, 1, GL_RGBA, GL_FLOAT, row.data() );
		return row;
	}

	/// The whole picture, top first, RGBA floats.
	std::vector< float > readAll()
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return flipRows( pixels, width, height );
	}

	std::vector< unsigned char > readBack()
	{
		std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	void end()
	{
		plugin.DeInitGL();
		dropTargets();
	}
};

/// A check's session: the knobs, the quiet hook, the perturbation.
bool open( Session& s, int W, int H, const Knobs& k, int perturb, bool quietNoise = true )
{
	apply( s.plugin, k );
	s.plugin.SetPerturbForTest( perturb );
	s.plugin.SetQuietForTest( quietNoise );
	return s.begin( W, H );
}

/// Least squares: v( x ) ~ c0 + c1 cos( w ( x + 1/2 ) ) + c2 sin( w ( x + 1/2 ) )
/// over [ a, b ). Returns the amplitude and the phase phi of A cos( w x' + phi ).
void fitSinusoid( const std::vector< double >& v, int a, int b, double w, double& amplitude, double& phase )
{
	double M[ 3 ][ 3 ] = {}, r[ 3 ] = {};
	for( int x = a; x < b; ++x )
	{
		const double f[ 3 ] = { 1.0, std::cos( w * ( x + 0.5 ) ), std::sin( w * ( x + 0.5 ) ) };
		for( int i = 0; i < 3; ++i )
		{
			r[ i ] += f[ i ] * v[ static_cast< size_t >( x ) ];
			for( int j = 0; j < 3; ++j )
				M[ i ][ j ] += f[ i ] * f[ j ];
		}
	}
	//Gaussian elimination, 3 x 3.
	for( int i = 0; i < 3; ++i )
	{
		int pivot = i;
		for( int j = i + 1; j < 3; ++j )
			if( std::fabs( M[ j ][ i ] ) > std::fabs( M[ pivot ][ i ] ) )
				pivot = j;
		std::swap( M[ i ], M[ pivot ] );
		std::swap( r[ i ], r[ pivot ] );
		for( int j = i + 1; j < 3; ++j )
		{
			const double f = M[ j ][ i ] / M[ i ][ i ];
			for( int c = i; c < 3; ++c )
				M[ j ][ c ] -= f * M[ i ][ c ];
			r[ j ] -= f * r[ i ];
		}
	}
	double c[ 3 ];
	for( int i = 2; i >= 0; --i )
	{
		double s = r[ i ];
		for( int j = i + 1; j < 3; ++j )
			s -= M[ i ][ j ] * c[ j ];
		c[ i ] = s / M[ i ][ i ];
	}
	amplitude = std::hypot( c[ 1 ], c[ 2 ] );
	phase     = std::atan2( -c[ 2 ], c[ 1 ] );
}

/// Y' or U of a top-first output row.
std::vector< double > channelOf( const std::vector< float >& row, int W, int channel )
{
	std::vector< double > out( static_cast< size_t >( W ) );
	for( int x = 0; x < W; ++x )
	{
		double y, u, v;
		rgbToYuv( row.data() + 4 * x, y, u, v );
		out[ static_cast< size_t >( x ) ] = channel == 0 ? y : channel == 1 ? u : v;
	}
	return out;
}

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAIL";
}

int report( bool ok, bool quiet, const char* format, ... ) __attribute__( ( format( printf, 3, 4 ) ) );
int report( bool ok, bool quiet, const char* format, ... )
{
	++g_checks;
	if( !ok )
		++g_failures;
	//Quiet is a negative control's run: its failures are the point, and the
	//summary line says so. --perturb runs the same thing verbosely.
	if( quiet )
		return ok ? 0 : 1;
	va_list args;
	va_start( args, format );
	std::printf( "   %-4s ", verdict( ok ) );
	std::vprintf( format, args );
	std::printf( "\n" );
	va_end( args );
	return ok ? 0 : 1;
}

void note( bool quiet, const char* format, ... ) __attribute__( ( format( printf, 2, 3 ) ) );
void note( bool quiet, const char* format, ... )
{
	if( quiet )
		return;
	va_list args;
	va_start( args, format );
	std::printf( "        " );
	std::vprintf( format, args );
	std::printf( "\n" );
	va_end( args );
}

/// The light the shutter passes over [a, b], in frames: the stated geometry
/// (blade openings of `open` of each 1 / B, centred in it) intersected with

double sigmaUsFor( double fHalfMHz )
{
	return std::sqrt( std::log( 2.0 ) / 2.0 ) / ( kPi * fHalfMHz );
}

/// The gain at f (MHz) of luma (channel 0) or chroma (1) through the plugin:
/// a cosine of that frequency on a flat mid grey, the middle row read back,
/// its fundamental fitted over the interior.
double measureGain( Session& s, const Stated& st, double fMHz, int channel, int margin, long& frame, double* phase = nullptr )
{
	const int W = s.width, H = s.height;
	const double usPP = st.Active() / W;
	const double w    = 2.0 * kPi * fMHz * usPP;
	const double a    = channel == 0 ? 0.2 : 0.1;
	const Picture p   = yuvPicture( W, H, [ & ]( int x, int, double& y, double& u, double& ) {
        const double c = a * std::cos( w * ( x + 0.5 ) );
        if( channel == 0 )
            y = 0.5 + c;
        else
            u = c;
	} );
	if( !s.render( frame++, p ) )
		return -1.0;
	const std::vector< double > v = channelOf( s.readRow( H / 2 ), W, channel );
	double amplitude = 0.0, ph = 0.0;
	fitSinusoid( v, margin, W - margin, w, amplitude, ph );
	if( phase )
		*phase = ph;
	return amplitude / a;
}

/// Bisection on rendered pictures for the frequency where the gain is 1/2.
bool measuredHalf( Session& s, const Stated& st, int channel, int margin, double lo, double hi, long& frame, double& half )
{
	if( !( measureGain( s, st, lo, channel, margin, frame ) > 0.5 ) || !( measureGain( s, st, hi, channel, margin, frame ) < 0.5 ) )
		return false;
	for( int i = 0; i < 20; ++i )
	{
		const double mid = 0.5 * ( lo + hi );
		( measureGain( s, st, mid, channel, margin, frame ) > 0.5 ? lo : hi ) = mid;
	}
	half = 0.5 * ( lo + hi );
	return true;
}

/// Lines of horizontal resolution per picture height for a frequency: two
/// lines a cycle over the active line, times the 3 : 4 aspect.
double tvLines( double fMHz, const Stated& st )
{
	return 2.0 * fMHz * st.Active() * 0.75;
}

//---------------------------------------------------------------------------
// --chroma
//---------------------------------------------------------------------------
int runChroma( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "chroma: the colour-under band's half amplitude, measured by bisection on rendered sinusoids, %dx%d\n", W, H );
	int failures = 0;
	const int k  = stepOf( W );
	for( int standard = 0; standard < 2; ++standard )
	{
		const Stated& st  = kStated[ standard ];
		const double usPP = st.Active() / W;
		const double nyq  = 0.5 / usPP;
		{
			Session s;
			Knobs kn;
			kn.standard = standard;
			if( !open( s, W, H, kn, perturb ) )
				return 1;
			long frame      = 0;
			const int margin = static_cast< int >( std::ceil( 6.0 * sigmaUsFor( kChromaHalfMHz ) / usPP + 0.45 / usPP ) ) + 4 * k + 4;
			const double hi  = std::min( 1.5, 0.9 * nyq );
			double half      = 0.0;
			const bool found = measuredHalf( s, st, 1, margin, 0.05, hi, frame, half );
			//The chain -- the intake's box and the display's reconstruction
			//included -- is stated to be half at 0.5 MHz at every raster.
			const double predicted = kChromaHalfMHz;
			const double tol       = k == 1 ? 2e-4 : 5e-4;
			if( !found )
				failures += report( false, quiet, "%s chroma: no half-amplitude crossing between 0.05 and %.2f MHz", st.name, hi );
			else
				failures += report( std::fabs( half - predicted ) <= tol, quiet,
				                    "%s chroma half amplitude at %.5f MHz = %.1f lines (stated %.1f MHz = %.1f lines, tol %.0e MHz)",
				                    st.name, half, tvLines( half, st ), predicted, tvLines( kChromaHalfMHz, st ), tol );
			s.end();
		}
		//Luma: where this raster can carry the band edge, the Speed's own;
		//where it cannot, at least that luma is not the chroma's.
		const double sigmaLumaPx = sigmaUsFor( kLumaHalfMHz[ 0 ] ) / usPP;
		for( int speed = 0; speed < 3; ++speed )
		{
			Session s;
			Knobs kn;
			kn.standard = standard;
			kn.speed    = speed;
			if( !open( s, W, H, kn, perturb ) )
				return 1;
			long frame       = 0;
			const int margin = static_cast< int >( std::ceil( 6.0 * sigmaUsFor( kLumaHalfMHz[ speed ] ) / usPP ) ) + 4 * k + 4;
			if( sigmaLumaPx >= 1.0 )
			{
				double half      = 0.0;
				const double hi  = 0.95 * nyq;
				const bool found = measuredHalf( s, st, 0, margin, 0.5, hi, frame, half );
				const double predicted = kLumaHalfMHz[ speed ];
				const double tol       = k == 1 ? 2e-4 : 2e-3;
				if( !found )
					failures += report( false, quiet, "%s luma %s: no half-amplitude crossing below %.2f MHz", st.name, controls::SpeedName( speed ), hi );
				else
					failures += report( std::fabs( half - predicted ) <= tol, quiet, "%s luma %s half amplitude at %.4f MHz = %.0f lines (stated %.2f MHz, tol %.0e MHz)", st.name,
					                    controls::SpeedName( speed ), half, tvLines( half, st ), predicted, tol );
			}
			else
			{
				const double g = measureGain( s, st, kChromaHalfMHz, 0, margin, frame );
				failures += report( g >= 0.95, quiet, "%s luma %s at 0.5 MHz keeps %.4f (this raster's Nyquist is %.2f MHz: it cannot carry the %.2f MHz band edge)", st.name,
				                    controls::SpeedName( speed ), g, nyq, kLumaHalfMHz[ speed ] );
			}
			s.end();
		}
	}
	return failures;
}

//---------------------------------------------------------------------------
// --delay
//---------------------------------------------------------------------------
int runDelay( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "delay: a colour edge's chroma lags its luma edge by the stated group delay, %dx%d\n", W, H );
	int failures = 0;
	const int k  = stepOf( W );
	for( int standard = 0; standard < 2; ++standard )
	{
		const Stated& st  = kStated[ standard ];
		const double usPP = st.Active() / W;
		const double whole = ( 5.0 * usPP <= 1.0 ? 5.0 : 3.0 ) * usPP;//a whole number of pixels, at k = 1
		for( const double delayUs : { whole, 0.45 } )
		{
			Session s;
			Knobs kn;
			kn.standard = standard;
			kn.delayUs  = delayUs;
			if( !open( s, W, H, kn, perturb ) )
				return 1;
			const double expected = delayUs / usPP;
			const char* kind      = std::fabs( expected - std::round( expected ) ) < 1e-9 ? "whole" : "fractional";

			//The edge: luma and chroma step together at the pixel boundary W/2.
			const int xe    = W / 2;
			const Picture p = yuvPicture( W, H, [ & ]( int x, int, double& y, double& u, double& ) {
                y = x < xe ? 0.35 : 0.60;
                u = x < xe ? -0.06 : 0.06;
			} );
			if( !s.render( 0, p ) )
				return 1;
			const std::vector< float > row = s.readRow( H / 2 );
			const std::vector< double > Y  = channelOf( row, W, 0 );
			const std::vector< double > U  = channelOf( row, W, 1 );
			const int reach = static_cast< int >( std::ceil( 6.0 * sigmaUsFor( kChromaHalfMHz ) / usPP + expected ) ) + 4 * k + 6;
			auto centroid   = [ & ]( const std::vector< double >& v ) {
                double m = 0.0, t = 0.0;
                for( int x = std::max( 0, xe - reach ); x < std::min( W - 1, xe + reach ); ++x )
                {
                    const double d = v[ static_cast< size_t >( x + 1 ) ] - v[ static_cast< size_t >( x ) ];
                    m += ( x + 1 ) * d;
                    t += d;
                }
                return m / t;
			};
			const double cy = centroid( Y ), cu = centroid( U );
			if( k == 1 )
			{
				failures += report( std::fabs( cy - xe ) <= 3e-3, quiet, "%s luma edge at %.5f px (the edge is at %d: luma has no delay)", st.name, cy, xe );
				failures += report( std::fabs( ( cu - cy ) - expected ) <= 3e-3, quiet, "%s %s: chroma lags luma by %.5f px, stated %.3f us = %.5f px (tol 3e-3 px)", st.name, kind,
				                    cu - cy, delayUs, expected );
			}
			else
				note( quiet, "%s %s: the edge's centroid lag is %.4f px against %.4f (not asserted at k = %d: see AGENTS.md)", st.name, kind, cu - cy, expected, k );

			//The same delay as a phase, at 0.12 MHz, at any k.
			long frame        = 1;
			const int margin  = reach;
			double py = 0.0, pu = 0.0;
			measureGain( s, st, 0.12, 0, margin, frame, &py );
			measureGain( s, st, 0.12, 1, margin, frame, &pu );
			const double w   = 2.0 * kPi * 0.12 * usPP;
			const double lag = std::remainder( py - pu, 2.0 * kPi ) / w;
			failures += report( std::fabs( lag - expected ) <= 1e-2, quiet, "%s %s: at 0.12 MHz chroma's phase lags luma's by %.5f px, stated %.5f px (tol 1e-2 px)", st.name, kind, lag, expected );
			s.end();
		}
	}
	return failures;
}

//---------------------------------------------------------------------------
// --switch
//---------------------------------------------------------------------------
int runSwitch( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "switch: the head switch disturbs the lines from %.1f H before V sync and nothing above, %dx%d\n", kSwitchBefore, W, H );
	int failures = 0;
	const int k  = stepOf( W );
	for( int standard = 0; standard < 2; ++standard )
	{
		const Stated& st  = kStated[ standard ];
		const double usPP = st.Active() / W;
		//The switch, in field lines from the top of the field's active
		//picture: the field has FieldActive whole lines, and after the last of
		//them preEq line periods run before V sync.
		const double at   = st.FieldActive() - ( kSwitchBefore - st.preEq );
		const int partial = std::floor( at ) < at ? static_cast< int >( std::floor( at ) ) : -1;
		const int full    = static_cast< int >( std::ceil( at ) );
		//On a line it cuts, the switch lands at its fraction of the line
		//period, counted from the leading edge of sync.
		const double xFrom = partial >= 0 ? ( ( at - std::floor( at ) ) * st.line - st.sync - st.back ) / usPP : 0.0;

		std::vector< float > a, b;
		for( int pass = 0; pass < 2; ++pass )
		{
			Session s;
			Knobs kn;
			kn.standard   = standard;
			kn.headSwitch = pass == 0 ? 0.0 : 1.0;
			if( !open( s, W, H, kn, perturb ) )
				return 1;
			const Picture p = yuvPicture( W, H, [ & ]( int x, int, double& y, double& u, double& ) {
                y = 0.5 + 0.3 * std::cos( 2.0 * kPi * ( x + 0.5 ) / 9.7 );
                u = 0.05 * std::cos( 2.0 * kPi * ( x + 0.5 ) / 23.0 );
			} );
			if( !s.render( 0, p ) )
				return 1;
			( pass == 0 ? a : b ) = s.readAll();
			s.end();
		}
		int firstDisturbed = -1, wrongAbove = 0, quietInRegion = 0, partialRows = 0, partialEarly = 0, partialLate = 0;
		for( int r = 0; r < H; ++r )
		{
			const int l = lineOfRow( r, st.frameLines, H );
			const int m = l >> 1;
			int differ = 0, early = 0, late = 0;
			for( int x = 0; x < W; ++x )
			{
				const size_t i = ( static_cast< size_t >( r ) * W + x ) * 4;
				const bool d   = a[ i ] != b[ i ] || a[ i + 1 ] != b[ i + 1 ] || a[ i + 2 ] != b[ i + 2 ];
				differ += d;
				if( d && x < xFrom - 3 * k - 1 )
					++early;
				if( d && x >= xFrom )
					++late;
			}
			if( differ && firstDisturbed < 0 )
				firstDisturbed = r;
			if( m == partial )
			{
				++partialRows;
				partialEarly += early;
				partialLate += late > 0;
			}
			else if( m >= full )
				quietInRegion += differ < W / 2;
			else
				wrongAbove += differ > 0;
		}
		const int predictedRow = [ & ] {
			for( int r = 0; r < H; ++r )
				if( ( lineOfRow( r, st.frameLines, H ) >> 1 ) >= ( partial >= 0 ? partial : full ) )
					return r;
			return -1;
		}();
		failures += report( wrongAbove == 0, quiet, "%s: no row above field line %.1f changes (%d do)", st.name, at, wrongAbove );
		failures += report( firstDisturbed == predictedRow, quiet, "%s: the first disturbed row is %d, predicted %d (frame line %d: %.1f lines before each field's V sync)", st.name,
		                    firstDisturbed, predictedRow, predictedRow >= 0 ? lineOfRow( predictedRow, st.frameLines, H ) : -1, kSwitchBefore );
		failures += report( quietInRegion == 0, quiet, "%s: every row from field line %d down is torn across most of its width (%d are not)", st.name, full, quietInRegion );
		if( partial >= 0 )
			failures += report( partialEarly == 0 && partialRows > 0 && partialLate == partialRows, quiet,
			                    "%s: field line %d is cut at %.1f px (%.2f us into the active line): %d row(s), untouched before it, torn after", st.name, partial, xFrom,
			                    xFrom * usPP, partialRows );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --pal
//---------------------------------------------------------------------------
int runPal( int W, int H, int perturb, bool quiet = false )
{
	const double phi = 20.0 * kPi / 180.0;
	if( !quiet )
		std::printf( "pal: a %.0f-degree playback phase error on every line, %dx%d\n", phi * 180.0 / kPi, W, H );
	int failures = 0;
	const double u0 = 0.10, v0 = 0.06;
	const double hue0 = std::atan2( v0, u0 ), sat0 = std::hypot( u0, v0 );
	for( int standard = 0; standard < 2; ++standard )
	{
		const Stated& st = kStated[ standard ];
		Session s;
		Knobs kn;
		kn.standard = standard;
		if( !open( s, W, H, kn, perturb ) )
			return 1;
		s.plugin.SetPhaseForTest( true, phi );
		const Picture p = yuvPicture( W, H, [ & ]( int, int, double& y, double& u, double& v ) {
            y = 0.5;
            u = u0;
            v = v0;
		} );
		if( !s.render( 0, p ) )
			return 1;
		const std::vector< float > all = s.readAll();
		double worstHue = 0.0, worstSat = 0.0;
		int rows        = 0;
		for( int r = 0; r < H; ++r )
		{
			//The first line of each field has no line 1H before it.
			if( lineOfRow( r, st.frameLines, H ) < 2 )
				continue;
			double su = 0.0, sv = 0.0;
			int n     = 0;
			for( int x = W / 4; x < 3 * W / 4; ++x )
			{
				double y, u, v;
				rgbToYuv( all.data() + ( static_cast< size_t >( r ) * W + x ) * 4, y, u, v );
				su += u;
				sv += v;
				++n;
			}
			su /= n;
			sv /= n;
			const double hue = std::atan2( sv, su ) - hue0 - ( standard == 1 ? phi : 0.0 );
			const double sat = std::hypot( su, sv ) / sat0 - ( standard == 0 ? std::cos( phi ) : 1.0 );
			worstHue         = std::max( worstHue, std::fabs( hue ) );
			worstSat         = std::max( worstSat, std::fabs( sat ) );
			++rows;
		}
		if( standard == 0 )
		{
			failures += report( worstHue <= 1e-4, quiet, "PAL: no hue shift on any of %d rows (worst %.2e rad, tol 1e-4)", rows, worstHue );
			failures += report( worstSat <= 1e-4, quiet, "PAL: saturation cos 20 = %.4f on every row (worst deviation %.2e, tol 1e-4)", std::cos( phi ), worstSat );
		}
		else
		{
			failures += report( worstHue <= 1e-4, quiet, "NTSC: the hue turns by the full 20 degrees on every one of %d rows (worst deviation %.2e rad)", rows, worstHue );
			failures += report( worstSat <= 1e-4, quiet, "NTSC: saturation kept (worst deviation %.2e)", worstSat );
		}
		s.end();
	}
	return failures;
}

//---------------------------------------------------------------------------
// --doc
//---------------------------------------------------------------------------
int runDoc( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "doc: a forced dropout, noise on, %dx%d\n", W, H );
	int failures = 0;
	const int k  = stepOf( W );
	for( int standard = 0; standard < 2; ++standard )
	{
		const Stated& st   = kStated[ standard ];
		const int N        = st.frameLines;
		const int line     = N / 2 + 1;
		const double us0   = 12.0, us1 = 30.0;
		const int Ws       = ( W + k - 1 ) / k;
		const double usPS  = st.Active() / W * k;
		const Picture p    = yuvPicture( W, H, [ & ]( int x, int r, double& y, double& u, double& v ) {
            y = 0.45 + 0.25 * std::sin( 0.37 * r + 0.05 * x );
            u = 0.08 * std::sin( 0.11 * r - 0.03 * x );
            v = 0.06 * std::cos( 0.23 * r + 0.02 * x );
		} );
		for( int docOn = 1; docOn >= 0; --docOn )
		{
			Session s;
			Knobs kn;
			kn.standard    = standard;
			kn.doc         = docOn != 0;
			kn.chromaNoise = 0.5;
			if( !open( s, W, H, kn, perturb, false ) )
				return 1;
			s.plugin.SetDropoutForTest( true, line, us0, us1 );
			if( !s.render( 3, p ) )
				return 1;
			const std::vector< float > lines = s.plugin.ReadLinesForTest();
			auto at = [ & ]( int l, int j, int c ) { return lines[ ( static_cast< size_t >( l ) * Ws + j ) * 4 + c ]; };
			int in = 0, same = 0;
			double white = 0.0;
			for( int j = 0; j < Ws; ++j )
			{
				if( !( static_cast< float >( j ) >= static_cast< float >( us0 / usPS ) && static_cast< float >( j ) < static_cast< float >( us1 / usPS ) ) )
					continue;
				++in;
				same += at( line, j, 0 ) == at( line - 2, j, 0 ) && at( line, j, 1 ) == at( line - 2, j, 1 ) && at( line, j, 2 ) == at( line - 2, j, 2 );
				white += at( line, j, 0 );
			}
			if( docOn )
			{
				failures += report( in > 0 && same == in, quiet, "%s DOC on: all %d samples of the dropout on line %d are line %d's, exactly (%d differ)", st.name, in, line, line - 2,
				                    in - same );
				if( H >= N )
				{
					//And through the display: rows of the two lines, where every
					//Catmull-Rom tap is inside the dropout.
					const std::vector< float > all = s.readAll();
					int ra = -1, rb = -1;
					for( int r = 0; r < H; ++r )
					{
						const int l = lineOfRow( r, N, H );
						if( l == line && ra < 0 )
							ra = r;
						if( l == line - 2 && rb < 0 )
							rb = r;
					}
					int checked = 0, equal = 0;
					for( int x = 0; x < W && ra >= 0 && rb >= 0; ++x )
					{
						const double sx = ( x - 0.5 * ( k - 1 ) ) / k;
						const int i0    = static_cast< int >( std::floor( sx ) );
						if( i0 - 1 < us0 / usPS + 1 || i0 + 2 >= us1 / usPS - 1 )
							continue;
						++checked;
						const float* pa = all.data() + ( static_cast< size_t >( ra ) * W + x ) * 4;
						const float* pb = all.data() + ( static_cast< size_t >( rb ) * W + x ) * 4;
						equal += pa[ 0 ] == pb[ 0 ] && pa[ 1 ] == pb[ 1 ] && pa[ 2 ] == pb[ 2 ];
					}
					failures += report( checked > 0 && equal == checked, quiet, "%s DOC on, on screen: row %d (line %d) is row %d (line %d) across the dropout, %d of %d pixels", st.name, ra,
					                    line, rb, line - 2, equal, checked );
				}
				else
					note( quiet, "%s: %d rows show every %.1fth line, so line %d and line %d are not both on screen; the line raster above is what they sample", st.name, H,
					      static_cast< double >( N ) / H, line, line - 2 );
			}
			else
				failures += report( in > 0 && same == 0 && white / in > 0.85, quiet, "%s DOC off: the dropout is a white streak (mean Y' %.3f over %d samples), not line %d", st.name,
				                    white / std::max( 1, in ), in, line - 2 );
			s.end();
		}
	}
	return failures;
}

//---------------------------------------------------------------------------
// --generation
//---------------------------------------------------------------------------
int runGeneration( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "generation: a copy of a copy, the chroma chain composed with the tape's filter again, %dx%d\n", W, H );
	int failures = 0;
	const int k  = stepOf( W );
	const double probes[] = { 0.15, 0.25, 0.35, 0.45, 0.6 };
	for( int standard = 0; standard < 2; ++standard )
	{
		const Stated& st  = kStated[ standard ];
		const double usPP = st.Active() / W;
		const int margin  = static_cast< int >( std::ceil( 6.0 * std::sqrt( 2.0 ) * sigmaUsFor( kChromaHalfMHz ) / usPP + 2 * 0.45 / usPP ) ) + 4 * k + 4;
		const double hi   = std::min( 1.5, 0.45 / usPP );
		double gains[ 2 ][ 5 ] = {};
		double halves[ 2 ]     = { 0.0, 0.0 };
		double lag             = 0.0;
		bool found             = true;
		for( int gens = 1; gens <= 2; ++gens )
		{
			Session s;
			Knobs kn;
			kn.standard   = standard;
			kn.generation = gens;
			if( !open( s, W, H, kn, perturb ) )
				return 1;
			long frame = 0;
			for( int i = 0; i < 5; ++i )
				gains[ gens - 1 ][ i ] = measureGain( s, st, probes[ i ], 1, margin, frame );
			found &= measuredHalf( s, st, 1, margin, 0.05, hi, frame, halves[ gens - 1 ] );
			if( gens == 2 )
			{
				double py = 0.0, pu = 0.0;
				measureGain( s, st, 0.12, 0, margin, frame, &py );
				measureGain( s, st, 0.12, 1, margin, frame, &pu );
				lag = std::remainder( py - pu, 2.0 * kPi ) / ( 2.0 * kPi * 0.12 * usPP );
			}
			s.end();
		}
		//The second generation multiplies the first's response by the tape's
		//own filter, the stated Gaussian half at 0.5 MHz.
		double worst = 0.0;
		for( int i = 0; i < 5; ++i )
			worst = std::max( worst, std::fabs( gains[ 1 ][ i ] / gains[ 0 ][ i ] - gaussianGain( probes[ i ], kChromaHalfMHz ) ) );
		failures += report( worst <= 5e-4, quiet, "%s: generation 2 over generation 1 is the tape's 0.5 MHz Gaussian at 0.15-0.6 MHz (worst %.1e, tol 5e-4)", st.name, worst );
		failures += report( found && ( k > 1 || std::fabs( halves[ 1 ] - kChromaHalfMHz / std::sqrt( 2.0 ) ) <= 2e-4 ), quiet,
		                    "%s: two generations' chroma half amplitude %.5f MHz = %.1f lines, one's %.5f%s", st.name, halves[ 1 ], tvLines( halves[ 1 ], st ), halves[ 0 ],
		                    k == 1 ? " (0.5 / sqrt 2 = 0.35355, tol 2e-4)" : " (at k > 1 the first generation's own Gaussian is narrower: not sqrt 2)" );
		failures += report( std::fabs( lag - 2.0 * 0.45 / usPP ) <= 1e-2, quiet, "%s: two generations lag the chroma %.4f px, twice one playback's %.4f", st.name, lag, 0.45 / usPP );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --tracking
//---------------------------------------------------------------------------
/// The stated geometry, by its own route: the crossover (the head centred
/// between two tracks of its azimuth) rests at the middle of the vertical
/// interval, and a tracking error of e pitches moves it up the field by
/// e / slope; RF falls off from it at `slope` a field.
struct Bar
{
	const Stated& st;
	double a0, uRest;
	explicit Bar( const Stated& s ) : st( s )
	{
		a0    = st.fieldLines - st.FieldActive() - st.preEq;
		uRest = std::fmod( ( st.fieldLines - st.preEq + 0.5 * ( st.fieldLines - st.FieldActive() ) ) / st.fieldLines, 1.0 );
	}
	double weight( int m, double e ) const
	{
		const double u  = ( m + 0.5 + a0 ) / st.fieldLines;
		double uc       = uRest - e / kSlope;
		double dist     = u - uc;
		dist -= std::round( dist );
		return std::clamp( ( kThreshold - kSlope * std::fabs( dist ) ) / kThreshold, 0.0, 1.0 );
	}
	/// After the 1H comb, the chroma lost on field line m.
	double kill( int m, double e ) const
	{
		return m < 1 ? weight( m, e ) : 0.5 * ( weight( m, e ) + weight( m - 1, e ) );
	}
};

/// The chroma lost per field line of field 0, measured from the line raster.
std::vector< double > measuredKill( Colourunder& plugin, const Stated& st, int Ws, double u0, double v0 )
{
	const std::vector< float > lines = plugin.ReadLinesForTest();
	std::vector< double > kill( static_cast< size_t >( st.FieldActive() ) );
	for( int m = 0; m < st.FieldActive(); ++m )
	{
		double su = 0.0, sv = 0.0;
		int n     = 0;
		for( int j = Ws / 4; j < 3 * Ws / 4; ++j )
		{
			su += lines[ ( static_cast< size_t >( 2 * m ) * Ws + j ) * 4 + 1 ];
			sv += lines[ ( static_cast< size_t >( 2 * m ) * Ws + j ) * 4 + 2 ];
			++n;
		}
		kill[ static_cast< size_t >( m ) ] = 1.0 - std::hypot( su / n, sv / n ) / std::hypot( u0, v0 );
	}
	return kill;
}

int runTracking( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "tracking: the noise bar against the stated geometry, %dx%d\n", W, H );
	int failures = 0;
	const int k  = stepOf( W );
	const int Ws = ( W + k - 1 ) / k;
	const double u0 = 0.08, v0 = 0.05;
	const Picture flatColour = yuvPicture( W, H, [ & ]( int, int, double& y, double& u, double& v ) {
        y = 0.5;
        u = u0;
        v = v0;
	} );
	for( int standard = 0; standard < 2; ++standard )
	{
		const Stated& st = kStated[ standard ];
		const Bar bar( st );
		auto profileError = [ & ]( const std::vector< double >& kill, double e, int& visible ) {
			double worst = 0.0;
			visible      = 0;
			for( int m = 1; m < st.FieldActive(); ++m )
			{
				const double want = bar.kill( m, e );
				worst             = std::max( worst, std::fabs( kill[ static_cast< size_t >( m ) ] - want ) );
				visible += kill[ static_cast< size_t >( m ) ] > 1e-4;
			}
			return worst;
		};
		//1. Forced errors: the whole profile, line by line.
		{
			Session s;
			Knobs kn;
			kn.standard = standard;
			if( !open( s, W, H, kn, perturb ) )
				return 1;
			double worst = 0.0;
			int last = -1, monotone = 1, full = 0;
			std::string widths;
			long frame = 0;
			for( const double e : { 0.0, 0.03, 0.06, 0.09, 0.12, 0.15, 0.3, 0.6, 1.0 } )
			{
				s.plugin.SetTrackingForTest( true, e );
				if( !s.render( frame++, flatColour ) )
					return 1;
				int visible     = 0;
				worst           = std::max( worst, profileError( measuredKill( s.plugin, st, Ws, u0, v0 ), e, visible ) );
				if( e <= 0.15 )
				{
					monotone &= visible >= last;
					last = visible;
				}
				full = std::max( full, visible );
				widths += " " + std::to_string( visible );
			}
			failures += report( worst <= 1e-4, quiet, "%s: the chroma lost on every field line is the stated geometry's, for 9 errors from 0 to 1 pitch (worst %.2e, tol 1e-4)", st.name, worst );
			failures += report( monotone && full > 0, quiet, "%s: the bar is hidden in the vertical interval at no error and widens as it enters (lines:%s)", st.name, widths.c_str() );
			s.end();
		}
		//2. The drift: the bar walks, and is where the reported error says.
		{
			Session s;
			Knobs kn;
			kn.standard = standard;
			kn.tracking = 1.0;
			if( !open( s, W, H, kn, perturb ) )
				return 1;
			double worst = 0.0, lo = 1e9, hi = -1e9;
			for( long f = 0; f <= 20 * 60; f += 24 )
			{
				if( !s.render( f, flatColour ) )
					return 1;
				const double e = s.plugin.LastTrackingForTest();
				int visible    = 0;
				worst          = std::max( worst, profileError( measuredKill( s.plugin, st, Ws, u0, v0 ), e, visible ) );
				lo             = std::min( lo, e );
				hi             = std::max( hi, e );
			}
			failures += report( worst <= 1e-4, quiet, "%s: over 20 s at Tracking 1 the bar is where the drifting error puts it on all 51 frames sampled (worst %.2e)", st.name, worst );
			failures += report( hi - lo >= 0.3, quiet, "%s: the error drifted over %.3f to %.3f pitches, walking the bar %.0f lines", st.name, lo, hi, ( hi - lo ) / kSlope * st.fieldLines );
			s.end();
		}
	}
	//3. Seeded and frame-relative: the same second is the same error at 60
	//and at 144 frames a second (1.5 s: 60 x 1.5 and 144 x 1.5 are whole frames).
	{
		double e[ 2 ] = { 0.0, 0.0 };
		const double fps[ 2 ] = { 60.0, 144.0 };
		for( int i = 0; i < 2; ++i )
		{
			Session s;
			Knobs kn;
			kn.tracking = 1.0;
			s.fps       = fps[ i ];
			if( !open( s, W, H, kn, perturb ) )
				return 1;
			for( long f = 0; f <= static_cast< long >( 1.5 * fps[ i ] ); ++f )
				if( !s.render( f, flatColour ) )
					return 1;
			e[ i ] = s.plugin.LastTrackingForTest();
			s.end();
		}
		failures += report( std::fabs( e[ 0 ] - e[ 1 ] ) <= 1e-12, quiet, "1.5 s in, the error is %.12f at 60 fps and %.12f at 144 fps", e[ 0 ], e[ 1 ] );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --resize
//---------------------------------------------------------------------------
int runResize( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "resize: every moving thing across a resize to 1.5x and back, %dx%d\n", W, H );
	int failures = 0;
	auto run     = [ & ]( bool resize, std::vector< std::vector< float > >& out ) -> bool {
        Session s;
        Knobs kn;
        kn.tracking    = 1.0;
        kn.wear        = 1.0;
        kn.headSwitch  = 0.5;
        kn.generation  = 2;
        kn.chromaNoise = 1.0;
        if( !open( s, W, H, kn, perturb, false ) )
            return false;
        out.clear();
        for( long m = 0; m < 60; ++m )
        {
            if( resize && m == 20 )
                s.resize( W * 3 / 2, H * 3 / 2 );
            if( resize && m == 28 )
                s.resize( W, H );
            const Picture p = yuvPicture( s.width, s.height, [ & ]( int x, int r, double& y, double& u, double& v ) {
                y = 0.5 + 0.2 * std::sin( 0.07 * x + 0.05 * r );
                u = 0.06;
                v = -0.04;
            } );
            if( !s.render( m, p ) )
                return false;
            if( m >= 40 )
                out.push_back( s.readAll() );
        }
        s.end();
        return true;
	};
	std::vector< std::vector< float > > a, b;
	if( !run( false, a ) || !run( true, b ) )
		return 1;
	//Identical float operations on identical numbers: equal on a
	//deterministic GPU. The software renderer is not bit-repeatable; four
	//ULP of the value is its allowance (gate's rule).
	size_t differ = 0, moved = 0;
	for( size_t f = 0; f < a.size(); ++f )
		for( size_t i = 0; i < a[ f ].size(); ++i )
		{
			if( std::fabs( a[ f ][ i ] - b[ f ][ i ] ) > 4.0 * 2.0 * kU * std::fabs( a[ f ][ i ] ) + 1e-30 )
				++differ;
			moved += a[ f ][ i ] != a[ 0 ][ i ];
		}
	failures += report( differ == 0, quiet, "frames 40-59 after a resize to %dx%d and back: every subpixel is the unresized run's (%zu differ)", W * 3 / 2, H * 3 / 2, differ );
	failures += report( moved > 0, quiet, "and those frames move (%zu subpixels change over them), so there was state to keep", moved );
	return failures;
}

//---------------------------------------------------------------------------
// --negative
//---------------------------------------------------------------------------
int runNegative( int W, int H )
{
	std::printf( "negative controls: each perturbation of the plugin's model must FAIL its check, %dx%d\n", W, H );
	struct Control
	{
		int bits;
		const char* what;
		int ( *check )( int, int, int, bool );
		const char* name;
	};
	const Control list[] = {
		{ model::kPerturbChromaAsLuma, "chroma given luma's bandwidth", runChroma, "--chroma" },
		{ model::kPerturbNoDelay, "the chroma's group delay left out", runDelay, "--delay" },
		{ model::kPerturbSwitchFromActive, "switch 6.5 lines from active's end", runSwitch, "--switch" },
		{ model::kPerturbNoPalAverage, "PAL's line average skipped", runPal, "--pal" },
		{ model::kPerturbDocAdjacent, "DOC repeats the other field's line", runDoc, "--doc" },
		{ model::kPerturbGenerationOnce, "chroma band-limited once, not per copy", runGeneration, "--generation" },
		{ model::kPerturbTrackingFrozen, "the bar ignores the tracking error", runTracking, "--tracking" },
		{ model::kPerturbResizeResetsClock, "a resize restarts the clock", runResize, "--resize" },
	};
	int failures = 0;
	for( const Control& c : list )
	{
		const int before = g_failures;
		const int checks = g_checks;
		const int failed = c.check( W, H, c.bits, true );
		g_failures       = before;
		g_checks         = checks;
		failures += report( failed > 0, false, "%-40s -> %s fails (%d of its checks)", c.what, c.name, failed );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --names and --model: no GL.
//---------------------------------------------------------------------------
int runNames()
{
	std::printf( "names: nothing the host will silently truncate; every name unique\n" );
	Colourunder plugin;
	int failures = 0;
	std::set< std::string > seen;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		if( p.index >= Colourunder::PT_ABOUT_FIRST )
			continue;
		failures += report( p.name.size() <= 16, false, "%-16s %2zu characters", p.name.c_str(), p.name.size() );
		failures += report( seen.insert( p.name ).second, false, "%-16s unique", p.name.c_str() );
	}
	failures += report( std::string( "SW Colourunder" ).size() <= 16, false, "display name 'SW Colourunder' is %zu characters", std::string( "SW Colourunder" ).size() );
	return failures;
}

int runModel()
{
	std::printf( "model: the plugin's numbers against the stated ones\n" );
	int failures = 0;
	for( int i = 0; i < 2; ++i )
	{
		const Stated& st           = kStated[ i ];
		const model::Standard& m   = model::StandardOf( i );
		failures += report( std::fabs( m.Active() - st.Active() ) < 1e-12 && m.frameLines == st.frameLines && m.fieldLines == st.fieldLines && m.preEqualising == st.preEq, false,
		                    "%s: active %.4f us, %d lines, %.1f a field, %.1f lines of pre-equalising", st.name, st.Active(), st.frameLines, st.fieldLines, st.preEq );
		const double carrier = st.carrierLines * st.fH;
		failures += report( std::fabs( m.colourUnderHz - carrier ) < 1e-6, false, "%s colour-under carrier %.3f kHz = %.3f fH", st.name, carrier / 1000.0, st.carrierLines );
		failures += report( model::kChromaHalfHz < carrier, false, "%s: the 0.5 MHz band sits under the %.0f kHz carrier (its lower sideband stays above 0 Hz)", st.name, carrier / 1000.0 );
		const double lines = tvLines( kChromaHalfMHz, st );
		failures += report( lines > 35.0 && lines < 45.0, false, "%s: 0.5 MHz is %.1f lines of chroma resolution (\"about 40\")", st.name, lines );
		failures += report( std::fabs( model::SwitchLine( m, 0 ) - ( st.FieldActive() - ( kSwitchBefore - st.preEq ) ) ) < 1e-12, false, "%s: the switch at field line %.1f", st.name,
		                    model::SwitchLine( m, 0 ) );
	}
	for( int sp = 0; sp < 3; ++sp )
		failures += report( std::fabs( model::LumaHalfHz( sp ) / 1e6 - kLumaHalfMHz[ sp ] ) < 1e-12, false, "luma %s: %.2f MHz = %.0f lines (PAL)", controls::SpeedName( sp ),
		                    kLumaHalfMHz[ sp ], tvLines( kLumaHalfMHz[ sp ], kStated[ 0 ] ) );
	failures += report( std::fabs( model::SigmaUs( 0.5e6 ) - sigmaUsFor( 0.5 ) ) < 1e-12, false, "the 0.5 MHz Gaussian's sigma is %.4f us", sigmaUsFor( 0.5 ) );
	return failures;
}

//---------------------------------------------------------------------------
// The moving card, for --out, the sweep and the bench: a row of colour
// patches, a grey ramp, a white disc on an orbit, a static black square, a
// drifting blue bar, and a flashing patch (on for 6 frames in 30).
//---------------------------------------------------------------------------
std::vector< unsigned char > buildCard( int width, int height, int64_t frame )
{
	std::vector< unsigned char > img( static_cast< size_t >( width ) * height * 4 );
	const double t  = static_cast< double >( frame ) / 60.0;
	const double cx = 0.72 + 0.14 * std::cos( 1.2 * t ), cy = 0.68 + 0.16 * std::sin( 1.2 * t );
	const double barX = std::fmod( 0.05 * t, 1.0 );
	const bool flash  = frame % 30 < 6;
	const double patches[ 8 ][ 3 ] = {
		{ 0.80, 0.10, 0.10 }, { 0.88, 0.67, 0.55 }, { 0.90, 0.85, 0.15 }, { 0.15, 0.60, 0.20 },
		{ 0.20, 0.80, 0.85 }, { 0.15, 0.25, 0.85 }, { 0.53, 0.81, 0.92 }, { 0.95, 0.95, 0.95 },
	};
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double fx = ( x + 0.5 ) / width, fy = ( y + 0.5 ) / height;
			double r = 0.40, g = 0.40, b = 0.40;
			if( fy > 0.06 && fy < 0.30 )
			{
				const int i = std::clamp( static_cast< int >( ( fx - 0.04 ) / 0.115 ), 0, 7 );
				if( fx > 0.04 && fx < 0.96 && std::fmod( fx - 0.04, 0.115 ) < 0.105 )
				{
					r = patches[ i ][ 0 ];
					g = patches[ i ][ 1 ];
					b = patches[ i ][ 2 ];
				}
			}
			if( fy > 0.36 && fy < 0.46 && fx > 0.04 && fx < 0.96 )
				r = g = b = ( fx - 0.04 ) / 0.92;
			if( fx > 0.08 && fx < 0.28 && fy > 0.56 && fy < 0.92 )
				r = g = b = flash ? 0.98 : 0.02;
			const double dx = ( fx - cx ) * width, dy = ( fy - cy ) * height;
			if( dx * dx + dy * dy < ( height * 0.08 ) * ( height * 0.08 ) )
				r = g = b = 0.98;
			if( std::fabs( fx - barX ) < 0.02 && fy > 0.5 )
			{
				r = 0.2;
				g = 0.3;
				b = 0.9;
			}
			unsigned char* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			px[ 0 ]           = static_cast< unsigned char >( std::lround( 255.0 * r ) );
			px[ 1 ]           = static_cast< unsigned char >( std::lround( 255.0 * g ) );
			px[ 2 ]           = static_cast< unsigned char >( std::lround( 255.0 * b ) );
			px[ 3 ]           = 255;
		}
	return img;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( Colourunder& plugin, int width, int height, int frames, double fps, size_t& stateBytes )
{
	Session session;
	session.floatOutput = false;
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.index < Colourunder::PT_ABOUT_FIRST && p.type != FF_TYPE_BUFFER && p.type != FF_TYPE_EVENT && p.type != FF_TYPE_TEXT )
			session.plugin.SetFloatParameter( p.index, p.value );
	session.fps = fps;
	if( !session.begin( width, height ) )
		return -1.0;

	//The card is uploaded once: a host's frame is already on the GPU, and
	//the plugin's cost does not depend on what the frame holds.
	const std::vector< unsigned char > card = buildCard( width, height, 0 );
	const int warmup                        = 20;
	for( int frame = 0; frame < warmup; ++frame )
		session.render( frame, card );
	glFinish();

	double best = 1e9;
	for( int run = 0; run < 3; ++run )
	{
		const auto start = std::chrono::steady_clock::now();
		for( int frame = 0; frame < frames; ++frame )
			session.renderAt( warmup + run * frames + frame );
		glFinish();
		const auto end       = std::chrono::steady_clock::now();
		const double seconds = std::chrono::duration< double >( end - start ).count();
		best                 = std::min( best, seconds * 1000.0 / static_cast< double >( frames ) );
	}
	stateBytes = session.plugin.StateBytesForTest();
	session.end();
	return best;
}

int runBench( Colourunder& plugin, int frames, double fps )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720  ", 1280, 720 },
		{ "1920x1080 ", 1920, 1080 },
		{ "3840x2160 ", 3840, 2160 },
	};
	std::printf( "%d frames each, best of three runs, after a 20-frame warm-up, glFinish both sides, the card uploaded once.\n\n", frames );
	std::printf( "resolution     ms/frame   equivalent fps   %% of a 60fps frame   state held\n" );
	for( const Size& size : sizes )
	{
		size_t bytes    = 0;
		const double ms = benchAt( plugin, size.width, size.height, frames, fps, bytes );
		std::printf( "%s    %7.3f       %8.0f            %5.1f%%          %6.2f MB\n", size.name, ms, ms > 0.0 ? 1000.0 / ms : 0.0,
		             ms / 16.667 * 100.0, static_cast< double >( bytes ) / 1048576.0 );
	}
	std::printf( "\nState is the line raster: one RGBA32F buffer N lines by the host's width and\n"
	             "five N by at most 1024 samples, plus the per-line data texture. Nothing on\n"
	             "the GPU is carried from one frame to the next. Whatever the settings above\n"
	             "were, they are what was measured; --set measures another.\n" );
	return 0;
}


//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"cutest -- render and measure the Colourunder deck\n"
		"\n"
		"  --out PATH          render the moving card through the plugin (default /tmp/colourunder.png)\n"
		"  --average           write the mean of every frame rendered, not the last\n"
		"  --size WxH          raster (default 1280x720)\n"
		"  --frames N          frames to render before reading back (default 90)\n"
		"  --fps N             synthetic display rate driving the clock (default 60)\n"
		"  --source card|flat|white|black   what to feed (card moves); --level V for flat\n"
		"  --set \"Name=V\"      set a parameter by its display name (element index for options).\n"
		"                      Repeatable.\n"
		"  --list              every parameter, its kind, default and range\n"
		"\n"
		"  checks that render, at --size:\n"
		"  --chroma            chroma's half amplitude is the colour-under band's; luma the Speed's\n"
		"  --delay             chroma lags luma by the stated group delay, whole-pixel and fractional\n"
		"  --switch            the head switch disturbs the lines from 6.5 H before V sync, no others\n"
		"  --pal               a phase error: desaturation on PAL, hue shift on NTSC\n"
		"  --doc               a forced dropout with DOC on is the line 1H before it, exactly\n"
		"  --generation        two generations: the composed chroma filter\n"
		"  --tracking          the bar where the error puts it, widening, walking, any frame rate\n"
		"  --resize            the state survives a resize\n"
		"  --negative          every check above can fail\n"
		"  --perturb BITS      run the checks verbosely against a perturbed model (bits in Model.h)\n"
		"\n"
		"  checks that need no GL:\n"
		"  --names             nothing the host will silently truncate\n"
		"  --model             the plugin's numbers against the stated ones\n"
		"\n"
		"  --bench             time ProcessOpenGL at 720p, 1080p and 4K, and the state held\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       parameter cues for --pipe: 'frame Name Value'\n"
		"  --allow-no-gl       with checks: report SKIP rather than FAIL when no GL context can be made\n"
		"\n"
		"  CUTEST_RENDERER=software   use Apple's software renderer (a GPU-less CI runner's)\n" );
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	namespace shaders = colourunder::shaders;
	const std::pair< const char*, std::string > files[] = {
		{ "vertex.vert", shaders::Vertex() },   { "intakev.frag", shaders::IntakeV() }, { "intakeh.frag", shaders::IntakeH() },
		{ "noise.frag", shaders::Noise() },     { "tape.frag", shaders::Tape() },       { "comb.frag", shaders::Comb() },
		{ "doc.frag", shaders::Doc() },         { "display.frag", shaders::Display() },
	};
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line, the fleet's format.
//
// A STANDARD parameter ramps linearly between cues. An option, a boolean
// and an integer STEP: they hold the last cue at or before the frame,
// because there is nothing between Xenon and Tungsten to ramp through. An
// event fires on its cue frame only.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame, unsigned int type )
{
	if( track.empty() )
		return 0.0f;
	if( type == FF_TYPE_EVENT )
	{
		for( const auto& cue : track )
			if( cue.first == frame )
				return cue.second;
		return 0.0f;
	}
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a = track[ i - 1 ];
			const auto& b = track[ i ];
			if( type != FF_TYPE_STANDARD )
				return frame == b.first ? b.second : a.second;
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? static_cast< float >( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}


} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/colourunder.png";
	std::string scriptPath;
	std::string dumpDir;
	std::string source = "card";
	double level   = 0.5;
	int width      = 1280;
	int height     = 720;
	int frames     = 90;
	int failRender = -1;
	int perturb    = 0;
	double fps     = 60.0;
	bool wantList  = false;
	bool wantBench = false;
	bool wantPipe  = false;
	bool average   = false;
	bool allowNoGL = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	const std::set< std::string > rendered = { "--chroma", "--delay", "--switch", "--pal", "--doc", "--generation", "--tracking", "--resize", "--negative" };
	const std::set< std::string > offline  = { "--names", "--model" };

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--source" && hasNext )
			source = argv[ ++i ];
		else if( argument == "--level" && hasNext )
			level = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );
		else if( argument == "--fail-render-at" && hasNext )
			failRender = std::atoi( argv[ ++i ] );//test hook: verify.sh proves --pipe exits 1 on a failed render
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--average" )
			average = true;
		else if( argument == "--allow-no-gl" )
			allowNoGL = true;
		else if( rendered.count( argument ) || offline.count( argument ) )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	if( wantList )
	{
		//No GL needed: answered before a context is made, so it works in CI.
		Colourunder plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low, p.high );
		return 0;
	}

	if( !checks.empty() )
	{
		bool needGL = false;
		for( const std::string& check : checks )
		{
			if( check == "--names" || check == "--model" )
			{
				if( check == "--names" )
					runNames();
				else
					runModel();
				std::printf( "\n" );
			}
			else
				needGL = true;
		}

		if( needGL )
		{
			CGLContextObj context = createContext();
			if( context == nullptr && allowNoGL )
				std::printf( "   SKIP  could not create an OpenGL 4.1 core context, accelerated or software.\n"
				             "         The rendering checks and their negative controls were NOT run.\n" );
			else if( context == nullptr )
			{
				std::printf( "   FAIL  could not create an OpenGL 4.1 core context\n" );
				++g_failures;
			}
			else
			{
				for( const std::string& check : checks )
				{
					if( check == "--chroma" )
						runChroma( width, height, perturb );
					else if( check == "--delay" )
						runDelay( width, height, perturb );
					else if( check == "--switch" )
						runSwitch( width, height, perturb );
					else if( check == "--pal" )
						runPal( width, height, perturb );
					else if( check == "--doc" )
						runDoc( width, height, perturb );
					else if( check == "--generation" )
						runGeneration( width, height, perturb );
					else if( check == "--tracking" )
						runTracking( width, height, perturb );
					else if( check == "--resize" )
						runResize( width, height, perturb );
					else if( check == "--negative" )
						runNegative( width, height );
					else
						continue;
					std::printf( "\n" );
				}
				CGLSetCurrentContext( nullptr );
				CGLDestroyContext( context );
			}
		}
		std::printf( "%d checks, %d failed\n", g_checks, g_failures );
		return g_failures == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}
	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	Session session;
	session.floatOutput = false;
	session.fps         = fps;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}
	session.plugin.SetPerturbForTest( perturb );

	if( wantBench )
		return finish( runBench( session.plugin, frames < 40 ? 60 : frames, fps ) );

	if( wantPipe )
	{
		//Everything but the video goes to stderr: one stray byte in stdout is
		//a torn frame for the rest of the reel.
		struct Automation
		{
			unsigned int index;
			unsigned int type;
			Track track;
		};
		std::vector< Automation > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				automation.push_back( { static_cast< unsigned int >( index ), session.plugin.GetParamType( static_cast< unsigned int >( index ) ), entry.second } );
			}
		}

		//A closed stdout must be a failed write we can see, not a SIGPIPE
		//that kills the process with 141 before it can say so.
		std::signal( SIGPIPE, SIG_IGN );

		if( !session.begin( width, height ) )
			return finish( 1 );

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		int status = 0;
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			//A partial frame is the end of the stream, never a frame.
			if( got < frame.size() )
			{
				if( got > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes, %dx%d): dropped\n", got, frame.size(), width, height );
				break;
			}

			//Through the plugin's own setter, so a cue moves what a slider
			//would, and an event is a press.
			for( const Automation& a : automation )
				session.plugin.SetFloatParameter( a.index, valueAt( a.track, index, a.type ) );

			const bool ok = index != failRender && session.render( index, frame );
			if( !ok )
			{
				std::fprintf( stderr, "render failed at frame %d\n", index );
				status = 1;
				break;
			}

			const std::vector< unsigned char > out = session.readBack();
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			//The reader has gone: rendering on into a closed pipe is work
			//nobody will see, and a short frame is worse than none.
			if( written < out.size() )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				status = 1;
				break;
			}
		}
		session.end();
		return finish( status );
	}

	if( !session.begin( width, height ) )
		return finish( 1 );
	std::vector< double > sum;
	for( int frame = 0; frame < frames; ++frame )
	{
		bool ok = false;
		if( source == "card" )
			ok = session.render( frame, buildCard( width, height, frame ) );
		else if( source == "flat" )
			ok = session.render( frame, flat( width, height, level ) );
		else if( source == "white" )
			ok = session.render( frame, flat( width, height, 1.0 ) );
		else if( source == "black" )
			ok = session.render( frame, flat( width, height, 0.0 ) );
		else
		{
			std::fprintf( stderr, "unknown --source %s\n", source.c_str() );
			return finish( 2 );
		}
		if( !ok )
			return finish( 1 );
		if( average )
		{
			const std::vector< unsigned char > image = session.readBack();
			sum.resize( image.size(), 0.0 );
			for( size_t i = 0; i < image.size(); ++i )
				sum[ i ] += image[ i ];
		}
	}

	std::vector< unsigned char > image = session.readBack();
	if( average )
		for( size_t i = 0; i < image.size(); ++i )
			image[ i ] = static_cast< unsigned char >( std::lround( sum[ i ] / frames ) );
	session.end();
	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}
	std::printf( "wrote %s (%dx%d, %d frames%s)\n", outPath.c_str(), width, height, frames, average ? ", averaged" : "" );
	return finish( 0 );
}

// The reference side of demo/tools/check_port.sh. NOT a copy of the plugin:
// check_port.mjs pastes three pieces of source/Colourunder.{h,cpp} into the
// @@ markers below at run time, unedited -- the ParamID enum, the anonymous
// namespace (loc, bindTarget, bindTextures, unbindTextures, setKernel,
// frameSeed) and the whole of Colourunder::Colourunder() and
// Colourunder::ProcessOpenGL -- and compiles them against the plugin's own
// Model.cpp, Controls.cpp and Clock.cpp. What this file supplies is only the
// scaffolding those pieces need to compile without a GL context or the FFGL
// SDK: GL entry points and ffglex classes that RECORD what the plugin does
// (every uniform, by name, every texture bound per unit, every framebuffer
// drawn into, the LineData upload) instead of doing it.
//
// Input on stdin, one scenario per line:
//   W H p0 .. p9 n t0 .. t(n-1)
// (the ten parameter floats in ParamID order, then n host times). Output: the
// declarations the constructor made, then per frame the record check_port.mjs
// compares with demo/model.js's planFrame().

#include "Model.h"
#include "Controls.h"
#include "Clock.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <map>
#include <string>
#include <vector>

//---------------------------------------------------------------------------
// GL, recorded.
//---------------------------------------------------------------------------
using GLuint  = unsigned int;
using GLint   = int;
using GLenum  = unsigned int;
using GLsizei = int;
using GLfloat = float;
using GLubyte = unsigned char;
using FFUInt32 = unsigned int;
using FFResult = unsigned int;

constexpr GLenum GL_VIEWPORT = 0x0BA2, GL_TEXTURE_2D = 0x0DE1, GL_RGBA32F = 0x8814, GL_RGBA = 0x1908,
                 GL_FLOAT = 0x1406, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800,
                 GL_NEAREST = 0x2600, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803,
                 GL_CLAMP_TO_EDGE = 0x812F, GL_FRAMEBUFFER = 0x8D40, GL_TEXTURE0 = 0x84C0,
                 GL_VENDOR = 0x1F00, GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02, GL_FRAMEBUFFER_BINDING = 0x8CA6,
                 GL_PACK_ALIGNMENT = 0x0D05;
constexpr FFResult FF_SUCCESS = 0, FF_FAIL = 1;
constexpr FFUInt32 FF_TYPE_BOOLEAN = 0, FF_TYPE_EVENT = 1, FF_TYPE_STANDARD = 10, FF_TYPE_OPTION = 11,
                   FF_TYPE_INTEGER = 13, FF_TYPE_TEXT = 100;

namespace rec
{
std::map< GLuint, std::string > names;///< texture and framebuffer ids -> buffer names
std::vector< std::string > uniformNames;
GLuint program = 0;
std::map< GLuint, std::string > programNames;
int activeUnit = 0;
GLuint bound[ 8 ] = {};
GLuint framebuffer = 0;
GLuint nextTexture = 500;
std::vector< std::string > lines;///< the current pass's uniform lines
std::string out;

std::string bits( float v )
{
	uint32_t u;
	std::memcpy( &u, &v, 4 );
	char b[ 16 ];
	std::snprintf( b, sizeof b, "%08x", u );
	return b;
}
std::string nameOf( GLuint id )
{
	auto it = names.find( id );
	return it == names.end() ? ( id == 0 ? std::string( "none" ) : "tex" + std::to_string( id ) ) : it->second;
}
} // namespace rec

GLint glGetUniformLocation( GLuint program, const char* name )
{
	(void)program;
	rec::uniformNames.push_back( name );
	return static_cast< GLint >( rec::uniformNames.size() - 1 );
}
static const std::string& uname( GLint location )
{
	return rec::uniformNames[ static_cast< size_t >( location ) ];
}
void glUniform1i( GLint l, GLint v )
{
	rec::lines.push_back( "I " + uname( l ) + " " + std::to_string( v ) );
}
void glUniform1ui( GLint l, GLuint v )
{
	rec::lines.push_back( "U " + uname( l ) + " " + std::to_string( v ) );
}
void glUniform1f( GLint l, GLfloat v )
{
	rec::lines.push_back( "F " + uname( l ) + " " + rec::bits( v ) );
}
void glUniform1fv( GLint l, GLsizei n, const GLfloat* v )
{
	std::string s = "A " + uname( l ) + " " + std::to_string( n );
	for( int i = 0; i < n; ++i )
		s += " " + rec::bits( v[ i ] );
	rec::lines.push_back( s );
}
void glUniform4fv( GLint l, GLsizei n, const GLfloat* v )
{
	std::string s = "V " + uname( l ) + " " + std::to_string( n );
	for( int i = 0; i < 4 * n; ++i )
		s += " " + rec::bits( v[ i ] );
	rec::lines.push_back( s );
}
void glGetIntegerv( GLenum, GLint* v )
{
	v[ 0 ] = v[ 1 ] = v[ 2 ] = v[ 3 ] = 0;
}
void glGenTextures( GLsizei n, GLuint* ids )
{
	for( int i = 0; i < n; ++i )
	{
		ids[ i ] = rec::nextTexture++;
		rec::names[ ids[ i ] ] = "lineData";
	}
}
void glDeleteTextures( GLsizei, const GLuint* ) {}
void glBindTexture( GLenum, GLuint t )
{
	rec::bound[ rec::activeUnit ] = t;
}
void glActiveTexture( GLenum unit )
{
	rec::activeUnit = static_cast< int >( unit - GL_TEXTURE0 );
}
void glTexImage2D( GLenum, GLint, GLint, GLsizei w, GLsizei h, GLint, GLenum, GLenum, const void* )
{
	rec::out += "ALLOC lineData " + std::to_string( w ) + " " + std::to_string( h ) + "\n";
}
void glTexParameteri( GLenum, GLenum, GLint ) {}
void glTexSubImage2D( GLenum, GLint, GLint, GLint, GLsizei w, GLsizei h, GLenum, GLenum, const void* data )
{
	const float* f = static_cast< const float* >( data );
	std::string s = "LINEDATA " + rec::nameOf( rec::bound[ rec::activeUnit ] ) + " " + std::to_string( w ) + " " + std::to_string( h );
	for( int i = 0; i < w * h * 4; ++i )
		s += " " + rec::bits( f[ i ] );
	rec::out += s + "\n";
}
void glBindFramebuffer( GLenum, GLuint fbo )
{
	rec::framebuffer = fbo;
}
void glViewport( GLint, GLint, GLsizei, GLsizei ) {}
void glPixelStorei( GLenum, GLint ) {}
void glReadPixels( GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void* ) {}
const GLubyte* glGetString( GLenum )
{
	return reinterpret_cast< const GLubyte* >( "refport" );
}

//---------------------------------------------------------------------------
// ffglex and the plugin's own classes, as recorders.
//---------------------------------------------------------------------------
namespace ffglex
{
struct FFGLShader
{
	GLuint id;
	FFGLShader( GLuint i, const char* name ) : id( i )
	{
		rec::programNames[ i ] = name;
	}
	GLuint GetGLID() const
	{
		return id;
	}
};
struct ScopedShaderBinding
{
	explicit ScopedShaderBinding( GLuint p )
	{
		rec::program = p;
	}
	~ScopedShaderBinding()
	{
		rec::program = 0;
	}
};
struct FFGLScreenQuad
{
	void Draw()
	{
		std::string s = "PASS " + rec::programNames[ rec::program ] + " -> " + rec::nameOf( rec::framebuffer ) + " textures";
		int last = -1;
		for( int u = 0; u < 8; ++u )
			if( rec::bound[ u ] != 0 )
				last = u;
		for( int u = 0; u <= last; ++u )
			s += " " + rec::nameOf( rec::bound[ u ] );
		rec::out += s + "\n";
		std::sort( rec::lines.begin(), rec::lines.end() );
		for( const auto& l : rec::lines )
			rec::out += "  " + l + "\n";
		rec::lines.clear();
	}
};
} // namespace ffglex

namespace colourunder
{
class PassBuffer
{
public:
	enum class Sampling
	{
		Nearest,
		Linear
	};
	PassBuffer( GLuint id, const char* name ) : tex( id ), fbo( id + 1000 )
	{
		rec::names[ tex ] = name;
		rec::names[ fbo ] = name;
	}
	bool Ensure( int w, int h, GLenum, Sampling )
	{
		width  = w;
		height = h;
		return true;
	}
	GLuint TextureID() const
	{
		return tex;
	}
	GLuint GetGLID() const
	{
		return fbo;
	}
	void ResizeViewPort() {}
	int GetWidth() const
	{
		return width;
	}
	int GetHeight() const
	{
		return height;
	}
	bool IsValid() const
	{
		return true;
	}

private:
	GLuint tex, fbo;
	int width = 0, height = 0;
};

namespace diag
{
inline void error( const std::string& ) {}
inline void info( const std::string& ) {}
inline void init() {}
} // namespace diag
} // namespace colourunder

struct FFGLTextureStruct
{
	FFUInt32 Width, Height, HardwareWidth, HardwareHeight;
	GLuint Handle;
};
struct ProcessOpenGLStruct
{
	FFUInt32 numInputTextures;
	FFGLTextureStruct** inputTextures;
	GLuint HostFBO;
};
struct FFGLLog
{
	static void LogToHost( const char* ) {}
};

namespace stoatworks::about
{
inline constexpr unsigned kParamCount = 4;
struct Button
{
	const char* label;
};
inline std::vector< Button > buttons()
{
	return { { "Project page" }, { "Source on GitHub" }, { "Support the work" } };
}
inline const char* defaultText()
{
	return "about";
}
} // namespace stoatworks::about

//---------------------------------------------------------------------------
// The plugin class, as far as ProcessOpenGL and the constructor reach. The
// member names and types are Colourunder.h's.
//---------------------------------------------------------------------------
class Colourunder
{
public:
	Colourunder();
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL );

//@@ENUM@@

	//CFFGLPlugin's parameter declarations, recorded.
	std::string decl;
	void SetMinInputs( unsigned ) {}
	void SetMaxInputs( unsigned ) {}
	void SetTimeSupported( bool ) {}
	float GetFloatParameter( unsigned index )
	{
		return params[ index ];
	}
	void SetParamInfo( unsigned index, const char* name, unsigned type, float value )
	{
		declare( index, name, type, value );
	}
	void SetParamInfo( unsigned index, const char* name, unsigned type, bool value )
	{
		declare( index, name, type, value ? 1.0f : 0.0f );
	}
	void SetParamInfo( unsigned index, const char* name, unsigned type, const char* )
	{
		declare( index, name, type, 0.0f );
	}
	void SetParamInfof( unsigned index, const char* name, unsigned type )
	{
		SetParamInfo( index, name, type, GetFloatParameter( index ) );
	}
	void SetOptionParamInfo( unsigned index, const char* name, unsigned count, float value )
	{
		declare( index, name, FF_TYPE_OPTION, value );
		decl += "  COUNT " + std::to_string( count ) + "\n";
	}
	void SetParamElementInfo( unsigned index, unsigned element, const char* name, float value )
	{
		decl += "  ELEMENT " + std::to_string( index ) + " " + std::to_string( element ) + " " + name + " " + rec::bits( value ) + "\n";
	}
	void SetParamRange( unsigned index, float lo, float hi )
	{
		decl += "  RANGE " + std::to_string( index ) + " " + rec::bits( lo ) + " " + rec::bits( hi ) + "\n";
	}
	void SetParamGroup( unsigned index, std::string group )
	{
		decl += "  GROUP " + std::to_string( index ) + " " + group + "\n";
	}
	void declare( unsigned index, const char* name, unsigned type, float value )
	{
		decl += "PARAM " + std::to_string( index ) + " " + std::to_string( type ) + " " + rec::bits( value ) + " " + name + "\n";
	}

	ffglex::FFGLShader intakeVShader{ 1, "intakev" };
	ffglex::FFGLShader intakeHShader{ 2, "intakeh" };
	ffglex::FFGLShader noiseShader{ 3, "noise" };
	ffglex::FFGLShader tapeShader{ 4, "tape" };
	ffglex::FFGLShader combShader{ 5, "comb" };
	ffglex::FFGLShader docShader{ 6, "doc" };
	ffglex::FFGLShader displayShader{ 7, "display" };
	ffglex::FFGLScreenQuad quad;

	colourunder::PassBuffer columns{ 10, "columns" };
	colourunder::PassBuffer intake{ 11, "intake" };
	colourunder::PassBuffer work[ 3 ] = { { 12, "work0" }, { 13, "work1" }, { 14, "work2" } };
	colourunder::PassBuffer noise{ 15, "noise" };
	GLuint lineData = 0;
	int lineDataN   = 0;
	int finalBuffer = 2;

	colourunder::Clock clock;
	bool hostTimeSeen = false;
	double hostTime   = 0.0;///< CFFGLPlugin's member, which SetTime writes
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

	float params[ PT_COUNT ] = {};
};

using namespace ffglex;
using namespace colourunder;

//@@ANON@@

//@@CONSTRUCTOR@@

//@@PROCESS@@

int main()
{
	rec::names[ 0 ]  = "host";
	rec::names[ 99 ] = "picture";
	{
		Colourunder declared;
		std::cout << declared.decl;
	}
	int W, H;
	while( std::cin >> W >> H )
	{
		Colourunder plugin;
		for( int i = 0; i < 10; ++i )
			std::cin >> plugin.params[ i ];
		int n;
		std::cin >> n;
		std::cout << "SCENARIO " << W << " " << H << "\n";
		FFGLTextureStruct picture{ static_cast< FFUInt32 >( W ), static_cast< FFUInt32 >( H ), static_cast< FFUInt32 >( W ), static_cast< FFUInt32 >( H ), 99 };
		FFGLTextureStruct* inputs[ 1 ] = { &picture };
		ProcessOpenGLStruct process{ 1, inputs, 0 };
		for( int f = 0; f < n; ++f )
		{
			double t;
			std::cin >> t;
			//cutest's renderAt: the unit declared, then Colourunder::SetTime.
			plugin.clock.SetScaleForTest( 1.0 );
			plugin.hostTimeSeen = true;
			plugin.hostTime     = t;
			rec::out.clear();
			const FFResult r = plugin.ProcessOpenGL( &process );
			char head[ 128 ];
			std::snprintf( head, sizeof head, "FRAME %d result %u seconds %.17g tracking %.17g\n", f, r, plugin.clock.Now(), plugin.lastTracking );
			std::cout << head << rec::out;
		}
	}
	return 0;
}

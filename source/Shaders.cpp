#include "Shaders.h"

namespace colourunder::shaders
{
namespace
{
const char* const kVersion = "#version 410 core\n";

//---------------------------------------------------------------------------
// The vertex shader every pass shares.
//---------------------------------------------------------------------------
const char* const kVertexBody = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// Shared by the passes that need them. BT.601's Y' and the PAL/NTSC U, V
// scalings, both ways; the fleet's integer hash.
//---------------------------------------------------------------------------
const char* const kCommon = R"(
const float kUScale = 0.492111;
const float kVScale = 0.877283;

vec3 rgbToYuv( vec3 c )
{
	float y = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
	return vec3( y, kUScale * ( c.b - y ), kVScale * ( c.r - y ) );
}

vec3 yuvToRgb( vec3 yuv )
{
	float r = yuv.x + yuv.z / kVScale;
	float b = yuv.x + yuv.y / kUScale;
	float g = ( yuv.x - 0.299 * r - 0.114 * b ) / 0.587;
	return vec3( r, g, b );
}

uint hashInt( uint v )
{
	uint state = v * 747796405u + 2891336453u;
	uint word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

float hashUnit( uint h )
{
	return float( h >> 8u ) * ( 1.0 / 16777216.0 );
}
)";

//---------------------------------------------------------------------------
// intakev: each line of the standard as the area average of the host rows it
// covers. In units of 1/N of a row, line l spans [ l H, ( l + 1 ) H ) and row
// r spans [ r N, ( r + 1 ) N ): the overlaps are integers and sum to H.
// The picture is recorded as handed over: Resolume's demo clips with alpha
// are black wherever they are transparent (rgb <= a on 99.6 % of pixels, so
// premultiplied), which is already the picture over black. Multiplying by
// alpha again would darken every soft edge twice.
//---------------------------------------------------------------------------
const char* const kIntakeVBody = R"(
uniform sampler2D Source;
uniform int HostH;
uniform int Lines;

out vec4 fragColor;

void main()
{
	ivec2 p   = ivec2( gl_FragCoord.xy );
	int lo    = p.y * HostH;
	int hi    = ( p.y + 1 ) * HostH;
	int first = lo / Lines;
	int last  = ( hi - 1 ) / Lines;
	vec4 sum  = vec4( 0.0 );
	for( int r = first; r <= last; ++r )
	{
		int overlap = min( ( r + 1 ) * Lines, hi ) - max( r * Lines, lo );
		vec4 c      = texelFetch( Source, ivec2( p.x, HostH - 1 - r ), 0 );
		sum += float( overlap ) * c;
	}
	fragColor = sum / float( HostH );
}
)";

//---------------------------------------------------------------------------
// intakeh: the host's pixels onto the line's samples. Y' through the luma
// Gaussian (Speed's bandwidth, in host pixels, centred on the sample); U and
// V through a box of the sample's own k pixels, the anti-alias the chroma
// kernel does not need at k = 1 and is identity there.
//---------------------------------------------------------------------------
const char* const kIntakeHBody = R"(
uniform sampler2D Lines;
uniform int HostW;
uniform int K;
uniform int YFirst;
uniform int YCount;
uniform float YW[ 96 ];

out vec4 fragColor;

void main()
{
	ivec2 p  = ivec2( gl_FragCoord.xy );
	int base = p.x * K;
	float y  = 0.0;
	for( int n = 0; n < YCount; ++n )
	{
		int x = clamp( base + YFirst + n, 0, HostW - 1 );
		y += YW[ n ] * rgbToYuv( texelFetch( Lines, ivec2( x, p.y ), 0 ).rgb ).x;
	}
	vec2 c = vec2( 0.0 );
	for( int n = 0; n < K; ++n )
	{
		int x = clamp( base + n, 0, HostW - 1 );
		c += rgbToYuv( texelFetch( Lines, ivec2( x, p.y ), 0 ).rgb ).yz;
	}
	fragColor = vec4( y, c / float( K ), 1.0 );
}
)";

//---------------------------------------------------------------------------
// noise: four channels of seeded white noise of unit variance, uniform in
// [ -sqrt 3, sqrt 3 ]. A pure function of ( sample, line, channel, Seed ),
// and Seed of ( video frame, generation ), so a frame is the same picture
// every time it is rendered.
//---------------------------------------------------------------------------
const char* const kNoiseBody = R"(
uniform int Samples;
uniform int LineCount;
uniform uint Seed;

out vec4 fragColor;

void main()
{
	ivec2 p = ivec2( gl_FragCoord.xy );
	uint at = uint( p.x ) + uint( Samples ) * uint( p.y );
	vec4 n;
	for( int c = 0; c < 4; ++c )
		n[ c ] = ( hashUnit( hashInt( at * 4u + uint( c ) + hashInt( Seed ) ) ) * 2.0 - 1.0 ) * 1.7320508;
	fragColor = n;
}
)";

//---------------------------------------------------------------------------
// tape: one generation's record and playback, per sample.
//---------------------------------------------------------------------------
const char* const kTapeBody = R"(
uniform sampler2D Src;
uniform sampler2D NoiseTex;
uniform sampler2D LineData;
uniform int Samples;
uniform int Gen;          //this generation's row of LineData
uniform int Pal;

uniform int YFirst;       //luma: this generation's filter (identity in generation 1)
uniform int YCount;
uniform float YW[ 96 ];
uniform int CFirst;       //chroma: record and playback together, the group delay included
uniform int CCount;
uniform float CW[ 96 ];
uniform int NYFirst;      //the FM noise's shape
uniform int NYCount;
uniform float NYW[ 96 ];
uniform int NCFirst;      //the chroma noise: the playback filter alone
uniform int NCCount;
uniform float NCW[ 96 ];

uniform float LumaSigma;
uniform float ChromaSigma;
uniform int BurstLine;    //the field line the switch falls on, or -1
uniform float BurstStart; //the sample it falls at
uniform int BurstSamples;
uniform float BurstAmount;

uniform int DropCount;
uniform vec4 Drops[ 24 ]; //( line, first sample, end sample, 0 )

out vec4 fragColor;

ivec2 at( int x, int l )
{
	return ivec2( clamp( x, 0, Samples - 1 ), l );
}

void main()
{
	ivec2 p = ivec2( gl_FragCoord.xy );
	int l   = p.y;

	float y = 0.0;
	for( int n = 0; n < YCount; ++n )
		y += YW[ n ] * texelFetch( Src, at( p.x + YFirst + n, l ), 0 ).x;
	vec2 c = vec2( 0.0 );
	for( int n = 0; n < CCount; ++n )
		c += CW[ n ] * texelFetch( Src, at( p.x + CFirst + n, l ), 0 ).yz;

	float ny = 0.0;
	for( int n = 0; n < NYCount; ++n )
		ny += NYW[ n ] * texelFetch( NoiseTex, at( p.x + NYFirst + n, l ), 0 ).x;
	vec2 nc = vec2( 0.0 );
	for( int n = 0; n < NCCount; ++n )
		nc += NCW[ n ] * texelFetch( NoiseTex, at( p.x + NCFirst + n, l ), 0 ).yz;
	float raw = texelFetch( NoiseTex, p, 0 ).w;

	vec4 ld = texelFetch( LineData, ivec2( l, Gen ), 0 );
	y += LumaSigma * ld.x * ny;
	c += ChromaSigma * ld.x * nc;

	//The bar: where the head reads the wrong track the FM demodulator loses
	//lock and the picture is noise; the burst goes, and the colour killer
	//with it.
	y = mix( y, 0.5 + 0.3 * raw, ld.y );
	c *= 1.0 - ld.y;

	//The switching transient, from the switch instant.
	if( ( l >> 1 ) == BurstLine && float( p.x ) >= BurstStart && float( p.x ) < BurstStart + float( BurstSamples ) )
	{
		y = mix( y, 0.5 + 0.4 * raw, BurstAmount );
		c *= 1.0 - BurstAmount;
	}

	//The playback phase error, as its cosine and sine (computed in double on
	//the CPU: GLSL leaves sin and cos's precision to the driver, and Apple's
	//software renderer is 8e-4 rad out). PAL's V axis is inverted on
	//alternate lines of a field, so after the decoder puts it back the same
	//error turns the other way on the next line.
	float cs = ld.z, sn = ld.w;
	if( Pal != 0 && ( ( l >> 1 ) & 1 ) != 0 )
		sn = -sn;
	c = vec2( cs * c.x - sn * c.y, sn * c.x + cs * c.y );

	//Missing oxide: no carrier, a white streak, flagged for the compensator.
	float flag = 0.0;
	for( int i = 0; i < DropCount; ++i )
		if( int( Drops[ i ].x ) == l && float( p.x ) >= Drops[ i ].y && float( p.x ) < Drops[ i ].z )
			flag = 1.0;
	if( flag > 0.5 )
	{
		y = 0.92 + 0.05 * raw;
		c = vec2( 0.0 );
	}

	fragColor = vec4( y, c, flag );
}
)";

//---------------------------------------------------------------------------
// comb: the 1H chroma average. The crosstalk canceller's comb on an NTSC
// deck and PAL's delay line both add a line to the one 1H before it -- line
// l - 2 of the frame, the same field. The first line of each field has
// nothing above it and is left alone.
//---------------------------------------------------------------------------
const char* const kCombBody = R"(
uniform sampler2D Src;
uniform int Skip;

out vec4 fragColor;

void main()
{
	ivec2 p = ivec2( gl_FragCoord.xy );
	vec4 v  = texelFetch( Src, p, 0 );
	if( Skip == 0 && p.y >= 2 )
		v.yz = 0.5 * ( v.yz + texelFetch( Src, ivec2( p.x, p.y - 2 ), 0 ).yz );
	fragColor = v;
}
)";

//---------------------------------------------------------------------------
// doc: the dropout compensator. A flagged sample takes the sample from the
// 1H delay line: the same place on the line before in the same field -- or,
// if that line dropped out there too, the one before that, because the
// delay line holds what the compensator itself put out.
//---------------------------------------------------------------------------
const char* const kDocBody = R"(
uniform sampler2D Src;
uniform int Enabled;
uniform int Step;

out vec4 fragColor;

void main()
{
	ivec2 p = ivec2( gl_FragCoord.xy );
	vec4 v  = texelFetch( Src, p, 0 );
	if( Enabled != 0 && v.w > 0.5 )
	{
		for( int k = 1; k <= 8; ++k )
		{
			int l = p.y - k * Step;
			if( l < 0 )
				break;
			vec4 u = texelFetch( Src, ivec2( p.x, l ), 0 );
			if( u.w < 0.5 )
			{
				v = u;
				break;
			}
		}
	}
	fragColor = vec4( v.xyz, 0.0 );
}
)";

//---------------------------------------------------------------------------
// display: to the host. Each host row shows its nearest line (a TV draws a
// line as a stripe); across, the line's samples by Catmull-Rom, which at
// k = 1 and no displacement reads each sample exactly. The displacement is
// the line's time-base error as the TV shows it; past either end of the line
// is blanking.
//---------------------------------------------------------------------------
const char* const kDisplayBody = R"(
uniform sampler2D Lines;
uniform sampler2D Source;
uniform sampler2D LineData;
uniform int HostW;
uniform int HostH;
uniform int LineCount;
uniform int Samples;
uniform int K;
uniform int TimeRow;
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

vec3 tap( int i, int l )
{
	return texelFetch( Lines, ivec2( clamp( i, 0, Samples - 1 ), l ), 0 ).xyz;
}

void main()
{
	int x   = clamp( int( floor( uv.x * float( HostW ) ) ), 0, HostW - 1 );
	int rgl = clamp( int( floor( uv.y * float( HostH ) ) ), 0, HostH - 1 );
	int r   = HostH - 1 - rgl;
	int l   = ( ( 2 * r + 1 ) * LineCount ) / ( 2 * HostH );

	vec4 ld = texelFetch( LineData, ivec2( l, TimeRow ), 0 );
	float s = ( float( x ) - 0.5 * float( K - 1 ) ) / float( K );
	if( s >= ld.y )
		s -= ld.x;

	vec3 yuv = vec3( 0.0 );
	if( s >= -0.5 && s <= float( Samples ) - 0.5 )
	{
		float i0 = floor( s );
		float t  = s - i0;
		int i    = int( i0 );
		float t2 = t * t, t3 = t2 * t;
		float w0 = 0.5 * ( -t3 + 2.0 * t2 - t );
		float w1 = 0.5 * ( 3.0 * t3 - 5.0 * t2 + 2.0 );
		float w2 = 0.5 * ( -3.0 * t3 + 4.0 * t2 + t );
		float w3 = 0.5 * ( t3 - t2 );
		yuv = w0 * tap( i - 1, l ) + w1 * tap( i, l ) + w2 * tap( i + 1, l ) + w3 * tap( i + 2, l );
	}

	vec4 src  = texelFetch( Source, ivec2( x, rgl ), 0 );
	vec3 rgb  = clamp( yuvToRgb( yuv ), 0.0, 1.0 );
	fragColor = vec4( mix( src.rgb, rgb, MixAmount ), mix( src.a, 1.0, MixAmount ) );
}
)";

std::string assemble( const char* body, bool common = false )
{
	std::string s = kVersion;
	if( common )
		s += kCommon;
	s += body;
	return s;
}
} // namespace

std::string Vertex()
{
	return assemble( kVertexBody );
}

std::string IntakeV()
{
	return assemble( kIntakeVBody );
}

std::string IntakeH()
{
	return assemble( kIntakeHBody, true );
}

std::string Noise()
{
	return assemble( kNoiseBody, true );
}

std::string Tape()
{
	return assemble( kTapeBody );
}

std::string Comb()
{
	return assemble( kCombBody );
}

std::string Doc()
{
	return assemble( kDocBody );
}

std::string Display()
{
	return assemble( kDisplayBody, true );
}

} // namespace colourunder::shaders

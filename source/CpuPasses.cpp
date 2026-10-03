#include "CpuPasses.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace colourunder::cpu
{
namespace
{
//---------------------------------------------------------------------------
// kCommon. //= mirrored (Shaders.cpp, kCommon)
//---------------------------------------------------------------------------
constexpr float kUScale = 0.492111f;
constexpr float kVScale = 0.877283f;

inline void rgbToYuv( float r, float g, float b, float& y, float& u, float& v )
{
	y = 0.299f * r + 0.587f * g + 0.114f * b;
	u = kUScale * ( b - y );
	v = kVScale * ( r - y );
}

inline void yuvToRgb( float y, float u, float v, float& r, float& g, float& b )
{
	r = y + v / kVScale;
	b = y + u / kUScale;
	g = ( y - 0.299f * r - 0.114f * b ) / 0.587f;
}

/// The fleet's hash, which is the GLSL `hashInt` bit for bit.
inline uint32_t hashInt( uint32_t v )
{
	return model::Hash( v );
}

inline float hashUnit( uint32_t h )
{
	return static_cast< float >( h >> 8u ) * ( 1.0f / 16777216.0f );
}

/// GLSL's mix, as the specification writes it.
inline float mixf( float a, float b, float t )
{
	return a * ( 1.0f - t ) + b * t;
}

//---------------------------------------------------------------------------
// The line raster: N lines by Ws samples, one plane per channel (the GPU's
// RGBA32F texture, split so a line's channel is contiguous).
//---------------------------------------------------------------------------
struct Lines
{
	int N  = 0;
	int Ws = 0;
	std::vector< float > c[ 4 ];

	void allocate( int n, int ws )
	{
		N  = n;
		Ws = ws;
		for( auto& plane : c )
			plane.assign( static_cast< size_t >( n ) * ws, 0.0f );
	}
	float* at( int channel, int line )
	{
		return c[ channel ].data() + static_cast< size_t >( line ) * Ws;
	}
	const float* at( int channel, int line ) const
	{
		return c[ channel ].data() + static_cast< size_t >( line ) * Ws;
	}
};

/// out[ x ] = sum over n of w[ n ] * in[ clamp( x + first + n ) ], n ascending
/// -- the shaders' kernel loops, with the clamp done once into `pad` so the
/// inner loop runs across x.
void convolve( const float* in, int Ws, const model::Kernel& k, float* out, std::vector< float >& pad )
{
	const int span = Ws + k.count - 1;
	pad.resize( static_cast< size_t >( span ) );
	for( int j = 0; j < span; ++j )
		pad[ static_cast< size_t >( j ) ] = in[ std::clamp( j + k.first, 0, Ws - 1 ) ];
	std::fill( out, out + Ws, 0.0f );
	for( int n = 0; n < k.count; ++n )
	{
		const float w  = k.weights[ n ];
		const float* p = pad.data() + n;
		for( int x = 0; x < Ws; ++x )
			out[ x ] += w * p[ x ];
	}
}

//---------------------------------------------------------------------------
// intakev and intakeh, fused per line: the host rows onto line l (the
// columns buffer's row, never stored whole), then its pixels onto samples.
//---------------------------------------------------------------------------
struct IntakeScratch
{
	std::vector< float > sum[ 3 ];
	std::vector< float > Y, U, V;
	std::vector< float > padY;
};

void intakeLine( const frame::Plan& plan, const float* picture, int l, Lines& intake, IntakeScratch& s )
{
	const model::Raster& R = plan.raster;
	const int W = R.W, H = R.H, N = R.N, K = R.k, Ws = R.Ws;

	//= mirrored (Shaders.cpp, intakev): the area average of the host rows
	//line l covers, in units of 1/N of a row. Rows from the top; the
	//picture's row 0 is the bottom.
	for( auto& v : s.sum )
		v.assign( static_cast< size_t >( W ), 0.0f );
	const int lo    = l * H;
	const int hi    = ( l + 1 ) * H;
	const int first = lo / N;
	const int last  = ( hi - 1 ) / N;
	for( int r = first; r <= last; ++r )
	{
		const int overlap = std::min( ( r + 1 ) * N, hi ) - std::max( r * N, lo );
		const float w     = static_cast< float >( overlap );
		const float* row  = picture + static_cast< size_t >( H - 1 - r ) * W * 4;
		for( int ch = 0; ch < 3; ++ch )
		{
			float* sum = s.sum[ ch ].data();
			for( int x = 0; x < W; ++x )
				sum[ x ] += w * row[ static_cast< size_t >( x ) * 4 + ch ];
		}
	}
	const float rows = static_cast< float >( H );
	s.Y.resize( static_cast< size_t >( W ) );
	s.U.resize( static_cast< size_t >( W ) );
	s.V.resize( static_cast< size_t >( W ) );
	for( int x = 0; x < W; ++x )
		rgbToYuv( s.sum[ 0 ][ x ] / rows, s.sum[ 1 ][ x ] / rows, s.sum[ 2 ][ x ] / rows, s.Y[ x ], s.U[ x ], s.V[ x ] );

	//= mirrored (Shaders.cpp, intakeh): Y' through the Speed's Gaussian in
	//host pixels, centred on the sample; U and V a box of its own K pixels.
	const model::Kernel& k = plan.intakeLuma;
	const int span         = ( Ws - 1 ) * K + k.count;
	s.padY.resize( static_cast< size_t >( span ) );
	for( int j = 0; j < span; ++j )
		s.padY[ static_cast< size_t >( j ) ] = s.Y[ static_cast< size_t >( std::clamp( j + k.first, 0, W - 1 ) ) ];
	float* y = intake.at( 0, l );
	float* u = intake.at( 1, l );
	float* v = intake.at( 2, l );
	float* a = intake.at( 3, l );
	std::fill( y, y + Ws, 0.0f );
	for( int n = 0; n < k.count; ++n )
	{
		const float w  = k.weights[ n ];
		const float* p = s.padY.data() + n;
		for( int i = 0; i < Ws; ++i )
			y[ i ] += w * p[ static_cast< size_t >( i ) * K ];
	}
	const float box = static_cast< float >( K );
	for( int i = 0; i < Ws; ++i )
	{
		const int base = i * K;
		float cu = 0.0f, cv = 0.0f;
		for( int n = 0; n < K; ++n )
		{
			const int x = std::clamp( base + n, 0, W - 1 );
			cu += s.U[ static_cast< size_t >( x ) ];
			cv += s.V[ static_cast< size_t >( x ) ];
		}
		u[ i ] = cu / box;
		v[ i ] = cv / box;
		a[ i ] = 1.0f;
	}
}

//---------------------------------------------------------------------------
// noise and tape, fused per line: the tape is the only reader of the noise
// and reads only its own line of it.
//---------------------------------------------------------------------------
struct TapeScratch
{
	std::vector< float > noise[ 4 ];
	std::vector< float > y, u, v, ny, nu, nv;
	std::vector< float > pad;
};

void tapeLine( const frame::Plan& plan, int g, const Lines& src, int l, Lines& dst, TapeScratch& s )
{
	const int Ws               = plan.raster.Ws;
	const frame::Generation& G = plan.gen[ g - 1 ];
	for( auto* v : { &s.y, &s.u, &s.v, &s.ny, &s.nu, &s.nv } )
		v->resize( static_cast< size_t >( Ws ) );

	//= mirrored (Shaders.cpp, noise): four channels of seeded white noise
	//of unit variance, a pure function of ( sample, line, channel, Seed ).
	const uint32_t seed = hashInt( G.seed );
	for( int ch = 0; ch < 4; ++ch )
		s.noise[ ch ].resize( static_cast< size_t >( Ws ) );
	for( int x = 0; x < Ws; ++x )
	{
		const uint32_t at = static_cast< uint32_t >( x ) + static_cast< uint32_t >( Ws ) * static_cast< uint32_t >( l );
		for( int ch = 0; ch < 4; ++ch )
			s.noise[ ch ][ static_cast< size_t >( x ) ] = ( hashUnit( hashInt( at * 4u + static_cast< uint32_t >( ch ) + seed ) ) * 2.0f - 1.0f ) * 1.7320508f;
	}

	//= mirrored (Shaders.cpp, tape): one generation's record and playback.
	convolve( src.at( 0, l ), Ws, G.luma, s.y.data(), s.pad );
	convolve( src.at( 1, l ), Ws, G.chroma, s.u.data(), s.pad );
	convolve( src.at( 2, l ), Ws, G.chroma, s.v.data(), s.pad );
	convolve( s.noise[ 0 ].data(), Ws, plan.lumaNoiseShape, s.ny.data(), s.pad );
	convolve( s.noise[ 1 ].data(), Ws, plan.chromaNoiseShape, s.nu.data(), s.pad );
	convolve( s.noise[ 2 ].data(), Ws, plan.chromaNoiseShape, s.nv.data(), s.pad );
	const float* raw = s.noise[ 3 ].data();

	const float* ld        = plan.LineAt( g - 1, l );
	const float lumaGain   = plan.lumaSigma * ld[ 0 ];
	const float chromaGain = plan.chromaSigma * ld[ 0 ];
	const bool burstLine   = ( l >> 1 ) == plan.burstLine;
	const float burstEnd   = plan.burstStart + static_cast< float >( plan.burstSamples );
	const float cs         = ld[ 2 ];
	const float sn         = ( plan.pal != 0 && ( ( l >> 1 ) & 1 ) != 0 ) ? -ld[ 3 ] : ld[ 3 ];

	float* oy = dst.at( 0, l );
	float* ou = dst.at( 1, l );
	float* ov = dst.at( 2, l );
	float* of = dst.at( 3, l );
	for( int x = 0; x < Ws; ++x )
	{
		float y  = s.y[ x ];
		float cu = s.u[ x ];
		float cv = s.v[ x ];
		y += lumaGain * s.ny[ x ];
		cu += chromaGain * s.nu[ x ];
		cv += chromaGain * s.nv[ x ];

		//The bar.
		y = mixf( y, 0.5f + 0.3f * raw[ x ], ld[ 1 ] );
		cu *= 1.0f - ld[ 1 ];
		cv *= 1.0f - ld[ 1 ];

		//The switching transient.
		const float fx = static_cast< float >( x );
		if( burstLine && fx >= plan.burstStart && fx < burstEnd )
		{
			y = mixf( y, 0.5f + 0.4f * raw[ x ], plan.burstAmount );
			cu *= 1.0f - plan.burstAmount;
			cv *= 1.0f - plan.burstAmount;
		}

		//The playback phase error, PAL's V switch on alternate field lines.
		const float ru = cs * cu - sn * cv;
		const float rv = sn * cu + cs * cv;
		cu             = ru;
		cv             = rv;

		//Missing oxide.
		float flag = 0.0f;
		for( int i = 0; i < G.dropCount; ++i )
		{
			const float* d = G.drops + 4 * i;
			if( static_cast< int >( d[ 0 ] ) == l && fx >= d[ 1 ] && fx < d[ 2 ] )
				flag = 1.0f;
		}
		if( flag > 0.5f )
		{
			y  = 0.92f + 0.05f * raw[ x ];
			cu = 0.0f;
			cv = 0.0f;
		}

		oy[ x ] = y;
		ou[ x ] = cu;
		ov[ x ] = cv;
		of[ x ] = flag;
	}
}

//---------------------------------------------------------------------------
// comb and doc, fused per line.
//---------------------------------------------------------------------------

//= mirrored (Shaders.cpp, comb): the 1H chroma average, the line l - 2
//(1H earlier, the same field), at one sample. Y and the flag pass through.
inline void combAt( const frame::Plan& plan, const Lines& tape, int x, int l, float out[ 4 ] )
{
	for( int ch = 0; ch < 4; ++ch )
		out[ ch ] = tape.at( ch, l )[ x ];
	if( plan.combSkip == 0 && l >= 2 )
	{
		out[ 1 ] = 0.5f * ( out[ 1 ] + tape.at( 1, l - 2 )[ x ] );
		out[ 2 ] = 0.5f * ( out[ 2 ] + tape.at( 2, l - 2 )[ x ] );
	}
}

void docLine( const frame::Plan& plan, const Lines& tape, int l, Lines& dst )
{
	const int Ws = plan.raster.Ws;
	float* oy    = dst.at( 0, l );
	float* ou    = dst.at( 1, l );
	float* ov    = dst.at( 2, l );
	float* of    = dst.at( 3, l );
	//The comb across the whole line first (combAt, unrolled so it runs
	//across x), then the compensator only where a sample is flagged.
	const float* ty = tape.at( 0, l );
	const float* tu = tape.at( 1, l );
	const float* tv = tape.at( 2, l );
	const float* tf = tape.at( 3, l );
	std::copy( ty, ty + Ws, oy );
	if( plan.combSkip == 0 && l >= 2 )
	{
		const float* pu = tape.at( 1, l - 2 );
		const float* pv = tape.at( 2, l - 2 );
		for( int x = 0; x < Ws; ++x )
		{
			ou[ x ] = 0.5f * ( tu[ x ] + pu[ x ] );
			ov[ x ] = 0.5f * ( tv[ x ] + pv[ x ] );
		}
	}
	else
	{
		std::copy( tu, tu + Ws, ou );
		std::copy( tv, tv + Ws, ov );
	}
	std::fill( of, of + Ws, 0.0f );
	if( plan.docEnabled == 0 )
		return;

	for( int x = 0; x < Ws; ++x )
	{
		//= mirrored (Shaders.cpp, doc): a flagged sample takes the nearest
		//unflagged one 1H, 2H ... earlier in the same field, up to 8.
		if( !( tf[ x ] > 0.5f ) )
			continue;
		for( int k = 1; k <= 8; ++k )
		{
			const int m = l - k * plan.docStep;
			if( m < 0 )
				break;
			float u[ 4 ];
			combAt( plan, tape, x, m, u );
			if( u[ 3 ] < 0.5f )
			{
				oy[ x ] = u[ 0 ];
				ou[ x ] = u[ 1 ];
				ov[ x ] = u[ 2 ];
				break;
			}
		}
	}
}

//---------------------------------------------------------------------------
// display. //= mirrored (Shaders.cpp, display): each host row its nearest
// line; across, Catmull-Rom from the line's samples, displaced by the line's
// time-base; past either end, blanking; Y'UV to R'G'B', clamped; Mix.
//---------------------------------------------------------------------------
void displayRow( const frame::Plan& plan, const Lines& lines, const float* picture, float* out, int rgl )
{
	const model::Raster& R = plan.raster;
	const int W = R.W, H = R.H, N = R.N, K = R.k, Ws = R.Ws;
	const int r       = H - 1 - rgl;
	const int l       = ( ( 2 * r + 1 ) * N ) / ( 2 * H );
	const float* ld   = plan.LineAt( frame::Plan::kTimeRow, l );
	const float* ly   = lines.at( 0, l );
	const float* lu   = lines.at( 1, l );
	const float* lv   = lines.at( 2, l );
	const float* src  = picture + static_cast< size_t >( rgl ) * W * 4;
	float* dst        = out + static_cast< size_t >( rgl ) * W * 4;
	const float mix   = plan.mixAmount;
	const float upper = static_cast< float >( Ws ) - 0.5f;
	auto tap          = [ Ws ]( const float* line, int i ) { return line[ std::clamp( i, 0, Ws - 1 ) ]; };

	for( int x = 0; x < W; ++x )
	{
		float s = ( static_cast< float >( x ) - 0.5f * static_cast< float >( K - 1 ) ) / static_cast< float >( K );
		if( s >= ld[ 1 ] )
			s -= ld[ 0 ];

		float y = 0.0f, u = 0.0f, v = 0.0f;
		if( s >= -0.5f && s <= upper )
		{
			const float i0 = std::floor( s );
			const float t  = s - i0;
			const int i    = static_cast< int >( i0 );
			const float t2 = t * t, t3 = t2 * t;
			const float w0 = 0.5f * ( -t3 + 2.0f * t2 - t );
			const float w1 = 0.5f * ( 3.0f * t3 - 5.0f * t2 + 2.0f );
			const float w2 = 0.5f * ( -3.0f * t3 + 4.0f * t2 + t );
			const float w3 = 0.5f * ( t3 - t2 );
			y = w0 * tap( ly, i - 1 ) + w1 * tap( ly, i ) + w2 * tap( ly, i + 1 ) + w3 * tap( ly, i + 2 );
			u = w0 * tap( lu, i - 1 ) + w1 * tap( lu, i ) + w2 * tap( lu, i + 1 ) + w3 * tap( lu, i + 2 );
			v = w0 * tap( lv, i - 1 ) + w1 * tap( lv, i ) + w2 * tap( lv, i + 1 ) + w3 * tap( lv, i + 2 );
		}

		float rgb[ 3 ];
		yuvToRgb( y, u, v, rgb[ 0 ], rgb[ 1 ], rgb[ 2 ] );
		const float* sp = src + static_cast< size_t >( x ) * 4;
		float* dp       = dst + static_cast< size_t >( x ) * 4;
		for( int ch = 0; ch < 3; ++ch )
			dp[ ch ] = mixf( sp[ ch ], std::clamp( rgb[ ch ], 0.0f, 1.0f ), mix );
		dp[ 3 ] = mixf( sp[ 3 ], 1.0f, mix );
	}
}
} // namespace

void Serial( int count, const std::function< void( int, int ) >& body )
{
	if( count > 0 )
		body( 0, count );
}

void Render( const frame::Plan& plan, const float* picture, float* out, const ParallelFor& parallel, int rowFirst, int rowLast )
{
	const model::Raster& R = plan.raster;
	rowFirst               = std::clamp( rowFirst, 0, R.H );
	rowLast                = std::clamp( rowLast, rowFirst, R.H );

	//The GPU's intake, work[ 0 ] and work[ 2 ] buffers: the source of a
	//generation, the tape's output, and the compensator's -- which is the
	//next generation's source, so two rasters ping-pong.
	Lines a, b;
	a.allocate( R.N, R.Ws );
	b.allocate( R.N, R.Ws );

	parallel( R.N, [ & ]( int first, int last ) {
		IntakeScratch scratch;
		for( int l = first; l < last; ++l )
			intakeLine( plan, picture, l, a, scratch );
	} );

	for( int g = 1; g <= plan.generations; ++g )
	{
		parallel( R.N, [ & ]( int first, int last ) {
			TapeScratch scratch;
			for( int l = first; l < last; ++l )
				tapeLine( plan, g, a, l, b, scratch );
		} );
		parallel( R.N, [ & ]( int first, int last ) {
			for( int l = first; l < last; ++l )
				docLine( plan, b, l, a );
		} );
	}

	parallel( rowLast - rowFirst, [ & ]( int first, int last ) {
		for( int row = rowFirst + first; row < rowFirst + last; ++row )
			displayRow( plan, a, picture, out, row );
	} );
}

} // namespace colourunder::cpu

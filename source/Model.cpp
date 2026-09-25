#include "Model.h"

#include <algorithm>
#include <cmath>

namespace colourunder::model
{
namespace
{
/**
	The two systems, from the documents that define them (clamp's table, and
	its sources: ITU-R BT.470-6 and BT.1700; SMPTE 170M).

	625/50: line 64 us; 1.65 us front porch, 4.7 us sync, 5.7 us back porch,
	so 51.95 us active; 576 active lines in the digital raster; 312.5 lines a
	field; the pre-equalising pulses fill the 2.5 lines between a field's last
	active line and its broad pulses (field 1 ends with line 310, V sync starts
	half-way through 313; field 2 ends half-way through 623, V sync at line 1).

	525/59.94: line 1001 / 15.75 MHz; 1.5, 4.7, 4.7 us, so 52.6556 us active;
	480 active lines in the digital raster; 262.5 lines a field; 3 lines of
	pre-equalising pulses (lines 1-3 before V sync on 4-6, 263.5-266.5 before
	266.5).

	The colour-under carriers: PAL 40.125 fH (40 fH + 1953 Hz, as vhs-decode
	writes it from IEC 774-1) = 626.953 kHz; NTSC 40 fH = 629.371 kHz.
*/
constexpr double kFhPal  = 15625.0;
constexpr double kFhNtsc = 15750000.0 / 1001.0;

const Standard kStandards[ kStandardCount ] = {
	{ "625/50 (PAL)", 64.0, 1.65, 4.7, 5.7, 576, 312.5, 2.5, 25, 1, 40.125 * kFhPal, kFhPal },
	{ "525/59.94 (NTSC)", 1001.0 / 15.75, 1.5, 4.7, 4.7, 480, 262.5, 3.0, 30000, 1001, 40.0 * kFhNtsc, kFhNtsc },
};

double frac( double x )
{
	return x - std::floor( x );
}

double gauss( double x, double sigma )
{
	return std::exp( -x * x / ( 2.0 * sigma * sigma ) );
}

double hashSigned( uint32_t h )
{
	return HashUnit( h ) * 2.0 - 1.0;
}

uint32_t mix( uint32_t a, uint32_t b )
{
	return Hash( a ^ ( Hash( b ) + 0x9e3779b9u + ( a << 6 ) + ( a >> 2 ) ) );
}

uint32_t low( int64_t v )
{
	return static_cast< uint32_t >( static_cast< uint64_t >( v ) & 0xffffffffu ) ^ static_cast< uint32_t >( static_cast< uint64_t >( v ) >> 32 );
}

constexpr uint32_t kDriftSeed   = 0x43550001u;
constexpr uint32_t kDropSeed    = 0x43550002u;
constexpr uint32_t kPhaseSeed   = 0x43550003u;
constexpr uint32_t kJitterSeed  = 0x43550004u;
} // namespace

const Standard& StandardOf( int index )
{
	return kStandards[ std::clamp( index, 0, kStandardCount - 1 ) ];
}

double LumaHalfHz( int speed )
{
	switch( speed )
	{
	case kLP: return 3.0e6 * 230.0 / 250.0;
	case kEP: return 2.4e6;
	default: return 3.0e6;
	}
}

double LumaNoise( int speed )
{
	switch( speed )
	{
	case kLP: return 0.018;
	case kEP: return 0.026;
	default: return 0.012;
	}
}

double ChromaNoiseFactor( int speed )
{
	switch( speed )
	{
	case kLP: return 1.25;
	case kEP: return 1.6;
	default: return 1.0;
	}
}

Raster MakeRaster( const Standard& s, int W, int H )
{
	Raster r;
	r.W           = std::max( 1, W );
	r.H           = std::max( 1, H );
	r.k           = std::max( 1, ( r.W + kMaxSamples - 1 ) / kMaxSamples );
	r.Ws          = ( r.W + r.k - 1 ) / r.k;
	r.N           = s.frameLines;
	r.usPerPixel  = s.Active() / r.W;
	r.usPerSample = r.usPerPixel * r.k;
	return r;
}

int LineOfRow( int row, int N, int H )
{
	return static_cast< int >( ( static_cast< int64_t >( 2 * row + 1 ) * N ) / ( 2 * static_cast< int64_t >( H ) ) );
}

int VSign( int standard, int line )
{
	if( standard != kPAL )
		return 1;
	return ( FieldLineOf( line ) & 1 ) ? -1 : 1;
}

double SigmaUs( double fHalfHz )
{
	//exp( -2 pi^2 sigma^2 f^2 ) = 1/2  =>  sigma = sqrt( ln 2 / 2 ) / ( pi f )
	return std::sqrt( std::log( 2.0 ) / 2.0 ) / ( kPi * fHalfHz ) * 1e6;
}

double SigmaUsWith( double fHalfHz, double others )
{
	if( others >= 1.0 )
		return SigmaUs( fHalfHz );
	if( others <= 0.5 )
		return 0.0;
	//exp( -2 pi^2 sigma^2 f^2 ) = 1 / ( 2 others )
	return std::sqrt( std::log( 2.0 * others ) / 2.0 ) / ( kPi * fHalfHz ) * 1e6;
}

double DisplayGain( double cyclesPerPixel, int k )
{
	if( k <= 1 )
		return 1.0;
	//Host pixel x reads sample position s = ( x - ( k - 1 ) / 2 ) / k; the
	//fundamental through taps at floor( s ) - 1 .. + 2 is
	//sum_n w_n( t ) e^{ i 2 pi nu ( n - t ) }, nu in cycles per sample.
	const double nu = cyclesPerPixel * k;
	double re       = 0.0;
	for( int c = 0; c < k; ++c )
	{
		const double s  = ( c - 0.5 * ( k - 1 ) ) / k;
		const double t  = s - std::floor( s );
		const double t2 = t * t, t3 = t2 * t;
		const double w[ 4 ] = { 0.5 * ( -t3 + 2.0 * t2 - t ), 0.5 * ( 3.0 * t3 - 5.0 * t2 + 2.0 ), 0.5 * ( -3.0 * t3 + 4.0 * t2 + t ), 0.5 * ( t3 - t2 ) };
		for( int n = -1; n <= 2; ++n )
			re += w[ n + 1 ] * std::cos( 2.0 * kPi * nu * ( n - t ) );
	}
	return re / k;
}

double BoxGain( double cyclesPerPixel, int k )
{
	if( k <= 1 )
		return 1.0;
	return std::fabs( std::sin( kPi * k * cyclesPerPixel ) / ( k * std::sin( kPi * cyclesPerPixel ) ) );
}

Kernel Gaussian( double sigma, double delay, double centre )
{
	Kernel k;
	const double shift = delay + centre;//weight of offset o is G( o + shift )
	if( sigma <= 1e-9 )
	{
		//The identity, moved by a whole number of samples only. A fractional
		//shift with no width is not a filter anyone asked for.
		k.first        = -static_cast< int >( std::lround( shift ) );
		k.count        = 1;
		k.weights[ 0 ] = 1.0f;
		return k;
	}
	//At least 4 sigma each side of the centre. floor( 4 sigma ) is not: at
	//sigma 1.24 it is 3.2 sigma, and a 1.3e-3 tail moved NTSC LP's luma band
	//edge 3e-4 MHz (cutest --chroma at 960x540 found it).
	const double reach = 4.0 * sigma;
	int lo             = static_cast< int >( std::floor( -shift - reach ) );
	int hi             = static_cast< int >( std::ceil( -shift + reach ) );
	if( hi - lo + 1 > kMaxTaps )
	{
		const int excess = hi - lo + 1 - kMaxTaps;
		lo += excess / 2;
		hi = lo + kMaxTaps - 1;
	}
	double w[ kMaxTaps ];
	double total = 0.0;
	for( int o = lo; o <= hi; ++o )
	{
		w[ o - lo ] = gauss( o + shift, sigma );
		total += w[ o - lo ];
	}
	k.first = lo;
	k.count = hi - lo + 1;
	for( int i = 0; i < k.count; ++i )
		k.weights[ i ] = static_cast< float >( w[ i ] / total );
	return k;
}

Kernel NoiseShape( double sigma, double lo )
{
	const double wide = std::sqrt( sigma * sigma + lo * lo );
	const Kernel a    = Gaussian( sigma, 0.0 );
	const Kernel b    = Gaussian( wide, 0.0 );
	Kernel k;
	k.first = std::min( a.first, b.first );
	const int last = std::max( a.first + a.count, b.first + b.count ) - 1;
	k.count = std::min( kMaxTaps, last - k.first + 1 );
	double w[ kMaxTaps ] = {};
	double power         = 0.0;
	for( int i = 0; i < k.count; ++i )
	{
		const int o  = k.first + i;
		const int ia = o - a.first, ib = o - b.first;
		double v     = 0.0;
		if( ia >= 0 && ia < a.count )
			v += a.weights[ ia ];
		if( ib >= 0 && ib < b.count )
			v -= b.weights[ ib ];
		w[ i ] = v;
		power += v * v;
	}
	const double scale = power > 0.0 ? 1.0 / std::sqrt( power ) : 0.0;
	for( int i = 0; i < k.count; ++i )
		k.weights[ i ] = static_cast< float >( w[ i ] * scale );
	return k;
}

Kernel UnitPower( Kernel k )
{
	double power = 0.0;
	for( int i = 0; i < k.count; ++i )
		power += static_cast< double >( k.weights[ i ] ) * k.weights[ i ];
	const double scale = power > 0.0 ? 1.0 / std::sqrt( power ) : 0.0;
	for( int i = 0; i < k.count; ++i )
		k.weights[ i ] = static_cast< float >( k.weights[ i ] * scale );
	return k;
}

uint32_t Hash( uint32_t v )
{
	const uint32_t state = v * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

double HashUnit( uint32_t h )
{
	return ( h >> 8 ) * ( 1.0 / 16777216.0 );
}

double Drift( double seconds, uint32_t stream )
{
	//Catmull-Rom through seeded knots, in double: C1, and a pure function of
	//elapsed time and the stream, so the same second is the same error.
	const double x   = seconds / kDriftKnotSeconds;
	const double k0d = std::floor( x );
	const double t   = x - k0d;
	const int64_t k0 = static_cast< int64_t >( k0d );
	auto knot        = [ & ]( int64_t k ) { return hashSigned( mix( mix( kDriftSeed, stream ), low( k ) ) ); };
	const double p0 = knot( k0 - 1 ), p1 = knot( k0 ), p2 = knot( k0 + 1 ), p3 = knot( k0 + 2 );
	const double v = 0.5 * ( 2.0 * p1 + ( -p0 + p2 ) * t + ( 2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 ) * t * t
	                         + ( -p0 + 3.0 * p1 - 3.0 * p2 + p3 ) * t * t * t );
	return std::clamp( v, -1.0, 1.0 );
}

double TrackingError( double amount, double seconds, uint32_t stream )
{
	return amount * ( 1.0 + 0.8 * Drift( seconds, stream ) );
}

TrackGeometry Geometry( const Standard& s )
{
	TrackGeometry g;
	const double Lf = s.fieldLines;
	const double Nf = s.FieldActive();
	g.a0            = Lf - Nf - s.preEqualising;
	g.uBlank        = frac( ( Lf - s.preEqualising + ( Lf - Nf ) / 2.0 ) / Lf );
	return g;
}

double RfAt( const Standard& s, double e, double m, int perturb )
{
	const TrackGeometry g = Geometry( s );
	if( perturb & kPerturbTrackingFrozen )
		e = 0.0;
	const double u  = ( m + 0.5 + g.a0 ) / s.fieldLines;
	const double u0 = g.uBlank - 1.0 / kTrackSlope;
	const double p  = e + kTrackSlope * ( u - u0 );
	const double d  = std::fabs( p - 2.0 * std::round( p / 2.0 ) );
	return 1.0 - d;
}

double BarCentreLine( const Standard& s, double e )
{
	const TrackGeometry g = Geometry( s );
	const double u        = frac( g.uBlank - e / kTrackSlope );
	double m              = u * s.fieldLines - g.a0 - 0.5;
	if( m >= s.fieldLines - g.a0 - 0.5 )
		m -= s.fieldLines;
	return m;
}

double BarWeight( double rf )
{
	return std::clamp( ( kRfThreshold - rf ) / kRfThreshold, 0.0, 1.0 );
}

double NoiseGain( double rf )
{
	const double x = std::clamp( ( kRfNoiseKnee - rf ) / kRfNoiseKnee, 0.0, 1.0 );
	return 1.0 + 3.0 * x * x;
}

double SwitchLine( const Standard& s, int perturb )
{
	const double before = ( perturb & kPerturbSwitchFromActive ) ? kSwitchBeforeVSync : kSwitchBeforeVSync - s.preEqualising;
	return s.FieldActive() - before;
}

double SwitchOffsetUs( const Standard& s, double switchLine )
{
	const double f = frac( switchLine );
	if( f <= 0.0 )
		return 0.0;
	return std::max( 0.0, f * s.line - s.sync - s.backPorch );
}

std::vector< Dropout > Dropouts( int64_t frame, int generation, double perFrame, int N, double activeUs )
{
	std::vector< Dropout > list;
	if( perFrame <= 0.0 )
		return list;
	const uint32_t base = mix( mix( kDropSeed, static_cast< uint32_t >( generation ) ), low( frame ) );
	const double p      = std::min( 1.0, perFrame / kMaxDropouts );
	for( int i = 0; i < kMaxDropouts; ++i )
	{
		const uint32_t h = mix( base, static_cast< uint32_t >( i ) );
		if( HashUnit( h ) >= p )
			continue;
		Dropout d;
		d.line          = std::min( N - 1, static_cast< int >( HashUnit( Hash( h + 1u ) ) * N ) );
		const double u2 = HashUnit( Hash( h + 3u ) );
		const double len = 1.0 + 20.0 * u2 * u2;
		d.us0           = HashUnit( Hash( h + 2u ) ) * activeUs;
		d.us1           = std::min( activeUs, d.us0 + len );
		list.push_back( d );
	}
	return list;
}

double PhaseError( int64_t frame, int generation, int line, double sigmaRad )
{
	if( sigmaRad <= 0.0 )
		return 0.0;
	//Smooth down the field: knots every six field lines, smoothstep between,
	//so neighbouring lines of a field carry nearly the same error -- which is
	//what lets PAL's line average cancel it.
	const int field  = FieldOf( line );
	const double m   = FieldLineOf( line ) / 6.0;
	const int k      = static_cast< int >( std::floor( m ) );
	const double t   = m - k;
	const double s   = t * t * ( 3.0 - 2.0 * t );
	const uint32_t b = mix( mix( mix( kPhaseSeed, static_cast< uint32_t >( generation ) ), low( frame ) ), static_cast< uint32_t >( field ) );
	const double a   = hashSigned( mix( b, static_cast< uint32_t >( k ) ) );
	const double c   = hashSigned( mix( b, static_cast< uint32_t >( k + 1 ) ) );
	return sigmaRad * ( a + ( c - a ) * s );
}

double BarJitter( int64_t frame, int line )
{
	return hashSigned( mix( mix( kJitterSeed, low( frame ) ), static_cast< uint32_t >( line ) ) );
}

} // namespace colourunder::model

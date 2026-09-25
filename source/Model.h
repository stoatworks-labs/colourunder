#pragma once

#include <cstdint>
#include <vector>

/**
	The deck as arithmetic. No GL, no FFGL: the plugin and the harness's
	offline checks both link this, and everything here is computed in double
	once a frame and handed to the GPU as float uniforms or one small texture.

	**The raster.** The host frame is placed as the active picture of a 625/50
	or 525/59.94 frame (clamp's timing, from BT.470-6 and SMPTE 170M): its
	width is the active line time, its height the standard's active lines, two
	interleaved fields. The signal chain runs on a LINE RASTER of N lines (576
	or 480) by Ws samples, where Ws = ceil( W / k ) and k = ceil( W / 1024 ) is
	a whole number of host pixels per sample: no picture this chain makes has
	anything above 3 MHz in it, and 1024 samples across 52 us is 19.7 MHz.

	    host picture  --intake-->  line raster (Y', U, V)  --x Generation-->
	                  tape: filters, noise, tracking, dropouts, the head switch
	                  comb: the 1H chroma average (PAL's delay line, NTSC's
	                        crosstalk comb)
	                  doc:  the dropout compensator
	    line raster  --display-->  host picture: nearest line, Catmull-Rom
	                               across, the time-base displacement

	**The one idea** is the colour-under system: chroma is recorded as a
	narrow band heterodyned down under the FM luma, so it has a fraction of
	luma's bandwidth, it picks up band-limited noise, and it comes back with a
	phase error that PAL's alternating V axis turns into desaturation and NTSC
	shows as a hue shift. Everything else -- the head switch, the tracking bar,
	the dropouts -- is the helical scan it rides on. See AGENTS.md.
*/
namespace colourunder::model
{

constexpr double kPi = 3.14159265358979323846;

/// Negative-control hooks: a bitmask the shipped plugin always carries at 0.
enum Perturb : int
{
	kPerturbNone             = 0,
	kPerturbChromaAsLuma     = 1 << 0,///< chroma given luma's bandwidth (the spec's --chroma negative)
	kPerturbNoPalAverage     = 1 << 1,///< PAL's line average skipped (the spec's --pal negative)
	kPerturbNoDelay          = 1 << 2,///< the chroma group delay left out
	kPerturbSwitchFromActive = 1 << 3,///< the head switch counted 6.5 lines from the end of the active picture, not from V sync
	kPerturbDocAdjacent      = 1 << 4,///< the compensator repeats the adjacent frame line (the other field), not 1H earlier
	kPerturbGenerationOnce   = 1 << 5,///< the chroma band-limited in generation 1 only
	kPerturbTrackingFrozen   = 1 << 6,///< the tracking bar ignores the error and sits at rest
	kPerturbResizeResetsClock = 1 << 7,///< a resize restarts the clock (the photofinish class of bug)
};

//---------------------------------------------------------------------------
// The two systems.
//---------------------------------------------------------------------------
enum StandardIndex
{
	kPAL  = 0,
	kNTSC = 1,
	kStandardCount
};

struct Standard
{
	const char* name;
	double line;        ///< line period, us
	double frontPorch;  ///< us
	double sync;        ///< us
	double backPorch;   ///< us
	int frameLines;     ///< active lines in a frame (two fields): the digital raster
	double fieldLines;  ///< line periods in one field, the half line included
	double preEqualising;///< line periods from the end of a field's active picture to the start of its V sync
	int64_t frameNum;   ///< frames per second, as num / den
	int64_t frameDen;
	double colourUnderHz;///< the colour-under carrier (informational: see ColourUnderHz())
	double lineHz;      ///< fH

	double Active() const
	{
		return line - frontPorch - sync - backPorch;
	}
	int FieldActive() const
	{
		return frameLines / 2;
	}
	double FrameRate() const
	{
		return static_cast< double >( frameNum ) / static_cast< double >( frameDen );
	}
};

const Standard& StandardOf( int index );

//---------------------------------------------------------------------------
// The format's numbers. ATTRIBUTIONS.md and AGENTS.md say where each comes
// from and which are unconfirmed.
//---------------------------------------------------------------------------

/// The colour-under band: chroma's half-amplitude (-6 dB) frequency, Hz, as
/// a baseband U/V bandwidth. The same for both standards and every speed.
constexpr double kChromaHalfHz = 0.5e6;

enum SpeedIndex
{
	kSP = 0,
	kLP = 1,
	kEP = 2,
	kSpeedCount
};

/// Luma's half-amplitude frequency by tape speed, Hz: 3.0 MHz at SP; LP from
/// the 250 -> 230 line ratio; EP an assumption (see AGENTS.md).
double LumaHalfHz( int speed );

/// The luma noise's standard deviation in code values, by speed (assumed).
double LumaNoise( int speed );
/// Chroma noise scales with speed by this factor (assumed).
double ChromaNoiseFactor( int speed );

/// The head switch: this many line periods before the start of V sync.
constexpr double kSwitchBeforeVSync = 6.5;
/// The TV's horizontal AFC recovery after the switch, in lines (assumed).
constexpr double kAfcLines = 12.0;
/// The switching transient's length, us (assumed).
constexpr double kSwitchBurstUs = 2.0;

/// The de-emphasis corner that shapes the FM noise: below it the noise is
/// cut (the IEC 774-1 de-emphasis mid frequency, via vhs-decode).
constexpr double kDeemphasisHz = 273755.82;

/// Tracking geometry: the head crosses this many track pitches over one
/// field (a fixed track-angle mismatch between the recording and this deck),
/// and the FM demodulator loses lock below this fraction of full RF.
constexpr double kTrackSlope     = 2.0;
constexpr double kRfThreshold    = 0.06;
constexpr double kRfNoiseKnee    = 0.15;
/// The tracking error's drift: knots this far apart, seconds.
constexpr double kDriftKnotSeconds = 1.7;

/// How many generations, and how many dropouts a generation may hold.
constexpr int kGenerationsMin = 1;
constexpr int kGenerationsMax = 5;
constexpr int kMaxDropouts    = 24;

/// Taps per kernel, the most any uniform array holds.
constexpr int kMaxTaps = 96;

/// The line raster never holds more than this many samples a line.
constexpr int kMaxSamples = 1024;

//---------------------------------------------------------------------------
// Rasters.
//---------------------------------------------------------------------------
struct Raster
{
	int W = 0, H = 0;   ///< the host
	int k = 1;          ///< host pixels per sample
	int Ws = 0;         ///< samples per line
	int N = 0;          ///< lines (the standard's frame lines)
	double usPerSample = 0.0;
	double usPerPixel  = 0.0;
};

Raster MakeRaster( const Standard& s, int W, int H );

/// The frame line a host row (from the TOP) displays: floor( ( 2r + 1 ) N / 2H ).
int LineOfRow( int row, int N, int H );

/// A frame line's field and line within the field.
inline int FieldOf( int line )
{
	return line & 1;
}
inline int FieldLineOf( int line )
{
	return line >> 1;
}

/// The PAL V switch, as seen after the decoder re-inverts it: +1 or -1 by
/// field line. NTSC is +1 everywhere.
int VSign( int standard, int line );

//---------------------------------------------------------------------------
// Kernels, in samples (or host pixels at the intake).
//---------------------------------------------------------------------------
struct Kernel
{
	int first = 0;///< the offset of weights[ 0 ], i - j
	int count = 1;
	float weights[ kMaxTaps ] = { 1.0f };
};

/// The sampled Gaussian of `sigma` delayed by `delay`: output j reads input
/// j + first + n with weight G( first + n + delay ), normalised to sum 1,
/// truncated at 4 sigma. sigma <= 0 is the (possibly shifted) identity.
/// `centre` is a fractional offset of the output position (the intake's
/// sample centres sit at ( k - 1 ) / 2 host pixels).
Kernel Gaussian( double sigma, double delay, double centre = 0.0 );

/// The FM noise shape: G( sigma ) - G( sqrt( sigma^2 + lo^2 ) ), zero mean,
/// normalised to unit L2 so white noise of unit variance comes out at unit
/// variance.
Kernel NoiseShape( double sigma, double lo );

/// A Gaussian normalised to unit L2 (for noise).
Kernel UnitPower( Kernel k );

/// sigma in time (us) for a half-amplitude frequency (Hz): the Gaussian
/// whose response exp( -2 pi^2 sigma^2 f^2 ) is 1/2 at fHalf.
double SigmaUs( double fHalfHz );

/// The same, for a Gaussian that shares the band edge with other stages
/// whose gain there is `others`: the one whose response is 1 / ( 2 others )
/// at fHalf, so the chain's is exactly 1/2. `others` <= 1/2 cannot be met
/// and gives the narrowest the raster holds (sigma 0).
double SigmaUsWith( double fHalfHz, double others );

/// The display's Catmull-Rom from samples k host pixels apart, as the
/// fundamental of a frequency (cycles per host pixel) sees it, averaged over
/// the k phases the host pixels sit at. 1 at k = 1.
double DisplayGain( double cyclesPerPixel, int k );

/// The intake's box of k host pixels (the chroma's anti-alias). 1 at k = 1.
double BoxGain( double cyclesPerPixel, int k );

//---------------------------------------------------------------------------
// The tape and the deck, per frame.
//---------------------------------------------------------------------------

/// The fleet's hash (PCG output mix), exact in 32 bits on both sides.
uint32_t Hash( uint32_t v );
double HashUnit( uint32_t h );

/// The seeded slow wander in [-1, 1] behind the tracking error, as a
/// function of elapsed seconds; `stream` separates the generations.
double Drift( double seconds, uint32_t stream );

/// The tracking error, in track pitches, for a Tracking amount.
double TrackingError( double amount, double seconds, uint32_t stream );

/// Field position of the head for field line m, and the RF it reads there.
struct TrackGeometry
{
	double uBlank = 0.0;///< where the crossover rests at zero error, field fraction from V sync
	double a0     = 0.0;///< line periods from V sync start to the first active line
};
TrackGeometry Geometry( const Standard& s );

/// RF level ( 0..1 ) at field line m for tracking error e.
double RfAt( const Standard& s, double e, double m, int perturb );

/// The field line the bar is centred on for error e (may lie outside the
/// active lines; the closed form the harness checks against is its own).
double BarCentreLine( const Standard& s, double e );

/// The bar's weight from RF: 0 above threshold, 1 at no RF.
double BarWeight( double rf );

/// The FM noise gain from RF.
double NoiseGain( double rf );

/// Where the head switch falls, as a field line with a fraction (PAL 284.0,
/// NTSC 236.5), and the active-line sample it falls at on its own line.
double SwitchLine( const Standard& s, int perturb );
double SwitchOffsetUs( const Standard& s, double switchLine );

struct Dropout
{
	int line = 0;   ///< frame line
	double us0 = 0.0, us1 = 0.0;///< active-line time
};

/// A generation's dropouts in video frame F.
std::vector< Dropout > Dropouts( int64_t frame, int generation, double perFrame, int N, double activeUs );

/// A generation's per-line chroma phase error, radians.
double PhaseError( int64_t frame, int generation, int line, double sigmaRad );

/// Per-line jitter in the tracking bar, a unit value in [-1, 1].
double BarJitter( int64_t frame, int line );

} // namespace colourunder::model

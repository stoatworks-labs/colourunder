#include "Frame.h"

#include <algorithm>
#include <cmath>

namespace colourunder::frame
{

uint32_t FrameSeed( int64_t frame, int generation )
{
	const uint64_t f = static_cast< uint64_t >( frame );
	return model::Hash( static_cast< uint32_t >( f ) ^ model::Hash( static_cast< uint32_t >( f >> 32 ) + 0x4355AA00u + 977u * static_cast< uint32_t >( generation ) ) );
}

Plan Make( const Values& values, int W, int H, double seconds, const Hooks& hooks )
{
	Plan plan;
	const int perturb = hooks.perturb;
	const bool quiet  = hooks.quiet;

	//---------------------------------------------------------------------
	// The settings.
	//---------------------------------------------------------------------
	const int standardIndex   = controls::OptionIndex( values.standard, model::kStandardCount );
	const int speed           = controls::OptionIndex( values.speed, model::kSpeedCount );
	const double tracking     = controls::Tracking( values.tracking );
	const double switchUs     = controls::HeadSwitchUs( values.headSwitch );
	const double dropsPerFrame = controls::DropoutsPerFrame( values.wear );
	const double wearGain     = controls::WearNoiseGain( values.wear );
	const int generations     = controls::Generation( values.generation );
	const bool doc            = values.doc >= 0.5f;
	const double delayUs      = ( perturb & model::kPerturbNoDelay ) ? 0.0 : controls::ChromaDelayUs( values.chromaDelay );
	const double chromaNoise  = quiet ? 0.0 : controls::ChromaNoise( values.chromaNoise ) * model::ChromaNoiseFactor( speed );
	const double phaseSigma   = quiet ? 0.0 : controls::PhaseSigmaRad( values.chromaNoise );
	const double lumaNoise    = quiet ? 0.0 : model::LumaNoise( speed );

	const model::Standard& standard = model::StandardOf( standardIndex );
	plan.standardIndex              = standardIndex;
	plan.raster                     = model::MakeRaster( standard, W, H );
	const model::Raster& R          = plan.raster;
	plan.seconds                    = seconds;
	plan.frame                      = static_cast< int64_t >( std::floor( seconds * standard.FrameRate() ) );
	plan.generations                = generations;
	const int64_t frame             = plan.frame;

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
	plan.intakeLuma                = model::Gaussian( lumaFirstUs / R.usPerPixel, 0.0, -0.5 * ( R.k - 1 ) );
	const model::Kernel lumaLater  = model::Gaussian( lumaSigmaUs / R.usPerSample, 0.0 );
	const model::Kernel identity   = model::Gaussian( 0.0, 0.0 );
	const model::Kernel chromaFirst = model::Gaussian( chromaFirstUs / R.usPerSample, delayUs / R.usPerSample );
	const model::Kernel chroma     = model::Gaussian( chromaSigmaUs / R.usPerSample, delayUs / R.usPerSample );
	const model::Kernel delayOnly  = model::Gaussian( 0.0, delayUs / R.usPerSample );
	plan.chromaNoiseShape          = model::UnitPower( model::Gaussian( chromaSigmaUs / std::sqrt( 2.0 ) / R.usPerSample, delayUs / R.usPerSample ) );
	plan.lumaNoiseShape            = model::NoiseShape( lumaSigmaUs / R.usPerSample, lowSigmaUs / R.usPerSample );

	//---------------------------------------------------------------------
	// Per line: the tracking bar, the noise gain, the phase error, the
	// switch; and the display's time-base. In double.
	//---------------------------------------------------------------------
	const double switchLine = model::SwitchLine( standard, perturb );
	const int switchAt      = static_cast< int >( std::floor( switchLine ) );
	const double switchSample = model::SwitchOffsetUs( standard, switchLine ) / R.usPerSample;
	plan.lineData.assign( static_cast< size_t >( R.N ) * Plan::kDataRows * 4, 0.0f );
	std::vector< double > finalBar( static_cast< size_t >( R.N ), 0.0 );
	for( int g = 1; g <= generations; ++g )
	{
		const double e = hooks.forceTracking ? hooks.forcedTracking : model::TrackingError( tracking, seconds, static_cast< uint32_t >( g ) );
		if( g == 1 )
			plan.tracking = e;
		for( int l = 0; l < R.N; ++l )
		{
			const int m     = model::FieldLineOf( l );
			const double rf = model::RfAt( standard, e, m, perturb );
			const double w  = model::BarWeight( rf );
			float* t        = plan.lineData.data() + ( static_cast< size_t >( g - 1 ) * R.N + l ) * 4;
			t[ 0 ]          = static_cast< float >( model::NoiseGain( rf ) * wearGain );
			t[ 1 ]          = static_cast< float >( w );
			const double phi = hooks.forcePhase ? hooks.forcedPhase : model::PhaseError( frame, g, l, phaseSigma );
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
		float* t = plan.lineData.data() + ( static_cast< size_t >( Plan::kTimeRow ) * R.N + l ) * 4;
		t[ 0 ]   = static_cast< float >( shift );
		t[ 1 ]   = static_cast< float >( from );
	}

	//---------------------------------------------------------------------
	// The generations: each one's filters, its noise seed and its dropouts,
	// in samples.
	//---------------------------------------------------------------------
	for( int g = 1; g <= generations; ++g )
	{
		Generation& G = plan.gen[ g - 1 ];
		G.luma        = g == 1 ? identity : lumaLater;
		const bool bandLimit = g == 1 || !( perturb & model::kPerturbGenerationOnce );
		G.chroma      = g == 1 ? chromaFirst : bandLimit ? chroma : delayOnly;
		G.seed        = FrameSeed( frame, g );

		std::vector< model::Dropout > drops = model::Dropouts( frame, g, dropsPerFrame, R.N, standard.Active() );
		if( g == 1 && hooks.forceDropout )
			drops.insert( drops.begin(), hooks.forcedDrop );
		G.dropCount = std::min( model::kMaxDropouts, static_cast< int >( drops.size() ) );
		for( int i = 0; i < G.dropCount; ++i )
		{
			G.drops[ 4 * i + 0 ] = static_cast< float >( drops[ i ].line );
			G.drops[ 4 * i + 1 ] = static_cast< float >( drops[ i ].us0 / R.usPerSample );
			G.drops[ 4 * i + 2 ] = static_cast< float >( drops[ i ].us1 / R.usPerSample );
		}
	}

	plan.lumaSigma    = static_cast< float >( lumaNoise );
	plan.chromaSigma  = static_cast< float >( chromaNoise );
	plan.pal          = standardIndex == model::kPAL ? 1 : 0;
	plan.burstLine    = switchUs > 0.0 ? switchAt : -1;
	plan.burstStart   = static_cast< float >( switchSample );
	plan.burstSamples = std::max( 1, static_cast< int >( std::lround( model::kSwitchBurstUs / R.usPerSample ) ) );
	plan.burstAmount  = static_cast< float >( std::min( 1.0, values.headSwitch * 1.5 ) );
	plan.combSkip     = ( standardIndex == model::kPAL && ( perturb & model::kPerturbNoPalAverage ) ) ? 1 : 0;
	plan.docEnabled   = doc ? 1 : 0;
	plan.docStep      = ( perturb & model::kPerturbDocAdjacent ) ? 1 : 2;
	plan.mixAmount    = controls::Amount( values.mix );
	return plan;
}

} // namespace colourunder::frame

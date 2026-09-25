#include "Controls.h"

#include "Model.h"

#include <algorithm>
#include <cmath>

namespace colourunder::controls
{
namespace
{
double unit( float value )
{
	return std::clamp( static_cast< double >( value ), 0.0, 1.0 );
}
} // namespace

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

const char* StandardName( int index )
{
	return index == model::kNTSC ? "NTSC" : "PAL";
}

const char* SpeedName( int index )
{
	switch( index )
	{
	case model::kLP: return "LP";
	case model::kEP: return "EP";
	default: return "SP";
	}
}

double Tracking( float value )
{
	return unit( value );
}

double HeadSwitchUs( float value )
{
	return 2.0 * unit( value );
}

double DropoutsPerFrame( float value )
{
	const double v = unit( value );
	return 12.0 * v * v;
}

double WearNoiseGain( float value )
{
	return 1.0 + 0.8 * unit( value );
}

int Generation( float value )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), model::kGenerationsMin, model::kGenerationsMax );
}

double ChromaDelayUs( float value )
{
	return unit( value );
}

float ChromaDelayParam( double us )
{
	return static_cast< float >( std::clamp( us, 0.0, 1.0 ) );
}

double ChromaNoise( float value )
{
	return 0.04 * unit( value );
}

double PhaseSigmaRad( float value )
{
	return 15.0 * unit( value ) * model::kPi / 180.0;
}

float Amount( float value )
{
	return static_cast< float >( unit( value ) );
}

} // namespace colourunder::controls

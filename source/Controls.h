#pragma once

/**
	Host parameters are 0..1; these are what they mean.

	`SetParamInfo` clamps a STANDARD default into 0..1 *before* returning, and
	`SetParamRange` can only be called afterwards, so every slider here is a
	plain 0..1 float and the conversions live in this one file, which the
	plugin and the harness both use. Options are mapped by INDEX: an option
	parameter's range reads back 0..1 from the SDK whatever its element count.
	Generation is a real FF_TYPE_INTEGER with a real range.
*/
namespace colourunder::controls
{

int OptionIndex( float value, int count );

const char* StandardName( int index );
const char* SpeedName( int index );

/// Tracking: the error's scale, in track pitches at its mean, v.
double Tracking( float value );

/// Head Switch: the head-to-head time-base step, 2 v us.
double HeadSwitchUs( float value );

/// Wear: dropouts per video frame, 12 v^2 ...
double DropoutsPerFrame( float value );
/// ... and the RF it costs: the noise gain 1 + 0.8 v.
double WearNoiseGain( float value );

/// Generation: the integer, clamped to 1..5.
int Generation( float value );

/// Chroma Delay: the playback filters' group delay, 1.0 v us.
double ChromaDelayUs( float value );
/// The value that is exactly this many microseconds.
float ChromaDelayParam( double us );

/// Chroma Noise: the chroma noise's standard deviation per channel, 0.04 v,
/// and the phase error's, 15 v degrees (in radians).
double ChromaNoise( float value );
double PhaseSigmaRad( float value );

float Amount( float value );

} // namespace colourunder::controls
